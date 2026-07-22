#pragma once

#include "board_session.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"
#include "register_map.h"
#include "board_catalog.h"
#include "tool_version.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <cmath>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace psb::tui {

using namespace ftxui;

inline const std::vector<std::string> kOpModes  = {"Normal", "Automatic"};
inline const std::vector<std::string> kStartPol = {"Load NVS Config", "Factory Default"};
inline const std::vector<std::string> kBaudNames = {"115200", "9600", "19200", "38400"};

// Builds one board's full dashboard: connection modal, SysConfig popup,
// menu bar, tab bar/content (Monitor + CH0..CHn), status bar. Call once per
// board at startup; the board switcher picks which already-built dashboard
// is visible. Everything here reads/writes only `board`'s own fields (or
// `busWorker`'s shared queue for this board's bus) — never another board's
// state, so N of these coexisting is safe.
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    Component globalPreferences,
                                    std::function<size_t()> liveBoardCount) {
    // ---- Connection inputs (live in the connection modal) ----
    auto baudInp  = Input(&board.baudVal,  "baud");
    auto slaveInp = Input(&board.slaveVal, "id");

    // Port list & selection
    auto portList = std::make_shared<std::vector<std::string>>();
    auto portIdx = std::make_shared<int>(-1);

    auto doScanPorts = [&board, portList, portIdx, &screen] {
        *portList = PsbSerialBus::scanPorts();
        *portIdx = selectedPortIndex(*portList, board.portVal);
        board.portVal = *portIdx >= 0 ? (*portList)[*portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };

    auto portDropdown = Dropdown(portList.get(), portIdx.get());
    auto visiblePortDropdown = Maybe(portDropdown, [portList] { return !portList->empty(); });
    auto bScan = Button("Rescan", [doScanPorts] { doScanPorts(); });

    auto doConnect = [&board, &busWorker, &screen, &running, timeoutMs] {
        // board.connected must guard here, not just at each call site: the
        // toggle button below only ever reaches doConnect() while neither
        // is true (it calls doDisconnect() instead when connected), but
        // Connect All (main.cpp) calls board.connect() unconditionally on
        // every board. Without this, re-invoking doConnect() on an already-
        // connected board spawns a second thread that calls
        // client->verifyProtocol() concurrently with the bus worker's own
        // in-flight polling of the same ModbusClientPort — a real data race
        // that crashed the app (segfault in ModbusObject::objectName()),
        // not a harmless redundant no-op.
        if (board.portVal.empty() || board.connecting || board.connected) return;
        board.abortConnect = false;
        board.connecting = true;
        board.connectStart = std::chrono::steady_clock::now();
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Connecting to " + board.portVal + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&board, &busWorker, &screen, &running, timeoutMs] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(board.baudVal);  } catch (...) {}
            try { slave = std::stoi(board.slaveVal); } catch (...) {}
            // rebind() first so a corrected slave ID takes effect even if
            // the bus itself is already open (a sibling board on this bus
            // connected first).
            board.client->rebind(slave);
            bool ok = board.bus->isConnected() || board.bus->connect(board.portVal, baud, timeoutMs);
            if (ok) ok = board.client->verifyProtocol();
            board.connected = ok && !board.abortConnect;
            if (board.abortConnect) { ok = false; }
            if (!running) { board.connecting = false; return; }
            if (ok) {
                { std::lock_guard<std::mutex> lk(busWorker.workMutex);
                  busWorker.workQueue.push([&board, &screen, &running] {
                      doFullScan(*board.client, board.connected, board.data, screen, running);
                      board.data.valid = board.connected.load();
                      board.pendingChannelCount.store(board.data.numChannels(), std::memory_order_release);
                      board.pendingSync.store(true, std::memory_order_release);
                      screen.PostEvent(Event::Custom);
                  }); }
                busWorker.workCv.notify_one();
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - board.connectStart).count();
            { std::lock_guard<std::mutex> lk(board.statusMutex);
              if (board.abortConnect)
                  board.statusMsg = "Connection aborted";
              else if (ok)
                  board.statusMsg = "";
              else {
                  auto e = board.client->lastError();
                  board.statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                              + " (after " + std::to_string(static_cast<int>(elapsed / 1000.0)) + "s)";
              }
            }
            board.connecting = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    auto doDisconnect = [&board, &busWorker, &screen] {
        board.abortConnect = true;
        // While connecting, the detached thread in doConnect owns the port
        // right now (calling client->rebind()/verifyProtocol() outside the
        // bus worker's queue — see doConnect's own comment). Enqueuing an
        // active disconnect here would run client->disconnect() on the
        // worker thread concurrently with that in-flight call on the same
        // non-thread-safe ModbusClientPort — the same crash class
        // doConnect's board.connected guard fixes for Connect All, but
        // symmetric on Disconnect All. Just flag the abort and let the
        // connect thread unwind itself and post its own refresh, matching
        // the toggle button's own "Abort" behavior below.
        if (board.connecting.load()) return;
        board.connected = false; board.data.valid = false;
        // Enqueue disconnect on this board's bus worker to serialise with
        // in-flight Modbus I/O — avoids use-after-free on the bus's port.
        // Only disconnects the bus if no *other* board on it is still
        // connected — a shared bus stays open for sibling boards.
        { std::lock_guard<std::mutex> lk(busWorker.workMutex);
          busWorker.workQueue.push([&board, &busWorker] {
              board.client->disconnect();
              bool anyOtherConnected = false;
              for (BoardSession* b : busWorker.boards)
                  if (b != &board && b->connected.load()) { anyOtherConnected = true; break; }
              if (!anyOtherConnected) busWorker.bus->disconnect();
          }); }
        busWorker.workCv.notify_one();
        board.tabTitles = {"Monitor"}; board.activeTab = std::min(board.activeTab, 0);
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Disconnected"; }
        screen.PostEvent(Event::Custom);
    };

    // Exposes the same connect/disconnect logic the toggle button below
    // uses, for main.cpp's Connect All/Disconnect All to call directly —
    // see BoardSession::connect/disconnect's own comment.
    board.connect = doConnect;
    board.disconnect = doDisconnect;

    // ---- Connect/Disconnect/Abort toggle button ----
    ButtonOption connBtnOpt{};
    connBtnOpt.transform = [&board](const EntryState& es) -> Element {
        std::string lbl = board.connected.load() ? "Disconnect"
                        : board.connecting.load()  ? "Abort"
                        : "Connect";
        auto e = text("[ " + lbl + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    auto bConnToggle = Button("", [&board, &screen, doConnect, doDisconnect, doScanPorts] {
        if (board.connected.load())     doDisconnect();
        else if (board.connecting.load()) board.abortConnect = true;
        else {
            doScanPorts();
            board.showConnModal = true;
            screen.PostEvent(Event::Custom);
        }
    }, connBtnOpt);

    // ---- Connection modal ----
    auto bConnInModal = ActionButton("Connect", [&board, doConnect] {
        if (!board.portVal.empty() && !board.connecting) { doConnect(); board.showConnModal = false; }
    });
    auto bCancelConn = ActionButton("Cancel", [&board, &screen] {
        board.showConnModal = false; screen.PostEvent(Event::Custom);
    });
    auto connModalForm   = Container::Vertical({visiblePortDropdown, bScan, baudInp, slaveInp, bConnInModal, bCancelConn});
    auto connModalPopup  = Renderer(connModalForm, [&board, portList, portIdx, visiblePortDropdown, bScan, baudInp, slaveInp, bConnInModal, bCancelConn] {
        if (*portIdx >= 0 && *portIdx < static_cast<int>(portList->size()))
            board.portVal = (*portList)[*portIdx];
        Element portChoice = portList->empty()
            ? text("(no ports found)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Connection Settings — " + board.nickname + " ") | bold | center,
            separator(),
            hbox({ text("Port  : "), portChoice, text(" "), bScan->Render() }),
            hbox({ text("Baud  : "), baudInp->Render()  | size(WIDTH, EQUAL, 8)  }),
            hbox({ text("Slave : "), slaveInp->Render() | size(WIDTH, EQUAL, 5)  }),
            separator(),
            hbox({ bConnInModal->Render(), text("  "), bCancelConn->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 42);
    });

    // Always available (not gated on connection state) — the exact case
    // this exists for is a stale board that can never connect, which
    // otherwise has no way to leave the topology short of a restart. No
    // confirmation prompt, matching Remove Bus/Remove Board in the wizard
    // (Global Constraints) — reversible via Add.
    auto bRemove = ActionButton("Remove", [requestRemove] { requestRemove(); });

    // ---- SysConfig popup ----
    auto bSysCfg = ActionButton("Setting", [&board, &screen] {
        if (!board.showSysCfg && board.data.valid) syncDataToInputs(board.data, board.inputs);
        board.showSysCfg = !board.showSysCfg; screen.PostEvent(Event::Custom);
    });

    // scOpMode shares inputs.opModeIdx with menuModeC; autoCommit=true writes on every click.
    auto scOpMode  = InlineCycler(kOpModes, &board.inputs.opModeIdx, [&board] {
        postWrite(*board.appState, board.inputs, "OpMode",
            [&board] { return board.client->writeOperatingMode(static_cast<OpMode>(board.inputs.opModeIdx)); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    }, /*autoCommit=*/true);
    auto scStartup = InlineCycler(kStartPol, &board.inputs.startupIdx, [&board] {
        postWrite(*board.appState, board.inputs, "StartupPol",
            [&board] { return board.client->writeStartupChannelPolicy((uint16_t)board.inputs.startupIdx); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    });
    auto scSlave = Input(&board.inputs.slaveAddr, "1-247");
    auto scBaud = InlineCycler(kBaudNames, &board.inputs.baudIdx, [] {});

    auto scSaveModbus = ActionButton("Save Modbus", [&board, &busWorker, &screen] {
        uint16_t slaveAddress = 0;
        if (!parseModbusSlaveAddress(board.inputs.slaveAddr, slaveAddress)) {
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Error: slave address must be 1-247"; }
            screen.PostEvent(Event::Custom);
            return;
        }
        if (board.inputs.baudIdx < 0 || board.inputs.baudIdx >= static_cast<int>(kBaudNames.size())) {
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Error: invalid baud rate"; }
            screen.PostEvent(Event::Custom);
            return;
        }

        const std::string stagedSlave = board.inputs.slaveAddr;
        const int stagedBaud = board.inputs.baudIdx;
        const SystemConfig current = board.data.sysCfg;
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Writing Modbus config..."; }
        screen.PostEvent(Event::Custom);

        std::function<void()> item = [&board, &screen, stagedSlave, stagedBaud, current] {
            auto result = saveModbusSettings(
                stagedSlave, stagedBaud, current,
                [&board](uint16_t value) { return board.client->writeSlaveAddress(value); },
                [&board](uint16_t value) { return board.client->writeBaudRateCode(value); });

            if (result == ModbusSettingsSaveResult::Success) {
                board.data.sysCfg = board.client->readSystemConfig();
                syncDataToInputs(board.data, board.inputs);
            }
            std::string resultMessage = modbusSettingsStatusMessage(result, board.client->lastError());
            { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = std::move(resultMessage); }
            screen.PostEvent(Event::Custom);
        };

        { std::lock_guard<std::mutex> lk(busWorker.workMutex); busWorker.workQueue.push(std::move(item)); }
        busWorker.workCv.notify_one();
    });

    auto saveSystemConfig = [&board] {
        postWrite(*board.appState, board.inputs, "Save",
            [&board] { return board.client->sendParamAction(-1, ParamAction::Save); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    };
    auto scSave    = Button("Save", saveSystemConfig);
    auto scLoad    = Button("Load",    [&board] { postWrite(*board.appState, board.inputs, "Load",
        [&board] { return board.client->sendParamAction(-1, ParamAction::Load);         }, [&board] { board.data.sysCfg = board.client->readSystemConfig(); }); });
    auto scFactory = Button("Factory", [&board] { postWrite(*board.appState, board.inputs, "Factory",
        [&board] { return board.client->sendParamAction(-1, ParamAction::FactoryReset); }, [&board] { board.data.sysCfg = board.client->readSystemConfig(); }); });
    // SoftwareReset: send reset and mark disconnected — device will reboot.
    auto scReset   = Button("Reset",   [&board] {
        postWrite(*board.appState, board.inputs, "SysReset",
            [&board] { return board.client->sendParamAction(-1, ParamAction::SoftwareReset); },
            [&board] { board.connected = false; board.data.valid = false; board.client->disconnect(); });
        board.showSysCfg = false;
    });
    auto scClose   = Button("Close",   [&board, &screen] { board.showSysCfg = false; screen.PostEvent(Event::Custom); });

    auto sysCfgForm = Container::Vertical({
        scOpMode, scStartup, scSave, scLoad, scFactory,
        scSlave, scBaud, scSaveModbus,
        scReset, scClose,
    });
    auto sysCfgPopup = Renderer(sysCfgForm, [&board, scOpMode, scStartup, scSave, scLoad, scFactory, scSlave, scBaud, scSaveModbus, scReset, scClose] {
        return vbox({
            text(" System Config — " + board.nickname + " ") | bold | center, separator(),
            hbox({ text("Working Mode  : "), scOpMode->Render()  }),
            hbox({ text("Startup Policy: "), scStartup->Render() }),
            separator(),
            hbox({ scSave->Render(), text("  "), scLoad->Render(), text("  "), scFactory->Render() }) | center,
            separator(),
            text(" Modbus (next boot) ") | bold | center,
            hbox({ text("Slave Address : "), scSlave->Render() | size(WIDTH, EQUAL, 8) }),
            hbox({ text("Baud Rate     : "), scBaud->Render() }),
            scSaveModbus->Render() | center,
            separator(),
            scReset->Render() | center,
            separator(),
            scClose->Render() | center,
        }) | border | size(WIDTH, EQUAL, 48);
    });

    // ---- Menu bar mode cycler (shares opModeIdx with scOpMode) ----
    auto menuModeC = InlineCycler(kOpModes, &board.inputs.opModeIdx, [&board] {
        postWrite(*board.appState, board.inputs, "OpMode",
            [&board] { return board.client->writeOperatingMode(static_cast<OpMode>(board.inputs.opModeIdx)); },
            [&board] { board.data.sysCfg = board.client->readSystemConfig(); });
    }, /*autoCommit=*/true);

    auto menuSave = ActionButton("Save", saveSystemConfig);
    auto connectedMenuSave = Maybe(menuSave, [&board] { return board.connected.load(); });
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bRemove});

    // ---- Tab bar ----
    MenuOption tabOpt = MenuOption::Horizontal();
    tabOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto tabBar = Menu(&board.tabTitles, &board.activeTab, tabOpt);

    // ---- Tab content: Monitor + CH0..CH15 ----
    Components tabComponents = { makeMonitorTab(*board.appState, board.inputs) };
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        tabComponents.push_back(makeChannelTab(*board.appState, board.inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &board.activeTab);

    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto statusBar    = Container::Horizontal({bSysCfg});
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});

    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove,
                                         tabBar, tabContent, bSysCfg, globalQuit, globalSetup, globalPreferences,
                                         liveBoardCount] {
        if (board.pendingSync.exchange(false, std::memory_order_acq_rel)) {
            if (board.connected.load() && board.data.valid) {
                int nc = board.pendingChannelCount.load(std::memory_order_acquire);
                rebuildChannelTitles(board.tabTitles, nc);
                int maxTab = static_cast<int>(board.tabTitles.size()) - 1;
                if (board.activeTab > maxTab) board.activeTab = maxTab;
                syncDataToInputs(board.data, board.inputs);
            }
        }
        reconcileDisconnectedTabs(
            board.connected.load() && board.data.valid, board.tabTitles, board.activeTab);

        std::string msg;
        { std::lock_guard<std::mutex> lk(board.statusMutex); msg = board.statusMsg; }

        // --- Channel count + system telemetry ---
        std::string chTxt = "--";
        if (board.data.valid) chTxt = std::to_string(board.data.numChannels());

        std::string fwTxt = "--", protoTxt = "--";
        std::string variantTxt = "PSB";
        std::string uptimeTxt = "--s";
        bool hasEnvSensor = false;
        char tmpS[16], humS[16];
        tmpS[0] = humS[0] = 0;
        if (board.data.valid) {
            const auto& si = board.data.sysInfo;
            fwTxt    = reg::formatFwVersion(si.fwVersion);
            protoTxt = std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor);
            variantTxt = catalog::variantName(si.variantId);
            uptimeTxt = std::to_string(si.uptimeSec) + "s";
            hasEnvSensor = (si.sysCapFlags & SysCap::ENV_SENSOR) != 0;
            if (hasEnvSensor) {
                snprintf(tmpS, sizeof(tmpS), "%.1fC",  si.boardTempRaw    * 0.1);
                snprintf(humS, sizeof(humS), "%.1f%%", si.boardHumidityRaw * 0.1);
            }
        }

        // --- Breathing green: cosine wave 0→1→0 over 2 s ----
        auto breathColor = []() -> Color {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count() % 2000;
            double b = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(ms) / 2000.0);
            return Color::RGB(0, 50 + static_cast<int>(b * 200), 0);
        };

        static constexpr auto kSysStaleThreshold = std::chrono::seconds(10);
        bool sysStale = board.connected.load() && board.data.sysStale(kSysStaleThreshold);
        Element connDotEl;
        if (sysStale) {
            connDotEl = text(" \xe2\x97\x8f ") | color(Color::Red) | bold;
        } else if (board.connected.load()) {
            connDotEl = text(" \xe2\x97\x8f ") | color(breathColor()) | bold;
        } else if (board.connecting.load()) {
            connDotEl = text(" \xe2\x8f\xb3 ") | color(Color::Yellow) | bold;
        } else {
            connDotEl = text(" \xe2\x97\x8b ") | color(Color::GrayDark);
        }

        // --- Menu bar ---
        bool isOnline = board.connected.load();
        Element centerGroup;
        if (isOnline) {
            std::string telemetry = " " + uptimeTxt;
            if (hasEnvSensor)
                telemetry += "  |  T: " + std::string(tmpS) + "  H: " + std::string(humS);
            telemetry += " ";
            centerGroup = hbox({ connDotEl, text(telemetry) });
        } else {
            centerGroup = text("");
        }
        Element modeElement = isOnline || board.connecting.load()
            ? menuModeC->Render()
            : text("[ " + kOpModes[board.inputs.opModeIdx] + " ]") | dim;
        Element saveElement = isOnline
            ? connectedMenuSave->Render()
            : text("[ Save ]") | dim;
        // Remove only makes sense with a sibling to remove down to — see
        // Global Constraints. In single-board mode, this row also folds in
        // globalQuit/globalSetup's own rendered output (visual-only merge
        // — see Task 3's comment in board_switcher.h): with 2+ boards,
        // those same Components render as their own separate row one level
        // up instead, so they're omitted here to avoid rendering twice.
        size_t boardCount = liveBoardCount();
        Element removeElement = boardCount > 1 ? bRemove->Render() : text("");
        Elements menuBarParts = {
            text(" " + variantTxt + " ") | bold,
            separator(),
            text(" " + chTxt + " Channels "),
            separator(),
            modeElement,
            text(" "),
            saveElement,
            filler(),
            centerGroup,
            filler(),
            bConnToggle->Render(),
            text(" "),
            removeElement,
        };
        if (boardCount <= 1) {
            // Quit last — the same right-corner placement board_switcher.h's
            // multi-board row uses, keeping this one destructive action set
            // apart from Setup/Preferences even without a dedicated filler
            // here (the row's existing pair of fillers around centerGroup
            // already pushes this whole tail block flush to the right edge;
            // adding a third filler would unbalance that split).
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalSetup->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalPreferences->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalQuit->Render());
        }
        auto menuBarEl = hbox(std::move(menuBarParts));

        // --- Status bar (static colour — no breathing) ---
        auto connColor = board.connected.load() ? Color::Green
                       : board.connecting.load()  ? Color::Yellow
                       : Color::GrayDark;
        Element connTextEl;
        if (board.connected.load()) {
            connTextEl = text(" " + board.portVal + " @" + board.baudVal + " #" + board.slaveVal + " ") | color(connColor);
        } else if (board.connecting.load()) {
            connTextEl = text(" Connecting... ") | color(Color::Yellow);
        } else {
            connTextEl = text(" offline ") | color(Color::GrayDark);
        }

        bool isErr = msg.find("Error") != std::string::npos;
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                  | size(WIDTH, GREATER_THAN, 30),
            filler(),
            text((isOnline ? " FW:" + fwTxt + "  Proto:" + protoTxt + "  " : " ")
                 + "TUI:" TOOL_VERSION_STRING " "),
            filler(),
            connTextEl,
            text(" "),
            bSysCfg->Render(),
        });

        return vbox({
            menuBarEl,
            separator(),
            hbox({ tabBar->Render() | flex }),
            separator(),
            tabContent->Render() | flex,
            separator(),
            statusBarEl,
        });
    }) | Modal(sysCfgPopup,  &board.showSysCfg)
       | Modal(connModalPopup, &board.showConnModal)
       | CatchEvent([&board, &screen](Event e) {
           if (board.showSysCfg    && e == Event::Escape) { board.showSysCfg    = false; screen.PostEvent(Event::Custom); return true; }
           if (board.showConnModal && e == Event::Escape) { board.showConnModal  = false; screen.PostEvent(Event::Custom); return true; }
           return false;
       });

    return root;
}

} // namespace psb::tui

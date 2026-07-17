#include "psb_modbus_client.h"
#include "config_manager.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static psb::PsbModbusClient g_client;
static psb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;

// Reads never surface failure to the caller (readChannelInfo/Config/CalConfig
// return a plain struct, defaulted/partial on a transient read error, with
// isConnected() unaffected). doFullScan runs exactly once at connect time,
// right where this codebase has repeatedly observed USB-serial (e.g. CH340)
// reopen flakiness bite hardest — so retry each read once, using lastError()
// changing as the per-call failure signal (it's sticky/never cleared on
// success, so comparing before/after isolates whether *this* call set a new
// one). Without this, a single glitched read permanently corrupts that one
// channel's displayed data until the user manually interacts with it
// (triggering refreshCh) or reconnects.
template <typename Fn>
static auto readWithRetry(Fn&& fn) -> decltype(fn()) {
    auto before = g_client.lastError();
    auto result = fn();
    if (g_client.lastError() != before) {
        result = fn();
    }
    return result;
}

// Scans only what the Monitor table actually displays: ChannelInfo (status/
// V/I/Vop/faults) plus the output, protection, and output-enabled
// ChannelConfig blocks (Vset, ramp, I-limit, iProtMode for the Fault
// column). Recovery-policy and derate-step — shown only on the Channel tab —
// are deliberately left out here and fetched lazily the first time that
// channel's tab is opened (see tab_channel.h), since scanning them for every
// channel up front was pure overhead for what Monitor needs to render.
//
// Stages all channel results locally and publishes (chInfo/chCfg/chLoaded)
// in one shot at the very end, so Monitor shows a single "Scanning
// channels... X/N" message throughout (via scanProgress) and then reveals
// the whole table at once — not a row-by-row trickle, which read as a
// torn/inconsistent table rather than an obviously-still-loading one.
//
// Interleaves a system-status read after every channel so the menu bar's
// uptime/temp/humidity keep ticking with real (not extrapolated) data
// throughout the scan instead of freezing for its whole duration — the
// serial bus is still shared with the channel reads (can't run truly
// concurrently), but this keeps the readout at most one channel's-worth
// stale rather than stuck for the whole scan.
static void doFullScan(psb::tui::ScannedData& data, ScreenInteractive& screen,
                       std::atomic<bool>& running) {
    for (int ch = 0; ch < psb::tui::MAX_CHANNELS; ++ch) {
        data.chLoaded[ch] = false;
        data.chDetailLoaded[ch] = false;
    }

    data.sysInfo = readWithRetry([&] { return g_client.readSystemInfo(); });
    data.lastSysUpdate = std::chrono::steady_clock::now();
    data.sysCfg  = readWithRetry([&] { return g_client.readSystemConfig(); });
    int n = data.numChannels();
    // Gate on g_connected, not an unconditional true — if the user hits
    // Disconnect while this (queued) scan is running, g_connected already
    // flipped false on the UI thread, and this must not resurrect `valid`
    // out from under it.
    data.valid = g_connected.load();
    data.scanProgress = 0;
    if (running) screen.PostEvent(Event::Custom);

    psb::ChannelInfo   chInfoStaging[psb::tui::MAX_CHANNELS];
    psb::ChannelConfig chCfgStaging[psb::tui::MAX_CHANNELS];

    for (int ch = 0; ch < n; ++ch) {
        chInfoStaging[ch] = readWithRetry([&] { return g_client.readChannelInfo(ch); });
        // Zero capability flags means the connect-time probe itself failed
        // both of readWithRetry's attempts (readChannelInfo's capability
        // fetch is one all-or-nothing transaction — no real channel
        // legitimately reports zero capability bits). Worth fighting harder
        // for here: getting this wrong silently renders the whole row "n/a"
        // for the rest of the session (doPollScan's self-heal below covers
        // the case this still misses).
        for (int attempt = 0; attempt < 2 && chInfoStaging[ch].chCapFlags == 0; ++attempt)
            chInfoStaging[ch] = g_client.readChannelInfo(ch);
        uint16_t caps = chInfoStaging[ch].chCapFlags;
        g_client.readChannelOutputBlock(ch, caps, chCfgStaging[ch]);
        g_client.readChannelProtectionBlock(ch, caps, chCfgStaging[ch]);
        g_client.readChannelOutputEnabledBlock(ch, caps, chCfgStaging[ch]);

        data.scanProgress = ch + 1;
        g_client.readSystemStatus(data.sysInfo);
        if (running) screen.PostEvent(Event::Custom);
    }

    for (int ch = 0; ch < n; ++ch) {
        data.chInfo[ch]   = chInfoStaging[ch];
        data.chCfg[ch]    = chCfgStaging[ch];
        data.chLoaded[ch] = true;
    }
    if (running) screen.PostEvent(Event::Custom);
}

// A channel that fails this many consecutive status polls in a row is
// flagged offline (see doPollScan below) — a real, user-visible fault
// (unresponsive channel, not just one glitched transaction), not a
// transient blip; readWithRetry-style single-retry is deliberately not
// enough tolerance for this class of decision.
static constexpr int kChannelOfflineThreshold = 5;

// Response timeout used for routine polling reads only (system/channel
// status, capability self-heal) — deliberately much shorter than the port's
// normal connect-time timeout (default 3000ms). Confirmed live against real
// hardware that this board/cable link has genuine, occasional transient
// Modbus failures (independently reproduced with mbpoll alone, no TUI
// involved) — with the default timeout, one such failure mid-sweep froze
// the whole poll cycle (and the uptime counter with it) for up to 3 full
// seconds. A routine poll that fails is expected to just succeed next
// cycle, so it should fail fast here rather than block the UI.
static constexpr int kPollTimeoutMs = 300;

// Publishes system status and channel status on two independent cadences,
// each a single PostEvent:
//  - System status (uptime/temp/humidity) publishes the instant it's read,
//    so the menu bar ticks every poll cycle regardless of how long the
//    10-channel sweep below takes — coupling it to the sweep's completion
//    is what previously made uptime visibly update only once every ~3-5s
//    on a 10-channel board instead of every cycle.
//  - Channel status is swept into a local staging copy and published to
//    `data` in one shot once the sweep is done (or interrupted — see
//    below), rather than mutating `data` in place channel-by-channel. The
//    latter let the ~12 Hz breathing-LED animation thread's continuous
//    redraws catch the table mid-sweep, showing a torn mix of channels
//    already refreshed this cycle and channels still showing the previous
//    cycle's values; staging then publishing atomically means every repaint
//    sees one consistent snapshot of the whole table.
//
// Tracks consecutive read failures per channel in `data` directly (rare,
// edge-triggered changes — no torn-read concern like the per-cycle
// measurement data above) and flags a channel offline, with a one-time
// status message, after kChannelOfflineThreshold consecutive failures; any
// single success clears it.
//
// `hasPendingWork` lets the sweep bail out early when a write is queued —
// the worker loop drains that write right away instead of making it wait
// for the whole sweep; any channels not yet reached this tick simply keep
// their staged (pre-sweep) values and get re-polled next tick — no data
// loss, since this is continuous live polling, not a one-shot scan.
static void doPollScan(psb::tui::ScannedData& data, ScreenInteractive& screen,
                       std::atomic<bool>& running,
                       const std::function<bool()>& hasPendingWork,
                       std::string& statusMsg, std::mutex& statusMutex) {
    int n = data.numChannels();

    // Publish system status (uptime/temp/humidity) immediately, on its own —
    // it has no consistency relationship with the per-channel sweep below,
    // and gating it behind the whole sweep is what made the uptime counter
    // only tick once per full sweep (~3-5s on a 10-channel board) instead of
    // every poll cycle. readSystemStatus() merges in place, so this is safe
    // to publish straight to `data` without a staging copy.
    if (g_client.readSystemStatus(data.sysInfo, kPollTimeoutMs))
        data.lastSysUpdate = std::chrono::steady_clock::now();
    if (running) screen.PostEvent(Event::Custom);

    psb::ChannelInfo chStaging[psb::tui::MAX_CHANNELS];
    for (int ch = 0; ch < n; ++ch) chStaging[ch] = data.chInfo[ch];

    std::vector<int> newlyOffline;
    for (int ch = 0; ch < n; ++ch) {
        if (hasPendingWork()) break;
        if (!psb::tui::shouldPollChannel(ch, n)) continue;

        // Self-heal a channel whose capability flags never got captured
        // correctly (0 is not a real hardware configuration — every
        // channel reports at least one bit). doFullScan's connect-time
        // retries can still miss a persistent glitch; without this, such a
        // channel is otherwise stuck showing "n/a" everywhere for the rest
        // of the session, since readChannelStatus only ever reuses the caps
        // it's handed, never re-derives them. One cheap extra transaction
        // per affected channel per poll cycle, only while still unknown.
        if (chStaging[ch].chCapFlags == 0) {
            uint16_t caps = 0;
            if (g_client.readChannelCapabilities(ch, caps, kPollTimeoutMs) && caps != 0)
                chStaging[ch].chCapFlags = caps;
        }

        bool ok = g_client.readChannelStatus(ch, chStaging[ch].chCapFlags, chStaging[ch], kPollTimeoutMs);
        if (ok) {
            data.chPollFailCount[ch] = 0;
            data.chOffline[ch] = false;
        } else if (++data.chPollFailCount[ch] > kChannelOfflineThreshold && !data.chOffline[ch]) {
            data.chOffline[ch] = true;
            newlyOffline.push_back(ch);
        }
    }

    // Publish the channel sweep atomically — a render can never observe a
    // partially-refreshed set of channels.
    for (int ch = 0; ch < n; ++ch) data.chInfo[ch] = chStaging[ch];

    if (!newlyOffline.empty()) {
        std::lock_guard<std::mutex> lk(statusMutex);
        statusMsg = "Error: CH" + std::to_string(newlyOffline.front()) + " not responding — marked offline";
    }
    if (running) screen.PostEvent(Event::Custom);
}

static void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(1);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}

int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-p" && i+1 < argc) portArg        = argv[++i];
        else if (a == "-b" && i+1 < argc) baudArg        = std::stoi(argv[++i]);
        else if (a == "-i" && i+1 < argc) slaveArg       = std::stoi(argv[++i]);
        else if (a == "-t" && i+1 < argc) timeoutArg     = std::stoi(argv[++i]);
        else if (a == "-s" && i+1 < argc) g_pollInterval = std::stoi(argv[++i]);
    }

    std::string cfgPort    = portArg.empty() ? (g_cfg.port.empty() ? "/dev/ttyUSB0" : g_cfg.port) : portArg;
    std::string cfgBaud    = std::to_string(baudArg != 115200 ? baudArg : g_cfg.baudRate);
    std::string cfgSlaveId = std::to_string(slaveArg != 1     ? slaveArg : g_cfg.slaveId);

    auto screen = ScreenInteractive::Fullscreen();
    int  activeTab    = 0;
    bool showSysCfg   = false;
    bool showConnModal = false;
    std::atomic<bool> connecting{false};
    std::atomic<bool> abortConnect{false};
    std::chrono::steady_clock::time_point connectStart;
    std::string statusMsg;
    std::mutex  statusMutex;
    psb::tui::ScannedData data;
    std::atomic<bool> running{true};
    std::atomic<int>  pendingChannelCount{-1};
    std::atomic<bool> pendingSync{false};
    std::queue<std::function<void()>> workQueue;
    std::mutex                        workMutex;
    std::condition_variable           workCv;

    psb::tui::AppState appState{g_client, g_connected, data, statusMsg, statusMutex, workQueue, workMutex, workCv, screen};
    psb::tui::ConfigInputs inputs;

    // ---- Modbus worker thread — serialises all serial I/O ----
    auto hasPendingWork = [&] {
        std::lock_guard<std::mutex> lk(workMutex);
        return !workQueue.empty();
    };
    std::thread modbusWorker([&] {
        while (running) {
            {
                // While connected, only wait a short floor between poll
                // sweeps (bounded by the sweep's own real duration, not an
                // extra artificial delay) — while idle/disconnected, fall
                // back to g_pollInterval so this thread isn't spinning for
                // no reason.
                auto waitDur = g_connected.load() ? std::chrono::milliseconds(50)
                                                   : std::chrono::seconds(g_pollInterval);
                std::unique_lock<std::mutex> lk(workMutex);
                workCv.wait_for(lk, waitDur,
                    [&] { return !workQueue.empty() || !running; });
            }
            for (;;) {
                std::function<void()> item;
                { std::lock_guard<std::mutex> lk(workMutex);
                  if (workQueue.empty()) break;
                  item = std::move(workQueue.front()); workQueue.pop(); }
                item();
            }
            if (running && g_connected) {
                doPollScan(data, screen, running, hasPendingWork, statusMsg, statusMutex);
                // Re-check g_connected, not just isConnected() — if the user
                // clicked Disconnect while this sweep was in flight,
                // g_connected already flipped false on the UI thread, but
                // the queued disconnect() job hasn't drained yet, so
                // isConnected() alone would still read true here and
                // resurrect a cleared `valid` right back to stale content.
                data.valid = g_connected.load() && g_client.isConnected();
            }
        }
    });

    // ---- Animation thread — drives breathing LED at ~12 Hz when connected ----
    std::thread animThread([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (running && g_connected.load()) screen.PostEvent(Event::Custom);
        }
    });

    // ---- Connection inputs (live in the connection modal) ----
    std::string portVal = cfgPort, baudVal = cfgBaud, slaveVal = cfgSlaveId;
    auto baudInp  = Input(&baudVal,  "baud");
    auto slaveInp = Input(&slaveVal, "id");

    // Port list & selection
    auto portList = std::make_shared<std::vector<std::string>>();
    int portIdx = -1;

    auto doScanPorts = [&] {
        *portList = psb::PsbModbusClient::scanPorts();
        portIdx = psb::tui::selectedPortIndex(*portList, portVal);
        portVal = portIdx >= 0 ? (*portList)[portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };

    auto portDropdown = Dropdown(portList.get(), &portIdx);
    auto visiblePortDropdown = Maybe(portDropdown, [&] { return !portList->empty(); });
    auto bScan = Button("Rescan", [&] { doScanPorts(); });

    // ---- Tab titles — dynamic; rebuilt after connect ----
    std::vector<std::string> tabTitles = {"Monitor"};

    auto doConnect = [&] {
        if (portVal.empty() || connecting) return;
        abortConnect = false;
        connecting = true;
        connectStart = std::chrono::steady_clock::now();
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connecting to " + portVal + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(baudVal);  } catch (...) {}
            try { slave = std::stoi(slaveVal); } catch (...) {}
            bool ok = g_client.connect(portVal, baud, slave, timeoutArg);
            g_connected = ok && !abortConnect;
            if (abortConnect) { if (ok) g_client.disconnect(); ok = false; }
            if (!running) { if (ok) g_client.disconnect(); connecting = false; return; }
            if (ok) {
                { std::lock_guard<std::mutex> lk(workMutex);
                  workQueue.push([&] {
                      doFullScan(data, screen, running); data.valid = g_connected.load();
                      pendingChannelCount.store(data.numChannels(), std::memory_order_release);
                      pendingSync.store(true, std::memory_order_release);
                      screen.PostEvent(Event::Custom);
                  }); }
                workCv.notify_one();
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connectStart).count();
            { std::lock_guard<std::mutex> lk(statusMutex);
              if (abortConnect)
                  statusMsg = "Connection aborted";
              else if (ok)
                  statusMsg = "";
              else {
                  auto e = g_client.lastError();
                  statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                              + " (after " + std::to_string(static_cast<int>(elapsed / 1000.0)) + "s)";
              }
            }
            connecting = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    auto doDisconnect = [&] {
        abortConnect = true;
        g_connected = false; data.valid = false;
        // Enqueue disconnect on the worker thread to serialise with in-flight
        // Modbus I/O — avoids use-after-free on m_impl->port.
        { std::lock_guard<std::mutex> lk(workMutex);
          workQueue.push([&] { g_client.disconnect(); }); }
        workCv.notify_one();
        tabTitles = {"Monitor"}; activeTab = std::min(activeTab, 0);
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Disconnected"; }
        screen.PostEvent(Event::Custom);
    };

    // ---- Connect/Disconnect/Abort toggle button ----
    ButtonOption connBtnOpt{};
    connBtnOpt.transform = [&](const EntryState& es) -> Element {
        std::string lbl = g_connected.load() ? "Disconnect"
                        : connecting.load()  ? "Abort"
                        : "Connect";
        auto e = text("[ " + lbl + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    auto bConnToggle = Button("", [&] {
        if (g_connected.load())     doDisconnect();
        else if (connecting.load()) abortConnect = true;
        else {
            doScanPorts();
            showConnModal = true;
            screen.PostEvent(Event::Custom);
        }
    }, connBtnOpt);

    // ---- Connection modal ----
    auto bConnInModal = psb::tui::ActionButton("Connect", [&] {
        if (!portVal.empty() && !connecting) { doConnect(); showConnModal = false; }
    });
    auto bCancelConn = psb::tui::ActionButton("Cancel", [&] {
        showConnModal = false; screen.PostEvent(Event::Custom);
    });
    auto connModalForm   = Container::Vertical({visiblePortDropdown, bScan, baudInp, slaveInp, bConnInModal, bCancelConn});
    auto connModalPopup  = Renderer(connModalForm, [&] {
        if (portIdx >= 0 && portIdx < static_cast<int>(portList->size()))
            portVal = (*portList)[portIdx];
        Element portChoice = portList->empty()
            ? text("(no ports found)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Connection Settings ") | bold | center,
            separator(),
            hbox({ text("Port  : "), portChoice, text(" "), bScan->Render() }),
            hbox({ text("Baud  : "), baudInp->Render()  | size(WIDTH, EQUAL, 8)  }),
            hbox({ text("Slave : "), slaveInp->Render() | size(WIDTH, EQUAL, 5)  }),
            separator(),
            hbox({ bConnInModal->Render(), text("  "), bCancelConn->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 42);
    });

    auto bQuit = psb::tui::ActionButton("Quit", [&] {
        running = false; workCv.notify_all(); screen.ExitLoopClosure()();
    });

    // ---- SysConfig popup ----
    static const std::vector<std::string> kOpModes  = {"Normal", "Automatic"};
    static const std::vector<std::string> kStartPol = {"Load NVS Config", "Factory Default"};
    static const std::vector<std::string> kBaudNames = {"115200", "9600"};

    auto bSysCfg = psb::tui::ActionButton("Setting", [&] {
        if (!showSysCfg && data.valid) psb::tui::syncDataToInputs(data, inputs);
        showSysCfg = !showSysCfg; screen.PostEvent(Event::Custom);
    });

    // scOpMode shares inputs.opModeIdx with menuModeC; autoCommit=true writes on every click.
    auto scOpMode  = psb::tui::InlineCycler(kOpModes, &inputs.opModeIdx, [&] {
        psb::tui::postWrite(appState, inputs, "OpMode",
            [&] { return g_client.writeOperatingMode(static_cast<psb::OpMode>(inputs.opModeIdx)); },
            [&] { data.sysCfg = g_client.readSystemConfig(); });
    }, /*autoCommit=*/true);
    auto scStartup = psb::tui::InlineCycler(kStartPol, &inputs.startupIdx, [&] {
        psb::tui::postWrite(appState, inputs, "StartupPol",
            [&] { return g_client.writeStartupChannelPolicy((uint16_t)inputs.startupIdx); },
            [&] { data.sysCfg = g_client.readSystemConfig(); });
    });
    auto scSlave = Input(&inputs.slaveAddr, "1-247");
    auto scBaud = psb::tui::InlineCycler(kBaudNames, &inputs.baudIdx, [] {});

    auto scSaveModbus = psb::tui::ActionButton("Save Modbus", [&] {
        uint16_t slaveAddress = 0;
        if (!psb::tui::parseModbusSlaveAddress(inputs.slaveAddr, slaveAddress)) {
            {
                std::lock_guard<std::mutex> lk(statusMutex);
                statusMsg = "Error: slave address must be 1-247";
            }
            screen.PostEvent(Event::Custom);
            return;
        }
        if (inputs.baudIdx < 0 || inputs.baudIdx >= static_cast<int>(kBaudNames.size())) {
            {
                std::lock_guard<std::mutex> lk(statusMutex);
                statusMsg = "Error: invalid baud rate";
            }
            screen.PostEvent(Event::Custom);
            return;
        }

        const std::string stagedSlave = inputs.slaveAddr;
        const int stagedBaud = inputs.baudIdx;
        const psb::SystemConfig current = data.sysCfg;
        {
            std::lock_guard<std::mutex> lk(statusMutex);
            statusMsg = "Writing Modbus config...";
        }
        screen.PostEvent(Event::Custom);

        std::function<void()> item = [&, stagedSlave, stagedBaud, current] {
            auto result = psb::tui::saveModbusSettings(
                stagedSlave, stagedBaud, current,
                [&](uint16_t value) { return g_client.writeSlaveAddress(value); },
                [&](uint16_t value) { return g_client.writeBaudRateCode(value); });

            if (result == psb::tui::ModbusSettingsSaveResult::Success) {
                data.sysCfg = g_client.readSystemConfig();
                psb::tui::syncDataToInputs(data, inputs);
            }
            std::string resultMessage =
                psb::tui::modbusSettingsStatusMessage(result, g_client.lastError());
            {
                std::lock_guard<std::mutex> lk(statusMutex);
                statusMsg = std::move(resultMessage);
            }
            screen.PostEvent(Event::Custom);
        };

        {
            std::lock_guard<std::mutex> lk(workMutex);
            workQueue.push(std::move(item));
        }
        workCv.notify_one();
    });

    auto saveSystemConfig = [&] {
        psb::tui::postWrite(appState, inputs, "Save",
            [&] { return g_client.sendParamAction(-1, psb::ParamAction::Save); },
            [&] { data.sysCfg = g_client.readSystemConfig(); });
    };
    auto scSave    = Button("Save", saveSystemConfig);
    auto scLoad    = Button("Load",    [&] { psb::tui::postWrite(appState, inputs, "Load",
        [&] { return g_client.sendParamAction(-1, psb::ParamAction::Load);         }, [&] { data.sysCfg = g_client.readSystemConfig(); }); });
    auto scFactory = Button("Factory", [&] { psb::tui::postWrite(appState, inputs, "Factory",
        [&] { return g_client.sendParamAction(-1, psb::ParamAction::FactoryReset); }, [&] { data.sysCfg = g_client.readSystemConfig(); }); });
    // SoftwareReset: send reset and mark disconnected — device will reboot.
    auto scReset   = Button("Reset",   [&] {
        psb::tui::postWrite(appState, inputs, "SysReset",
            [&] { return g_client.sendParamAction(-1, psb::ParamAction::SoftwareReset); },
            [&] { g_connected = false; data.valid = false; g_client.disconnect(); });
        showSysCfg = false;
    });
    auto scClose   = Button("Close",   [&] { showSysCfg = false; screen.PostEvent(Event::Custom); });

    auto sysCfgForm = Container::Vertical({
        scOpMode, scStartup, scSave, scLoad, scFactory,
        scSlave, scBaud, scSaveModbus,
        scReset, scClose,
    });
    auto sysCfgPopup = Renderer(sysCfgForm, [&] {
        return vbox({
            text(" System Config ") | bold | center, separator(),
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
    auto menuModeC = psb::tui::InlineCycler(kOpModes, &inputs.opModeIdx, [&] {
        psb::tui::postWrite(appState, inputs, "OpMode",
            [&] { return g_client.writeOperatingMode(static_cast<psb::OpMode>(inputs.opModeIdx)); },
            [&] { data.sysCfg = g_client.readSystemConfig(); });
    }, /*autoCommit=*/true);

    auto menuSave = psb::tui::ActionButton("Save", saveSystemConfig);
    auto connectedMenuSave = Maybe(menuSave, [&] { return g_connected.load(); });
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bQuit});

    // ---- Tab bar ----
    MenuOption tabOpt = MenuOption::Horizontal();
    tabOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)  t = t | inverted;
        if (!e.active && !e.focused) t = t | dim;
        return t;
    };
    auto tabBar = Menu(&tabTitles, &activeTab, tabOpt);

    // ---- Tab content: Monitor + CH0..CH15 ----
    Components tabComponents = { psb::tui::makeMonitorTab(appState, inputs) };
    for (int ch = 0; ch < psb::tui::MAX_CHANNELS; ++ch)
        tabComponents.push_back(psb::tui::makeChannelTab(appState, inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &activeTab);

    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto statusBar    = Container::Horizontal({bSysCfg});
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});

    auto root = Renderer(mainContainer, [&] {
        if (pendingSync.exchange(false, std::memory_order_acq_rel)) {
            if (g_connected.load() && data.valid) {
                int nc = pendingChannelCount.load(std::memory_order_acquire);
                rebuildChannelTitles(tabTitles, nc);
                int maxTab = static_cast<int>(tabTitles.size()) - 1;
                if (activeTab > maxTab) activeTab = maxTab;
                psb::tui::syncDataToInputs(data, inputs);
            }
        }
        psb::tui::reconcileDisconnectedTabs(
            g_connected.load() && data.valid, tabTitles, activeTab);

        std::string msg;
        { std::lock_guard<std::mutex> lk(statusMutex); msg = statusMsg; }

        // --- Channel count + system telemetry ---
        std::string chTxt = "--";
        if (data.valid) chTxt = std::to_string(data.numChannels());

        std::string fwTxt = "--", protoTxt = "--";
        std::string uptimeTxt = "--s";
        // SYS_BOARD_TEMPERATURE/HUMIDITY have no backing register descriptor
        // at all on a board without CONFIG_SYS_STATUS (e.g. jw_lvb) — they
        // read back as the protocol's "reserved register" convention (0),
        // which looks exactly like a real (if suspiciously flat) reading
        // unless gated on the ENV_SENSOR capability bit.
        bool hasEnvSensor = false;
        char tmpS[16], humS[16];
        tmpS[0] = humS[0] = 0;
        if (data.valid) {
            const auto& si = data.sysInfo;
            char fw[16]; snprintf(fw, sizeof(fw), "0x%04X", si.fwVersion);
            fwTxt    = fw;
            protoTxt = std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor);
            uptimeTxt = std::to_string(si.uptimeSec) + "s";
            hasEnvSensor = (si.sysCapFlags & psb::SysCap::ENV_SENSOR) != 0;
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

        // --- Connection indicator (menu bar — breathing) ---
        // Stops breathing and turns solid red once the last successful
        // system-status poll is more than kSysStaleThreshold old — g_connected
        // alone can't detect a board that went unresponsive (e.g. powered
        // off) without the user ever clicking Disconnect, so without this
        // the dot kept breathing green forever even after channel rows had
        // already gone OFFLINE (see kChannelOfflineThreshold / chOffline).
        static constexpr auto kSysStaleThreshold = std::chrono::seconds(10);
        bool sysStale = g_connected.load() && data.sysStale(kSysStaleThreshold);
        Element connDotEl;
        if (sysStale) {
            connDotEl = text(" \xe2\x97\x8f ") | color(Color::Red) | bold;
        } else if (g_connected.load()) {
            connDotEl = text(" \xe2\x97\x8f ") | color(breathColor()) | bold;
        } else if (connecting.load()) {
            connDotEl = text(" \xe2\x8f\xb3 ") | color(Color::Yellow) | bold;
        } else {
            connDotEl = text(" \xe2\x97\x8b ") | color(Color::GrayDark);
        }

        // --- Menu bar ---
        bool isOnline = g_connected.load();
        Element centerGroup;
        if (isOnline) {
            std::string telemetry = " " + uptimeTxt;
            if (hasEnvSensor)
                telemetry += "  |  T: " + std::string(tmpS) + "  H: " + std::string(humS);
            telemetry += " ";
            centerGroup = hbox({
                connDotEl,
                text(telemetry),
            });
        } else {
            centerGroup = text("");
        }
        Element modeElement = isOnline || connecting.load()
            ? menuModeC->Render()
            : text("[ " + kOpModes[inputs.opModeIdx] + " ]") | dim;
        Element saveElement = isOnline
            ? connectedMenuSave->Render()
            : text("[ Save ]") | dim;
        auto menuBarEl = hbox({
            text(" PSB ") | bold,
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
            bQuit->Render(),
        });

        // --- Status bar (static colour — no breathing) ---
        auto connColor = g_connected.load() ? Color::Green
                       : connecting.load()  ? Color::Yellow
                       : Color::GrayDark;
        Element connTextEl;
        if (g_connected.load()) {
            connTextEl = text(" " + portVal + " @" + baudVal + " #" + slaveVal + " ") | color(connColor);
        } else if (connecting.load()) {
            connTextEl = text(" Connecting... ") | color(Color::Yellow);
        } else {
            connTextEl = text(" offline ") | color(Color::GrayDark);
        }

        // --- Status bar ---
        bool isErr = msg.find("Error") != std::string::npos;
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                  | size(WIDTH, GREATER_THAN, 30),
            filler(),
            isOnline ? text(" FW:" + fwTxt + "  Proto:" + protoTxt + " ") : text(""),
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
    }) | Modal(sysCfgPopup,  &showSysCfg)
       | Modal(connModalPopup, &showConnModal)
       | CatchEvent([&](Event e) {
           if (showSysCfg    && e == Event::Escape) { showSysCfg    = false; screen.PostEvent(Event::Custom); return true; }
           if (showConnModal && e == Event::Escape) { showConnModal  = false; screen.PostEvent(Event::Custom); return true; }
           return false;
       });

    if (!portArg.empty()) doConnect();

    screen.Loop(root);
    running = false;
    workCv.notify_all();
    if (modbusWorker.joinable()) modbusWorker.join();
    if (animThread.joinable())   animThread.join();
    g_client.disconnect();
    return 0;
}

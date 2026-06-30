#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "tab_monitor.h"
#include "tab_channel.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

using namespace ftxui;

static hvb::HvbModbusClient g_client;
static hvb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;

/* Full scan — used at connect time and on manual Refresh. */
static void doFullScan(hvb::tui::ScannedData& data) {
    data.sysInfo = g_client.readSystemInfo();
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) data.chInfo[ch]   = g_client.readChannelInfo(ch);
    data.sysCfg  = g_client.readSystemConfig();
    for (int ch = 0; ch < n; ++ch) data.chCfg[ch]    = g_client.readChannelConfig(ch, data.chInfo[ch].chCapFlags);
    for (int ch = 0; ch < n; ++ch) data.chCalCfg[ch] = g_client.readChannelCalConfig(ch, data.chInfo[ch].chCapFlags);
}

/* Poll scan — reads only dynamic registers. */
static void doPollScan(hvb::tui::ScannedData& data) {
    g_client.readSystemStatus(data.sysInfo);
    uint16_t activeMask = data.sysInfo.activeChMask;
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) {
        if ((activeMask & (1u << ch)) == 0) continue;
        g_client.readChannelStatus(ch, data.chInfo[ch].chCapFlags, data.chInfo[ch]);
    }
}

static void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(1);  // Monitor only — no System tab
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
    int  activeTab   = 0;
    bool showSysCfg  = false;
    std::atomic<bool> connecting{false};
    std::atomic<bool> abortConnect{false};
    std::chrono::steady_clock::time_point connectStart;
    std::string statusMsg;
    std::mutex  statusMutex;
    hvb::tui::ScannedData data;
    std::atomic<bool> running{true};
    std::atomic<int>  pendingChannelCount{-1};
    std::atomic<bool> pendingSync{false};
    std::queue<std::function<void()>> workQueue;
    std::mutex                        workMutex;
    std::condition_variable           workCv;

    hvb::tui::AppState appState{g_client, g_connected, data, statusMsg, statusMutex, workQueue, workMutex, workCv, screen};
    hvb::tui::ConfigInputs inputs;

    // ---- Modbus worker thread — serialises all serial I/O; never blocks the UI ----
    std::thread modbusWorker([&] {
        while (running) {
            {
                std::unique_lock<std::mutex> lk(workMutex);
                workCv.wait_for(lk, std::chrono::seconds(g_pollInterval),
                    [&] { return !workQueue.empty() || !running; });
            }
            // Drain all queued work items (writes, full scans) before polling
            for (;;) {
                std::function<void()> item;
                {
                    std::lock_guard<std::mutex> lk(workMutex);
                    if (workQueue.empty()) break;
                    item = std::move(workQueue.front());
                    workQueue.pop();
                }
                item();
            }
            // Periodic status poll
            if (running && g_connected) {
                doPollScan(data);
                data.valid = g_client.isConnected();
                if (running) screen.PostEvent(Event::Custom);
            }
        }
    });

    // ---- Connect interface (inline in menu bar) ----
    std::string portVal = cfgPort, baudVal = cfgBaud, slaveVal = cfgSlaveId;
    auto portInp  = Input(&portVal,  "port");
    auto baudInp  = Input(&baudVal,  "baud");
    auto slaveInp = Input(&slaveVal, "id");

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
            try { baud  = std::stoi(baudVal);    } catch (...) {}
            try { slave = std::stoi(slaveVal);   } catch (...) {}
            bool ok = g_client.connect(portVal, baud, slave, timeoutArg);
            g_connected = ok && !abortConnect;
            if (abortConnect) {
                if (ok) g_client.disconnect();
                ok = false;
            }
            if (!running) {  // Quit pressed while connecting — locals are being destroyed
                if (ok) g_client.disconnect();
                connecting = false;
                return;
            }
            if (ok) {
                // Post full scan + UI sync to worker — all Modbus I/O stays on one thread.
                {
                    std::lock_guard<std::mutex> lk(workMutex);
                    workQueue.push([&] {
                        doFullScan(data);
                        data.valid = true;
                        pendingChannelCount.store(data.numChannels(), std::memory_order_release);
                        pendingSync.store(true, std::memory_order_release);
                        screen.PostEvent(Event::Custom);
                    });
                }
                workCv.notify_one();
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connectStart).count();
            { std::lock_guard<std::mutex> lk(statusMutex);
              double sec = elapsed / 1000.0;
              if (abortConnect)
                  statusMsg = "Connection aborted";
              else if (ok)
                  statusMsg = "";  // success shown in menu bar indicator, not error box
              else {
                  auto e = g_client.lastError();
                  statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                              + " (after " + std::to_string(static_cast<int>(sec)) + "s)"; }
            }
            connecting = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    // ---- Menu bar buttons ----
    auto bConnect    = hvb::tui::ActionButton("Connect",    [&] { if (!g_connected && !connecting) doConnect(); });
    auto bDisconnect = hvb::tui::ActionButton("Disconnect", [&] {
        abortConnect = true;  // cancel any in-flight connect thread
        g_connected = false; data.valid = false;
        g_client.disconnect();
        tabTitles = {"Monitor"}; activeTab = std::min(activeTab, 0);
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Disconnected"; }
    });
    auto bQuit = hvb::tui::ActionButton("Quit", [&] {
        running = false; workCv.notify_all(); screen.ExitLoopClosure()();
    });

    // ---- SysConfig popup ----
    static const std::vector<std::string> kOpModes  = {"Normal", "Automatic"};
    static const std::vector<std::string> kStartPol = {"Load NVS Config", "Factory Default"};

    auto bSysCfg = hvb::tui::ActionButton("SysConfig", [&] { showSysCfg = !showSysCfg; screen.PostEvent(Event::Custom); });

    auto scOpMode  = hvb::tui::InlineCycler(kOpModes, &inputs.opModeIdx, [&]{
        hvb::tui::postWrite(appState, inputs, "OpMode",
            [&]{ return g_client.writeOperatingMode(static_cast<hvb::OpMode>(inputs.opModeIdx)); },
            [&]{ data.sysCfg = g_client.readSystemConfig(); });
    });
    auto scStartup = hvb::tui::InlineCycler(kStartPol, &inputs.startupIdx, [&]{
        hvb::tui::postWrite(appState, inputs, "StartupPol",
            [&]{ return g_client.writeStartupChannelPolicy((uint16_t)inputs.startupIdx); },
            [&]{ data.sysCfg = g_client.readSystemConfig(); });
    });
    auto scSave    = Button("Save",    [&]{ hvb::tui::postWrite(appState, inputs, "Save", [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::Save); }, [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
    auto scLoad    = Button("Load",    [&]{ hvb::tui::postWrite(appState, inputs, "Load", [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::Load); }, [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
    auto scFactory = Button("Factory", [&]{ hvb::tui::postWrite(appState, inputs, "Factory", [&]{ return g_client.sendParamAction(-1, hvb::ParamAction::FactoryReset); }, [&]{ data.sysCfg = g_client.readSystemConfig(); }); });
    auto scClose   = Button("Close", [&]{ showSysCfg = false; screen.PostEvent(Event::Custom); });

    auto sysCfgForm = Container::Vertical({scOpMode, scStartup, scSave, scLoad, scFactory, scClose});
    auto sysCfgPopup = Renderer(sysCfgForm, [&] {
        return vbox({
            text(" System Config ") | bold | center, separator(),
            hbox({ text("Working Mode  : "),  scOpMode->Render() }),
            hbox({ text("Startup Policy: "), scStartup->Render() }),
            separator(),
            hbox({ scSave->Render(), text("  "), scLoad->Render(), text("  "), scFactory->Render() }) | center,
            separator(),
            scClose->Render() | center,
        }) | border | size(WIDTH, EQUAL, 42);
    });

    auto menuBar = Container::Horizontal({
        portInp, baudInp, slaveInp, bConnect, bDisconnect, bQuit,
    });

    // ---- Tab bar ----
    MenuOption tabOpt = MenuOption::Horizontal();
    tabOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                 t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)   t = t | inverted;
        if (!e.active && !e.focused)  t = t | dim;
        return t;
    };
    auto tabBar = Menu(&tabTitles, &activeTab, tabOpt);

    // ---- Tab content: Monitor + CH0..CH15 ----
    Components tabComponents = {
        hvb::tui::makeMonitorTab(appState, inputs),
    };
    for (int ch = 0; ch < hvb::tui::MAX_CHANNELS; ++ch)
        tabComponents.push_back(hvb::tui::makeChannelTab(appState, inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &activeTab);

    // ---- Status bar ----
    auto statusBar = Container::Horizontal({bSysCfg});

    // ---- Full layout ----
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});

    auto root = Renderer(mainContainer, [&] {
        // Apply any pending post-connect state update on the UI thread to avoid races
        // on tabTitles, activeTab, and inputs (bound to FTXUI widgets by raw pointer).
        if (pendingSync.exchange(false, std::memory_order_acq_rel)) {
            int nc = pendingChannelCount.load(std::memory_order_acquire);
            rebuildChannelTitles(tabTitles, nc);
            int maxTab = static_cast<int>(tabTitles.size()) - 1;
            if (activeTab > maxTab) activeTab = maxTab;
            hvb::tui::syncDataToInputs(data, inputs);
        }

        std::string msg;
        { std::lock_guard<std::mutex> lk(statusMutex); msg = statusMsg; }

        // --- Status indicator ---
        Color indColor = Color::Yellow;
        std::string indTxt = "\xe2\x97\x8b Disconnected";
        if (g_connected.load()) {
            indTxt = "\xe2\x97\x8f " + portVal;
            indColor = Color::Green;
        } else if (connecting.load()) {
            indTxt = "\xe2\x8f\xb3 Connecting...";
            indColor = Color::Yellow;
        }

        // --- Sys mode & channel count ---
        std::string modeTxt = "--", chTxt = "--";
        if (data.valid) {
            modeTxt = opModeName(data.sysInfo.activeOpMode);
            chTxt = std::to_string(data.numChannels());
        }

        // --- Menu bar render ---
        auto menuBarEl = hbox({
            text(" HVB ") | bold,
            text(" " + indTxt + " ") | color(indColor),
            separator(),
            text(" Mode: " + modeTxt + " "),
            text(" Ch: " + chTxt + " "),
            separator(),
            text(" P:"),
            portInp->Render() | size(WIDTH, EQUAL, 14),
            text(" @"),
            baudInp->Render() | size(WIDTH, EQUAL, 6),
            text(" #"),
            slaveInp->Render() | size(WIDTH, EQUAL, 3),
            bConnect->Render(), text(" "),
            bDisconnect->Render(), text(" "),
            bQuit->Render(),
        });

        // --- Status bar render ---
        std::string fwTxt = "--", protoTxt = "--";
        char tmpS[16], humS[16];
        std::string uptimeTxt = "-- s";
        tmpS[0] = humS[0] = 0;
        if (data.valid) {
            const auto& si = data.sysInfo;
            char fw[16]; snprintf(fw, sizeof(fw), "0x%04X", si.fwVersion);
            fwTxt = fw;
            protoTxt = std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor);
            uptimeTxt = std::to_string(si.uptimeSec) + "s";
            snprintf(tmpS, sizeof(tmpS), "%.1fC", si.boardTempRaw * 0.1);
            snprintf(humS, sizeof(humS), "%.1f%%", si.boardHumidityRaw * 0.1);
        }
        bool isErr = msg.find("Error") != std::string::npos;
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green)),
            separator(),
            text(" FW:" + fwTxt + "  Proto:" + protoTxt + " "),
            separator(),
            text(" Up:" + uptimeTxt + "  " + std::string(tmpS) + "  " + std::string(humS) + " "),
            filler(),
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
    }) | Modal(sysCfgPopup, &showSysCfg)
       | CatchEvent([&](Event e) {
        if (showSysCfg && e == Event::Escape) { showSysCfg = false; screen.PostEvent(Event::Custom); return true; }
        return false;
    });

    // Auto-connect if port given on command line — reuse doConnect so all guards apply.
    if (!portArg.empty()) doConnect();

    screen.Loop(root);
    running = false;
    workCv.notify_all();
    if (modbusWorker.joinable()) modbusWorker.join();
    g_client.disconnect();
    return 0;
}

#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "tab_monitor.h"
#include "tab_system.h"
#include "tab_channel.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace ftxui;

static hvb::HvbModbusClient g_client;
static hvb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;
static std::mutex           g_scanMutex;

/* Full scan — used at connect time and on manual Refresh.
   Reads capabilities, config, and cal coefficients (slow path). */
static void doFullScan(hvb::tui::ScannedData& data) {
    data.sysInfo = g_client.readSystemInfo();
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) data.chInfo[ch]   = g_client.readChannelInfo(ch);
    data.sysCfg  = g_client.readSystemConfig();
    for (int ch = 0; ch < n; ++ch) data.chCfg[ch]    = g_client.readChannelConfig(ch, data.chInfo[ch].chCapFlags);
    for (int ch = 0; ch < n; ++ch) data.chCalCfg[ch] = g_client.readChannelCalConfig(ch, data.chInfo[ch].chCapFlags);
}

/* Poll scan — runs every poll interval.
   Reads only dynamic registers (runtime_state + measurements).
   Static fields (protocol version, variant, caps, supported channels, fw version)
   are cached from the last full scan and never re-read.
   Only channels in the active mask are polled. */
static void doPollScan(hvb::tui::ScannedData& data) {
    /* Merge dynamic system fields into the cached sysInfo */
    g_client.readSystemStatus(data.sysInfo);

    uint16_t activeMask = data.sysInfo.activeChMask;
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) {
        if ((activeMask & (1u << ch)) == 0) continue;
        g_client.readChannelStatus(ch, data.chInfo[ch].chCapFlags, data.chInfo[ch]);
    }
}

static void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(2);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}

int main(int argc, char** argv) {
    g_cfg.load();

    // refactor: use cli11    
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

    std::string modalPort    = portArg.empty() ? (g_cfg.port.empty() ? "/dev/ttyUSB0" : g_cfg.port) : portArg;
    std::string modalBaud    = std::to_string(baudArg != 115200 ? baudArg : g_cfg.baudRate);
    std::string modalSlaveId = std::to_string(slaveArg != 1     ? slaveArg : g_cfg.slaveId);

    auto screen = ScreenInteractive::Fullscreen();
    int  activeTab   = 0;
    bool showModal   = false;
    std::atomic<bool> connecting{false};
    std::atomic<bool> abortConnect{false};
    std::chrono::steady_clock::time_point connectStart;
    std::string statusMsg;
    std::mutex  statusMutex;
    hvb::tui::ScannedData data;
    std::atomic<bool> running{true};

    // todo: dedicated state machine library (fsm)?
    hvb::tui::AppState appState{g_client, g_connected, data, statusMsg, statusMutex, g_scanMutex, screen};
    hvb::tui::ConfigInputs inputs;

    // ---- Poll thread ----
    std::thread pollThread([&] {
        while (running) {
            if (g_connected) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doPollScan(data); }
                data.valid = g_client.isConnected();
                if (running) screen.PostEvent(Event::Custom);
            }
            for (int i = 0; i < g_pollInterval * 10 && running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ---- Connection modal (triggered by Connect button) ----
    auto portInput  = Input(&modalPort,    "e.g. /dev/ttyUSB0");
    auto baudInput  = Input(&modalBaud,    "115200");
    auto slaveInput = Input(&modalSlaveId, "1");

    // ---- Tab titles — dynamic; rebuilt after connect ----
    std::vector<std::string> tabTitles = {"Monitor", "System"};

    auto doConnect = [&] {
        if (modalPort.empty() || connecting) return;
        abortConnect = false;
        connecting = true;
        connectStart = std::chrono::steady_clock::now();
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connecting to " + modalPort + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(modalBaud);    } catch (...) {}
            try { slave = std::stoi(modalSlaveId); } catch (...) {}
            bool ok = g_client.connect(modalPort, baud, slave, timeoutArg);
            g_connected = ok && !abortConnect;
            if (abortConnect) {
                if (ok) g_client.disconnect();
                ok = false;
            }
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                syncDataToInputs(data, inputs);
                rebuildChannelTitles(tabTitles, data.numChannels());
                int maxTab = static_cast<int>(tabTitles.size()) - 1;
                if (activeTab > maxTab) activeTab = maxTab;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connectStart).count();
            { std::lock_guard<std::mutex> lk(statusMutex);
              double sec = elapsed / 1000.0;
              if (abortConnect)
                  statusMsg = "Connection aborted";
              else if (ok)
                  statusMsg = "Connected " + modalPort + "  (" + std::to_string(data.numChannels()) + " ch)";
              else {
                  auto e = g_client.lastError();
                  statusMsg = "Error: " + (e.empty() ? "connection failed" : e)
                              + " (after " + std::to_string(static_cast<int>(sec)) + "s)"; }
            }
            connecting = false;
            showModal  = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    auto modalButtons = Container::Horizontal({
        Button("Connect", doConnect),
        Button("Cancel",  [&] { showModal = false; }),
    });
    auto modalForm = Container::Vertical({portInput, baudInput, slaveInput, modalButtons});
    auto bAbort = Button("Abort", [&] {
        abortConnect = true;
        showModal = false;
        connecting = false;
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connection aborted"; }
    });
    auto abortContainer = Container::Vertical({bAbort});
    auto modalRenderer = Renderer(Container::Vertical({modalForm, abortContainer}), [&] {
        if (connecting) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connectStart).count();
            char etime[32];
            snprintf(etime, sizeof(etime), "%.1f s", elapsed / 1000.0);
            return vbox({
                text(" Connecting to HVB ") | bold | center,
                separator(),
                text(" " + modalPort + "  @" + modalBaud + "  #" + modalSlaveId + " ") | dim,
                text(""),
                text(" Elapsed: " + std::string(etime)) | center,
                text(""),
                text(" If this takes too long, check:") | dim,
                text("   - Board is powered on") | dim,
                text("   - USB cable is connected") | dim,
                text("   - Correct port is selected") | dim,
                text(""),
                separator(),
                bAbort->Render() | center,
            }) | border | size(WIDTH, EQUAL, 50);
        }
        return vbox({
            text(" Connect to HVB ") | bold | center, separator(),
            hbox({ text("Port    : "), portInput->Render()  }),
            hbox({ text("Baud    : "), baudInput->Render()  }),
            hbox({ text("Slave ID: "), slaveInput->Render() }),
            separator(),
            modalButtons->Render() | center,
        }) | border | size(WIDTH, EQUAL, 50);
    });

    // ---- Toolbar buttons ----
    auto bConnect = hvb::tui::ActionButton("Connect", [&] {
        if (!g_connected && !connecting) showModal = true;
    });
    auto bDisconnect = hvb::tui::ActionButton("Disconnect", [&] {
        g_connected = false;
        data.valid  = false;
        tabTitles   = {"Monitor", "System"};
        activeTab   = std::min(activeTab, 1);
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Disconnected"; }
    });
    auto bRefresh = hvb::tui::ActionButton("Refresh", [&] {
        if (g_connected) {
            { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
            syncDataToInputs(data, inputs);
            data.valid = true;
        }
    });
    auto bQuit = hvb::tui::ActionButton("Quit", [&] {
        running = false;
        screen.ExitLoopClosure()();
    });
    auto toolbarRow = Container::Horizontal({bConnect, bDisconnect, bRefresh, bQuit});

    // ---- Tab bar (mouse-clickable, keyboard-navigable) ----
    MenuOption tabOpt = MenuOption::Horizontal();
    tabOpt.entries_option.transform = [](const EntryState& e) -> Element {
        auto t = text("  " + e.label + "  ");
        if (e.active)                 t = t | bold | color(Color::Cyan);
        if (e.focused && !e.active)   t = t | inverted;
        if (!e.active && !e.focused)  t = t | dim;
        return t;
    };
    auto tabBar = Menu(&tabTitles, &activeTab, tabOpt);

    // Tab content — built upfront with all 18 slots (Monitor + System + 16 channels).
    // Per-channel tabs render a placeholder for out-of-range channels or when disconnected.
    // The tab bar (tabTitles) controls visibility by resizing the title list.
    Components tabComponents = {
        hvb::tui::makeMonitorTab(appState, inputs),
        hvb::tui::makeSystemTab(appState, inputs),
    };
    for (int ch = 0; ch < hvb::tui::MAX_CHANNELS; ++ch)
        tabComponents.push_back(hvb::tui::makeChannelTab(appState, inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &activeTab);

    // ---- Full layout: toolbar + tab bar + content ----
    auto mainContainer = Container::Vertical({toolbarRow, tabBar, tabContent});

    auto root = Renderer(mainContainer, [&] {
        std::string msg;
        { std::lock_guard<std::mutex> lk(statusMutex); msg = statusMsg; }

        std::string connTxt = connecting.load()
            ? " \xe2\x8f\xb3 Connecting..."
            : (g_connected ? (" \xe2\x97\x8f  " + modalPort) : " \xe2\x97\x8b  Disconnected");
        bool connOk = g_connected.load();

        bool isErr = msg.find("Error") != std::string::npos;
        auto statusEl = text(" " + msg)
            | (isErr ? color(Color::Red) : (connOk ? color(Color::Green) : color(Color::Yellow)));

        return vbox({
            // Title + connection indicator + toolbar buttons
            hbox({
                text(" HVB ") | bold,
                separator(),
                text(connTxt) | (connOk ? color(Color::Green) : color(Color::Yellow)),
                filler(),
                toolbarRow->Render(),
                text(" "),
            }),
            separator(),
            // Tab bar
            hbox({ tabBar->Render() }),
            separator(),
            // Active tab content
            tabContent->Render() | flex,
            separator(),
            // Status bar
            hbox({ statusEl, filler() }),
        });
    }) | Modal(modalRenderer, &showModal)
       | CatchEvent([&](Event e) {
           if (e == Event::Escape) {
               if (connecting) { abortConnect = true; connecting = false; showModal = false;
                   { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connection aborted"; }
                   screen.PostEvent(Event::Custom); return true; }
               if (showModal) { showModal = false; return true; }
           }
           return false;
       });

    // Auto-connect if port given on command line
    if (!portArg.empty()) {
        std::thread([&] {
            bool ok = g_client.connect(portArg, baudArg, slaveArg, timeoutArg);
            g_connected = ok;
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                syncDataToInputs(data, inputs);
                rebuildChannelTitles(tabTitles, data.numChannels());
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok ? "" : "Error: " + g_client.lastError(); }
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    screen.Loop(root);
    running = false;
    if (pollThread.joinable()) pollThread.join();
    g_client.disconnect();
    return 0;
}

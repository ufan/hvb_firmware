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
static int g_pollInterval = 2;
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
   Reads only dynamic data (system status + per-channel measurements).
   Capabilities are hardware-fixed and already cached in chInfo[ch].chCapFlags
   from the last full scan; they are passed in to skip the FC04 offset-9 read. */
static void doPollScan(hvb::tui::ScannedData& data) {
    data.sysInfo = g_client.readSystemInfo();
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch)
        data.chInfo[ch] = g_client.readChannelInfo(ch, data.chInfo[ch].chCapFlags);
}

static void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(2);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}

int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 500;
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
    std::string statusMsg;
    std::mutex  statusMutex;
    hvb::tui::ScannedData data;
    std::atomic<bool> running{true};

    hvb::tui::AppState appState{g_client, g_connected, data, statusMsg, statusMutex, g_scanMutex, screen};

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
        connecting = true;
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connecting to " + modalPort + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(modalBaud);    } catch (...) {}
            try { slave = std::stoi(modalSlaveId); } catch (...) {}
            bool ok = g_client.connect(modalPort, baud, slave, timeoutArg);
            g_connected = ok;
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                rebuildChannelTitles(tabTitles, data.numChannels());
                int maxTab = static_cast<int>(tabTitles.size()) - 1;
                if (activeTab > maxTab) activeTab = maxTab;
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok
                ? ("Connected " + modalPort + "  (" + std::to_string(data.numChannels()) + " ch)")
                : ("Error: " + g_client.lastError()); }
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
    auto modalRenderer = Renderer(modalForm, [&] {
        return vbox({
            text(" Connect to HVB ") | bold | center, separator(),
            hbox({ text("Port    : "), portInput->Render()  }),
            hbox({ text("Baud    : "), baudInput->Render()  }),
            hbox({ text("Slave ID: "), slaveInput->Render() }),
            separator(),
            modalButtons->Render() | center,
            text(connecting ? "Connecting..." : "") | dim | center,
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
            rebuildChannelTitles(tabTitles, data.numChannels());
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

    // ---- Tab content (built upfront, phantom channels show placeholder) ----
    Components tabComponents = {
        hvb::tui::makeMonitorTab(appState),
        hvb::tui::makeSystemTab(appState),
    };
    for (int ch = 0; ch < hvb::tui::MAX_CHANNELS; ++ch)
        tabComponents.push_back(hvb::tui::makeChannelTab(appState, ch));
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
           // Only intercept Escape to close the modal; everything else passes through.
           if (showModal && e == Event::Escape) { showModal = false; return true; }
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

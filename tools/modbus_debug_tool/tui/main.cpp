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

int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 500;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-p" && i+1 < argc) portArg    = argv[++i];
        else if (a == "-b" && i+1 < argc) baudArg    = std::stoi(argv[++i]);
        else if (a == "-i" && i+1 < argc) slaveArg   = std::stoi(argv[++i]);
        else if (a == "-t" && i+1 < argc) timeoutArg = std::stoi(argv[++i]);
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

    // AppState bundles everything the tab factories need
    hvb::tui::AppState appState{g_client, g_connected, data, statusMsg, statusMutex, screen};

    // ---- Poll thread ----
    std::thread pollThread([&] {
        while (running) {
            if (g_connected) {
                data.sysInfo = g_client.readSystemInfo();
                for (int ch = 0; ch < 2; ++ch) data.chInfo[ch] = g_client.readChannelInfo(ch);
                data.sysCfg  = g_client.readSystemConfig();
                for (int ch = 0; ch < 2; ++ch) data.chCfg[ch]  = g_client.readChannelConfig(ch);
                data.valid = g_client.isConnected();
                if (running) screen.PostEvent(Event::Custom);
            }
            for (int i = 0; i < g_pollInterval * 10 && running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ---- Connection modal ----
    auto portInput  = Input(&modalPort,    "e.g. /dev/ttyUSB0");
    auto baudInput  = Input(&modalBaud,    "115200");
    auto slaveInput = Input(&modalSlaveId, "1");

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
                data.sysInfo = g_client.readSystemInfo();
                for (int ch = 0; ch < 2; ++ch) data.chInfo[ch] = g_client.readChannelInfo(ch);
                data.sysCfg  = g_client.readSystemConfig();
                for (int ch = 0; ch < 2; ++ch) data.chCfg[ch]  = g_client.readChannelConfig(ch);
                data.valid = true;
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok ? "Connected " + modalPort : "Connect failed: " + g_client.lastError(); }
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

    // ---- Tab content (tab factories) ----
    std::vector<std::string> tabTitles = {"Mon", "Sys", "CH0", "CH1"};
    auto tabSelector = Toggle(&tabTitles, &activeTab);

    auto tabContent = Container::Tab({
        hvb::tui::makeMonitorTab(appState),
        hvb::tui::makeSystemTab(appState),
        hvb::tui::makeChannelTab(appState, 0),
        hvb::tui::makeChannelTab(appState, 1),
    }, &activeTab);

    // topBar: visual only — tabSelector rendered here but NOT in focus chain
    auto topBar = Renderer([&] {
        std::string msg;
        { std::lock_guard<std::mutex> lk(statusMutex); msg = statusMsg; }
        std::string connStr = g_connected
            ? ("\xe2\x97\x8f Connected " + modalPort)   // ● Connected
            : (msg.empty() ? "\xe2\x97\x8b Disconnected" : msg);
        return hbox({
            text(" HVB TUI ") | bold,
            separator(),
            text(" " + connStr + " ") | (g_connected ? color(Color::Green) : color(Color::Red)),
            separator(),
            tabSelector->Render(),
            filler(),
            text(" q:quit  r:poll  c:connect  d:disconnect  1-4:tabs ") | dim,
        });
    });

    // mainContainer: topBar (visual) + tabContent (interactive only)
    // tabSelector is NOT added here so ↑/↓ always reach the active tab
    auto mainContainer = Container::Vertical({topBar, tabContent});

    auto root = mainContainer
        | Modal(modalRenderer, &showModal)
        | CatchEvent([&](Event e) {
            if (showModal) {
                if (e == Event::Escape) { showModal = false; return true; }
                return false;
            }
            if (e == Event::Character('q')) { running = false; screen.ExitLoopClosure()(); return true; }
            if (e == Event::Character('r')) {
                if (g_connected) {
                    data.sysInfo = g_client.readSystemInfo();
                    for (int i = 0; i < 2; ++i) data.chInfo[i] = g_client.readChannelInfo(i);
                    data.sysCfg  = g_client.readSystemConfig();
                    for (int i = 0; i < 2; ++i) data.chCfg[i]  = g_client.readChannelConfig(i);
                    data.valid = true;
                }
                return true;
            }
            if (e == Event::Character('c')) { if (!g_connected && !connecting) showModal = true; return true; }
            if (e == Event::Character('d')) {
                g_client.disconnect(); g_connected = false; data.valid = false;
                { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Disconnected"; }
                return true;
            }
            if (e == Event::Character('1')) { activeTab = 0; return true; }
            if (e == Event::Character('2')) { activeTab = 1; return true; }
            if (e == Event::Character('3')) { activeTab = 2; return true; }
            if (e == Event::Character('4')) { activeTab = 3; return true; }
            return false;
        });

    // Auto-connect if port given on command line
    if (!portArg.empty()) {
        std::thread([&] {
            bool ok = g_client.connect(portArg, baudArg, slaveArg, timeoutArg);
            g_connected = ok;
            if (ok) {
                data.sysInfo = g_client.readSystemInfo();
                for (int i = 0; i < 2; ++i) data.chInfo[i] = g_client.readChannelInfo(i);
                data.sysCfg  = g_client.readSystemConfig();
                for (int i = 0; i < 2; ++i) data.chCfg[i]  = g_client.readChannelConfig(i);
                data.valid = true;
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok ? "" : "Connect failed: " + g_client.lastError(); }
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    screen.Loop(root);
    running = false;
    if (pollThread.joinable()) pollThread.join();
    g_client.disconnect();
    return 0;
}

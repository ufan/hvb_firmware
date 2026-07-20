#include "psb_modbus_client.h"
#include "topology_config.h"
#include "register_map.h"
#include "board_catalog.h"
#include "tool_version.h"
#include "tab_monitor.h"
#include "tab_channel.h"
#include "tui_policy.h"
#include "modbus_settings.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static psb::PsbModbusClient g_client;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 1;

int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    auto* baudOpt = app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    auto* slaveOpt = app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    CLI11_PARSE(app, argc, argv);

    // -p auto-connects at startup, exactly like today; --topology (or its
    // default path) only pre-fills the connection modal's fields — it does
    // NOT by itself auto-connect, same distinction ConfigManager's old
    // auto-load-as-default behavior had (the user still clicks Connect).
    bool autoConnect = !portArg.empty();
    std::string cfgPort = portArg;
    if (portArg.empty()) {
        bool topologyExplicit = topologyOpt->count() > 0;
        if (psb::TopologyConfig::exists(topologyPath)) {
            auto topo = psb::TopologyConfig::load(topologyPath);
            if (!topo.has_value()) {
                std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
                return 1;
            }
            // Phase 1: the dashboard below is still single-board only. A
            // topology resolving to more than one board is a clear, named
            // error rather than silently only using the first one — the
            // multi-board dashboard lands in a later phase (see
            // docs/superpowers/specs/2026-07-20-multi-board-topology-design.md).
            if (topo->totalBoardCount() > 1) {
                std::cerr << "Topology config " << topologyPath << " has " << topo->totalBoardCount()
                          << " boards — the multi-board dashboard isn't available yet in this build.\n"
                          << "Use --port for a specific board, or trim the topology file to one board.\n";
                return 1;
            }
            if (topo->totalBoardCount() == 1) {
                const auto& bus = topo->buses.front();
                const auto& board = bus.boards.front();
                cfgPort = bus.port;
                if (baudOpt->count() == 0) baudArg = bus.baudRate;
                if (slaveOpt->count() == 0) slaveArg = board.slaveId;
            }
        } else if (topologyExplicit) {
            std::cerr << "Topology config error: " << topologyPath << " not found\n";
            return 1;
        }
        // Neither -p nor a resolvable --topology: fall back to today's
        // hardcoded first-run guess.
        if (cfgPort.empty()) cfgPort = "/dev/ttyUSB0";
    }
    std::string cfgBaud    = std::to_string(baudArg);
    std::string cfgSlaveId = std::to_string(slaveArg);

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

    screen.Loop(root);
    running = false;
    workCv.notify_all();
    if (modbusWorker.joinable()) modbusWorker.join();
    if (animThread.joinable())   animThread.join();
    g_client.disconnect();
    return 0;
}

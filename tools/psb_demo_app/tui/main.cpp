#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "topology_config.h"
#include "board_session.h"
#include "board_dashboard.h"
#include "board_switcher.h"
#include "tool_version.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <CLI/CLI.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

static int g_pollInterval = 1;

int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    CLI11_PARSE(app, argc, argv);

    psb::TopologyConfig topo;
    if (!portArg.empty()) {
        topo = psb::TopologyConfig::singleBoard(portArg, baudArg, slaveArg, "board1");
    } else {
        bool topologyExplicit = topologyOpt->count() > 0;
        if (psb::TopologyConfig::exists(topologyPath)) {
            auto loaded = psb::TopologyConfig::load(topologyPath);
            if (!loaded.has_value()) {
                std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
                return 1;
            }
            topo = std::move(*loaded);
        } else if (topologyExplicit) {
            std::cerr << "Topology config error: " << topologyPath << " not found\n";
            return 1;
        } else {
            // Neither -p nor a resolvable --topology: fall back to today's
            // hardcoded first-run guess.
            topo = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
        }
    }
    if (topo.totalBoardCount() == 0) {
        std::cerr << "Topology config " << topologyPath << " has no boards configured.\n";
        return 1;
    }
    // -p or a genuinely multi-board topology auto-connects every resolved
    // board at startup; a topology resolving to exactly one board only
    // pre-fills that board's connection modal — same distinction the
    // single-board Phase established (ConfigManager's old
    // auto-load-as-default behavior never auto-connected by itself either;
    // the user still clicked Connect).
    bool autoConnectAll = !portArg.empty() || topo.totalBoardCount() > 1;

    auto screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> running{true};

    // ---- Build one BusWorker per bus, one BoardSession per board ----
    std::vector<std::unique_ptr<psb::tui::BusWorker>> busWorkers;
    std::vector<std::unique_ptr<psb::tui::BoardSession>> boards;
    for (const auto& busCfg : topo.buses) {
        auto bw = std::make_unique<psb::tui::BusWorker>();
        bw->bus = std::make_shared<psb::PsbSerialBus>();
        for (const auto& boardCfg : busCfg.boards) {
            auto b = std::make_unique<psb::tui::BoardSession>();
            b->nickname = boardCfg.nickname;
            b->bus = bw->bus;
            b->client = std::make_unique<psb::PsbBoardSession>(bw->bus, boardCfg.slaveId);
            b->portVal = busCfg.port;
            b->baudVal = std::to_string(busCfg.baudRate);
            b->slaveVal = std::to_string(boardCfg.slaveId);
            // AppState has only reference members and no user-declared
            // constructor — it's an aggregate, initializable via brace-init
            // (`new AppState{...}`) but not paren-init, so make_unique<>()
            // (which calls `new T(args...)`) can't construct it directly.
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutArg);
            bw->boards.push_back(b.get());
            boards.push_back(std::move(b));
        }
        busWorkers.push_back(std::move(bw));
    }

    // ---- Bus worker threads — one per bus, serialises all I/O for every
    //      board sharing it (client-architecture-and-pitfalls.md §2.1,
    //      generalized from one port total to one port per bus). Each
    //      thread's first action, before entering its poll loop, is an
    //      auto-connect sweep over its own boards if autoConnectAll — the
    //      bus itself connects once (all boards on it share one physical
    //      port/baud, by definition of "same bus" in the topology schema),
    //      then each board is verified and full-scanned in turn. ----
    for (auto& bwPtr : busWorkers) {
        psb::tui::BusWorker& bw = *bwPtr;
        bw.thread = std::thread([&bw, &running, &screen, autoConnectAll, timeoutArg] {
            if (autoConnectAll && !bw.boards.empty()) {
                std::string port = bw.boards.front()->portVal;
                int baud = 115200;
                try { baud = std::stoi(bw.boards.front()->baudVal); } catch (...) {}
                bool busOk = bw.bus->connect(port, baud, timeoutArg);
                for (psb::tui::BoardSession* b : bw.boards) {
                    b->connecting = true;
                    screen.PostEvent(Event::Custom);
                    bool ok = busOk && b->client->verifyProtocol();
                    b->connected = ok;
                    b->connecting = false;
                    { std::lock_guard<std::mutex> lk(b->statusMutex);
                      b->statusMsg = ok ? "" : "Error: " + (busOk ? b->client->lastError() : bw.bus->lastError()); }
                    if (ok) {
                        psb::tui::doFullScan(*b->client, b->connected, b->data, screen, running);
                        b->data.valid = b->connected.load();
                        b->pendingChannelCount.store(b->data.numChannels(), std::memory_order_release);
                        b->pendingSync.store(true, std::memory_order_release);
                    }
                    screen.PostEvent(Event::Custom);
                }
            }
            while (running) {
                {
                    bool anyConnected = false;
                    for (psb::tui::BoardSession* b : bw.boards)
                        if (b->connected.load()) { anyConnected = true; break; }
                    auto waitDur = anyConnected ? std::chrono::milliseconds(50)
                                                : std::chrono::seconds(g_pollInterval);
                    std::unique_lock<std::mutex> lk(bw.workMutex);
                    bw.workCv.wait_for(lk, waitDur, [&] { return !bw.workQueue.empty() || !running; });
                }
                for (;;) {
                    std::function<void()> item;
                    { std::lock_guard<std::mutex> lk(bw.workMutex);
                      if (bw.workQueue.empty()) break;
                      item = std::move(bw.workQueue.front()); bw.workQueue.pop(); }
                    item();
                }
                for (psb::tui::BoardSession* b : bw.boards) {
                    if (!running) break;
                    if (b->connected.load()) {
                        auto hasPendingWork = [&bw] {
                            std::lock_guard<std::mutex> lk(bw.workMutex);
                            return !bw.workQueue.empty();
                        };
                        psb::tui::doPollScan(*b->client, b->data, screen, running, hasPendingWork, b->statusMsg, b->statusMutex);
                        b->data.valid = b->connected.load() && b->client->isConnected();
                    }
                }
            }
        });
    }

    // ---- Animation thread — drives breathing LED at ~12 Hz for whichever
    //      board is currently visible. Each board's Renderer computes its
    //      own breathing color fresh every repaint (a pure function of
    //      wall-clock time), so one shared periodic repaint trigger is
    //      enough regardless of which board is on screen. ----
    std::thread animThread([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!running) break;
            bool anyConnected = false;
            for (auto& b : boards) if (b->connected.load()) { anyConnected = true; break; }
            if (anyConnected) screen.PostEvent(Event::Custom);
        }
    });

    // ---- Board switcher + active dashboard ----
    auto root = psb::tui::makeBoardSwitcher(boards, screen);

    screen.Loop(root);
    running = false;
    for (auto& bw : busWorkers) bw->workCv.notify_all();
    for (auto& bw : busWorkers) if (bw->thread.joinable()) bw->thread.join();
    if (animThread.joinable()) animThread.join();
    for (auto& bw : busWorkers) bw->bus->disconnect();
    return 0;
}

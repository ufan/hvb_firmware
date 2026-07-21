#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "topology_config.h"
#include "board_session.h"
#include "board_dashboard.h"
#include "board_switcher.h"
#include "wizard_state.h"
#include "wizard_screen.h"
#include "mode_select.h"
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

// Everything the running program needs to join/tear down cleanly — built
// once by buildRuntime(), used identically whether that happened before the
// wizard even ran (the common case) or right after it finished (Task 5's
// Connect Now).
// One in-flight board removal — see removeBoardLive/drainPendingRemovals.
// `done` is set by the board's own BusWorker thread (inside a queued work
// item) once it has erased this board from its own `boards` list; the UI
// thread polls it once per frame and only finishes cleanup (destroying the
// BoardSession, detaching from the switcher, tearing the bus down if now
// empty) once it observes `done == true` — guaranteeing the object is never
// touched by more than one thread at a time.
struct PendingRemoval {
    psb::tui::BusWorker* bw;
    psb::tui::BoardSession* board;
    std::shared_ptr<std::atomic<bool>> done;
};

struct Runtime {
    std::vector<std::unique_ptr<psb::tui::BusWorker>> busWorkers;
    std::vector<std::unique_ptr<psb::tui::BoardSession>> boards;
    // Guards `boards` specifically — animThread reads it every 80ms for the
    // program's entire runtime, and applyNewBoardsLive (UI thread) appends
    // to it mid-session (hot-attach). Before Task 7, every push_back into
    // `boards` happened in buildRuntime, before animThread existed, so no
    // lock was needed; applyNewBoardsLive is the first write that can race
    // a live reader, the same class of bug fixed for BusWorker::boards —
    // see runBusWorkerLoop's comment for the full reasoning.
    std::mutex boardsMutex;
    psb::tui::BoardSwitcher switcher;
    std::thread animThread;
    std::vector<PendingRemoval> pendingRemovals;
};

// The per-bus polling loop body: wait for work-or-timeout, drain the work
// queue, then poll every connected board once. Identical for every bus
// worker thread regardless of when that thread was started — factored out
// so Task 7's applyNewBoardsLive (hot-attaching a new bus mid-session) can
// start an identical loop without duplicating it verbatim.
//
// bw.boards is read here on the worker thread and — since Task 7 — can be
// appended to from the UI thread (applyNewBoardsLive, hot-attaching a board
// to an already-running bus) while this loop is live. Before Task 7,
// boards was only ever populated once, before any worker thread started,
// so an unguarded read/iterate was safe; it no longer is. Both read sites
// below take a locked snapshot copy (workMutex, the same mutex
// applyNewBoardsLive's push_back now holds) rather than holding the lock
// for the whole iteration — the poll loop calls doPollScan(), which blocks
// on real I/O, and holding a lock the UI thread also needs (to enqueue
// work) across that would serialize UI responsiveness against bus latency.
// A pointer to a board added after a snapshot is taken simply isn't polled
// until the next cycle (at most ~50ms/g_pollInterval later, matching the
// underlying poll cadence) — not a correctness issue, since a board is
// never removed from `boards` once added, so every pointer any snapshot
// ever holds stays valid for the program's lifetime.
void runBusWorkerLoop(psb::tui::BusWorker& bw, ScreenInteractive& screen, std::atomic<bool>& running) {
    while (running && !bw.stopRequested) {
        std::vector<psb::tui::BoardSession*> boardsSnapshot;
        {
            std::unique_lock<std::mutex> lk(bw.workMutex);
            boardsSnapshot = bw.boards;
            bool anyConnected = false;
            for (psb::tui::BoardSession* b : boardsSnapshot)
                if (b->connected.load()) { anyConnected = true; break; }
            auto waitDur = anyConnected ? std::chrono::milliseconds(50)
                                        : std::chrono::seconds(g_pollInterval);
            bw.workCv.wait_for(lk, waitDur, [&] { return !bw.workQueue.empty() || !running; });
        }
        for (;;) {
            std::function<void()> item;
            { std::lock_guard<std::mutex> lk(bw.workMutex);
              if (bw.workQueue.empty()) break;
              item = std::move(bw.workQueue.front()); bw.workQueue.pop(); }
            item();
        }
        // Re-snapshot after draining the queue — a hot-attach's connect
        // work item (applyNewBoardsLive) may have just run above, and its
        // board should be eligible for this very poll cycle rather than
        // waiting one extra tick.
        { std::lock_guard<std::mutex> lk(bw.workMutex);
          boardsSnapshot = bw.boards; }
        for (psb::tui::BoardSession* b : boardsSnapshot) {
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
}

// Entry point for both removal paths (Global Constraints: dashboard Remove
// button and the wizard's mid-session Remove-then-Apply must call this same
// function, never two separate implementations). Finds the BusWorker
// currently owning `board`, and enqueues a work item on that bus's own
// queue that does the actual detach from bw.boards — on the worker thread
// itself, so nothing on the UI thread ever races that thread's own reads of
// its `boards` list. Registers a pendingRemoval; the real cleanup (object
// destruction, switcher detach, bus teardown if now empty) happens later,
// once drainPendingRemovals observes the work item finished.
//
// No-op if `board` isn't found in any BusWorker — defensive; both callers
// only ever pass boards they can see are currently live.
void removeBoardLive(Runtime& rt, ScreenInteractive& screen,
                     std::atomic<bool>& running, psb::tui::BoardSession* board) {
    psb::tui::BusWorker* owningBw = nullptr;
    for (auto& bwPtr : rt.busWorkers) {
        for (auto* b : bwPtr->boards) {
            if (b == board) { owningBw = bwPtr.get(); break; }
        }
        if (owningBw) break;
    }
    if (!owningBw) return;

    auto done = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(owningBw->workMutex);
        owningBw->workQueue.push([owningBw, board, done, &screen] {
            {
                std::lock_guard<std::mutex> lk2(owningBw->workMutex);
                for (size_t i = 0; i < owningBw->boards.size(); ++i) {
                    if (owningBw->boards[i] == board) {
                        owningBw->boards.erase(owningBw->boards.begin() + i);
                        break;
                    }
                }
            }
            done->store(true);
            screen.PostEvent(Event::Custom);
        });
    }
    owningBw->workCv.notify_one();

    rt.pendingRemovals.push_back({owningBw, board, done});
}

// Drains completed removals once per frame (called from main()'s root
// Renderer). For each entry whose worker-thread detach has finished:
// detaches from the switcher, destroys the BoardSession (only safe now —
// the owning BusWorker thread has confirmed it will never touch this
// pointer again), and — if that was the bus's last board — tears the bus
// down too (Global Constraints: empty-bus teardown).
//
// The bus-teardown join below is a bounded, brief exception to "never block
// the UI thread on worker activity": by this point bw->boards is already
// empty, so the worker thread has at most one harmless empty-loop iteration
// left (no doPollScan calls, since there's nothing left to poll) before it
// observes stopRequested and exits — worst case is bounded by
// kPollTimeoutMs (300ms) if a poll for the just-removed board was already
// in flight from a snapshot taken before this cycle's queue drain, not a
// full connect-timeout-scale stall. This is the one case in this codebase's
// removal machinery where a brief block is an accepted tradeoff rather than
// a further staged hand-off, since it's bounded, small, and rare (only
// happens when a user removes the last board on a given bus).
void drainPendingRemovals(Runtime& rt, psb::TopologyConfig& topo,
                          ScreenInteractive& screen, std::atomic<bool>& running) {
    for (size_t i = 0; i < rt.pendingRemovals.size(); ) {
        auto& pr = rt.pendingRemovals[i];
        if (!pr.done->load()) { ++i; continue; }

        rt.switcher.detachBoard(pr.board->nickname);

        // Keep main()'s topo in sync — the dashboard's Remove button (the
        // only removal path that lands here without first going through
        // the wizard's own topo edit) has no other way to make a
        // subsequent Setup reopen or Save reflect the removal. Harmless,
        // redundant no-op for a wizard-triggered removal: onMidSessionFinish
        // already overwrites topo wholesale from the wizard's edited state
        // (which no longer contains this board) before this async drain
        // ever runs, so the search below simply finds nothing to erase.
        for (auto& busCfg : topo.buses) {
            auto& boards = busCfg.boards;
            for (size_t m = 0; m < boards.size(); ++m) {
                if (boards[m].nickname == pr.board->nickname) {
                    boards.erase(boards.begin() + m);
                    break;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(rt.boardsMutex);
            for (size_t j = 0; j < rt.boards.size(); ++j) {
                if (rt.boards[j].get() == pr.board) {
                    rt.boards.erase(rt.boards.begin() + j);
                    break;
                }
            }
        }

        bool busEmpty = false;
        { std::lock_guard<std::mutex> lk(pr.bw->workMutex);
          busEmpty = pr.bw->boards.empty(); }
        if (busEmpty) {
            pr.bw->stopRequested = true;
            pr.bw->workCv.notify_all();
            if (pr.bw->thread.joinable()) pr.bw->thread.join();
            pr.bw->bus->disconnect();
            for (size_t k = 0; k < rt.busWorkers.size(); ++k) {
                if (rt.busWorkers[k].get() == pr.bw) {
                    rt.busWorkers.erase(rt.busWorkers.begin() + k);
                    break;
                }
            }
        }

        rt.pendingRemovals.erase(rt.pendingRemovals.begin() + i);
        // Do not increment i — the next element (if any) shifted into this slot.
    }
}

// Builds BusWorker/BoardSession for every bus/board in `topo` into `rt`,
// starts one worker thread per bus (Phase 2), starts the shared animation
// thread. Identical to Phase 2's inline main() body, extracted so Task 6 can
// call it either at startup or after the wizard finishes.
//
// Takes `rt` as an out-parameter rather than returning it by value: the
// animation thread below captures `&rt` by reference, and that capture must
// refer to the one object that lives for the rest of main()'s frame — a
// return-by-value here would only be safe under NRVO, which the standard
// doesn't guarantee. Passing the caller's already-in-place `Runtime rt;` by
// reference removes that dependency entirely (this codebase already had one
// real use-after-free from a similar lifetime assumption — see
// board_switcher.h's history — not worth risking a second, especially since
// Task 7 adds more branches to this function that would make NRVO even
// less certain to hold).
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup) {
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
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });
            // Same "lock even though provably safe here" reasoning as the
            // rt.boards push right below — this bw's worker thread hasn't
            // been spawned yet at this point, but locking anyway means
            // that isn't a fact a future edit could silently invalidate.
            { std::lock_guard<std::mutex> lk(bw->workMutex);
              bw->boards.push_back(b.get()); }
            { std::lock_guard<std::mutex> lk(rt.boardsMutex);
              rt.boards.push_back(std::move(b)); }
        }
        rt.busWorkers.push_back(std::move(bw));
    }

    for (auto& bwPtr : rt.busWorkers) {
        psb::tui::BusWorker& bw = *bwPtr;
        bw.thread = std::thread([&bw, &running, &screen, autoConnectAll, timeoutMs] {
            // Locked snapshot for the same reason runBusWorkerLoop's own
            // reads take one — a fast mid-session Setup click can hot-attach
            // a board to this same bus (applyNewBoardsLive, UI thread)
            // while this initial connect sweep is still running here.
            std::vector<psb::tui::BoardSession*> initialBoards;
            { std::lock_guard<std::mutex> lk(bw.workMutex);
              initialBoards = bw.boards; }
            if (autoConnectAll && !initialBoards.empty()) {
                std::string port = initialBoards.front()->portVal;
                int baud = 115200;
                try { baud = std::stoi(initialBoards.front()->baudVal); } catch (...) {}
                bool busOk = bw.bus->connect(port, baud, timeoutMs);
                for (psb::tui::BoardSession* b : initialBoards) {
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
            runBusWorkerLoop(bw, screen, running);
        });
    }

    rt.animThread = std::thread([&rt, &screen, &running] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!running) break;
            bool anyConnected = false;
            { // rt.boardsMutex — see Runtime::boardsMutex's comment. Held
              // only for this cheap atomic-flag scan, not across PostEvent.
              std::lock_guard<std::mutex> lk(rt.boardsMutex);
              for (auto& b : rt.boards) if (b->connected.load()) { anyConnected = true; break; }
            }
            if (anyConnected) screen.PostEvent(Event::Custom);
        }
    });

    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup);
}

void joinRuntime(Runtime& rt, std::atomic<bool>& running) {
    running = false;
    for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
    for (auto& bw : rt.busWorkers) if (bw->thread.joinable()) bw->thread.join();
    if (rt.animThread.joinable()) rt.animThread.join();
    for (auto& bw : rt.busWorkers) bw->bus->disconnect();
}

// Attaches every bus/board present in `newTopo` but absent from the
// currently-running `rt` — strictly additive (see Task 7's Global
// Constraints scope note). A bus already running is matched by port,
// following the same technique the bus worker thread itself already uses
// to learn its own port (bw.boards.front()->portVal) — BusWorker has no
// port field of its own. A brand-new bus gets its own BusWorker and thread,
// built the same way buildRuntime() builds every other bus (and runs the
// same runBusWorkerLoop() body — no duplicated loop logic).
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup) {
    for (const auto& busCfg : newTopo.buses) {
        psb::tui::BusWorker* existingBw = nullptr;
        for (auto& bw : rt.busWorkers)
            if (!bw->boards.empty() && bw->boards.front()->portVal == busCfg.port)
                { existingBw = bw.get(); break; }

        for (const auto& boardCfg : busCfg.boards) {
            bool alreadyRunning = false;
            if (existingBw)
                for (auto* b : existingBw->boards)
                    if (b->nickname == boardCfg.nickname) { alreadyRunning = true; break; }
            if (alreadyRunning) continue;

            psb::tui::BusWorker* targetBw = existingBw;
            if (!targetBw) {
                auto bw = std::make_unique<psb::tui::BusWorker>();
                bw->bus = std::make_shared<psb::PsbSerialBus>();
                bw->bus->connect(busCfg.port, busCfg.baudRate, timeoutMs);
                targetBw = bw.get();
                rt.busWorkers.push_back(std::move(bw));
                psb::tui::BusWorker& bwRef = *targetBw;
                bwRef.thread = std::thread([&bwRef, &running, &screen] {
                    runBusWorkerLoop(bwRef, screen, running);
                });
                existingBw = targetBw;
            }

            auto b = std::make_unique<psb::tui::BoardSession>();
            b->nickname = boardCfg.nickname;
            b->bus = targetBw->bus;
            b->client = std::make_unique<psb::PsbBoardSession>(targetBw->bus, boardCfg.slaveId);
            b->portVal = busCfg.port;
            b->baudVal = std::to_string(busCfg.baudRate);
            b->slaveVal = std::to_string(boardCfg.slaveId);
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                targetBw->workQueue, targetBw->workMutex, targetBw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });

            // Connect + full-scan this one board right away, on its bus's
            // own worker queue — never block the UI thread, same discipline
            // doConnect() in board_dashboard.h already follows.
            psb::tui::BoardSession* bPtr = b.get();
            psb::tui::BusWorker* bwPtr = targetBw;
            // boards.push_back happens under the same lock as the
            // workQueue push (not just the queue push alone) — runBusWorkerLoop
            // (the worker thread) reads bw.boards without holding this lock
            // for the whole iteration, but always takes a locked snapshot
            // first; this push_back is the write that snapshot must never
            // observe half-finished. Before Task 7, boards was only ever
            // populated once, before any worker thread existed, so this
            // wasn't a concern — it is now, since this bus's worker thread
            // may already be live and running its own read loop.
            { std::lock_guard<std::mutex> lk(bwPtr->workMutex);
              bwPtr->workQueue.push([bPtr, &screen, &running] {
                  bool ok = bPtr->client->verifyProtocol();
                  bPtr->connected = ok;
                  { std::lock_guard<std::mutex> lk2(bPtr->statusMutex);
                    bPtr->statusMsg = ok ? "" : "Error: " + bPtr->client->lastError(); }
                  if (ok) {
                      psb::tui::doFullScan(*bPtr->client, bPtr->connected, bPtr->data, screen, running);
                      bPtr->data.valid = bPtr->connected.load();
                      bPtr->pendingChannelCount.store(bPtr->data.numChannels(), std::memory_order_release);
                      bPtr->pendingSync.store(true, std::memory_order_release);
                  }
                  screen.PostEvent(Event::Custom);
              });
              targetBw->boards.push_back(bPtr);
            }
            bwPtr->workCv.notify_one();

            rt.switcher.attachBoard(b->nickname, b->dashboard);
            // Same class of race as BusWorker.boards above, this time
            // against animThread (live for the program's whole runtime) —
            // see Runtime::boardsMutex's comment.
            { std::lock_guard<std::mutex> lk(rt.boardsMutex);
              rt.boards.push_back(std::move(b)); }
        }
    }
}

// Mirror of applyNewBoardsLive: finds every board currently live in `rt`
// but absent (by nickname) from `newTopo`, and removes each one via
// removeBoardLive — this is what makes the wizard's mid-session Remove
// Board (previously an in-memory-only edit that never touched the live
// session, see removeBoardLive's own history) actually take effect.
// Deliberately does not take rt.boardsMutex while calling removeBoardLive
// for each match — removeBoardLive only enqueues a work item and appends
// to pendingRemovals, it never itself touches rt.boards, so there's no
// double-lock/deadlock risk in taking the lock only for the initial scan.
void removeGoneBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                          ScreenInteractive& screen, std::atomic<bool>& running) {
    std::vector<psb::tui::BoardSession*> toRemove;
    {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) {
            bool stillPresent = false;
            for (const auto& busCfg : newTopo.buses) {
                for (const auto& boardCfg : busCfg.boards) {
                    if (boardCfg.nickname == b->nickname) { stillPresent = true; break; }
                }
                if (stillPresent) break;
            }
            if (!stillPresent) toRemove.push_back(b.get());
        }
    }
    for (auto* b : toRemove) removeBoardLive(rt, screen, running, b);
}

int main(int argc, char** argv) {
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 3000;
    std::string topologyPath = psb::TopologyConfig::defaultPath();
    bool setupFlag = false;
    app.add_option("-p,--port", portArg, "Serial port (quick single-board connect; auto-connects at startup)");
    app.add_option("-b,--baud", baudArg, "Baud rate")
        ->check(CLI::IsMember({9600, 19200, 38400, 115200}));
    app.add_option("-i,--id", slaveArg, "Slave ID")->check(CLI::Range(0, 247));
    app.add_option("-t,--timeout", timeoutArg, "Timeout ms");
    app.add_option("-s,--poll-interval", g_pollInterval, "Idle poll interval (s)");
    auto* topologyOpt = app.add_option("-T,--topology", topologyPath,
        "Topology config file (default: " + topologyPath + ")");
    app.add_flag("--setup", setupFlag, "Launch the interactive topology setup wizard");
    CLI11_PARSE(app, argc, argv);

    auto screen = ScreenInteractive::Fullscreen();

    // ---- Resolve (or build) the topology, running the wizard first when
    //      asked to or when needed (design spec case 3: a --topology path
    //      that doesn't exist yet auto-launches the wizard pre-targeting it
    //      as the Save destination, instead of silently falling through to
    //      hardcoded defaults). ----
    psb::TopologyConfig topo;
    bool haveTopo = false;
    bool topologyExplicit = topologyOpt->count() > 0;

    if (!portArg.empty() && !setupFlag) {
        topo = psb::TopologyConfig::singleBoard(portArg, baudArg, slaveArg, "board1");
        haveTopo = true;
    } else if (psb::TopologyConfig::exists(topologyPath)) {
        // Not gated on !setupFlag — --setup on an existing topology must
        // still seed the wizard with it (per the comment below); setupFlag
        // only decides whether the wizard also runs afterward.
        auto loaded = psb::TopologyConfig::load(topologyPath);
        if (!loaded.has_value()) {
            std::cerr << "Topology config error: could not parse " << topologyPath << "\n";
            return 1;
        }
        topo = std::move(*loaded);
        haveTopo = true;
    } else if (topologyExplicit) {
        // Case 3 — file named but missing (exists() already ruled out
        // above): wizard runs regardless of --setup, pre-targeting this path.
    } else if (!setupFlag) {
        // Neither -p nor a resolvable/explicit --topology, and --setup not
        // given: no CLI signal at all resolves what to do. Show the
        // mode-selection popup instead of guessing a hardcoded port (Sub-
        // project B; see mode_select.h and docs/superpowers/specs/
        // 2026-07-21-mode-architecture-design.md). Every other branch in
        // this chain (-p, an existing topology file, --topology naming a
        // missing file, --setup) is untouched — this replaces only the
        // old /dev/ttyUSB0 fallback.
        auto choice = psb::tui::showModeChoicePopup(screen);
        if (choice == psb::tui::ModeChoice::Cancelled) return 0;
        if (choice == psb::tui::ModeChoice::Single) {
            auto quick = psb::tui::showQuickConnectForm(screen);
            if (!quick.has_value()) return 0;
            topo = *quick;
            haveTopo = true;
        }
        // else Multi: leave haveTopo false — the existing runWizard logic
        // below launches the standalone wizard exactly as --setup/case-3
        // already do, with no changes needed here.
    }
    // else: setupFlag is true — always run the wizard next, regardless of
    // what topo/haveTopo currently hold (a pre-existing topology, if any,
    // seeds the wizard for editing rather than starting from empty).

    bool runWizard = setupFlag || !haveTopo;
    if (runWizard) {
        psb::tui::WizardState wiz;
        wiz.topologyPath = topologyPath;
        if (haveTopo) wiz.topo = topo;

        psb::tui::WizardOutcome outcome = psb::tui::WizardOutcome::Cancelled;
        auto wizardRoot = psb::tui::makeWizardScreen(wiz, screen, [&](psb::tui::WizardOutcome o) {
            outcome = o;
            screen.ExitLoopClosure()();
        });
        screen.Loop(wizardRoot);

        if (outcome == psb::tui::WizardOutcome::Cancelled) {
            if (!haveTopo) return 0;  // first run, cancelled — nothing to connect to
            // else: fall through using the pre-existing topo unchanged.
        } else {
            topo = wiz.topo;
            haveTopo = true;
        }
        if (topo.totalBoardCount() == 0) {
            std::cerr << "Topology has no boards configured — exiting.\n";
            return 0;
        }
    }

    bool autoConnectAll = !portArg.empty() || runWizard || topo.totalBoardCount() > 1;

    std::atomic<bool> running{true};

    auto showSetup = std::make_shared<bool>(false);
    auto midSessionWiz = std::make_shared<psb::tui::WizardState>();
    midSessionWiz->topologyPath = topologyPath;

    std::function<void()> openSetup = [showSetup, midSessionWiz, &topo, &screen] {
        midSessionWiz->topo = topo;  // seed with the currently-running topology
        midSessionWiz->selectedBus = -1;
        midSessionWiz->selectedBoard = -1;
        midSessionWiz->statusMsg.clear();
        *showSetup = true;
        screen.PostEvent(Event::Custom);
    };

    Runtime rt;

    // Constructed once, not once per board — Quit notifies every bus's
    // worker (not just one board's, unlike the old per-board version this
    // replaces), so quitting wakes every bus thread immediately rather
    // than letting some wait out their idle poll interval. Setup reuses
    // the existing openSetup closure unchanged.
    auto bGlobalQuit = psb::tui::ActionButton("Quit", [&running, &rt, &screen] {
        running = false;
        for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
        screen.ExitLoopClosure()();
    });
    auto bGlobalSetup = psb::tui::ActionButton("Setup", [openSetup] { openSetup(); });

    buildRuntime(rt, topo, screen, running, timeoutArg, autoConnectAll, openSetup, bGlobalQuit, bGlobalSetup);

    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, timeoutArg, openSetup,
                               bGlobalQuit, bGlobalSetup]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            // Tear down what's gone before attaching what's new — the two
            // sets are always disjoint (a board can't be both removed and
            // newly-added in the same edit), so the order between them
            // doesn't affect correctness, but removing first reads slightly
            // more naturally. topo is overwritten immediately below; the
            // live teardown/attach these two calls kick off both finish
            // asynchronously a frame or two later (removeBoardLive's
            // staged hand-off, applyNewBoardsLive's queued connect) — topo
            // is already correct by the time either one completes.
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, timeoutArg, openSetup,
                               bGlobalQuit, bGlobalSetup);
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish, /*allowScan=*/false);

    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &topo, &screen, &running] {
        drainPendingRemovals(rt, topo, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get());

    screen.Loop(rootWithSetup);
    joinRuntime(rt, running);
    return 0;
}

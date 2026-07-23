#include "psb_serial_bus.h"
#include "psb_board_session.h"
#include "topology_config.h"
#include "topology_rules.h"
#include "board_session.h"
#include "board_dashboard.h"
#include "board_switcher.h"
#include "wizard_state.h"
#include "wizard_screen.h"
#include "group_wizard_state.h"
#include "group_wizard_screen.h"
#include "group_monitor.h"
#include "mode_select.h"
#include "preferences_dialog.h"
#include "app_preferences.h"
#include "tool_version.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
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
static int g_connectTimeoutMs = 3000;

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
    std::function<std::string(const std::string&, int, const std::string&)> saveGroupAlias;
};

// The single reconciliation point for group dashboards — called any time
// `rt.boards` or `topo.groups` can change while the app is running (see
// every call site below). A group's dashboard (group_monitor.h) binds its
// row widgets, at construction, to whichever BoardSession currently owns
// each member's nickname — if that BoardSession is later erased from
// rt.boards, those bindings dangle. Rather than track that precisely, every
// group dashboard is torn down and rebuilt fresh here unconditionally: a
// group dashboard owns no hardware connection or worker thread of its own,
// so doing this on every board/group change is cheap and side-effect-free,
// and it uniformly covers every way `boards` or `topo.groups` can change
// (a board added filling in a previously-missing member, a board removed
// invalidating a member's row, a group added/removed/edited via the
// wizard) with one code path instead of several hand-diffed ones.
void refreshGroupDashboards(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen) {
    std::vector<std::pair<std::string, Component>> groupDashboards;
    for (const auto& g : topo.groups) {
        auto dash = psb::tui::makeGroupDashboard(g.name, g.channels, rt.boards, rt.switcher.jumpToBoard,
                                                 rt.saveGroupAlias);
        groupDashboards.emplace_back(g.name, std::move(dash));
    }
    rt.switcher.replaceGroups(std::move(groupDashboards));
    screen.PostEvent(Event::Custom);
}

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
    bool anyRemoved = false;
    for (size_t i = 0; i < rt.pendingRemovals.size(); ) {
        auto& pr = rt.pendingRemovals[i];
        if (!pr.done->load()) { ++i; continue; }
        anyRemoved = true;

        rt.switcher.detachBoard(pr.board->nickname);

        // Keep main()'s topo in sync — the dashboard's Remove button (the
        // only removal path that lands here without first going through
        // the wizard's own topo edit) has no other way to make a
        // subsequent Setup reopen or Save reflect the removal. Harmless,
        // redundant no-op for a wizard-triggered removal: onMidSessionFinish
        // already overwrites topo wholesale from the wizard's edited state
        // (which no longer contains this board) before this async drain
        // ever runs, so the search below simply finds nothing to erase.
        //
        // Erasing the bus entry itself when its last board is removed (not
        // just the board) mirrors the busEmpty teardown of rt.busWorkers
        // below — a bus that's actually live always has at least one board
        // while running, so an empty one here can only be the bus whose
        // last board this iteration just removed. Leaving it behind was a
        // real bug: it showed up as a dead, boardless entry in the Buses
        // panel the next time Setup/Topology reopened, and got needlessly
        // re-saved to disk.
        for (size_t b = 0; b < topo.buses.size(); ) {
            auto& boards = topo.buses[b].boards;
            for (size_t m = 0; m < boards.size(); ++m) {
                if (boards[m].nickname == pr.board->nickname) {
                    boards.erase(boards.begin() + m);
                    break;
                }
            }
            if (boards.empty()) {
                topo.buses.erase(topo.buses.begin() + b);
                continue;  // next bus (if any) shifted into this slot
            }
            ++b;
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
        bool busHasOtherPendingRemoval = false;
        if (busEmpty) {
            // A topology swap can remove several boards from the same bus at
            // once. All their queued worker removals may finish before the UI
            // drains the first PendingRemoval, making the bus empty while
            // later PendingRemoval entries still hold this same BusWorker*.
            // Tear the bus down only when draining the last such entry.
            for (size_t j = 0; j < rt.pendingRemovals.size(); ++j) {
                if (j == i) continue;
                if (rt.pendingRemovals[j].bw == pr.bw) {
                    busHasOtherPendingRemoval = true;
                    break;
                }
            }
        }
        if (busEmpty && !busHasOtherPendingRemoval) {
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
    if (anyRemoved) refreshGroupDashboards(rt, topo, screen);
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
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalGroup, Component globalPreferences,
                  Component globalConnectAll, Component globalDisconnectAll,
                  psb::tui::GetGroupMembership getGroupMembership,
                  psb::tui::SaveGroupAlias saveGroupAlias,
                  psb::tui::JumpToGroup jumpToGroup) {
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
                globalQuit, globalSetup, globalGroup, globalPreferences, [&rt] { return rt.boards.size(); },
                getGroupMembership, saveGroupAlias, jumpToGroup);
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
                    if (b->abortConnect) ok = false;
                    // b->connecting stays true through the scan below (not
                    // cleared right after verifyProtocol, as a prior version
                    // of this loop did) — this dashboard is already live and
                    // interactive at this point (built earlier in
                    // buildRuntime, before this sweep runs on the bus
                    // worker thread), so a fast user can reach the toggle
                    // button's Abort branch, which itself checks
                    // board.connecting.load() to decide whether a click sets
                    // abortConnect. Clearing connecting early would make a
                    // multi-second scan look fully idle instead of
                    // in-progress, and would make Abort unreachable during
                    // that window.
                    if (ok) {
                        psb::tui::doFullScan(*b->client, b->abortConnect, b->data, screen, running);
                        ok = b->data.allChannelsLoaded() && !b->abortConnect;
                        b->pendingChannelCount.store(b->data.numChannels(), std::memory_order_release);
                        b->pendingSync.store(true, std::memory_order_release);
                    }
                    b->connected = ok;
                    b->data.valid = b->connected.load();
                    b->connecting = false;
                    { std::lock_guard<std::mutex> lk(b->statusMutex);
                      b->statusMsg = b->abortConnect ? "Connection aborted"
                                   : ok ? "" : "Error: " + (busOk ? b->client->lastError() : bw.bus->lastError()); }
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

    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, screen, globalQuit, globalSetup, globalGroup,
                                              globalPreferences, globalConnectAll, globalDisconnectAll);
    refreshGroupDashboards(rt, topo, screen);
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
                        Component globalQuit, Component globalSetup, Component globalGroup,
                        Component globalPreferences,
                        psb::tui::GetGroupMembership getGroupMembership,
                        psb::tui::SaveGroupAlias saveGroupAlias,
                        psb::tui::JumpToGroup jumpToGroup) {
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
                globalQuit, globalSetup, globalGroup, globalPreferences, [&rt] { return rt.boards.size(); },
                getGroupMembership, saveGroupAlias, jumpToGroup);

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
                  if (bPtr->abortConnect) ok = false;
                  if (ok) {
                      psb::tui::doFullScan(*bPtr->client, bPtr->abortConnect, bPtr->data, screen, running);
                      ok = bPtr->data.allChannelsLoaded() && !bPtr->abortConnect;
                      bPtr->pendingChannelCount.store(bPtr->data.numChannels(), std::memory_order_release);
                      bPtr->pendingSync.store(true, std::memory_order_release);
                  }
                  bPtr->connected = ok;
                  bPtr->data.valid = bPtr->connected.load();
                  { std::lock_guard<std::mutex> lk2(bPtr->statusMutex);
                    bPtr->statusMsg = bPtr->abortConnect ? "Connection aborted"
                                     : ok ? "" : "Error: " + bPtr->client->lastError(); }
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
    // psb_demo_tui is a double-click-launched UI app (Sub-project C) — the
    // only CLI-level concept left is --version (and CLI11's own --help),
    // both standard conventions even for GUI-first tools. Every launch is a
    // fresh launch: the mode-selection popup always shows first, letting
    // the user choose single-board quick-connect or the standalone
    // Topology wizard (which is also where an existing topology file gets
    // loaded, via its own Path field — never silently auto-loaded here).
    // The global Topology button (main() below) reopens that same wizard
    // mid-session; connection timeout and idle poll interval are reachable
    // via the global Preferences button (see preferences_dialog.h) and
    // persist across launches via AppPreferences.
    CLI::App app{"PSB Demo TUI"};
    app.set_version_flag("--version", std::string("psb_demo_tui ") + TOOL_VERSION_STRING);
    CLI11_PARSE(app, argc, argv);

    if (auto prefs = psb::AppPreferences::load(psb::AppPreferences::defaultPath()); prefs.has_value()) {
        g_connectTimeoutMs = prefs->timeoutMs;
        g_pollInterval = prefs->pollIntervalS;
    }

    const std::string topologyPath = psb::TopologyConfig::defaultPath();

    auto screen = ScreenInteractive::Fullscreen();

    // ---- Every launch is a fresh launch: the mode-selection popup shows
    //      unconditionally, regardless of whether a topology.toml already
    //      exists on disk — no CLI flag or "existing file" check bypasses
    //      it. An existing file is never auto-loaded or auto-connected
    //      silently; it's one [Load] click away inside the standalone
    //      wizard's own Path field (which defaults to topologyPath below),
    //      the same way any other topology file is opened there. The
    //      wizard's own Load handler already reports a parse failure via
    //      its status line, so there's no separate eager load-and-exit-on-
    //      error path here anymore. ----
    psb::TopologyConfig topo;
    // Tracks wherever topo's content actually came from / belongs, since
    // topologyPath above is fixed at defaultPath() for the whole session
    // but the user can load a completely different file (quick-connect's
    // own lastSingleConnectPath(), or any file Browsed to inside the
    // wizard) — anything that needs to save topo back to disk outside the
    // wizard's own scope (which always correctly uses its own live
    // WizardState::topologyPath) must use this, not topologyPath, or it
    // silently writes into the wrong file. Updated at every point below
    // where topo itself is replaced.
    std::string currentTopologyPath = topologyPath;

    // Loops instead of the old popup-once-then-maybe-wizard sequence so
    // Back (from either the quick-connect form or the standalone wizard)
    // can cleanly return to mode selection instead of only ever exiting or
    // succeeding. The old "Cancelled but a topology already existed, fall
    // through unchanged" branch is dropped as genuinely dead code: it
    // could only be reached when a topology already existed before the
    // wizard ran, which never happened — single-board quick-connect (the
    // only path that ever produced one before this point) never fell
    // through to the wizard at all.
    for (;;) {
        auto choice = psb::tui::showModeChoicePopup(screen);
        if (choice == psb::tui::ModeChoice::Exit) return 0;
        if (choice == psb::tui::ModeChoice::Single) {
            auto quick = psb::tui::showQuickConnectForm(screen);
            if (quick.outcome == psb::tui::QuickConnectOutcome::Exit) return 0;
            if (quick.outcome == psb::tui::QuickConnectOutcome::Back) continue;
            topo = quick.topo;
            currentTopologyPath = psb::TopologyConfig::lastSingleConnectPath();
            break;
        }

        // Multi: run the wizard right here (not deferred to a separate
        // block) so Back can loop cleanly back to mode selection.
        psb::tui::WizardState wiz;
        wiz.topologyPath = topologyPath;

        psb::tui::WizardOutcome outcome = psb::tui::WizardOutcome::Cancelled;
        auto wizardRoot = psb::tui::makeWizardScreen(wiz, screen, [&](psb::tui::WizardOutcome o) {
            outcome = o;
            screen.ExitLoopClosure()();
        }, /*allowScan=*/true, /*scanViaLiveBus=*/{}, /*isLaunchFlow=*/true);
        screen.Loop(wizardRoot);

        if (outcome == psb::tui::WizardOutcome::Back) continue;
        if (outcome == psb::tui::WizardOutcome::Cancelled) return 0;  // first run, exited — nothing to connect to
        topo = wiz.topo;
        currentTopologyPath = wiz.topologyPath;
        if (topo.totalBoardCount() == 0) {
            std::cerr << "Topology has no boards configured — exiting.\n";
            return 0;
        }
        break;
    }

    // Always true by this point — every surviving path here represents a
    // deliberate "connect now" gesture: the wizard's Connect Now/Save &
    // Exit (runWizard was true; a Cancel with no topology already returned
    // above), or a single-board quick-connect's own Connect button. The old
    // `runWizard || topo.totalBoardCount() > 1` condition was a leftover
    // from before Sub-project C removed the topology.toml auto-load
    // bypass: quick-connect (exactly 1 board, runWizard false) fell through
    // both clauses and was silently left un-auto-connected — the user had
    // to click Connect a second time and re-enter values that had already
    // been supplied once, even though quick-connect's whole point is to
    // connect immediately. Found via manual testing.
    bool autoConnectAll = true;

    std::atomic<bool> running{true};

    auto showSetup = std::make_shared<bool>(false);
    auto midSessionWiz = std::make_shared<psb::tui::WizardState>();
    midSessionWiz->topologyPath = topologyPath;

    std::function<void()> openSetup = [showSetup, midSessionWiz, &topo, &currentTopologyPath, &screen] {
        midSessionWiz->topo = topo;  // seed with the currently-running topology
        // Also reseed the path, not just the topology content — without
        // this, midSessionWiz->topologyPath stays stuck at whatever it was
        // last set to (the launch-time default, the first time Setup is
        // ever opened) even when currentTopologyPath has since moved on
        // (e.g. launch loaded a non-default file). Clicking Apply without
        // personally re-Browsing inside the wizard would otherwise save
        // back into that stale path — the same wrong-file risk
        // currentTopologyPath exists to prevent.
        midSessionWiz->topologyPath = currentTopologyPath;
        midSessionWiz->selectedBus = -1;
        midSessionWiz->selectedBoard = -1;
        midSessionWiz->statusMsg.clear();
        *showSetup = true;
        screen.PostEvent(Event::Custom);
    };

    auto showGroupSetup = std::make_shared<bool>(false);
    auto groupWiz = std::make_shared<psb::tui::GroupWizardState>();

    // Mirrors openSetup exactly, including reseeding topologyPath on every
    // open (not just at construction) — the exact Critical bug Phase 1
    // shipped without, then had to fix after the fact. groupWiz->topo is
    // seeded from the *full* live topo (not a groups-only copy) so Save
    // (group_wizard_screen.h's bSave) never truncates the file's
    // buses/boards.
    std::function<void()> openGroupSetup = [showGroupSetup, groupWiz, &topo, &currentTopologyPath, &screen] {
        groupWiz->topo = topo;
        groupWiz->topologyPath = currentTopologyPath;
        groupWiz->selectedGroup = -1;
        groupWiz->selectedChannel = -1;
        groupWiz->statusMsg.clear();
        *showGroupSetup = true;
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
    auto bGlobalSetup = psb::tui::ActionButton("Topology", [openSetup] { openSetup(); });
    auto bGlobalGroup = psb::tui::ActionButton("Group", [openGroupSetup] { openGroupSetup(); });

    auto showPreferences = std::make_shared<bool>(false);
    auto prefsDialog = psb::tui::makePreferencesDialog(screen, showPreferences,
                                                        g_connectTimeoutMs, g_pollInterval);
    auto bGlobalPreferences = psb::tui::ActionButton("Preferences", prefsDialog.open);

    // Each is unconditionally clickable regardless of aggregate board
    // state — not gated on whether every board is already connected/
    // disconnected — calling the same board.connect/board.disconnect
    // closures makeBoardDashboard sets on every BoardSession, never a
    // second implementation of the connect/disconnect logic itself.
    // Re-triggering an already-connected/disconnected board is a safe
    // no-op: doConnect and doDisconnect (board_dashboard.h) each guard
    // against redundant/overlapping invocations themselves, precisely so
    // this call site doesn't have to duplicate that state checking.
    auto bGlobalConnectAll = psb::tui::ActionButton("Connect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->connect) b->connect();
    });
    auto bGlobalDisconnectAll = psb::tui::ActionButton("Disconnect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->disconnect) b->disconnect();
    });

    auto getGroupMembership = [&topo](const std::string& boardNickname, int ch) -> psb::tui::GroupMembership {
        for (int gi = 0; gi < static_cast<int>(topo.groups.size()); ++gi) {
            const auto& group = topo.groups[gi];
            for (int mi = 0; mi < static_cast<int>(group.channels.size()); ++mi) {
                const auto& ref = group.channels[mi];
                if (ref.boardNickname == boardNickname && ref.channelIndex == ch) {
                    std::string alias = ref.alias.empty() ? psb::defaultChannelAlias(ch) : ref.alias;
                    return {true, group.name, gi, mi, alias};
                }
            }
        }
        return {};
    };

    auto saveGroupChannelAliasToTopology = [&rt, &topo, &currentTopologyPath, &screen]
                                           (const std::string& boardNickname, int ch,
                                            const std::string& alias) -> std::string {
        std::string err = psb::renameGroupChannelAliasForBoardChannel(topo, boardNickname, ch, alias);
        if (!err.empty()) {
            screen.PostEvent(Event::Custom);
            return err;
        }
        if (!topo.save(currentTopologyPath)) {
            screen.PostEvent(Event::Custom);
            return "could not save group alias";
        }
        refreshGroupDashboards(rt, topo, screen);
        return "";
    };
    rt.saveGroupAlias = saveGroupChannelAliasToTopology;

    auto jumpToGroup = [&rt](const std::string& groupName, int memberIndex) {
        if (rt.switcher.jumpToGroup)
            rt.switcher.jumpToGroup(groupName, memberIndex);
    };

    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences, bGlobalConnectAll, bGlobalDisconnectAll,
                getGroupMembership, saveGroupChannelAliasToTopology, jumpToGroup);

    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &currentTopologyPath, &screen, &running,
                               openSetup, bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences,
                               getGroupMembership, saveGroupChannelAliasToTopology, jumpToGroup]
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
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences,
                               getGroupMembership, saveGroupChannelAliasToTopology, jumpToGroup);
            topo = midSessionWiz->topo;
            currentTopologyPath = midSessionWiz->topologyPath;
            refreshGroupDashboards(rt, topo, screen);
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
    // Finds the BusWorker already driving `port` (same matching technique
    // applyNewBoardsLive uses to find an existing bus by port) and enqueues
    // the scan as a work item on that worker's own queue, reusing its
    // already-open PsbSerialBus — no second connection opened on a port a
    // thread may already be driving. Returns false (caller falls back to
    // direct-connect scan) when no live worker owns this port yet, e.g. a
    // bus just added in this wizard session but not yet Applied.
    auto scanViaLiveBus = [&rt](const std::string& port, int start, int end,
                               std::function<void(std::vector<psb::tui::DiscoveredBoard>, std::string)> onDone) -> bool {
        for (auto& bwPtr : rt.busWorkers) {
            psb::tui::BusWorker& bw = *bwPtr;
            bool matches = false;
            { std::lock_guard<std::mutex> lk(bw.workMutex);
              matches = !bw.boards.empty() && bw.boards.front()->portVal == port; }
            if (!matches) continue;

            { std::lock_guard<std::mutex> lk(bw.workMutex);
              bw.workQueue.push([worker = &bw, start, end, onDone] {
                  // Runs on worker's own thread, inside its queue-drain step
                  // (see runBusWorkerLoop) — normal polling of this bus's
                  // already-connected boards is paused for the duration,
                  // exactly as any other queued work item already pauses
                  // it. scanBus() never opens/closes the port itself
                  // (wizard_scan.h), so worker->bus's existing connection
                  // is reused as-is.
                  auto results = psb::tui::scanBus(worker->bus, start, end, [](int) {});
                  std::string status = results.empty()
                      ? "No boards found in range."
                      : std::to_string(results.size()) + " board(s) found.";
                  onDone(std::move(results), std::move(status));
              }); }
            bw.workCv.notify_one();
            return true;
        }
        return false;
    };

    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish,
                                                            /*allowScan=*/true, scanViaLiveBus, /*isLaunchFlow=*/false);

    // Snapshot of currently-connected boards for the group wizard's Add
    // Channel picker — the picker scope is deliberately restricted to
    // currently-connected boards, never every board ever defined in the
    // topology, connected or not. Called fresh every time the picker opens
    // (group_wizard_screen.h's bAddChannel), never cached, so a board that
    // connects/disconnects while the modal sits open is picked up on the
    // next open.
    auto getLiveGroupBoards = [&rt]() -> psb::tui::LiveBoards {
        psb::tui::LiveBoards result;
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) {
            if (!b->connected.load()) continue;
            psb::LiveBoardInfo info;
            info.nickname = b->nickname;
            info.numChannels = b->data.numChannels();
            result.push_back(std::move(info));
        }
        return result;
    };

    // Syncs main()'s live topo (and the path it's tracked against) from
    // whatever the group wizard ended up with — same "sync back on finish"
    // pattern as onMidSessionFinish's topo = midSessionWiz->topo. Without
    // this, later group alias edits would save an in-memory topology that
    // never learned about the wizard's already-saved changes.
    auto onGroupSetupFinish = [showGroupSetup, groupWiz, &rt, &topo, &currentTopologyPath, &screen] {
        topo = groupWiz->topo;
        currentTopologyPath = groupWiz->topologyPath;
        refreshGroupDashboards(rt, topo, screen);
        *showGroupSetup = false;
        screen.PostEvent(Event::Custom);
    };

    auto groupWizardRoot = psb::tui::makeGroupWizardScreen(*groupWiz, screen, onGroupSetupFinish, getLiveGroupBoards);

    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &topo, &screen, &running] {
        drainPendingRemovals(rt, topo, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get())
       | Modal(groupWizardRoot, showGroupSetup.get())
       | Modal(prefsDialog.root, showPreferences.get());

    screen.Loop(rootWithSetup);
    joinRuntime(rt, running);
    return 0;
}

# Board Removal — Synced Dashboard/Wizard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user remove a board from `psb_demo_tui` two ways — a `Remove` button on that board's own dashboard menu bar (immediate), or the Setup wizard's mid-session "Remove Board" followed by Apply (batched) — with both paths producing the identical live end state, closing the previously-known gap where mid-session wizard removal never actually took effect on the running session.

**Architecture:** A single shared function, `removeBoardLive()`, is the only thing either entry point calls. It uses a staged hand-off (the same shape as this codebase's existing scan-thread and hot-attach fixes): the board's own `BusWorker` thread does the actual detach from its `bw.boards` list via a queued work item (so nothing ever destroys an object the worker thread could still be touching), and the UI thread drains the completion signal once per frame before finishing cleanup — destroying the `BoardSession`, detaching its tab from the switcher, and (if that was the bus's last board) tearing the bus's thread down too.

**Tech Stack:** C++17, FTXUI (vendored, `Detach()`/`ChildAt()`/`ChildCount()` on `ComponentBase` — confirmed present), the existing `BusWorker`/`BoardSession`/`Runtime`/`BoardSwitcher` machinery from Phase 3.

## Global Constraints

- **Empty-bus teardown**: removing the last board on a bus disconnects the port and joins that bus's worker thread — it does not stay alive idling.
- **No auto-save**: removal only updates the live session and `topo`; the topology file is untouched until the user explicitly clicks Save — matches every other mutation in this app.
- **No confirmation prompt**: Remove acts immediately, matching Remove Bus/Remove Board in the wizard today.
- **UI placement**: the dashboard's `Remove` button goes into today's existing single-level menu bar now (a later, separate redesign will relocate it into a two-level menu — this plan does not touch that layout).
- **Same function, both entry points**: the dashboard's `Remove` button and the wizard's mid-session Remove-then-Apply must both call `removeBoardLive()` — never two separate implementations that could drift.
- **No board is ever touched by more than one thread at a time.** The staged hand-off (queued work item → completion flag → UI-thread drain) is what guarantees this; do not shortcut it with a direct cross-thread erase or a blocking wait on the worker thread from the UI thread for the board-removal step itself (a bounded, brief join is acceptable only for the already-idle bus-teardown step — see Task 4).
- **Removing every board down to zero must not crash or leave a dangling thread** — no dedicated empty-state UI is required.
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, ideally against real hardware if available (`/dev/ttyACM0-3` had 3 real boards during this project's Phase 3 live-hardware pass).
- This plan is Sub-project A of a three-part effort (see `docs/superpowers/specs/2026-07-21-board-removal-sync-design.md`'s Context section). Sub-projects B (single/multi-board menu architecture) and C (CLI-args-to-settings-dialog) are explicitly out of scope here.

---

## Task 1: `BusWorker` gains a per-bus stop flag

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h` (the `BusWorker` struct)
- Modify: `tools/psb_demo_app/tui/main.cpp` (`runBusWorkerLoop`'s loop condition)

**Interfaces:**
- Produces: `BusWorker::stopRequested` (`std::atomic<bool>`, default `false`) — set by the UI thread once a bus's board list is confirmed empty (Task 4), read by `runBusWorkerLoop`'s own loop condition so exactly that one bus's thread exits cleanly without affecting any other bus's thread (today all bus threads share one global `running` flag with no per-bus granularity).

- [ ] **Step 1: Add the field**

Edit `tools/psb_demo_app/tui/board_session.h`. Find the `BusWorker` struct:

```cpp
struct BusWorker {
    std::shared_ptr<PsbSerialBus> bus;
    std::vector<BoardSession*> boards;   // non-owning — BoardSessions live in main.cpp's board list
    std::queue<std::function<void()>> workQueue;
    std::mutex workMutex;
    std::condition_variable workCv;
    std::thread thread;
};
```

Change it to:

```cpp
struct BusWorker {
    std::shared_ptr<PsbSerialBus> bus;
    std::vector<BoardSession*> boards;   // non-owning — BoardSessions live in main.cpp's board list
    std::queue<std::function<void()>> workQueue;
    std::mutex workMutex;
    std::condition_variable workCv;
    std::thread thread;
    // Set (only ever by the UI thread, only once this bus's `boards` is
    // confirmed empty — see main.cpp's drainPendingRemovals) to stop this
    // one bus's worker thread without touching any other bus's thread.
    // Every bus thread today shares one global `running` flag with no
    // per-bus granularity; this is what removing the last board on a bus
    // needs that `running` alone can't provide.
    std::atomic<bool> stopRequested{false};
};
```

- [ ] **Step 2: Update the loop condition**

Edit `tools/psb_demo_app/tui/main.cpp`. Find:

```cpp
void runBusWorkerLoop(psb::tui::BusWorker& bw, ScreenInteractive& screen, std::atomic<bool>& running) {
    while (running) {
```

Change to:

```cpp
void runBusWorkerLoop(psb::tui::BusWorker& bw, ScreenInteractive& screen, std::atomic<bool>& running) {
    while (running && !bw.stopRequested) {
```

- [ ] **Step 3: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -30`
Expected: clean build. This step alone has no observable behavior change yet — `stopRequested` is never set anywhere until Task 4 — this is purely additive plumbing.

- [ ] **Step 4: Confirm existing tests unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -10`
Expected: PASS, same count as before this task (this task touches no `psb_modbus_core` files).

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_session.h tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): add BusWorker::stopRequested for per-bus thread teardown"
```

---

## Task 2: `board_switcher.h` — `BoardSwitcher` gains `detachBoard`

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Consumes: `ftxui::ComponentBase::Detach()`/`ChildAt()` (confirmed present in the vendored FTXUI, `component_base.hpp`).
- Produces: `BoardSwitcher::detachBoard` — `std::function<void(const std::string& nickname)>`, symmetric to the existing `attachBoard`. Removes the matching nickname from `boardNames`, detaches that board's dashboard `Component` from `dashboardStack` (`Container::Tab`), and adjusts `activeBoard`/`mainSelected` so neither points at a stale or now-invisible slot. Task 4 calls this from `drainPendingRemovals`.

- [ ] **Step 1: Add `<algorithm>` for `std::max`**

Edit `tools/psb_demo_app/tui/board_switcher.h`. Find the include block:

```cpp
#include <functional>
#include <memory>
#include <string>
#include <vector>
```

Change to:

```cpp
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
```

- [ ] **Step 2: Add `detachBoard` to the `BoardSwitcher` struct**

Find:

```cpp
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
};
```

Change to:

```cpp
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
    std::function<void(const std::string& nickname)> detachBoard;
};
```

- [ ] **Step 3: Implement `detachBoard`**

Find the existing `attachBoard` lambda and the `return` statement right after it:

```cpp
    auto attachBoard = [boardNames, dashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        dashboardStack->Add(std::move(dashboard));
    };

    return BoardSwitcher{root, attachBoard};
```

Change to:

```cpp
    auto attachBoard = [boardNames, dashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        dashboardStack->Add(std::move(dashboard));
    };

    // Symmetric to attachBoard. FTXUI's Container::Tab indexes its active
    // child via `*selector_ % children_.size()` (confirmed in the vendored
    // source, container.cpp) — not a clamp, a modulo — so after an erase,
    // activeBoard needs explicit adjustment or it can silently jump to an
    // unrelated board rather than either staying on the same one or landing
    // on a sensible neighbor. The two adjustments below cover every case:
    // if the removed tab was before the active one, decrement to keep
    // tracking the same logical board (which just shifted down one index);
    // then clamp into the new valid range (handles the removed tab being
    // the active one, especially if it was also the last slot).
    //
    // mainSelected also needs attention: shrinking to <=1 board makes the
    // switcher bar invisible again (see root's Renderer below), and if
    // mainSelected were still pointing at switcherBar (index 0), keyboard
    // input would land on an invisible single-entry Menu — the exact class
    // of bug already found and fixed for single-board startup (see
    // mainSelected's own comment above). Forcing it back to dashboardStack
    // (index 1) whenever the board count drops to <=1 prevents recreating
    // that regression via removal instead of via startup.
    auto detachBoard = [boardNames, dashboardStack, activeBoard, mainSelected](const std::string& nickname) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            dashboardStack->ChildAt(i)->Detach();
            boardNames->erase(boardNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*activeBoard > removedIdx) --*activeBoard;
            if (*activeBoard >= static_cast<int>(boardNames->size()))
                *activeBoard = std::max(0, static_cast<int>(boardNames->size()) - 1);
            if (boardNames->size() <= 1) *mainSelected = 1;
            return;
        }
    };

    return BoardSwitcher{root, attachBoard, detachBoard};
```

- [ ] **Step 4: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -30`
Expected: clean build. `detachBoard` is unused until Task 4 wires it in — this step is a compile-only sanity check for the header itself.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add BoardSwitcher::detachBoard, symmetric to attachBoard"
```

---

## Task 3: `board_dashboard.h` — the `Remove` button

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Consumes: nothing new.
- Produces: `makeBoardDashboard` gains one new parameter, `std::function<void()> requestRemove`, threaded exactly like the existing `openSetup` parameter. A new `Remove` button in the menu bar calls it. Task 4's `main.cpp` call sites supply the actual closure (bound to each specific board).

- [ ] **Step 1: Add the parameter**

Edit `tools/psb_demo_app/tui/board_dashboard.h`. Find:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup) {
```

Change to:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove) {
```

- [ ] **Step 2: Add the button and put it in the menu bar's Component tree**

Find:

```cpp
    auto bQuit = ActionButton("Quit", [&running, &busWorker, &screen] {
        running = false; busWorker.workCv.notify_all(); screen.ExitLoopClosure()();
    });
```

Change to:

```cpp
    auto bQuit = ActionButton("Quit", [&running, &busWorker, &screen] {
        running = false; busWorker.workCv.notify_all(); screen.ExitLoopClosure()();
    });

    // Always available (not gated on connection state) — the exact case
    // this exists for is a stale board that can never connect, which
    // otherwise has no way to leave the topology short of a restart. No
    // confirmation prompt, matching Remove Bus/Remove Board in the wizard
    // (Global Constraints) — reversible via Add.
    auto bRemove = ActionButton("Remove", [requestRemove] { requestRemove(); });
```

Find:

```cpp
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bQuit});
```

Change to:

```cpp
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bRemove, bQuit});
```

- [ ] **Step 3: Render it and add it to the Renderer's capture list**

Find:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bQuit, tabBar, tabContent, bSysCfg, bOpenSetup] {
```

Change to:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove, bQuit, tabBar, tabContent, bSysCfg, bOpenSetup] {
```

Find:

```cpp
            bConnToggle->Render(),
            text(" "),
            bQuit->Render(),
        });
```

Change to:

```cpp
            bConnToggle->Render(),
            text(" "),
            bRemove->Render(),
            text(" "),
            bQuit->Render(),
        });
```

- [ ] **Step 4: Build — expected to fail (compile-only sanity check for this file in isolation)**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — `makeBoardDashboard`'s two call sites in `main.cpp` don't pass the new `requestRemove` argument yet. This is expected; Task 4 updates both call sites. Confirm the *only* errors are about the missing argument at the two `makeBoardDashboard(...)` call sites in `main.cpp` — if there's any other error, it's a real mistake in this task's edit, not an expected one.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): add Remove button to the board dashboard menu bar (not yet wired to main.cpp)"
```

---

## Task 4: `main.cpp` — `removeBoardLive`, the staged removal drain, and wiring the dashboard button

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `BusWorker::stopRequested` (Task 1), `BoardSwitcher::detachBoard` (Task 2), `makeBoardDashboard`'s new `requestRemove` parameter (Task 3).
- Produces: `Runtime::pendingRemovals`, `removeBoardLive(Runtime&, ScreenInteractive&, std::atomic<bool>&, psb::tui::BoardSession*)`, `drainPendingRemovals(Runtime&, ScreenInteractive&, std::atomic<bool>&)`. Task 5 reuses `removeBoardLive` for the wizard-side sync.

- [ ] **Step 1: Add `PendingRemoval` and `Runtime::pendingRemovals`**

Find:

```cpp
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
};
```

Change to:

```cpp
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
```

- [ ] **Step 2: Add `removeBoardLive` and `drainPendingRemovals`**

These need to go after `runBusWorkerLoop` (they reference `psb::tui::BusWorker`/`BoardSession` and — for `drainPendingRemovals` — `psb::tui::BoardSwitcher::detachBoard`) and before `buildRuntime` (which will call `removeBoardLive` from a lambda in Step 3).

Find the end of `runBusWorkerLoop`:

```cpp
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
```

Immediately after that closing `}`, add:

```cpp

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
// Renderer, Step 4 below). For each entry whose worker-thread detach has
// finished: detaches from the switcher, destroys the BoardSession (only
// safe now — the owning BusWorker thread has confirmed it will never touch
// this pointer again), and — if that was the bus's last board — tears the
// bus down too (Global Constraints: empty-bus teardown).
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
void drainPendingRemovals(Runtime& rt, ScreenInteractive& screen, std::atomic<bool>& running) {
    for (size_t i = 0; i < rt.pendingRemovals.size(); ) {
        auto& pr = rt.pendingRemovals[i];
        if (!pr.done->load()) { ++i; continue; }

        rt.switcher.detachBoard(pr.board->nickname);

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
```

- [ ] **Step 3: Wire `requestRemove` into both `makeBoardDashboard` call sites**

Find, in `buildRuntime`:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup);
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); });
```

Find, in `applyNewBoardsLive`:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup);
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); });
```

(`rt`, `screen`, and `running` are already parameters/captures in scope at both call sites — `buildRuntime` and `applyNewBoardsLive` both already take `Runtime& rt`, `ScreenInteractive& screen`, and `std::atomic<bool>& running`.)

- [ ] **Step 4: Drain pending removals once per frame**

Find, in `main()`:

```cpp
    auto rootWithSetup = Renderer(rt.switcher.root, [&rt] { return rt.switcher.root->Render(); })
        | Modal(midSessionWizardRoot, showSetup.get());
```

Change to:

```cpp
    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &screen, &running] {
        drainPendingRemovals(rt, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get());
```

- [ ] **Step 5: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 6: Confirm tests unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -10`
Expected: PASS, same count as before this task.

- [ ] **Step 7: Manual verification via tmux — the dashboard Remove button works end-to-end**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, against real hardware if available (a multi-drop bus with 2+ real boards was available during this project's Phase 3 live-hardware pass — reuse it if still attached):

1. Launch with a topology that has 2+ boards on the same bus (one can be a fake/stale port entry that will fail to connect — this is the exact bugfix scenario). Confirm both boards' dashboards are reachable via the switcher.
2. On the stale/unreachable board's dashboard, click `Remove`. Confirm: the switcher bar loses that board's tab, the remaining board's dashboard is undisturbed (uptime, if connected, keeps advancing throughout), and if the remaining board count is now 1, the switcher bar itself disappears (matching the pixel-identical single-board constraint) and keyboard input still reaches the remaining dashboard immediately (no repeat of the Task-4-Phase-3 focus regression — confirm by sending a keypress like Right/Enter into the toolbar right after the switcher bar vanishes, with no extra Tab/Down needed first).
3. If a second real bus/port is available: remove the *last* board on a bus, and confirm that bus's worker thread actually exits — `ps -T -p <pid> | wc -l` (or equivalent thread-count check) before and after, expecting the count to drop by one; also confirm no error/hang.
4. Remove every board down to zero (if practical) — confirm no crash, and `pgrep -af psb_demo_tui` before Quit still shows exactly one process (no runaway/zombie threads).
5. Quit cleanly after at least one removal — confirm the process exits and `pgrep -x psb_demo_tui` shows nothing afterward.

- [ ] **Step 8: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire the dashboard Remove button to a live, thread-safe removal"
```

---

## Task 5: Wizard-side sync — mid-session Remove Board actually takes effect

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `removeBoardLive` (Task 4).
- Produces: `removeGoneBoardsLive(Runtime&, const psb::TopologyConfig&, ScreenInteractive&, std::atomic<bool>&)`, wired into `onMidSessionFinish`.

- [ ] **Step 1: Add `removeGoneBoardsLive`**

This is `applyNewBoardsLive`'s mirror image — find every board currently live in `rt` but absent from the wizard's edited topology (matched by nickname; nicknames are globally unique across the whole topology, enforced by `wizard_state.h`'s `addBoard`), and remove each one via `removeBoardLive`.

Find the end of `applyNewBoardsLive`:

```cpp
            rt.switcher.attachBoard(b->nickname, b->dashboard);
            // Same class of race as BusWorker.boards above, this time
            // against animThread (live for the program's whole runtime) —
            // see Runtime::boardsMutex's comment.
            { std::lock_guard<std::mutex> lk(rt.boardsMutex);
              rt.boards.push_back(std::move(b)); }
        }
    }
}
```

Immediately after that closing `}`, add:

```cpp

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
```

- [ ] **Step 2: Call it from `onMidSessionFinish`, and correct its stale comment**

Find:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, timeoutArg, openSetup]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, timeoutArg, openSetup);
            // applyNewBoardsLive is strictly additive (Global Constraints) —
            // if the wizard's in-memory edits also removed a board that's
            // still running in rt, this overwrite makes topo stop
            // describing what's actually connected (a later Save would
            // just omit that board, which is likely what the user wanted,
            // but it's worth knowing this doesn't reconcile the two). Not a
            // bug: live removal was deliberately out of scope for this
            // task — see applyNewBoardsLive's own scope-note comment.
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

Change to:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, timeoutArg, openSetup]
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
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, timeoutArg, openSetup);
            topo = midSessionWiz->topo;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

- [ ] **Step 3: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 4: Confirm tests unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -10`
Expected: PASS, same count as before this task.

- [ ] **Step 5: Manual verification via tmux — wizard-side removal matches the dashboard button**

1. Launch with 2+ boards on the same bus, all connected. Click `Setup` from any board's dashboard (mid-session entry point). In the wizard, select the board to remove and click `Remove Board`, then `Apply`.
2. Confirm the end state matches Task 4 Step 7's dashboard-button scenario exactly: switcher tab gone, remaining board(s) undisturbed, switcher bar hidden if down to one board with keyboard input reaching the dashboard immediately.
3. If that was the last board on its bus, confirm the bus's thread actually exited (same thread-count check as Task 4 Step 7.3).
4. Quit cleanly afterward; confirm no leftover process/threads.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): mid-session Remove Board now actually removes the live board

Closes a known gap: applyNewBoardsLive's strictly-additive design
meant a removal made in the wizard's in-memory edit never took effect
on the live session even after clicking Apply — the board kept
running while topo silently stopped describing it. removeGoneBoardsLive
mirrors applyNewBoardsLive, diffing the wizard's edited topology
against what's actually live and calling the same removeBoardLive()
the dashboard's Remove button uses, so both paths now produce the
identical end state."
```

---

## Task 6: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -10`
Expected: PASS, same count as the pre-Task-1 baseline (this plan adds no new automated tests — every new function here is deeply thread/FTXUI-coupled, matching Phase 3's own precedent of verifying that class of code via tmux rather than Catch2; see this plan's Global Constraints and each task's manual-verification step).

- [ ] **Step 3: Re-run the full manual verification sequence once, end to end**

Repeat Task 4 Step 7 and Task 5 Step 5 back to back in one continuous session (don't restart the app between them) — add two boards, remove one via the dashboard button, remove another via the wizard's mid-session Remove Board, confirm both produce consistent behavior and the app remains stable throughout (no crash, no hang, uptime/data on any remaining board undisturbed by either removal).

- [ ] **Step 4: Clean up any temporary topology files created during manual verification**

Run: `rm -f` on any scratch topology paths used above (e.g. under `/tmp`).

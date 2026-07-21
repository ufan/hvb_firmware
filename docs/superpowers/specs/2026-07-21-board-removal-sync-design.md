# Board Removal — Synced Dashboard/Wizard Design

## Context

This is Sub-project A of a three-part follow-on to Phase 3 (the TUI interactive
setup wizard, `docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`
and `docs/superpowers/plans/2026-07-20-multi-board-topology-phase3-tui-wizard.md`).
The user reported and requested, all against `psb_demo_tui`:

- **Bugfix 1** (this spec): a stale/unreachable board in the topology can't be
  connected, and there's no way to remove it directly from the running
  dashboard — only from the Setup wizard, and even there, removing a board
  mid-session doesn't actually take effect on the live session (see
  "Existing gap" below). The two removal paths need to exist, work, and stay
  in sync: one distributed (a button on the board's own dashboard), one
  centralized (Remove Board in the Setup wizard).
- **Bugfix 2** (single/multi-board UI boundary) and **TUI optimizations 1–4**
  (two-level menu bar, single-mode menu collapse, CLI-args-to-settings-dialog,
  startup mode-selection popup + runtime mode switching) are **out of scope
  for this spec** — they are Sub-projects B and C, to be brainstormed
  separately. Sub-project B will relocate the Remove button this spec adds
  into a proper board-level menu once the two-level layout exists; the
  underlying removal mechanism this spec builds does not change.

## Goal

Let a user remove a board — live, from an already-running dashboard, or via
the Setup wizard's mid-session Remove Board — with both paths producing the
identical end state: the board disconnected, its thread-safety invariants
preserved (no board is ever touched by more than one thread at a time), its
bus torn down if it was the last board on that bus, and the in-memory
topology (`main()`'s `topo`) reflecting the removal so a subsequent Save or
Setup reopen is accurate.

## Existing Gap

`applyNewBoardsLive` (Task 7 of Phase 3) is strictly additive by design — the
mid-session wizard already lets a user click "Remove Board" against its
in-memory `WizardState.topo`, but on "Apply", only newly-added boards are
actually attached to the live `Runtime`; a removal in that in-memory edit is
silently dropped (the live board keeps running) while `topo` gets overwritten
to no longer mention it — a real divergence between what's tracked and what's
running, previously flagged as a known Minor finding. This spec closes that
gap as part of making the two removal paths equivalent.

## Confirmed Decisions

- **Empty-bus teardown**: removing the last board on a bus tears the bus down
  too — disconnects the port and joins the worker thread, rather than leaving
  an idle thread polling nothing.
- **No auto-save**: removal only updates the live session and `topo`;
  persisting to the topology file still requires the existing, separate Save
  action — consistent with every other mutation in this app (Add Bus/Add
  Board/Remove Bus already work this way).
- **No confirmation prompt**: Remove acts immediately, consistent with
  Remove Bus/Remove Board in the wizard today. Reversible via Add.
- **UI placement**: the dashboard's Remove button ships now in today's
  existing single-level menu bar (next to Connect/Disconnect) — a pragmatic
  placement. Sub-project B's two-level menu redesign will relocate it into
  the board-level menu later; only its position changes, not its behavior or
  underlying mechanism.

## Architecture

### `BusWorker` (`board_session.h`) — one new field

```cpp
std::atomic<bool> stopRequested{false};
```

`runBusWorkerLoop`'s loop condition becomes `while (running && !bw.stopRequested)`.
Today, every bus thread shares one global `running` flag with no per-bus
granularity — this is what lets exactly one bus's thread stop without
touching any other bus's thread, which keeps running normally.

### `board_switcher.h` — `BoardSwitcher` gains `detachBoard`

Symmetric to the existing `attachBoard`. Removes the matching nickname from
`boardNames` and calls `Detach()` — a real `ftxui::ComponentBase` method,
confirmed present in the vendored FTXUI (`component_base.hpp:46`) — on that
board's dashboard `Component`, unlinking it from the switcher's
`Container::Tab`. Clamps `activeBoard` if the removed tab was at or before
the currently active index (the same class of concern as the earlier
`MenuBase::Clamp()` bug found in Phase 3, this time for a shrinking list
rather than a growing one).

### `main.cpp` — `removeBoardLive`, the single shared removal function

```cpp
void removeBoardLive(Runtime& rt, ScreenInteractive& screen,
                     std::atomic<bool>& running, BoardSession* board);
```

Both removal entry points (the dashboard's Remove button, and the wizard's
mid-session Remove-then-Apply) call this exact function — this is what makes
"the same effect, synced" literally true rather than two parallel
implementations that could drift.

### `main.cpp` — `Runtime::pendingRemovals`

```cpp
struct PendingRemoval {
    BusWorker* bw;
    BoardSession* board;
    std::shared_ptr<std::atomic<bool>> done;
};
std::vector<PendingRemoval> pendingRemovals;   // new field on Runtime
```

### `board_dashboard.h` — one new `Remove` button

Added to the existing menu bar, wired to call `removeBoardLive` directly for
its own board (the distributed entry point).

### `main.cpp` — `removeGoneBoardsLive`, called from `onMidSessionFinish`

Symmetric counterpart to `applyNewBoardsLive`. Diffs the wizard's edited
topology against what's actually running in `rt`; for every board present
live but absent from the edit (by bus + nickname), calls `removeBoardLive`.
This is the centralized entry point, and what closes the "Existing Gap"
above.

## Data Flow — Single Removal, Step by Step

1. `removeBoardLive` finds the `BusWorker` owning `board` (scans `bw->boards`
   for the pointer) and enqueues a work item on that bus's `workQueue`:
   ```cpp
   { std::lock_guard<std::mutex> lk(bw->workMutex);
     // erase board's pointer from bw->boards
   }
   *done = true;
   screen.PostEvent(Event::Custom);
   ```
   and appends `{bw, board, done}` to `rt.pendingRemovals`.

2. **Why this needs no new synchronization on the worker side**:
   `runBusWorkerLoop` already takes a locked snapshot of `bw.boards` twice
   per cycle — once at the top, and again immediately after draining the
   work queue (added in Phase 3 Task 7 specifically so a hot-attached board
   is pollable the same cycle it's added). The removal work item runs during
   that same drain step, so the post-drain snapshot naturally excludes the
   removed board from that point on. If a poll for that board was already in
   flight from this cycle's *pre*-drain snapshot, it simply finishes
   normally — harmless, since the `BoardSession` object itself isn't
   destroyed yet at this point.

3. **Draining `pendingRemovals`** happens once per frame, at the top of
   `main.cpp`'s outer root `Renderer` — the same place/pattern as
   `wizard_screen.h`'s scan-drain and the existing
   `rebuildBusNames`/`rebuildBoardNames` calls. For each entry where `*done`
   is true:
   - Detach from the switcher: `rt.switcher.detachBoard(nickname)`.
   - Erase the `unique_ptr<BoardSession>` from `Runtime::boards` under the
     existing `boardsMutex` — only now is the object actually destroyed,
     after the worker thread has confirmed (via `*done`) it will never touch
     that pointer again.
   - If `bw->boards` is now empty: set `bw->stopRequested = true`, notify
     its condition variable, join its thread, disconnect its
     `PsbSerialBus`, and erase the `unique_ptr<BusWorker>` from
     `Runtime::busWorkers`.
   - Remove the drained entry from `pendingRemovals`.

## Wizard-Side Sync

`removeGoneBoardsLive`, called from `onMidSessionFinish` alongside the
existing `applyNewBoardsLive` call: walks every board currently live in
`rt`, and for each one not present (by bus + nickname) in the wizard's
edited topology, calls `removeBoardLive`. Since `topo = midSessionWiz->topo`
already happens synchronously right after in `onMidSessionFinish`, `topo` is
correct immediately — the live teardown just catches up a frame or two
later, the same lag `applyNewBoardsLive`'s async connect step already has
today.

## Error Handling

- `removeBoardLive` is a no-op if the board pointer isn't found in any
  `BusWorker` — defensive; shouldn't happen in practice since both callers
  only ever pass boards they can currently see are live.
- Double-clicking Remove on the dashboard is harmless: once a board is
  detached from the switcher (step 3 above), its dashboard/button no longer
  exists to be clicked again.

## Out of Scope / Known Limitation

Removing every board (down to zero) is not given any dedicated empty-state
UI by this spec — the switcher/dashboard will simply have nothing to show.
Defining what a "no boards configured" running state should look like is
naturally Sub-project B's concern (its mode-selection design already has to
handle "no boards yet" as a state). This spec only guarantees removing down
to zero doesn't crash or leave dangling threads — nothing more.

## Testing

This is deeply thread/FTXUI-dependent, matching every other piece of Phase
3's hot-attach work — there is no meaningful unit-test surface (consistent
with how `board_switcher.h` and hot-attach were verified). Verification is
tmux-driven, ideally against real hardware (a real multi-drop bus was
available during Phase 3's live-hardware pass):

- Remove on a board with live siblings leaves them undisturbed (uptime keeps
  advancing on the others throughout).
- Remove on the last board on a bus actually joins that bus's thread
  (`pgrep`/thread-count check before and after, matching the methodology
  Phase 3 already used).
- The wizard-side Remove-then-Apply path produces the identical end state
  (switcher tabs, `Runtime` contents, thread count) as the dashboard button,
  for the same board.
- Quit cleanly after a removal leaves no leftover process/threads.
- Removing every board down to zero doesn't crash and leaves no leftover
  threads (no dedicated empty-state UI expected — see "Out of Scope" above).

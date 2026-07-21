# Single/Multi-Board Mode Architecture — Design

## Context

This is Sub-project B of a three-part follow-on to Phase 3 (the TUI interactive
setup wizard) and Sub-project A (board removal, already merged — see
`docs/superpowers/specs/2026-07-21-board-removal-sync-design.md`). The user's
original request, decomposed during that spec's brainstorming session:

- **Bugfix 2**: no clear boundary between single- and multi-board usage in
  the UI layer — it's possible to reach "add another board" from a
  single-board launch, but the experience doesn't make sense as a boundary.
- **TUI optimizations 1, 2, 4**: a two-level menu bar in multi-board mode
  (global app actions vs. board-specific actions), the two levels collapsing
  into one merged row in single-board mode, and a dedicated startup
  mode-selection popup with runtime mode switching.

**TUI optimization 3** (migrating CLI args into a settings dialog) is
Sub-project C — explicitly out of scope here, though this design's startup
flow is written assuming C has already landed (no CLI arguments exist by the
time this ships in practice, or this design degrades gracefully if built
before C — see "Sequencing" below).

## Goal

Give the app an explicit, coherent notion of "mode" without inventing a new
piece of state to keep in sync: mode is *derived* from the current board
count, exactly the same principle the board switcher bar has already used
since Phase 2 (hidden below 2 boards, visible at 2+). Everything in this
design — the startup popup, the menu-bar split, the single-board collapse,
"runtime switching" — is built on that one idea.

## Confirmed Decisions

- **Mode is dynamic, not a stored flag.** Exactly 1 board = single-mode UI;
  2+ boards = multi-mode UI. No separate "mode" state to desync from reality.
- **The startup mode-selection popup shows on every launch** (once
  Sub-project C removes CLI args, there is no other signal to disambiguate
  what the user wants — every launch is effectively a fresh one). Two
  choices:
  - **Single mode**: a lightweight quick-connect form (port/baud/slave ID),
    pre-filled with the last-used values (a small stored preference,
    *distinct from any topology file*). No topology file is loaded or
    implicitly saved.
  - **Multi mode**: goes straight into the existing Setup wizard — Load an
    existing topology file, or build one from scratch (Add Bus/Add Board,
    including scan-assisted discovery).
- **Runtime switching (single → multi)**: the (now-global) `Setup` button
  opens the wizard pre-seeded with whatever's currently running — the exact
  mechanism the mid-session wizard already uses today, unchanged. Nothing
  about the currently-running board is lost; the user can Add Board/Add Bus
  and Save to finally persist a real topology file.
- **Menu split**: global (top-level, always exactly one instance) = `Quit`,
  `Setup`. Board-level (per-board, already exists) = operating-mode cycler,
  `Save`, `Connect`/`Disconnect`, `Remove` (already gated to 2+ boards per
  Sub-project A), `Setting`, and the read-only telemetry/status row.
- **Single-board collapse is a literal one-row visual merge** (not just
  "fewer separate rows") — achieved via **Approach 1**: `Quit`/`Setup`
  remain structurally owned by the switcher's own container (for keyboard
  focus) in every mode; the board's own dashboard Renderer *also* calls
  `Render()` on those same button objects when it detects single-board mode,
  folding their output into its own menu row. This is safe and
  well-precedented — FTXUI Components can be rendered from more than one
  call site (rendering is stateless per call); only *parenting* (which
  container a Component belongs to for focus purposes) is singular, and
  parenting never changes here.
- **The merge is visual-only, not a unified Tab-order.** `Quit`/`Setup` and
  the board's own row remain two separate keyboard focus groups even when
  visually adjacent on one line — accepted explicitly to avoid the
  re-parenting complexity a fully seamless Tab order would require (the
  exact class of FTXUI focus/parenting bug that has already bitten this
  codebase multiple times — see `board_switcher.h`'s own use-after-free and
  clamp-bug history).

## Sequencing Note

This design is written assuming Sub-project C (CLI args → settings dialog)
lands first or alongside it, since "every launch shows the popup" only makes
full sense once there's no `-p`/`--topology`/`--setup` to already answer the
question. If B ships before C, the popup should still show whenever none of
today's existing CLI-driven paths resolve a topology (i.e., replacing only
today's hardcoded `/dev/ttyUSB0` fallback) — every other existing entry path
(`-p`, an existing topology file, `--topology` naming a missing file,
`--setup`) keeps working exactly as it does today, untouched by this design.

## Architecture

### Startup flow (`main.cpp`)

A new pre-dashboard root, shown before any board/bus exists, offering two
buttons: "Single Board" and "Multi-Board Setup". This is structurally the
same kind of pre-dashboard `screen.Loop()` root the wizard's standalone entry
point (Phase 3, Task 6) already establishes — not a new pattern.

- **Single Board** chosen: opens a small quick-connect form — Port (dropdown
  via `PsbSerialBus::scanPorts()`, reusing the exact pattern
  `wizard_screen.h`'s Add Bus modal and the dashboard's own connection modal
  already use), Baud, Slave ID — pre-filled from a small stored preference
  file. Reuses the existing `TopologyConfig`/`BusConfig`/`BoardConfig`
  structures and their toml++-backed load/save (zero new serialization code)
  but saves to a **separate** path, e.g. `~/.psb_demo_app/last_single.toml`,
  distinct from `TopologyConfig::defaultPath()` (which stays reserved for a
  real, user-managed multi-board topology). Confirming builds a
  single-board `TopologyConfig` in memory (matching what `-p` already
  produces today via `TopologyConfig::singleBoard(...)`) and proceeds
  straight to the dashboard — no topology file is loaded, and this
  preference file is saved only, never treated as "the" topology.
- **Multi-Board Setup** chosen: launches the existing standalone wizard
  root unchanged (`makeWizardScreen`, `allowScan=true`), pre-targeting
  `TopologyConfig::defaultPath()` exactly as `--setup` does today.

### `main.cpp` — global buttons constructed once

```cpp
std::function<void()> openSetup = /* existing, unchanged */;

auto bGlobalQuit = ActionButton("Quit", [&running, &rt, &screen] {
    running = false;
    for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
    screen.ExitLoopClosure()();
});
auto bGlobalSetup = ActionButton("Setup", [openSetup] { openSetup(); });
```

`Quit`'s new implementation notifies every bus's worker (not just one
board's), a small correctness improvement over today's per-board version,
which only wakes the specific bus the clicked button happened to belong to
— other buses' threads would otherwise wait out their idle poll interval
before noticing `running` went false.

Both are threaded into `buildRuntime`, `applyNewBoardsLive`, and
`makeBoardSwitcher` as new parameters, the same way `openSetup`/
`requestRemove` (Sub-project A) already are.

### `board_switcher.h`

`makeBoardSwitcher` gains `globalQuit`/`globalSetup` parameters, wraps them
in a `globalMenuBar` container added as a **third** sibling in the
switcher's own tree:

```cpp
auto globalMenuBar = Container::Horizontal({globalQuit, globalSetup});
auto mainContainer = Container::Vertical(
    {globalMenuBar, switcherBar, dashboardStack}, mainSelected.get());
```

`mainSelected`'s indexing shifts from 2 slots to 3
(`0=globalMenuBar, 1=switcherBar, 2=dashboardStack`), but its *default and
clamped value stays pointed at `dashboardStack`* in every case that today
points at "the dashboard" — this is a straightforward re-index, not a
behavior change; it preserves the existing "keys reach the dashboard
immediately" principle Task 4 already established, just extended to three
slots instead of two. The Renderer renders `globalMenuBar`'s row only when
`boardNames->size() > 1`.

### `board_dashboard.h`

`makeBoardDashboard` gains `globalQuit`/`globalSetup` (render-only — never
added to its own `Container`, so no re-parenting, no focus-ownership
change) plus a live board-count accessor:

```cpp
std::function<size_t()> liveBoardCount  // e.g. [&rt] { return rt.boards.size(); }
```

Its own locally-constructed `bQuit`/`bOpenSetup` are removed entirely — the
file no longer builds its own Quit/Setup buttons at all. When
`liveBoardCount() <= 1`, its Renderer calls `globalQuit->Render()`/
`globalSetup->Render()` and folds their output into its own `menuBarEl`
`hbox`, alongside the board's own `Connect`/`Save`/mode-cycler. When
`liveBoardCount() > 1`, it omits them (the switcher's own `globalMenuBar`
row renders them instead, one level up).

`Remove` continues to render only when board count > 1 — already true from
Sub-project A, now doubly consistent with the mode-boundary principle this
design formalizes.

## Data Flow Summary

1. App launches → mode-selection popup (unless an unambiguous existing
   CLI-driven path resolves first, per "Sequencing Note").
2. **Single Board** → quick-connect form (pre-filled) → in-memory
   single-board `TopologyConfig`, preference file saved → dashboard.
   **Multi-Board Setup** → standalone wizard (`allowScan=true`) →
   dashboard, exactly as `--setup`/case-3 already work today.
3. Dashboard renders with `liveBoardCount()` driving both the switcher
   bar's visibility (unchanged, existing behavior) and now also the
   menu-bar layout (new): 1 board → merged single row; 2+ boards →
   separate global row + per-board row + switcher bar.
4. **Runtime switching**: clicking the (now-global, always-reachable)
   `Setup` button opens the mid-session wizard pre-seeded with the live
   topology — identical mechanism to today's mid-session Setup, just no
   longer gated behind "you already have 2+ boards" since it's a global
   button now. Adding a board via that path crosses the 1→2 threshold,
   and the *very next render* picks up the new `liveBoardCount()` and
   switches menu-bar layout automatically — no special-cased transition
   code needed, matching the render-time-decision principle already used
   throughout this codebase.

## Error Handling

- Quick-connect form validation (port required, baud/slave numeric ranges)
  reuses the same validation already present in the dashboard's own
  connection modal and the wizard's Add Bus modal — no new rules invented.
- If the single-mode preference file (`~/.psb_demo_app/last_single.toml`)
  doesn't exist yet (first-ever launch) or fails to parse, the quick-connect
  form simply starts blank — not an error, not surfaced to the user.

## Testing

Consistent with this codebase's established practice for FTXUI/threading-
coupled code (Phase 3, Sub-project A): no meaningful unit-test surface:
verification is tmux-driven, ideally against real hardware:

- Fresh launch (no CLI args, or with Sub-project C not yet landed, no
  resolvable existing path) shows the popup; both choices lead to a working
  dashboard.
- Single-board dashboard: menu row is genuinely one line containing both
  global (`Quit`/`Setup`) and board-level controls; `Remove` is absent.
- Multi-board dashboard (2+ boards): global row, switcher bar, and each
  board's own row all render as separate rows; `Remove` present.
- Crossing 1→2 (via Setup → Add Board) live-transitions the menu layout on
  the next render, with no visual glitch and no disruption to the
  already-running board(s) — same non-disruption bar Sub-project A's
  live-hardware pass already established for board add/remove.
- Crossing 2→1 (via Remove) live-transitions back to the merged single row,
  same non-disruption bar.
- Quick-connect form's pre-fill: connect once, quit, relaunch, choose Single
  Board again — form shows the previous values.
- Global `Quit` correctly notifies and joins every bus's worker thread, not
  just one — verified via the same `/proc/<pid>/status` thread-count
  methodology Sub-project A already used.

## Out of Scope

- Sub-project C (CLI args → settings dialog) — referenced here for
  sequencing only.
- Fully seamless single-row Tab order (explicitly decided against — see
  Confirmed Decisions).
- Any persisted "mode" flag — deliberately not built; mode is always
  derived live from board count.

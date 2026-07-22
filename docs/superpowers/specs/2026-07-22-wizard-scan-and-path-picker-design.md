# Mid-Session Scan Routing & Topology Path Picker — Design

## Context

Follow-on polish to `psb_demo_tui`'s Topology wizard, requested as a batch of
five items. Three were small enough to resolve and ship directly during this
same conversation, without a full plan:

- **Enlarge the wizard dialog** — it had no size floor at all
  (`| border` with no `size(...)`), rendering more compactly than the
  terminal allowed. Fixed: `size(WIDTH, GREATER_THAN, 100)` /
  `size(HEIGHT, GREATER_THAN, 30)` floors on the outer dialog.
- **Center the mode-select popup** — it was missing `| center` entirely, so
  an unconstrained bordered box rendered pinned to the terminal's top-left
  corner (looking like it "spans the left edge vertically"). One-line fix.
- **The empty-board fallback UI** (removing every board leaves a bare
  "Empty container" message) — **explicitly deferred**, to be its own
  dedicated task later. Nothing in this spec touches it.

Two more bugs surfaced while discussing the above and got fixed directly,
also already shipped:

- **Quick-connect never actually connected.** `autoConnectAll` was
  unconditionally false for the single-board quick-connect path — a gap
  left over from Sub-project C removing the old `-p`-driven auto-connect
  logic without carrying it forward for quick-connect's own board. Every
  surviving path to that point now represents deliberate connect intent
  (wizard Connect Now/Save & Exit, or quick-connect's own Connect button),
  so `autoConnectAll` is now unconditionally `true`.
- **Dangling boardless bus.** `drainPendingRemovals` erased a removed
  board from `topo.buses[i].boards` but never removed the bus entry
  itself when that left it empty, so a bus's last board being removed via
  the dashboard's Remove button left a dead entry behind in the Topology
  wizard's Buses panel. Fixed to mirror the existing `busEmpty` teardown
  already applied to `rt.busWorkers` in the same function. The wizard's
  own "Remove Board" stays deliberately separate from "Remove Bus" (an
  explicit two-step action there, unchanged) — only the live-session
  removal path was affected.

**This spec covers the two items that still need a real implementation
plan:**

1. **Mid-session bus scan** — currently disabled (`allowScan=false`)
   whenever the Topology wizard is reopened mid-session via the global
   button, because scanning opens a *second* serial connection on a port a
   `BusWorker` thread might already be driving. Needs a way to scan safely
   when a live connection already exists.
2. **Topology path file picker** — the wizard's Path field is
   manually-typed text only; needs a mouse-driven directory browser.

## Confirmed Decisions

- **Scan routing**: when the bus being scanned already has a live
  `BusWorker` (part of the currently-running session), the scan is
  enqueued as a work item on that worker's own queue, reusing its
  already-open `PsbSerialBus` — no second connection. If the bus has no
  live worker (a bus just added in this wizard session but not yet
  applied, or the standalone pre-dashboard entry point where nothing is
  running yet), scan falls back to today's direct-connect approach
  unchanged.
- **Accepted tradeoff**: while a live-bus scan's work item runs, that
  bus's normal polling of already-connected boards pauses (work items
  always drain before polling, per `runBusWorkerLoop`'s existing
  structure) — telemetry on sibling boards sharing that bus freezes for
  the scan's duration (bounded: ~200ms × range width, e.g. ~6s worst-case
  for the default 1–32 range).
- **File picker scope**: full directory navigation (not just a fixed
  listing of `~/.psb_demo_app/`) — `..` to go up, click a subdirectory to
  descend, click a `.toml` file to select it. Starts browsing at the Path
  field's current parent directory, or `~/.psb_demo_app/` if that's empty
  or invalid.
- **Selecting a file both sets the path and loads it immediately**
  (reusing the wizard's existing load logic) — a file picker's purpose is
  opening the file, not just typing its name for you.

## Architecture

### Item 1: `wizard_screen.h` — scan routing parameter

`makeWizardScreen` gains a new parameter:

```cpp
using ScanViaLiveBus = std::function<bool(const std::string& port, int start, int end,
                                          std::function<void(std::vector<DiscoveredBoard>, std::string)> onDone)>;

inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true,
                                  ScanViaLiveBus scanViaLiveBus = {});
```

`bStartScan`'s handler tries `scanViaLiveBus` first (if non-empty): if it
returns `true`, a live worker took the scan — it runs asynchronously and
reports back through `onDone`, which stages results through the exact same
`ScanUpdate`/`scanMutex`/`scanUpdateReady` hand-off already used for the
direct-connect path (this mechanism was already built for "a background
thread stages results, the UI thread drains them per frame" — reusing it
here is the whole reason no new synchronization primitive is needed). If
`scanViaLiveBus` returns `false` (or is empty), scan falls back to today's
`std::thread(...).detach()` direct-connect path, unchanged.

The **standalone pre-dashboard entry point** (`main.cpp`'s `runWizard`
block) passes no `scanViaLiveBus` — there's no `Runtime` yet at that point,
so every scan there is necessarily direct-connect, exactly as today.

### `main.cpp` — the live-routing implementation

Constructed once, alongside `openSetup`, and passed into the mid-session
`makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish, /*allowScan=*/true, scanViaLiveBus)`
call (note: `allowScan` also flips to `true` here now that scan is safe
mid-session):

```cpp
auto scanViaLiveBus = [&rt](const std::string& port, int start, int end,
                            std::function<void(std::vector<psb::tui::DiscoveredBoard>, std::string)> onDone) -> bool {
    for (auto& bwPtr : rt.busWorkers) {
        psb::tui::BusWorker& bw = *bwPtr;
        bool matches = false;
        { std::lock_guard<std::mutex> lk(bw.workMutex);
          matches = !bw.boards.empty() && bw.boards.front()->portVal == port; }
        if (!matches) continue;

        { std::lock_guard<std::mutex> lk(bw.workMutex);
          bw.workQueue.push([bwPtr = &bw, start, end, onDone] {
              auto results = psb::tui::scanBus(bwPtr->bus, start, end, [](int) {});
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
```

This reuses `scanBus`'s existing contract unchanged (`wizard_scan.h`: takes
a pre-connected `shared_ptr<PsbSerialBus>`, never opens/closes the port) —
`bw.bus` is already open, so no new connection is made at all. The matching
logic (find the `BusWorker` whose first board's port equals the target)
mirrors the same technique `applyNewBoardsLive` already uses to find an
existing bus by port.

`onDone` runs *on the `BusWorker`'s own thread* (it's invoked from inside a
queued work item, during `runBusWorkerLoop`'s queue-drain step) — this is
exactly the class of cross-thread hand-off `scanStaged`/`scanMutex`/
`scanUpdateReady` already exist to make safe, so `onDone`'s body (staging
results, flipping `scanUpdateReady`, `screen.PostEvent`) is the same shape
as the existing direct-connect scan thread's completion callback, just
reached from a different thread.

### Item 5: `tools/psb_demo_app/tui/topology_path_picker.h`

```cpp
struct PathPicker {
    Component root;
    std::function<void()> open;  // resets the listing, then shows
};

inline PathPicker makePathPicker(ScreenInteractive& screen,
                                 std::shared_ptr<bool> showPicker,
                                 std::string& targetPath);
```

Internal state: `currentDir` (the directory being browsed), parallel
vectors `entries` (display labels: `..`, `name/` for directories, `name`
for `.toml` files), `entryIsDir`, `entryFullPath`, and a selection index —
rendered as a `Menu` (matching `busMenu`/`boardMenu`'s existing style)
paired with `Open` and `Cancel` buttons below, not click-to-navigate (this
codebase's established Menu-plus-action-buttons pattern, not a new one).

Listing built via `std::filesystem::directory_iterator`: `..` first (unless
`currentDir` is already a filesystem root), then subdirectories
alphabetical, then `*.toml` files alphabetical. A directory that can't be
listed (permission error, etc.) just leaves the listing empty rather than
throwing or erroring the whole dialog.

`Open` on `..`/a directory entry: updates `currentDir`, rebuilds the
listing, resets selection to 0. `Open` on a file entry: sets `targetPath`
to its full path, triggers the same load logic `bLoadTopology` already
runs (populates `s.topo`, resets `s.selectedBus`/`s.selectedBoard`,
rebuilds the Buses/Boards panels, sets `s.statusMsg`), and closes
(`*showPicker = false`).

`wizard_screen.h` adds a `Browse...` button next to the existing Path
field and `Load` button, wired to `pathPicker.open()`. `open()` starts the
listing at `std::filesystem::path(s.topologyPath).parent_path()`, falling
back to `psb::TopologyConfig::defaultPath()`'s directory
(`~/.psb_demo_app/`) if that parent path is empty or doesn't exist.

## Error Handling

- `scanViaLiveBus` finding no matching live worker is not an error — it's
  the expected, common case (a brand-new bus, or the standalone entry
  point), and the caller silently falls back to direct-connect scan.
- A directory the picker can't read (permissions, doesn't exist after
  external deletion, etc.) renders as an empty listing plus `..` (if
  applicable) — no crash, no blocking error dialog.
- Opening a `.toml` file that fails to parse reports the same
  `"Error: could not load " + path` status message `bLoadTopology`
  already produces today — no new error path invented.

## Testing

Consistent with this codebase's established practice for FTXUI/threading-
coupled code: no meaningful unit-test surface for either item (both are
UI/concurrency-shaped); verification is tmux-driven against real hardware:

- **Item 1**: with 2+ boards live on the same bus, reopen Topology
  mid-session, Add Board, Start Scan — confirm results appear (routed
  through the live connection, no second-connection error), and confirm
  the *other* already-connected board's telemetry visibly pauses then
  resumes around the scan (expected, accepted tradeoff) rather than
  erroring or corrupting. Separately, Add Bus for a brand-new port (not
  yet Applied) and confirm its scan still works via the direct-connect
  fallback.
- **Item 5**: Browse from the wizard's Path field, confirm the listing
  starts at the right directory, navigate into a subdirectory and back
  out via `..`, select a `.toml` file and confirm it loads immediately
  (Buses/Boards panels populate, status message shows the loaded path).

## Out of Scope

- The empty-board fallback UI (deferred to its own task).
- Any change to the wizard's own "Remove Board" (stays separate from
  "Remove Bus", per the earlier decision made while fixing the dangling
  boardless bus bug — already shipped, unrelated to this spec's two
  items).

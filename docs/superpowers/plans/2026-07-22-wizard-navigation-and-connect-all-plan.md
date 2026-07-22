# Wizard Navigation Cleanup & Connect All/Disconnect All Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the path picker's `Open` a path-only action (no implicit load), remove the wizard's redundant `Save & Exit`/`Save & Close` button, add always-visible `Connect All`/`Disconnect All` buttons to the multi-board global row, center the quick-connect popup, rename `Cancel`→`Exit` during the launch flow, add a `Back` button that returns to mode selection, and fix a real regression along the way (mid-session Topology has been mislabeling `Connect Now`/`Apply` since scan routing was added).

**Architecture:** `makeWizardScreen` gains `bool isLaunchFlow`, independent of `allowScan` (which now means only "safe to scan") — it drives `bConnectNow`'s label, `bCancel`'s label, and a new `bBack`'s visibility. `mode_select.h`'s two dialogs return tri-state results (`ModeChoice`/`QuickConnectOutcome` each gain an explicit `Back`/`Exit` split). `main.cpp`'s topology resolution becomes an explicit loop so `Back` can return to mode selection cleanly. `BoardSession` gains `connect`/`disconnect` function members (set by `makeBoardDashboard` to the exact closures its own toggle button already uses) so `main.cpp`'s new Connect All/Disconnect All can act on every board without duplicating per-board connection logic.

**Tech Stack:** C++17, FTXUI (vendored), the existing `BoardSession`/`BusWorker`/`Runtime`/`WizardState` machinery from Phase 3 and Sub-projects A–C.

## Global Constraints

- `allowScan` continues to mean only "safe to scan" — never repurposed for launch-flow-vs-mid-session distinctions again (that conflation is exactly the bug this plan fixes).
- `Back` is only ever shown during the launch flow (standalone quick-connect / standalone wizard) — never mid-session, and never added to the Add Bus/Add Board sub-modals.
- `Connect All`/`Disconnect All` are only rendered in the multi-board global row (`board_switcher.h`) — never merged into the single-board dashboard row (`board_dashboard.h`'s existing per-board Connect/Disconnect already covers that case).
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, against real hardware if available. **Protect any real `~/.psb_demo_app/topology.toml`** — back it up before testing and restore it exactly afterward if one exists.

---

## Task 1: `topology_path_picker.h` — `Open` no longer auto-loads

**Files:**
- Modify: `tools/psb_demo_app/tui/topology_path_picker.h`

**Interfaces:**
- Produces: `makePathPicker(ScreenInteractive&, std::shared_ptr<bool> showPicker, std::string& targetPath) -> PathPicker` — drops the now-unused `onFileSelected` parameter entirely.

- [ ] **Step 1: Drop `onFileSelected` from the signature and `bOpen`'s handler**

Edit `tools/psb_demo_app/tui/topology_path_picker.h`, find:

```cpp
inline PathPicker makePathPicker(ScreenInteractive& screen,
                                 std::shared_ptr<bool> showPicker,
                                 std::string& targetPath,
                                 std::function<void()> onFileSelected) {
```

Change to:

```cpp
inline PathPicker makePathPicker(ScreenInteractive& screen,
                                 std::shared_ptr<bool> showPicker,
                                 std::string& targetPath) {
```

Find:

```cpp
    auto bOpen = ActionButton("Open", [entries, entryIsDir, entryFullPath, entryIdx,
                                       currentDir, rebuild, showPicker, &targetPath,
                                       onFileSelected, &screen] {
        int i = *entryIdx;
        if (i < 0 || i >= static_cast<int>(entries->size())) return;
        if ((*entryIsDir)[i]) {
            *currentDir = (*entryFullPath)[i];
            rebuild();
            screen.PostEvent(Event::Custom);
            return;
        }
        targetPath = (*entryFullPath)[i];
        *showPicker = false;
        onFileSelected();
        screen.PostEvent(Event::Custom);
    });
```

Change to:

```cpp
    // Deliberately does not load the selected file — only sets the path
    // field and closes. The picker may also be used to choose a save
    // location by picking an arbitrary existing file as a landing spot and
    // then hand-editing the filename, and the picked file might not even
    // be a valid topology file; Load stays an explicit, separate step the
    // wizard's own Load button already provides.
    auto bOpen = ActionButton("Open", [entries, entryIsDir, entryFullPath, entryIdx,
                                       currentDir, rebuild, showPicker, &targetPath, &screen] {
        int i = *entryIdx;
        if (i < 0 || i >= static_cast<int>(entries->size())) return;
        if ((*entryIsDir)[i]) {
            *currentDir = (*entryFullPath)[i];
            rebuild();
            screen.PostEvent(Event::Custom);
            return;
        }
        targetPath = (*entryFullPath)[i];
        *showPicker = false;
        screen.PostEvent(Event::Custom);
    });
```

- [ ] **Step 2: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -30`
Expected: FAIL — `wizard_screen.h`'s existing `makePathPicker(screen, showPathPicker, s.topologyPath, doLoadTopology)` call still passes 4 arguments. Confirm the *only* error is about that one call site — Task 2 fixes it.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/topology_path_picker.h
git commit -m "fix(psb_demo_tui): path picker's Open no longer auto-loads the file (not yet wired to wizard_screen.h)"
```

---

## Task 2: `wizard_screen.h` — `isLaunchFlow`, remove `bDone`, add `bBack`, fix label bug

**Files:**
- Modify: `tools/psb_demo_app/tui/wizard_screen.h`

**Interfaces:**
- Consumes: `makePathPicker`'s new 3-arg signature (Task 1).
- Produces: `WizardOutcome` becomes `{ Cancelled, ConnectNow, Back }` (`SavedOnly` removed). `makeWizardScreen` gains `bool isLaunchFlow = true`, independent of `allowScan`.

- [ ] **Step 1: Update `WizardOutcome` and add the `isLaunchFlow` parameter**

Edit `tools/psb_demo_app/tui/wizard_screen.h`, find:

```cpp
enum class WizardOutcome { Cancelled, SavedOnly, ConnectNow };
```

Change to:

```cpp
enum class WizardOutcome { Cancelled, ConnectNow, Back };
```

Find:

```cpp
// scanViaLiveBus: if non-empty and it returns true for a given port, a live
// BusWorker took the scan (routed through its own already-open connection —
// see main.cpp's implementation) and reports back asynchronously via the
// callback passed to it. Start Scan's handler falls back to today's direct-
// connect scan when this is empty or returns false for that port. The
// standalone pre-dashboard entry point passes nothing (no Runtime exists
// yet to route through).
using ScanViaLiveBus = std::function<bool(const std::string& port, int start, int end,
                                          std::function<void(std::vector<DiscoveredBoard>, std::string)> onDone)>;

inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true,
                                  ScanViaLiveBus scanViaLiveBus = {}) {
```

Change to:

```cpp
// scanViaLiveBus: if non-empty and it returns true for a given port, a live
// BusWorker took the scan (routed through its own already-open connection —
// see main.cpp's implementation) and reports back asynchronously via the
// callback passed to it. Start Scan's handler falls back to today's direct-
// connect scan when this is empty or returns false for that port. The
// standalone pre-dashboard entry point passes nothing (no Runtime exists
// yet to route through).
using ScanViaLiveBus = std::function<bool(const std::string& port, int start, int end,
                                          std::function<void(std::vector<DiscoveredBoard>, std::string)> onDone)>;

// isLaunchFlow: independent of allowScan (which now only means "safe to
// scan" — mid-session also allows scan, see scanViaLiveBus above). true
// only for main()'s pre-dashboard entry point, reached before any board is
// running; false for the mid-session Setup reopen. Drives bConnectNow's
// label ("Connect Now" vs "Apply"), bCancel's label ("Exit" vs "Cancel"),
// and whether bBack (returns to the mode-selection popup) is shown at all
// — mid-session has no mode selection to go back to.
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true,
                                  ScanViaLiveBus scanViaLiveBus = {},
                                  bool isLaunchFlow = true) {
```

- [ ] **Step 2: Drop the stale comment on `doLoadTopology` (no longer shared with the picker)**

Find:

```cpp
    // Extracted so the path picker's own "Open" (on a selected file) runs
    // the identical load — Load's own button and a picked file both funnel
    // through this one place, never two copies of the same logic.
    auto doLoadTopology = [&s, rebuildBusNames, rebuildBoardNames, &screen] {
```

Change to:

```cpp
    auto doLoadTopology = [&s, rebuildBusNames, rebuildBoardNames, &screen] {
```

- [ ] **Step 3: Update the `makePathPicker` call site to 3 args**

Find:

```cpp
    auto bLoadTopology = ActionButton("Load", doLoadTopology);
    auto showPathPicker = std::make_shared<bool>(false);
    auto pathPicker = makePathPicker(screen, showPathPicker, s.topologyPath, doLoadTopology);
    auto bBrowsePath = ActionButton("Browse...", pathPicker.open);
```

Change to:

```cpp
    auto bLoadTopology = ActionButton("Load", doLoadTopology);
    auto showPathPicker = std::make_shared<bool>(false);
    auto pathPicker = makePathPicker(screen, showPathPicker, s.topologyPath);
    auto bBrowsePath = ActionButton("Browse...", pathPicker.open);
```

- [ ] **Step 4: Remove `bDone`, fix `bConnectNow`'s label bug, rename `bCancel`, add `bBack`**

Find:

```cpp
    // Mid-session (allowScan=false), ConnectNow applies additive changes to
    // the already-running session rather than starting one, and Save &
    // Exit only closes this modal — the dashboard underneath keeps running,
    // nothing actually "exits". Reusing allowScan as the mid-session signal
    // (the same construction-time parameter that already distinguishes the
    // two call sites) to pick the labels that describe what each button
    // does in context, rather than always showing the pre-dashboard wording.
    auto bConnectNow = ActionButton(allowScan ? "Connect Now" : "Apply", [&s, onFinish] {
        onFinish(WizardOutcome::ConnectNow);
    });
    auto bDone = ActionButton(allowScan ? "Save & Exit" : "Save & Close", [&s, onFinish, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            onFinish(WizardOutcome::SavedOnly);
        } else {
            // Stay in the wizard on failure — exiting via onFinish(Cancelled)
            // here would be indistinguishable from the user clicking plain
            // Cancel, silently discarding their edits with no diagnostic.
            s.statusMsg = "Error: could not save to " + s.topologyPath;
            screen.PostEvent(Event::Custom);
        }
    });
    auto bCancel = ActionButton("Cancel", [onFinish] { onFinish(WizardOutcome::Cancelled); });

    auto mainContainer = Container::Vertical({
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bBrowsePath, bLoadTopology,
        bSave, bConnectNow, bDone, bCancel,
    });

    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bBrowsePath, bLoadTopology,
                                         bSave, bConnectNow, bDone, bCancel,
                                         rebuildBusNames, rebuildBoardNames] {
```

Change to:

```cpp
    // isLaunchFlow (not allowScan — see its own comment above) is what
    // distinguishes the two contexts this label describes: mid-session,
    // ConnectNow applies additive changes to the already-running session
    // rather than starting one. Using allowScan here was the bug — mid-
    // session gained allowScan=true when scan routing was added, which
    // silently mislabeled this button "Connect Now" instead of "Apply".
    auto bConnectNow = ActionButton(isLaunchFlow ? "Connect Now" : "Apply", [&s, onFinish] {
        onFinish(WizardOutcome::ConnectNow);
    });
    // "Exit" only during the launch flow (leaving here with no topology
    // exits the whole app — see main.cpp's topology-resolution loop);
    // "Cancel" mid-session, where backing out just closes this modal and
    // the dashboard keeps running underneath.
    auto bCancel = ActionButton(isLaunchFlow ? "Exit" : "Cancel", [onFinish] { onFinish(WizardOutcome::Cancelled); });
    // Only shown during the launch flow — returns to main()'s mode-
    // selection popup. Mid-session has no mode selection to go back to, so
    // this is never added to mainContainer or rendered there (the exact
    // technique allowScan already uses above to gate the scan-related
    // Components: excluded at container construction, not just hidden in
    // the Renderer).
    auto bBack = ActionButton("Back", [onFinish] { onFinish(WizardOutcome::Back); });

    Components mainChildren = {
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bBrowsePath, bLoadTopology,
        bSave, bConnectNow, bCancel,
    };
    if (isLaunchFlow) mainChildren.push_back(bBack);
    auto mainContainer = Container::Vertical(mainChildren);

    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bBrowsePath, bLoadTopology,
                                         bSave, bConnectNow, bCancel, bBack, isLaunchFlow,
                                         rebuildBusNames, rebuildBoardNames] {
```

- [ ] **Step 5: Update the bottom button row**

Find:

```cpp
            hbox({ bSave->Render(), text("  "), bConnectNow->Render(), text("  "),
                   bDone->Render(), text("  "), bCancel->Render() }) | center,
        }) | border | size(WIDTH, GREATER_THAN, 100) | size(HEIGHT, GREATER_THAN, 30);
```

Change to:

```cpp
            (isLaunchFlow
                ? hbox({ bSave->Render(), text("  "), bConnectNow->Render(), text("  "),
                         bBack->Render(), text("  "), bCancel->Render() })
                : hbox({ bSave->Render(), text("  "), bConnectNow->Render(), text("  "),
                         bCancel->Render() })) | center,
        }) | border | size(WIDTH, GREATER_THAN, 100) | size(HEIGHT, GREATER_THAN, 30);
```

- [ ] **Step 6: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp`'s standalone wizard call doesn't pass `isLaunchFlow` yet, and its mid-session call doesn't either (both still compile fine since `isLaunchFlow` has a default — so this should actually be a **clean build**, not a failure, unless `WizardOutcome::SavedOnly`'s removal breaks something else). Confirm clean build; if it fails, it should only be about `WizardOutcome::SavedOnly` no longer existing anywhere else — there should be no other reference (verified during plan-writing: only `wizard_screen.h`'s now-removed `bDone` ever produced it).

- [ ] **Step 7: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_screen.h
git commit -m "fix(psb_demo_tui): remove Save & Exit/Save & Close, add Back, fix Connect Now/Apply mislabeling bug"
```

---

## Task 3: `board_session.h` — `connect`/`disconnect` members

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h`

**Interfaces:**
- Produces: `BoardSession` gains `std::function<void()> connect, disconnect;`. Task 4 (`board_dashboard.h`) populates them; Task 7 (`main.cpp`) calls them.

- [ ] **Step 1: Add the two members**

Edit `tools/psb_demo_app/tui/board_session.h`, find:

```cpp
    ftxui::Component dashboard;  // built once by makeBoardDashboard()
};
```

Change to:

```cpp
    ftxui::Component dashboard;  // built once by makeBoardDashboard()

    // Set by makeBoardDashboard() to the same doConnect/doDisconnect
    // closures its own Connect/Disconnect toggle button already uses — lets
    // an external caller (main.cpp's Connect All/Disconnect All) trigger
    // identical per-board connection logic without duplicating it or
    // restructuring ownership. Empty (never called) until the dashboard is
    // built, which always happens before these could be reached.
    std::function<void()> connect;
    std::function<void()> disconnect;
};
```

- [ ] **Step 2: Build to confirm it compiles clean**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: clean build (new struct members with no assignment anywhere yet are harmless — `std::function` default-constructs empty).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_session.h
git commit -m "feat(psb_demo_tui): add BoardSession::connect/disconnect members (not yet populated)"
```

---

## Task 4: `board_dashboard.h` — populate `board.connect`/`board.disconnect`

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Produces: `makeBoardDashboard` assigns `board.connect`/`board.disconnect` to the same `doConnect`/`doDisconnect` closures its own toggle button calls.

- [ ] **Step 1: Assign the closures right after both are defined**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
    auto doDisconnect = [&board, &busWorker, &screen] {
        board.abortConnect = true;
        board.connected = false; board.data.valid = false;
        // Enqueue disconnect on this board's bus worker to serialise with
        // in-flight Modbus I/O — avoids use-after-free on the bus's port.
        // Only disconnects the bus if no *other* board on it is still
        // connected — a shared bus stays open for sibling boards.
        { std::lock_guard<std::mutex> lk(busWorker.workMutex);
          busWorker.workQueue.push([&board, &busWorker] {
              board.client->disconnect();
              bool anyOtherConnected = false;
              for (BoardSession* b : busWorker.boards)
                  if (b != &board && b->connected.load()) { anyOtherConnected = true; break; }
              if (!anyOtherConnected) busWorker.bus->disconnect();
          }); }
        busWorker.workCv.notify_one();
        board.tabTitles = {"Monitor"}; board.activeTab = std::min(board.activeTab, 0);
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Disconnected"; }
        screen.PostEvent(Event::Custom);
    };

    // ---- Connect/Disconnect/Abort toggle button ----
```

Change to:

```cpp
    auto doDisconnect = [&board, &busWorker, &screen] {
        board.abortConnect = true;
        board.connected = false; board.data.valid = false;
        // Enqueue disconnect on this board's bus worker to serialise with
        // in-flight Modbus I/O — avoids use-after-free on the bus's port.
        // Only disconnects the bus if no *other* board on it is still
        // connected — a shared bus stays open for sibling boards.
        { std::lock_guard<std::mutex> lk(busWorker.workMutex);
          busWorker.workQueue.push([&board, &busWorker] {
              board.client->disconnect();
              bool anyOtherConnected = false;
              for (BoardSession* b : busWorker.boards)
                  if (b != &board && b->connected.load()) { anyOtherConnected = true; break; }
              if (!anyOtherConnected) busWorker.bus->disconnect();
          }); }
        busWorker.workCv.notify_one();
        board.tabTitles = {"Monitor"}; board.activeTab = std::min(board.activeTab, 0);
        { std::lock_guard<std::mutex> lk(board.statusMutex); board.statusMsg = "Disconnected"; }
        screen.PostEvent(Event::Custom);
    };

    // Exposes the same connect/disconnect logic the toggle button below
    // uses, for main.cpp's Connect All/Disconnect All to call directly —
    // see BoardSession::connect/disconnect's own comment.
    board.connect = doConnect;
    board.disconnect = doDisconnect;

    // ---- Connect/Disconnect/Abort toggle button ----
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): populate BoardSession::connect/disconnect from the dashboard's own toggle logic"
```

---

## Task 5: `board_switcher.h` — `Connect All`/`Disconnect All` in the global row

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Produces: `makeBoardSwitcher` gains two new trailing `Component` parameters, `globalConnectAll, globalDisconnectAll`, rendered past the filler, immediately before `globalQuit`, in the multi-board global row only.

- [ ] **Step 1: Update the signature**

Edit `tools/psb_demo_app/tui/board_switcher.h`, find:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup,
                                       Component globalPreferences) {
```

Change to:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup,
                                       Component globalPreferences,
                                       Component globalConnectAll, Component globalDisconnectAll) {
```

- [ ] **Step 2: Add them to `globalMenuBar`'s children**

Find:

```cpp
    // Order matches the visual/Tab order the Renderer below produces
    // (Setup, Preferences, then Quit pushed to the right corner) — Container
    // children order drives keyboard Tab traversal even though the actual
    // visual arrangement is decided entirely in the Renderer, so keeping
    // the two in sync avoids Tab jumping in an order that doesn't match
    // what's on screen.
    auto globalMenuBar = Container::Horizontal({globalSetup, globalPreferences, globalQuit});
```

Change to:

```cpp
    // Order matches the visual/Tab order the Renderer below produces
    // (Setup, Preferences, then Connect All/Disconnect All/Quit pushed to
    // the right corner) — Container children order drives keyboard Tab
    // traversal even though the actual visual arrangement is decided
    // entirely in the Renderer, so keeping the two in sync avoids Tab
    // jumping in an order that doesn't match what's on screen.
    auto globalMenuBar = Container::Horizontal(
        {globalSetup, globalPreferences, globalConnectAll, globalDisconnectAll, globalQuit});
```

- [ ] **Step 3: Update the Renderer's capture list and the global row's `hbox`**

Find:

```cpp
    // The global row renders globalSetup/globalPreferences/globalQuit
    // individually rather than calling globalMenuBar->Render() as one
    // unit, so a filler() can push Quit to the right corner, separated
    // from Setup/Preferences — Quit is the one destructive, high-
    // consequence action here and reads more safely set apart from the
    // routine buttons next to it. globalMenuBar itself still exists and
    // still owns these three as children (for mainContainer's focus
    // tree); only its own Render() call is unused.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, globalSetup, globalPreferences, globalQuit,
                                         boardNames, activeBoard, mainSelected] {
        bool showBar = boardNames->size() > 1;
        if (!showBar) {
            return vbox({ dashboardStack->Render() | flex });
        }
        return vbox({
            hbox({
                globalSetup->Render(), text(" "), globalPreferences->Render(),
                filler(),
                globalQuit->Render(),
            }),
            separator(),
            hbox({
                switcherBar->Render(),
                separator(),
                dashboardStack->Render() | flex,
            }) | flex,
        });
    });
```

Change to:

```cpp
    // The global row renders globalSetup/globalPreferences/globalConnectAll/
    // globalDisconnectAll/globalQuit individually rather than calling
    // globalMenuBar->Render() as one unit, so a filler() can push the last
    // three to the right corner, separated from Setup/Preferences — Quit is
    // the one destructive, high-consequence action here, and Connect All/
    // Disconnect All are impactful-but-reversible enough to sit right next
    // to it rather than blend into the routine Setup/Preferences group.
    // globalMenuBar itself still exists and still owns all five as children
    // (for mainContainer's focus tree); only its own Render() call is
    // unused.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, globalSetup, globalPreferences,
                                         globalConnectAll, globalDisconnectAll, globalQuit,
                                         boardNames, activeBoard, mainSelected] {
        bool showBar = boardNames->size() > 1;
        if (!showBar) {
            return vbox({ dashboardStack->Render() | flex });
        }
        return vbox({
            hbox({
                globalSetup->Render(), text(" "), globalPreferences->Render(),
                filler(),
                globalConnectAll->Render(), text(" "), globalDisconnectAll->Render(), text(" "),
                globalQuit->Render(),
            }),
            separator(),
            hbox({
                switcherBar->Render(),
                separator(),
                dashboardStack->Render() | flex,
            }) | flex,
        });
    });
```

- [ ] **Step 4: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — `main.cpp`'s existing `makeBoardSwitcher(rt.boards, globalQuit, globalSetup, globalPreferences)` call doesn't pass the two new arguments yet. Confirm the *only* error is about that one call site — Task 7 updates it.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add Connect All/Disconnect All to the multi-board global row (not yet wired to main.cpp)"
```

---

## Task 6: `mode_select.h` — tri-state outcomes, `Back`, `Exit` rename, centering

**Files:**
- Modify: `tools/psb_demo_app/tui/mode_select.h`

**Interfaces:**
- Produces: `enum class ModeChoice { Single, Multi, Exit }` (was `{ Cancelled, Single, Multi }`), `enum class QuickConnectOutcome { Connected, Back, Exit }`, `struct QuickConnectResult { QuickConnectOutcome outcome; psb::TopologyConfig topo; }`, `showQuickConnectForm(ScreenInteractive&) -> QuickConnectResult` (was `-> std::optional<psb::TopologyConfig>`).

- [ ] **Step 1: Rewrite the file**

This file's changes touch nearly every line (enum rename throughout, a new outcome type, a new button, added centering) — replace the whole file rather than patching fragments.

Replace the entire contents of `tools/psb_demo_app/tui/mode_select.h` with:

```cpp
#pragma once

#include "topology_config.h"
#include "psb_serial_bus.h"
#include "tui_policy.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

enum class ModeChoice { Single, Multi, Exit };

// Pre-dashboard root offering a choice between a lightweight single-board
// quick-connect and the full multi-board Setup wizard. Shown whenever no
// other signal (an existing topology file, CLI args) has already resolved
// what to do — see main.cpp's topology-resolution loop and
// docs/superpowers/specs/2026-07-21-mode-architecture-design.md's
// Sequencing Note. Runs its own screen.Loop(), matching the same
// standalone pre-dashboard pattern the Setup wizard's own entry point
// (Phase 3, Task 6) already establishes — sequential Loop() calls on one
// ScreenInteractive is an already-proven pattern in this codebase.
inline ModeChoice showModeChoicePopup(ScreenInteractive& screen) {
    auto result = std::make_shared<ModeChoice>(ModeChoice::Exit);

    auto bSingle = ActionButton("Single Board", [result, &screen] {
        *result = ModeChoice::Single;
        screen.ExitLoopClosure()();
    });
    auto bMulti = ActionButton("Multi-Board Topology", [result, &screen] {
        *result = ModeChoice::Multi;
        screen.ExitLoopClosure()();
    });
    auto bExit = ActionButton("Exit", [result, &screen] {
        *result = ModeChoice::Exit;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Horizontal({bSingle, bMulti, bExit});
    // `| center` on the returned Element is the whole fix for this popup
    // rendering pinned to the top-left of the fullscreen root (looking
    // like it "spans the left edge") — an unconstrained bordered box has
    // no reason to move there otherwise.
    auto root = Renderer(container, [bSingle, bMulti, bExit] {
        return vbox({
            text(" How many boards? ") | bold | center,
            separator(),
            text("Choose how you want to start.") | center,
            separator(),
            hbox({ bSingle->Render(), text("  "), bMulti->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 60) | center;
    });

    screen.Loop(root);
    return *result;
}

enum class QuickConnectOutcome { Connected, Back, Exit };
struct QuickConnectResult {
    QuickConnectOutcome outcome;
    psb::TopologyConfig topo;  // valid only when outcome == Connected
};

// Quick-connect form for single-board mode: port/baud/slave ID only, no
// topology file involved. Pre-fills from lastSingleConnectPath() — a small
// stored preference, never the real topology file — and saves there again
// on success. Back returns to the mode-selection popup (see main.cpp's
// topology-resolution loop); Exit quits the app entirely.
inline QuickConnectResult showQuickConnectForm(ScreenInteractive& screen) {
    auto portVal = std::make_shared<std::string>();
    auto baudVal = std::make_shared<std::string>("115200");
    auto slaveVal = std::make_shared<std::string>("1");

    const std::string lastPath = psb::TopologyConfig::lastSingleConnectPath();
    if (auto prev = psb::TopologyConfig::load(lastPath);
        prev.has_value() && !prev->buses.empty() && !prev->buses[0].boards.empty()) {
        *portVal = prev->buses[0].port;
        *baudVal = std::to_string(prev->buses[0].baudRate);
        *slaveVal = std::to_string(prev->buses[0].boards[0].slaveId);
    }

    auto portList = std::make_shared<std::vector<std::string>>();
    auto portIdx = std::make_shared<int>(-1);
    auto doScanPorts = [portList, portIdx, portVal, &screen] {
        *portList = PsbSerialBus::scanPorts();
        *portIdx = selectedPortIndex(*portList, *portVal);
        *portVal = *portIdx >= 0 ? (*portList)[*portIdx] : std::string{};
        screen.PostEvent(Event::Custom);
    };
    doScanPorts();

    auto portDropdown = Dropdown(portList.get(), portIdx.get());
    auto visiblePortDropdown = Maybe(portDropdown, [portList] { return !portList->empty(); });
    auto bRescan = Button("Rescan", [doScanPorts] { doScanPorts(); });
    auto baudInp = Input(baudVal.get(), "baud");
    auto slaveInp = Input(slaveVal.get(), "1-247");

    auto outcome = std::make_shared<QuickConnectOutcome>(QuickConnectOutcome::Exit);

    auto bConnect = ActionButton("Connect", [&, portList, portIdx, portVal, outcome] {
        // Dropdown() only ever mutates portIdx as the user navigates it —
        // reading portList[*portIdx] here (the live selection) rather than
        // trusting a separately-tracked *portVal is required, or Connect
        // silently uses whatever port was selected at scan time regardless
        // of what the user picked afterward — the exact bug already found
        // and fixed in wizard_screen.h's Add Bus modal.
        std::string port = (*portIdx >= 0 && *portIdx < static_cast<int>(portList->size()))
            ? (*portList)[*portIdx] : *portVal;
        if (port.empty()) return;
        *portVal = port;
        *outcome = QuickConnectOutcome::Connected;
        screen.ExitLoopClosure()();
    });
    auto bBack = ActionButton("Back", [outcome, &screen] {
        *outcome = QuickConnectOutcome::Back;
        screen.ExitLoopClosure()();
    });
    auto bExit = ActionButton("Exit", [outcome, &screen] {
        *outcome = QuickConnectOutcome::Exit;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Vertical({visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bBack, bExit});
    auto root = Renderer(container, [portList, visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bBack, bExit] {
        Element portChoice = portList->empty()
            ? text("(no ports found — Rescan)") | dim | flex
            : visiblePortDropdown->Render() | flex;
        return vbox({
            text(" Quick Connect ") | bold | center,
            separator(),
            hbox({ text("Port  : "), portChoice, text(" "), bRescan->Render() }),
            hbox({ text("Baud  : "), baudInp->Render()  | size(WIDTH, EQUAL, 8) }),
            hbox({ text("Slave : "), slaveInp->Render() | size(WIDTH, EQUAL, 5) }),
            separator(),
            hbox({ bConnect->Render(), text("  "), bBack->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 44) | center;
    });

    screen.Loop(root);
    if (*outcome != QuickConnectOutcome::Connected) return QuickConnectResult{*outcome, {}};

    int baud = 115200, slave = 1;
    try { baud = std::stoi(*baudVal); } catch (...) {}
    try { slave = std::stoi(*slaveVal); } catch (...) {}
    auto cfg = psb::TopologyConfig::singleBoard(*portVal, baud, slave, "board1");
    cfg.save(lastPath);  // best-effort — a failed preference save isn't fatal
    return QuickConnectResult{QuickConnectOutcome::Connected, cfg};
}

} // namespace psb::tui
```

- [ ] **Step 2: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp`'s topology-resolution block still uses `ModeChoice::Cancelled` and treats `showQuickConnectForm`'s return as `std::optional<TopologyConfig>`. Confirm the errors are all within `main.cpp` — Task 7 rewrites that block.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/mode_select.h
git commit -m "feat(psb_demo_tui): mode popup/quick-connect gain Back, Exit rename, centering (not yet wired to main.cpp)"
```

---

## Task 7: `main.cpp` — wire everything together

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `makeWizardScreen`'s new `isLaunchFlow` parameter and `WizardOutcome::Back` (Task 2), `makeBoardSwitcher`'s two new trailing parameters (Task 5), `ModeChoice::Exit`/`QuickConnectResult`/`QuickConnectOutcome` (Task 6), `BoardSession::connect`/`disconnect` (Tasks 3–4).
- Produces: `bGlobalConnectAll`/`bGlobalDisconnectAll` constructed once and threaded into `buildRuntime`/`makeBoardSwitcher`; the topology-resolution loop supporting `Back`; the mid-session wizard call passing `isLaunchFlow=false`.

- [ ] **Step 1: Thread `globalConnectAll`/`globalDisconnectAll` through `buildRuntime`'s signature and its `makeBoardSwitcher` call**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalPreferences) {
```

Change to:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalPreferences, Component globalConnectAll, Component globalDisconnectAll) {
```

Find:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup, globalPreferences);
```

Change to:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup, globalPreferences,
                                              globalConnectAll, globalDisconnectAll);
```

- [ ] **Step 2: Rewrite the topology-resolution block as a loop**

Find:

```cpp
    psb::TopologyConfig topo;
    bool haveTopo = false;

    auto choice = psb::tui::showModeChoicePopup(screen);
    if (choice == psb::tui::ModeChoice::Cancelled) return 0;
    if (choice == psb::tui::ModeChoice::Single) {
        auto quick = psb::tui::showQuickConnectForm(screen);
        if (!quick.has_value()) return 0;
        topo = *quick;
        haveTopo = true;
    }
    // else Multi: leave haveTopo false — runWizard below launches the
    // standalone wizard, empty except for the Path field defaulting to
    // topologyPath, ready for [Load] to pull in an existing file.

    bool runWizard = !haveTopo;
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
```

Change to:

```cpp
    psb::TopologyConfig topo;

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
        if (topo.totalBoardCount() == 0) {
            std::cerr << "Topology has no boards configured — exiting.\n";
            return 0;
        }
        break;
    }
```

- [ ] **Step 3: Construct `bGlobalConnectAll`/`bGlobalDisconnectAll`, thread into `buildRuntime`'s call**

Find:

```cpp
    auto showPreferences = std::make_shared<bool>(false);
    auto prefsDialog = psb::tui::makePreferencesDialog(screen, showPreferences,
                                                        g_connectTimeoutMs, g_pollInterval);
    auto bGlobalPreferences = psb::tui::ActionButton("Preferences", prefsDialog.open);

    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalPreferences);
```

Change to:

```cpp
    auto showPreferences = std::make_shared<bool>(false);
    auto prefsDialog = psb::tui::makePreferencesDialog(screen, showPreferences,
                                                        g_connectTimeoutMs, g_pollInterval);
    auto bGlobalPreferences = psb::tui::ActionButton("Preferences", prefsDialog.open);

    // Each unconditionally acts on every board regardless of its current
    // state (Connect All also re-triggers already-connected boards — a
    // redundant no-op there, not gated on board.connected) — calling the
    // same board.connect/board.disconnect closures makeBoardDashboard sets
    // on every BoardSession, never a second implementation of the
    // connect/disconnect logic itself.
    auto bGlobalConnectAll = psb::tui::ActionButton("Connect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->connect) b->connect();
    });
    auto bGlobalDisconnectAll = psb::tui::ActionButton("Disconnect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->disconnect) b->disconnect();
    });

    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalPreferences, bGlobalConnectAll, bGlobalDisconnectAll);
```

- [ ] **Step 4: Pass `isLaunchFlow=false` at the mid-session wizard call site**

Find:

```cpp
    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish,
                                                            /*allowScan=*/true, scanViaLiveBus);
```

Change to:

```cpp
    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish,
                                                            /*allowScan=*/true, scanViaLiveBus, /*isLaunchFlow=*/false);
```

- [ ] **Step 5: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests -j$(nproc) 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 6: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same 382 assertions / 107 test cases as before this plan (no `psb_modbus_core` files touched).

- [ ] **Step 7: Manual verification**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, and protecting any real `~/.psb_demo_app/topology.toml` the same way prior rounds did:

1. **Mode popup**: confirm `Exit` (not `Cancel`) is the third button; clicking it exits the app.
2. **Quick-connect**: confirm it's now centered; confirm `Connect`/`Back`/`Exit` all present; click `Back` — confirm it returns to the mode popup (not the app exiting); choose Single Board again — confirm quick-connect shows fresh (pre-filled from the same saved preference as before, unaffected by the detour).
3. **Standalone wizard** (`Multi-Board Topology` from the popup): confirm `Save & Exit` is gone; confirm `Back` is present and returns to the mode popup; confirm the bottom-right button says `Exit` (not `Cancel`), and clicking it with no topology configured exits the app.
4. **Path picker**: Browse, select a `.toml` file, confirm the Path field updates but the Buses/Boards panels do *not* change until `Load` is clicked separately (Task 1's fix).
5. **Mid-session Topology** (global button, from a running single-board or multi-board session): confirm it now correctly shows `Apply` (not `Connect Now`) and `Cancel` (not `Exit`) — the label bug fix; confirm no `Back` button appears (nothing to go back to); confirm `Save & Close` is gone.
6. **Connect All / Disconnect All**: build a 2-board session (matching prior rounds' test setup), disconnect one board manually, then click the global `Connect All` — confirm both boards end up connected; click `Disconnect All` — confirm both end up disconnected. Confirm neither button appears in the single-board merged row (only the board's own Connect/Disconnect toggle does).

- [ ] **Step 8: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire Connect All/Disconnect All and the Back-capable launch-flow loop"
```

---

## Task 8: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, 382 assertions / 107 test cases — unchanged from this plan's starting baseline (no automated tests added; matching this codebase's established precedent that FTXUI/threading-coupled UI code is verified via tmux, not Catch2).

- [ ] **Step 3: Re-run the manual verification sequence once, end to end, in one continuous session**

Repeat Task 7 Step 7's full sequence back to back without relaunching between items where practical (mode popup → quick-connect → Back → mode popup → standalone wizard → Back → mode popup → quick-connect → Connect → mid-session Topology → Connect All/Disconnect All), confirming nothing regresses when these transitions happen in sequence.

- [ ] **Step 4: Confirm any real topology file was restored exactly**

If a real `~/.psb_demo_app/topology.toml` existed before this plan's testing began, diff it against the backup taken at the start and confirm it's byte-identical; restore it if anything in this plan's testing touched it.

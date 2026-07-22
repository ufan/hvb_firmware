# Mid-Session Scan Routing & Topology Path Picker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the Topology wizard's bus scan run safely when reopened mid-session (currently disabled entirely, since it used to always open a second serial connection on a port a `BusWorker` thread might already be driving), and add a mouse-driven directory browser for the wizard's topology-path field.

**Architecture:** `makeWizardScreen` gains an optional `scanViaLiveBus` callback; when present and it recognizes the target port as already live, the scan runs as a work item on that bus's own `BusWorker` queue (reusing its already-open connection) instead of opening a new one, reporting back through the exact same staged hand-off (`ScanUpdate`/`scanMutex`/`scanUpdateReady`) the existing direct-connect scan already uses. A new `topology_path_picker.h` adds a `Modal`-based directory browser, following this codebase's established Menu-plus-action-buttons pattern.

**Tech Stack:** C++17, FTXUI (vendored), `std::filesystem`, the existing `BusWorker`/`Runtime`/`WizardState` machinery from Phase 3 and Sub-projects A–C.

## Global Constraints

- No second `PsbSerialBus` is ever opened on a port a live `BusWorker` is already driving — this is the exact hazard mid-session scan was disabled to avoid, and the fix must close it, not just paper over it.
- The standalone pre-dashboard wizard entry point is unaffected — it has no `Runtime` yet, so it keeps using direct-connect scan unconditionally (passes no `scanViaLiveBus`).
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, against real hardware if available. **Be careful with any pre-existing real `~/.psb_demo_app/topology.toml`** — back it up before testing and restore it exactly afterward if one exists; use freshly-added fictional buses/boards for test scenarios instead of editing real hardware config.

---

## Task 1: `wizard_screen.h` — `scanViaLiveBus` parameter and routing

**Files:**
- Modify: `tools/psb_demo_app/tui/wizard_screen.h`

**Interfaces:**
- Produces: `using ScanViaLiveBus = std::function<bool(const std::string& port, int start, int end, std::function<void(std::vector<DiscoveredBoard>, std::string)> onDone)>;` and `makeWizardScreen`'s new trailing parameter `ScanViaLiveBus scanViaLiveBus = {}`. Task 2 (`main.cpp`) implements and passes one at the mid-session call site.

- [ ] **Step 1: Add the `ScanViaLiveBus` type and parameter**

Edit `tools/psb_demo_app/tui/wizard_screen.h`, find:

```cpp
// Builds the Topology wizard's Component — a bus/board list plus Add Bus, Add
// Board (Manual or Scan), Remove, Save/Save As, and Connect Now/Done.
// Reused unmodified as both `main()`'s standalone pre-dashboard root (Task
// 6) and a Modal overlay atop a live dashboard (Task 7's mid-session entry)
// — this function has no opinion on which; `onFinish` is how the caller
// finds out what happened.
inline Component makeWizardScreen(WizardState& s, ScreenInteractive& screen,
                                  std::function<void(WizardOutcome)> onFinish,
                                  bool allowScan = true) {
```

Change to:

```cpp
// Builds the Topology wizard's Component — a bus/board list plus Add Bus, Add
// Board (Manual or Scan), Remove, Save/Save As, and Connect Now/Done.
// Reused unmodified as both `main()`'s standalone pre-dashboard root (Task
// 6) and a Modal overlay atop a live dashboard (Task 7's mid-session entry)
// — this function has no opinion on which; `onFinish` is how the caller
// finds out what happened.
//
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

- [ ] **Step 2: Refactor `bStartScan` to try `scanViaLiveBus` first, sharing a `stageResult` helper**

Find:

```cpp
    auto bStartScan = ActionButton("Start Scan", [&s, scanStart, scanEnd, scanning,
                                                   scanProgress, scanMutex, scanStaged,
                                                   scanUpdateReady, &screen] {
        if (scanning->load() || s.selectedBus < 0) return;
        int start = 1, end = 32;
        try { start = std::stoi(*scanStart); } catch (...) {}
        try { end = std::stoi(*scanEnd); } catch (...) {}
        if (start < 0) start = 0;
        if (end > 247) end = 247;
        if (end < start) return;

        const std::string port = s.topo.buses[s.selectedBus].port;
        const int baud = s.topo.buses[s.selectedBus].baudRate;
        scanning->store(true);
        // Stage the "connecting" status through the same hand-off as the
        // final result — bStartScan runs on the UI thread, so writing
        // scanStatus directly here would be safe in isolation, but staging
        // it too keeps exactly one path ever touching the live vectors.
        {
            std::lock_guard<std::mutex> lk(*scanMutex);
            scanStaged->results.clear();
            scanStaged->labels.clear();
            scanStaged->status = "Connecting to " + port + "...";
        }
        scanUpdateReady->store(true);
        screen.PostEvent(Event::Custom);

        std::thread([&screen, scanning, scanProgress, scanMutex, scanStaged,
                     scanUpdateReady, port, baud, start, end] {
            auto scanBusHandle = std::make_shared<PsbSerialBus>();
            if (!scanBusHandle->connect(port, baud, 500)) {
                {
                    std::lock_guard<std::mutex> lk(*scanMutex);
                    scanStaged->results.clear();
                    scanStaged->labels.clear();
                    scanStaged->status = "Error: " + scanBusHandle->lastError();
                }
                scanUpdateReady->store(true);
                scanning->store(false);
                screen.PostEvent(Event::Custom);
                return;
            }
            auto results = scanBus(scanBusHandle, start, end, [&](int id) {
                scanProgress->store(id);
                screen.PostEvent(Event::Custom);
            });
            scanBusHandle->disconnect();
            std::vector<std::string> labels;
            for (const auto& r : results)
                labels.push_back(r.variantName + "  #" + std::to_string(r.slaveId));
            std::string status = results.empty()
                ? "No boards found in range."
                : std::to_string(results.size()) + " board(s) found.";
            {
                std::lock_guard<std::mutex> lk(*scanMutex);
                scanStaged->results = std::move(results);
                scanStaged->labels = std::move(labels);
                scanStaged->status = std::move(status);
            }
            scanUpdateReady->store(true);
            scanning->store(false);
            screen.PostEvent(Event::Custom);
        }).detach();
    });
```

Change to:

```cpp
    auto bStartScan = ActionButton("Start Scan", [&s, scanStart, scanEnd, scanning,
                                                   scanProgress, scanMutex, scanStaged,
                                                   scanUpdateReady, &screen, scanViaLiveBus] {
        if (scanning->load() || s.selectedBus < 0) return;
        int start = 1, end = 32;
        try { start = std::stoi(*scanStart); } catch (...) {}
        try { end = std::stoi(*scanEnd); } catch (...) {}
        if (start < 0) start = 0;
        if (end > 247) end = 247;
        if (end < start) return;

        const std::string port = s.topo.buses[s.selectedBus].port;
        const int baud = s.topo.buses[s.selectedBus].baudRate;
        scanning->store(true);
        // Stage the "connecting" status through the same hand-off as the
        // final result — bStartScan runs on the UI thread, so writing
        // scanStatus directly here would be safe in isolation, but staging
        // it too keeps exactly one path ever touching the live vectors.
        {
            std::lock_guard<std::mutex> lk(*scanMutex);
            scanStaged->results.clear();
            scanStaged->labels.clear();
            scanStaged->status = "Connecting to " + port + "...";
        }
        scanUpdateReady->store(true);
        screen.PostEvent(Event::Custom);

        // Stages a completed (or errored) scan result through the same
        // hand-off regardless of which path produced it — shared so a
        // live-bus scan (running on a BusWorker's own thread, reached via
        // scanViaLiveBus) and a direct-connect scan (its own std::thread,
        // below) report back identically; the UI-side drain in
        // addBoardPopup's Renderer doesn't need to know which one ran.
        auto stageResult = [scanning, scanMutex, scanStaged, scanUpdateReady, &screen]
                           (std::vector<DiscoveredBoard> results, std::string status) {
            std::vector<std::string> labels;
            for (const auto& r : results)
                labels.push_back(r.variantName + "  #" + std::to_string(r.slaveId));
            {
                std::lock_guard<std::mutex> lk(*scanMutex);
                scanStaged->results = std::move(results);
                scanStaged->labels = std::move(labels);
                scanStaged->status = std::move(status);
            }
            scanUpdateReady->store(true);
            scanning->store(false);
            screen.PostEvent(Event::Custom);
        };

        // Route through an already-open connection when this bus is part
        // of the currently-running session — no second PsbSerialBus opened
        // on a port a BusWorker thread may already be driving. Falls back
        // to the direct-connect path below when no live worker owns this
        // port (a bus just added in this wizard session but not yet
        // Applied, or the standalone pre-dashboard entry point, which
        // never has a scanViaLiveBus at all).
        if (scanViaLiveBus && scanViaLiveBus(port, start, end, stageResult)) {
            return;
        }

        std::thread([&screen, scanProgress, port, baud, start, end, stageResult] {
            auto scanBusHandle = std::make_shared<PsbSerialBus>();
            if (!scanBusHandle->connect(port, baud, 500)) {
                stageResult({}, "Error: " + scanBusHandle->lastError());
                return;
            }
            auto results = scanBus(scanBusHandle, start, end, [&](int id) {
                scanProgress->store(id);
                screen.PostEvent(Event::Custom);
            });
            scanBusHandle->disconnect();
            std::string status = results.empty()
                ? "No boards found in range."
                : std::to_string(results.size()) + " board(s) found.";
            stageResult(std::move(results), std::move(status));
        }).detach();
    });
```

- [ ] **Step 3: Build to confirm it compiles clean**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: clean build. `scanViaLiveBus`'s default (`= {}`) means the standalone call site (which doesn't pass it) keeps compiling unchanged — no other call site breaks, unlike previous plans' parameter additions.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_screen.h
git commit -m "feat(psb_demo_tui): add scanViaLiveBus routing to Start Scan (not yet wired to main.cpp)"
```

---

## Task 2: `main.cpp` — implement and wire `scanViaLiveBus`, enable mid-session scan

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `psb::tui::ScanViaLiveBus`, `psb::tui::DiscoveredBoard`, `psb::tui::scanBus` (Task 1 / existing `wizard_scan.h`), `Runtime::busWorkers`, `psb::tui::BusWorker`'s `bus`/`boards`/`workQueue`/`workMutex`/`workCv` fields (existing).
- Produces: the mid-session `makeWizardScreen` call site now passes `allowScan=true` and a working `scanViaLiveBus`.

- [ ] **Step 1: Construct `scanViaLiveBus` and thread it into the mid-session wizard, flipping `allowScan` to `true`**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
    auto midSessionWizardRoot = psb::tui::makeWizardScreen(*midSessionWiz, screen, onMidSessionFinish, /*allowScan=*/false);
```

Change to:

```cpp
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
                                                            /*allowScan=*/true, scanViaLiveBus);
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests -j$(nproc) 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 3: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same 382 assertions / 107 test cases as before this task (no `psb_modbus_core` files touched).

- [ ] **Step 4: Manual verification — mid-session scan, both routing paths**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, and the Global Constraints note above about protecting any real `~/.psb_demo_app/topology.toml`:

1. Launch, Single Board quick-connect to a real board (confirms auto-connect still works, unaffected by this task).
2. Open Topology (global button), Add Board on the *already-connected* bus, Start Scan — confirm results appear (routed live: no "second connection" error, no crash) and confirm the connected board's own telemetry visibly pauses then resumes around the scan (expected, accepted tradeoff from the spec).
3. Add Bus for a different port not yet Applied, Add Board on *that* new bus, Start Scan — confirm it still works via the direct-connect fallback (no live worker owns that port yet).
4. Cancel out without saving, confirming nothing was written to disk.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): route mid-session scan through the live BusWorker connection when possible"
```

---

## Task 3: `topology_path_picker.h` — directory browser

**Files:**
- Create: `tools/psb_demo_app/tui/topology_path_picker.h`

**Interfaces:**
- Consumes: `psb::TopologyConfig::defaultPath()` (existing), `ActionButton` (`widgets.h`).
- Produces: `struct PathPicker { Component root; std::function<void()> open; }`, `psb::tui::makePathPicker(ScreenInteractive&, std::shared_ptr<bool> showPicker, std::string& targetPath, std::function<void()> onFileSelected) -> PathPicker`. Task 4 (`wizard_screen.h`) constructs one and wires it to the Path field's new "Browse..." button.

- [ ] **Step 1: Write `topology_path_picker.h`**

Create `tools/psb_demo_app/tui/topology_path_picker.h`:

```cpp
#pragma once

#include "topology_config.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// Mouse-driven directory browser for the Topology wizard's Path field —
// ".." first (unless already at a filesystem root), then subdirectories
// alphabetical, then *.toml files alphabetical. Open on a directory entry
// navigates into it; Open on a file entry sets targetPath to its full
// path, calls onFileSelected() (the caller reuses its own Load logic —
// this file never needs to know about WizardState/TopologyConfig
// loading), and closes.
struct PathPicker {
    Component root;
    std::function<void()> open;  // starts browsing at targetPath's parent dir, then shows
};

inline PathPicker makePathPicker(ScreenInteractive& screen,
                                 std::shared_ptr<bool> showPicker,
                                 std::string& targetPath,
                                 std::function<void()> onFileSelected) {
    auto currentDir = std::make_shared<std::string>();
    auto entries = std::make_shared<std::vector<std::string>>();
    auto entryIsDir = std::make_shared<std::vector<bool>>();
    auto entryFullPath = std::make_shared<std::vector<std::string>>();
    auto entryIdx = std::make_shared<int>(0);
    auto status = std::make_shared<std::string>();

    auto rebuild = [currentDir, entries, entryIsDir, entryFullPath, entryIdx, status] {
        namespace fs = std::filesystem;
        entries->clear();
        entryIsDir->clear();
        entryFullPath->clear();
        status->clear();

        fs::path dir(*currentDir);
        if (dir.has_parent_path() && dir != dir.root_path()) {
            entries->push_back("..");
            entryIsDir->push_back(true);
            entryFullPath->push_back(dir.parent_path().string());
        }

        std::vector<std::pair<std::string, std::string>> dirs, files;
        try {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.is_directory()) {
                    dirs.push_back({e.path().filename().string() + "/", e.path().string()});
                } else if (e.is_regular_file() && e.path().extension() == ".toml") {
                    files.push_back({e.path().filename().string(), e.path().string()});
                }
            }
        } catch (const std::exception& ex) {
            *status = "Error: " + std::string(ex.what());
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        for (auto& entry : dirs) {
            entries->push_back(entry.first); entryIsDir->push_back(true); entryFullPath->push_back(entry.second);
        }
        for (auto& entry : files) {
            entries->push_back(entry.first); entryIsDir->push_back(false); entryFullPath->push_back(entry.second);
        }
        *entryIdx = 0;
    };

    auto entriesMenu = Menu(entries.get(), entryIdx.get());

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
    auto bCancel = ActionButton("Cancel", [showPicker, &screen] {
        *showPicker = false;
        screen.PostEvent(Event::Custom);
    });

    auto container = Container::Vertical({entriesMenu, bOpen, bCancel});
    auto root = Renderer(container, [currentDir, entries, entriesMenu, status, bOpen, bCancel] {
        Element listEl = entries->empty()
            ? text("(empty)") | dim
            : entriesMenu->Render() | frame | size(HEIGHT, LESS_THAN, 12);
        Elements body = {
            text(" Select Topology File ") | bold | center,
            separator(),
            text(*currentDir) | dim,
            listEl,
        };
        if (!status->empty()) body.push_back(text(*status) | color(Color::Red));
        body.push_back(separator());
        body.push_back(hbox({ bOpen->Render(), text("  "), bCancel->Render() }) | center);
        return vbox(std::move(body)) | border | size(WIDTH, EQUAL, 56);
    });

    auto open = [currentDir, rebuild, showPicker, &targetPath, &screen] {
        namespace fs = std::filesystem;
        fs::path startDir;
        if (!targetPath.empty()) {
            fs::path p(targetPath);
            if (p.has_parent_path()) startDir = p.parent_path();
        }
        if (startDir.empty() || !fs::exists(startDir)) {
            startDir = fs::path(psb::TopologyConfig::defaultPath()).parent_path();
        }
        *currentDir = startDir.string();
        rebuild();
        *showPicker = true;
        screen.PostEvent(Event::Custom);
    };

    return PathPicker{root, open};
}

} // namespace psb::tui
```

- [ ] **Step 2: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — nothing includes `topology_path_picker.h` yet. This is a **compile-only sanity check**, not functional: temporarily add `#include "topology_path_picker.h"` to the top of `tools/psb_demo_app/tui/wizard_screen.h`, rebuild, confirm it compiles clean, then remove that temporary include — Task 4 adds the real include and call sites.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/topology_path_picker.h
git commit -m "feat(psb_demo_tui): add topology_path_picker.h -- directory browser for the Path field (not yet wired)"
```

---

## Task 4: `wizard_screen.h` — wire in the Browse button

**Files:**
- Modify: `tools/psb_demo_app/tui/wizard_screen.h`

**Interfaces:**
- Consumes: `psb::tui::PathPicker`/`makePathPicker` (Task 3).
- Produces: a `Browse...` button next to the wizard's Path field and `Load` button, and `doLoadTopology` (extracted from `bLoadTopology`'s existing body) shared with the picker's file-selection callback.

- [ ] **Step 1: Add the include**

Edit `tools/psb_demo_app/tui/wizard_screen.h`, find:

```cpp
#include "wizard_state.h"
#include "wizard_scan.h"
#include "widgets.h"
#include "tui_policy.h"
#include "psb_serial_bus.h"
```

Change to:

```cpp
#include "wizard_state.h"
#include "wizard_scan.h"
#include "widgets.h"
#include "tui_policy.h"
#include "psb_serial_bus.h"
#include "topology_path_picker.h"
```

- [ ] **Step 2: Extract `doLoadTopology`, construct the picker and Browse button**

Find:

```cpp
    // ---- Save / Save As / Load / Connect / Cancel ----
    auto topologyPathInp = Input(&s.topologyPath, "topology file path");
    auto bLoadTopology = ActionButton("Load", [&s, rebuildBusNames, rebuildBoardNames, &screen] {
        auto loaded = psb::TopologyConfig::load(s.topologyPath);
        if (loaded.has_value()) {
            s.topo = std::move(*loaded);
            s.selectedBus = -1;
            s.selectedBoard = -1;
            s.dirty = false;
            s.statusMsg = "Loaded " + s.topologyPath;
            rebuildBusNames();
            rebuildBoardNames();
        } else {
            s.statusMsg = "Error: could not load " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    });
```

Change to:

```cpp
    // ---- Save / Save As / Load / Browse / Connect / Cancel ----
    auto topologyPathInp = Input(&s.topologyPath, "topology file path");
    // Extracted so the path picker's own "Open" (on a selected file) runs
    // the identical load — Load's own button and a picked file both funnel
    // through this one place, never two copies of the same logic.
    auto doLoadTopology = [&s, rebuildBusNames, rebuildBoardNames, &screen] {
        auto loaded = psb::TopologyConfig::load(s.topologyPath);
        if (loaded.has_value()) {
            s.topo = std::move(*loaded);
            s.selectedBus = -1;
            s.selectedBoard = -1;
            s.dirty = false;
            s.statusMsg = "Loaded " + s.topologyPath;
            rebuildBusNames();
            rebuildBoardNames();
        } else {
            s.statusMsg = "Error: could not load " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    };
    auto bLoadTopology = ActionButton("Load", doLoadTopology);
    auto showPathPicker = std::make_shared<bool>(false);
    auto pathPicker = makePathPicker(screen, showPathPicker, s.topologyPath, doLoadTopology);
    auto bBrowsePath = ActionButton("Browse...", pathPicker.open);
```

- [ ] **Step 3: Add `bBrowsePath` to the Path row**

Find:

```cpp
            hbox({ text("Path: "), topologyPathInp->Render() | flex, text(" "), bLoadTopology->Render() }),
```

Change to:

```cpp
            hbox({ text("Path: "), topologyPathInp->Render() | flex, text(" "), bBrowsePath->Render(),
                   text(" "), bLoadTopology->Render() }),
```

- [ ] **Step 4: Add `bBrowsePath` to `mainContainer`'s children (focus/Tab order)**

Find:

```cpp
    auto mainContainer = Container::Vertical({
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bLoadTopology,
        bSave, bConnectNow, bDone, bCancel,
    });
```

Change to:

```cpp
    auto mainContainer = Container::Vertical({
        busMenu, bAddBus, busSelectable,
        boardMenu, addBoardEnabled, boardSelectable,
        topologyPathInp, bBrowsePath, bLoadTopology,
        bSave, bConnectNow, bDone, bCancel,
    });
```

- [ ] **Step 5: Add `bBrowsePath` to the root Renderer's capture list**

Find:

```cpp
    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bLoadTopology,
                                         bSave, bConnectNow, bDone, bCancel,
                                         rebuildBusNames, rebuildBoardNames] {
```

Change to:

```cpp
    auto root = Renderer(mainContainer, [&s, busMenu, bAddBus, busSelectable,
                                         boardMenu, addBoardEnabled, boardSelectable,
                                         topologyPathInp, bBrowsePath, bLoadTopology,
                                         bSave, bConnectNow, bDone, bCancel,
                                         rebuildBusNames, rebuildBoardNames] {
```

- [ ] **Step 6: Layer the picker as a third `Modal`**

Find:

```cpp
    }) | Modal(addBusPopup, showAddBusPtr.get())
       | Modal(addBoardPopup, showAddBoardPtr.get());

    return root;
```

Change to:

```cpp
    }) | Modal(addBusPopup, showAddBusPtr.get())
       | Modal(addBoardPopup, showAddBoardPtr.get())
       | Modal(pathPicker.root, showPathPicker.get());

    return root;
```

- [ ] **Step 7: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests -j$(nproc) 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 8: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same 382 assertions / 107 test cases (no `psb_modbus_core` files touched).

- [ ] **Step 9: Manual verification — path picker**

Following the same hardware-protection note as Task 2:

1. From the standalone Topology wizard (fresh launch, no topology file, choose Multi-Board Topology), click `Browse...` — confirm the listing starts at `~/.psb_demo_app/` (no path typed yet).
2. Navigate into a subdirectory (if any exist) and back out via `..` — confirm the listing updates correctly each time.
3. Select a `.toml` file — confirm the modal closes, the Path field shows the full path, and the Buses/Boards panels populate immediately (auto-loaded, no separate Load click needed) with a "Loaded ..." status message.
4. Reopen Browse from a wizard whose Path field already has a value — confirm it starts in *that* file's parent directory, not `~/.psb_demo_app/`.
5. Cancel from the picker without selecting anything — confirm the Path field and loaded topology are unchanged.

- [ ] **Step 10: Commit**

```bash
git add tools/psb_demo_app/tui/wizard_screen.h
git commit -m "feat(psb_demo_tui): wire the topology path picker's Browse button into the wizard"
```

---

## Task 5: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, 382 assertions / 107 test cases — unchanged from this plan's starting baseline (neither task adds automated tests, matching this codebase's established precedent that FTXUI/threading-coupled UI code is verified via tmux, not Catch2).

- [ ] **Step 3: Re-run both manual verification sequences once, end to end, in one continuous session**

Repeat Task 2 Step 4 and Task 4 Step 9 back to back without relaunching between them where practical (quick-connect → mid-session scan on the live bus → mid-session scan on a not-yet-applied bus → Browse to load a different topology file), confirming nothing regresses when these happen in sequence rather than as isolated relaunches.

- [ ] **Step 4: Confirm any real topology file was restored exactly**

If a real `~/.psb_demo_app/topology.toml` existed before this plan's testing began, diff it against the backup taken at the start and confirm it's byte-identical; restore it if anything in this plan's testing touched it.

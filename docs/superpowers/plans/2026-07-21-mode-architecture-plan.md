# Single/Multi-Board Mode Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `psb_demo_tui` an explicit, live-derived mode boundary (single-board vs. multi-board, based purely on current board count — no stored flag), expressed as a startup mode-selection popup, a two-level menu bar (global `Quit`/`Setup` vs. per-board actions) that collapses into one visually-merged row in single-board mode, and `Remove` correctly gated to 2+ boards.

**Architecture:** `Quit`/`Setup` become single, globally-constructed `Component`s threaded into both `board_switcher.h` (which renders them as their own row when 2+ boards exist) and `board_dashboard.h` (which renders the *same* `Component`s, via a second `Render()` call, folded into its own menu row when there's only 1 board — no re-parenting, matching this codebase's established render-time-decision principle). A new `mode_select.h` adds the startup choice popup and a lightweight single-board quick-connect form, backed by a new preference path (`TopologyConfig::lastSingleConnectPath()`) kept deliberately separate from the real multi-board topology file.

**Tech Stack:** C++17, FTXUI (vendored), toml++ (vendored, via `TopologyConfig`), the existing `BoardSwitcher`/`BoardSession`/`BusWorker`/`Runtime` machinery from Phase 3 and Sub-project A.

## Global Constraints

- **Mode is derived from board count, never stored.** Exactly 1 board ⇒ single-mode UI; 2+ ⇒ multi-mode UI. No new persisted "mode" field anywhere.
- **The startup popup replaces only today's hardcoded `/dev/ttyUSB0` fallback** — every other existing entry path (`-p`, an existing topology file, `--topology` naming a missing file, `--setup`) is untouched by this plan (see the design spec's Sequencing Note).
- **Single mode never touches the real topology file** (`TopologyConfig::defaultPath()`). Its quick-connect form's pre-fill/save goes to a separate path, `TopologyConfig::lastSingleConnectPath()`.
- **The single-row merge in single-board mode is visual-only** — `Quit`/`Setup` remain structurally owned by the switcher's own container for keyboard-focus purposes in every mode; the board's own dashboard independently calls `Render()` on the *same* `Component` objects to fold their visual output into its own row. No re-parenting, no `Detach()`/`Add()` for this purpose.
- **`Remove` renders only when 2+ boards exist.** (Correction from the design spec: this was assumed already true from Sub-project A; verified against the actual shipped code during plan-writing that it isn't — `Remove` currently renders unconditionally. This plan adds the gating.)
- Build via `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui`. Test via `./build/psb_modbus_core/tests/psb_tests`. Manual verification via `tmux` per `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, against real hardware if available.

---

## Task 1: `TopologyConfig::lastSingleConnectPath()`

**Files:**
- Modify: `tools/psb_modbus_core/topology_config.h`
- Modify: `tools/psb_modbus_core/topology_config.cpp`
- Test: `tools/psb_modbus_core/tests/test_topology_config.cpp`

**Interfaces:**
- Produces: `static std::string TopologyConfig::lastSingleConnectPath()` — mirrors the existing `defaultPath()` exactly, just a different file name, so single mode's quick-connect preference never collides with or gets confused for a real multi-board topology file.

- [ ] **Step 1: Write the failing test**

Add to `tools/psb_modbus_core/tests/test_topology_config.cpp` (append; read the file first to match its exact existing style before inserting):

```cpp
TEST_CASE("TopologyConfig — lastSingleConnectPath differs from defaultPath", "[topology_config]") {
    CHECK(TopologyConfig::lastSingleConnectPath() != TopologyConfig::defaultPath());
}
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run: `cd tools && cmake --build build --target psb_tests 2>&1 | tail -20`
Expected: FAIL — `no member named 'lastSingleConnectPath' in 'psb::TopologyConfig'`.

- [ ] **Step 3: Add the declaration**

Edit `tools/psb_modbus_core/topology_config.h`, find:

```cpp
    static std::string defaultPath();  // ~/.psb_demo_app/topology.toml
```

Change to:

```cpp
    static std::string defaultPath();  // ~/.psb_demo_app/topology.toml
    // Deliberately separate from defaultPath() — single-board mode's quick-
    // connect form (mode_select.h) pre-fills from and saves to this path,
    // never the real multi-board topology file, so the two can never be
    // confused for one another.
    static std::string lastSingleConnectPath();  // ~/.psb_demo_app/last_single.toml
```

- [ ] **Step 4: Add the definition**

Edit `tools/psb_modbus_core/topology_config.cpp`, find:

```cpp
std::string TopologyConfig::defaultPath() {
    return homeDir() + "/.psb_demo_app/topology.toml";
}
```

Change to:

```cpp
std::string TopologyConfig::defaultPath() {
    return homeDir() + "/.psb_demo_app/topology.toml";
}

std::string TopologyConfig::lastSingleConnectPath() {
    return homeDir() + "/.psb_demo_app/last_single.toml";
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd tools && cmake --build build --target psb_tests && ./build/psb_modbus_core/tests/psb_tests "[topology_config]"`
Expected: PASS, all `[topology_config]`-tagged tests including the new one.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_modbus_core/topology_config.h tools/psb_modbus_core/topology_config.cpp tools/psb_modbus_core/tests/test_topology_config.cpp
git commit -m "feat(psb_modbus_core): add TopologyConfig::lastSingleConnectPath()"
```

---

## Task 2: `mode_select.h` — startup choice popup + quick-connect form

**Files:**
- Create: `tools/psb_demo_app/tui/mode_select.h`

**Interfaces:**
- Consumes: `psb::TopologyConfig::singleBoard()/load()/save()/lastSingleConnectPath()` (Task 1 + existing), `PsbSerialBus::scanPorts()`, `selectedPortIndex` (`tui_policy.h`), `ActionButton` (`widgets.h`).
- Produces: `enum class ModeChoice { Cancelled, Single, Multi }`, `psb::tui::showModeChoicePopup(ScreenInteractive&) -> ModeChoice`, `psb::tui::showQuickConnectForm(ScreenInteractive&) -> std::optional<psb::TopologyConfig>`. Task 5 calls both from `main()`'s startup flow.

- [ ] **Step 1: Write `mode_select.h`**

Create `tools/psb_demo_app/tui/mode_select.h`:

```cpp
#pragma once

#include "topology_config.h"
#include "psb_serial_bus.h"
#include "tui_policy.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

enum class ModeChoice { Cancelled, Single, Multi };

// Pre-dashboard root offering a choice between a lightweight single-board
// quick-connect and the full multi-board Setup wizard. Shown whenever no
// other signal (an existing topology file, CLI args) has already resolved
// what to do — see main.cpp's topology-resolution chain and
// docs/superpowers/specs/2026-07-21-mode-architecture-design.md's
// Sequencing Note. Runs its own screen.Loop(), matching the same
// standalone pre-dashboard pattern the Setup wizard's own entry point
// (Phase 3, Task 6) already establishes — sequential Loop() calls on one
// ScreenInteractive is an already-proven pattern in this codebase.
inline ModeChoice showModeChoicePopup(ScreenInteractive& screen) {
    auto result = std::make_shared<ModeChoice>(ModeChoice::Cancelled);

    auto bSingle = ActionButton("Single Board", [result, &screen] {
        *result = ModeChoice::Single;
        screen.ExitLoopClosure()();
    });
    auto bMulti = ActionButton("Multi-Board Setup", [result, &screen] {
        *result = ModeChoice::Multi;
        screen.ExitLoopClosure()();
    });
    auto bCancel = ActionButton("Cancel", [result, &screen] {
        *result = ModeChoice::Cancelled;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Horizontal({bSingle, bMulti, bCancel});
    auto root = Renderer(container, [bSingle, bMulti, bCancel] {
        return vbox({
            text(" How many boards? ") | bold | center,
            separator(),
            text("Choose how you want to start.") | center,
            separator(),
            hbox({ bSingle->Render(), text("  "), bMulti->Render(), text("  "), bCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 46);
    });

    screen.Loop(root);
    return *result;
}

// Quick-connect form for single-board mode: port/baud/slave ID only, no
// topology file involved. Pre-fills from lastSingleConnectPath() (Task 1)
// — a small stored preference, never the real topology file — and saves
// there again on success. Returns std::nullopt if cancelled.
inline std::optional<psb::TopologyConfig> showQuickConnectForm(ScreenInteractive& screen) {
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

    auto cancelled = std::make_shared<bool>(false);
    auto done = std::make_shared<bool>(false);

    auto bConnect = ActionButton("Connect", [&, portList, portIdx, portVal, done] {
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
        *done = true;
        screen.ExitLoopClosure()();
    });
    auto bCancel = ActionButton("Cancel", [cancelled, &screen] {
        *cancelled = true;
        screen.ExitLoopClosure()();
    });

    auto container = Container::Vertical({visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bCancel});
    auto root = Renderer(container, [portList, visiblePortDropdown, bRescan, baudInp, slaveInp, bConnect, bCancel] {
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
            hbox({ bConnect->Render(), text("  "), bCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 44);
    });

    screen.Loop(root);
    if (*cancelled || !*done) return std::nullopt;

    int baud = 115200, slave = 1;
    try { baud = std::stoi(*baudVal); } catch (...) {}
    try { slave = std::stoi(*slaveVal); } catch (...) {}
    auto cfg = psb::TopologyConfig::singleBoard(*portVal, baud, slave, "board1");
    cfg.save(lastPath);  // best-effort — a failed preference save isn't fatal
    return cfg;
}

} // namespace psb::tui
```

- [ ] **Step 2: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — nothing includes `mode_select.h` yet, so it isn't compiled by any translation unit. This is a **compile-only sanity check**, not functional: temporarily add `#include "mode_select.h"` to the top of `tools/psb_demo_app/tui/main.cpp`, rebuild, confirm it compiles clean, then remove that temporary include — Task 5 adds the real include and call sites.

Expected after the temporary include: clean build (both functions are template-free `inline` functions, only instantiated when called, so uncalled ones produce no warnings — this step mainly catches syntax/type errors).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/mode_select.h
git commit -m "feat(psb_demo_tui): add mode_select.h — startup mode choice + quick-connect form (not yet wired to main.cpp)"
```

---

## Task 3: `board_switcher.h` — global menu bar as a third sibling

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Produces: `makeBoardSwitcher` gains two new parameters (`Component globalQuit, Component globalSetup`), building a `globalMenuBar` container as a third sibling of `switcherBar`/`dashboardStack`, rendered as its own row only when 2+ boards exist. `mainSelected`'s indexing shifts from 2 slots to 3, defaults unchanged in spirit (still lands on `switcherBar` for 2+ boards, `dashboardStack` for ≤1 — just at new indices 1/2 instead of 0/1).

- [ ] **Step 1: Update the signature and build `globalMenuBar`**

Edit `tools/psb_demo_app/tui/board_switcher.h`, find:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards) {
    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto activeBoard = std::make_shared<int>(0);
    // Starting focus for the switcher's own outer container: with a single
    // board the switcher bar is a Menu with one entry, which unconditionally
    // swallows Event::Return and doesn't handle Left/Right at all (see
    // MenuBase::OnEvent in the vendored FTXUI source) — if it held initial
    // focus, keys typed immediately at launch would be silently dropped
    // until the user happened to Tab/Down into dashboardStack. So focus
    // starts on dashboardStack (index 1) for a single board, restoring
    // Phase 2's "keys reach the dashboard immediately" behavior. With 2+
    // boards, focus starts on switcherBar (index 0) as already established
    // and verified in Phase 2 — arrow keys need to reach the switcher first
    // so the user can move between boards.
    auto mainSelected = std::make_shared<int>(boards.size() > 1 ? 0 : 1);
```

Change to:

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup) {
    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto activeBoard = std::make_shared<int>(0);
    // Starting focus for the switcher's own outer container. Same
    // reasoning as before Sub-project B, just re-indexed for the new
    // 3-slot scheme (0=globalMenuBar, 1=switcherBar, 2=dashboardStack):
    // with a single board the switcher bar is a Menu with one entry, which
    // unconditionally swallows Event::Return and doesn't handle Left/Right
    // at all (see MenuBase::OnEvent in the vendored FTXUI source) — if it
    // held initial focus, keys typed immediately at launch would be
    // silently dropped until the user happened to Tab/Down into
    // dashboardStack. So focus starts on dashboardStack (now index 2) for
    // a single board, restoring Phase 2's "keys reach the dashboard
    // immediately" behavior. With 2+ boards, focus starts on switcherBar
    // (now index 1) as already established and verified in Phase 2 —
    // arrow keys need to reach the switcher first so the user can move
    // between boards. globalMenuBar (index 0) is never the default in
    // either case — Quit/Setup are reachable via explicit Tab/Shift-Tab,
    // not the first thing a keypress lands on.
    auto mainSelected = std::make_shared<int>(boards.size() > 1 ? 1 : 2);
```

- [ ] **Step 2: Build `globalMenuBar` and extend `mainContainer`**

Find:

```cpp
    auto dashboardStack = Container::Tab({}, activeBoard.get());
    for (auto& b : boards) dashboardStack->Add(b->dashboard);

    auto mainContainer = Container::Vertical({switcherBar, dashboardStack}, mainSelected.get());
```

Change to:

```cpp
    auto dashboardStack = Container::Tab({}, activeBoard.get());
    for (auto& b : boards) dashboardStack->Add(b->dashboard);

    auto globalMenuBar = Container::Horizontal({globalQuit, globalSetup});

    auto mainContainer = Container::Vertical({globalMenuBar, switcherBar, dashboardStack}, mainSelected.get());
```

- [ ] **Step 3: Render `globalMenuBar` as its own row when 2+ boards exist**

Find:

```cpp
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, boardNames, activeBoard, mainSelected] {
        bool showBar = boardNames->size() > 1;
        Elements top;
        if (showBar) {
            top.push_back(text(" Boards ") | bold | dim);
            top.push_back(switcherBar->Render());
            top.push_back(separator());
        }
        top.push_back(dashboardStack->Render() | flex);
        return vbox(std::move(top));
    });
```

Change to:

```cpp
    // globalMenuBar renders here as its own row only when 2+ boards exist
    // — with exactly one board, board_dashboard.h's own Renderer instead
    // calls Render() on these same globalQuit/globalSetup Components,
    // folding their output into its own menu row (a second render call
    // site for the same Components — safe, since rendering is stateless
    // per call; only *parenting*, unchanged here, is singular). This is
    // the visual-only single-row merge described in
    // docs/superpowers/specs/2026-07-21-mode-architecture-design.md.
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, globalMenuBar, boardNames, activeBoard, mainSelected] {
        bool showBar = boardNames->size() > 1;
        Elements top;
        if (showBar) {
            top.push_back(globalMenuBar->Render());
            top.push_back(separator());
            top.push_back(text(" Boards ") | bold | dim);
            top.push_back(switcherBar->Render());
            top.push_back(separator());
        }
        top.push_back(dashboardStack->Render() | flex);
        return vbox(std::move(top));
    });
```

- [ ] **Step 4: Re-index `detachBoard`'s single-board clamp**

Find:

```cpp
            if (boardNames->size() <= 1) *mainSelected = 1;
```

Change to:

```cpp
            if (boardNames->size() <= 1) *mainSelected = 2;
```

- [ ] **Step 5: Update the `BoardSwitcher` struct's doc comment reference**

No code change needed here — the struct itself (`BoardSwitcher { Component root; ...attachBoard; ...detachBoard; }`) is unchanged; only `makeBoardSwitcher`'s signature and body changed above.

- [ ] **Step 6: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -40`
Expected: FAIL — `main.cpp`'s existing `makeBoardSwitcher(rt.boards)` call site doesn't pass the two new arguments yet. Confirm the *only* error is about the missing arguments at that one call site — Task 5 updates it.

- [ ] **Step 7: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): add global menu bar (Quit/Setup) as a third switcher sibling (not yet wired to main.cpp)"
```

---

## Task 4: `board_dashboard.h` — merge global buttons in single-board mode, gate Remove

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Produces: `makeBoardDashboard` gains three new parameters (`Component globalQuit, Component globalSetup, std::function<size_t()> liveBoardCount`). Its locally-constructed `bQuit`/`bOpenSetup` are removed entirely. `Remove` now renders only when `liveBoardCount() > 1`. When `liveBoardCount() <= 1`, the menu row additionally renders `globalQuit`/`globalSetup`'s output (visual-only merge — see Task 3's comment).

- [ ] **Step 1: Update the signature**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove) {
```

Change to:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    std::function<size_t()> liveBoardCount) {
```

- [ ] **Step 2: Remove the locally-constructed `bQuit`**

Find:

```cpp
    auto bQuit = ActionButton("Quit", [&running, &busWorker, &screen] {
        running = false; busWorker.workCv.notify_all(); screen.ExitLoopClosure()();
    });

    // Always available (not gated on connection state) — the exact case
```

Change to:

```cpp
    // Always available (not gated on connection state) — the exact case
```

(This deletes `bQuit`'s construction entirely — `Quit` is now exclusively the globally-constructed `globalQuit` passed in.)

- [ ] **Step 3: Remove `bQuit` from `menuBar`'s children**

Find:

```cpp
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bRemove, bQuit});
```

Change to:

```cpp
    auto menuBar = Container::Horizontal({menuModeC, connectedMenuSave, bConnToggle, bRemove});
```

- [ ] **Step 4: Remove the locally-constructed `bOpenSetup`, drop it from `statusBar`**

Find:

```cpp
    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto bOpenSetup = ActionButton("Setup", [openSetup] { openSetup(); });
    auto statusBar    = Container::Horizontal({bSysCfg, bOpenSetup});
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});
```

Change to:

```cpp
    // ---- Status bar (connection details + SysConfig; Connect lives in the menu) ----
    auto statusBar    = Container::Horizontal({bSysCfg});
    auto mainContainer = Container::Vertical({menuBar, tabBar, tabContent, statusBar});
```

- [ ] **Step 5: Update the root Renderer's capture list**

Find:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove, bQuit, tabBar, tabContent, bSysCfg, bOpenSetup] {
```

Change to:

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove,
                                         tabBar, tabContent, bSysCfg, globalQuit, globalSetup, liveBoardCount] {
```

- [ ] **Step 6: Gate `Remove` and merge the global buttons in `menuBarEl`**

Find:

```cpp
        auto menuBarEl = hbox({
            text(" " + variantTxt + " ") | bold,
            separator(),
            text(" " + chTxt + " Channels "),
            separator(),
            modeElement,
            text(" "),
            saveElement,
            filler(),
            centerGroup,
            filler(),
            bConnToggle->Render(),
            text(" "),
            bRemove->Render(),
            text(" "),
            bQuit->Render(),
        });
```

Change to:

```cpp
        // Remove only makes sense with a sibling to remove down to — see
        // Global Constraints. In single-board mode, this row also folds in
        // globalQuit/globalSetup's own rendered output (visual-only merge
        // — see Task 3's comment in board_switcher.h): with 2+ boards,
        // those same Components render as their own separate row one level
        // up instead, so they're omitted here to avoid rendering twice.
        size_t boardCount = liveBoardCount();
        Element removeElement = boardCount > 1 ? bRemove->Render() : text("");
        Elements menuBarParts = {
            text(" " + variantTxt + " ") | bold,
            separator(),
            text(" " + chTxt + " Channels "),
            separator(),
            modeElement,
            text(" "),
            saveElement,
            filler(),
            centerGroup,
            filler(),
            bConnToggle->Render(),
            text(" "),
            removeElement,
        };
        if (boardCount <= 1) {
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalQuit->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalSetup->Render());
        }
        auto menuBarEl = hbox(std::move(menuBarParts));
```

- [ ] **Step 7: Drop `bOpenSetup` from `statusBarEl`**

Find:

```cpp
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                  | size(WIDTH, GREATER_THAN, 30),
            filler(),
            text((isOnline ? " FW:" + fwTxt + "  Proto:" + protoTxt + "  " : " ")
                 + "TUI:" TOOL_VERSION_STRING " "),
            filler(),
            connTextEl,
            text(" "),
            bSysCfg->Render(),
            text(" "),
            bOpenSetup->Render(),
        });
```

Change to:

```cpp
        auto statusBarEl = hbox({
            text(" " + msg + " ") | (isErr ? color(Color::Red) : color(Color::Green))
                                  | size(WIDTH, GREATER_THAN, 30),
            filler(),
            text((isOnline ? " FW:" + fwTxt + "  Proto:" + protoTxt + "  " : " ")
                 + "TUI:" TOOL_VERSION_STRING " "),
            filler(),
            connTextEl,
            text(" "),
            bSysCfg->Render(),
        });
```

- [ ] **Step 8: Build to confirm it compiles**

Run: `cd tools && cmake --build build --target psb_demo_tui 2>&1 | tail -60`
Expected: FAIL — `main.cpp`'s two existing `makeBoardDashboard(...)` call sites (in `buildRuntime` and `applyNewBoardsLive`) don't pass the three new arguments yet. Confirm the *only* errors are about missing arguments at those two call sites — Task 5 updates them.

- [ ] **Step 9: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): merge global Quit/Setup into single-board's own menu row, gate Remove to 2+ boards (not yet wired to main.cpp)"
```

---

## Task 5: `main.cpp` — wire everything together

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `psb::tui::showModeChoicePopup`/`showQuickConnectForm` (Task 2), `makeBoardSwitcher`'s new signature (Task 3), `makeBoardDashboard`'s new signature (Task 4).
- Produces: `bGlobalQuit`/`bGlobalSetup` constructed once in `main()`, threaded through `buildRuntime`/`applyNewBoardsLive`; the startup mode-selection flow replacing the hardcoded `/dev/ttyUSB0` fallback.

- [ ] **Step 1: Include `mode_select.h`**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
#include "wizard_state.h"
#include "wizard_screen.h"
#include "tool_version.h"
```

Change to:

```cpp
#include "wizard_state.h"
#include "wizard_screen.h"
#include "mode_select.h"
#include "tool_version.h"
```

- [ ] **Step 2: Thread `globalQuit`/`globalSetup` through `buildRuntime`**

Find:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup) {
```

Change to:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup) {
```

Find, still inside `buildRuntime`:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); });
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });
```

Find, still inside `buildRuntime`:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards);
```

Change to:

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup);
```

- [ ] **Step 3: Thread `globalQuit`/`globalSetup` through `applyNewBoardsLive`**

Find:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup) {
```

Change to:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup) {
```

Find, still inside `applyNewBoardsLive`:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); });
```

Change to:

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, [&rt] { return rt.boards.size(); });
```

- [ ] **Step 4: Replace the hardcoded fallback with the mode-selection popup**

Find:

```cpp
    } else if (!setupFlag) {
        // Neither -p nor a resolvable/explicit --topology, and --setup not
        // given: today's genuinely-first-run hardcoded guess.
        topo = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
        haveTopo = true;
    }
```

Change to:

```cpp
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
```

- [ ] **Step 5: Construct the global buttons once, before `buildRuntime`**

Find:

```cpp
    Runtime rt;
    buildRuntime(rt, topo, screen, running, timeoutArg, autoConnectAll, openSetup);
```

Change to:

```cpp
    Runtime rt;

    // Constructed once, not once per board — Quit notifies every bus's
    // worker (not just one board's, unlike the old per-board version this
    // replaces), so quitting wakes every bus thread immediately rather
    // than letting some wait out their idle poll interval. Setup reuses
    // the existing openSetup closure unchanged.
    auto bGlobalQuit = ActionButton("Quit", [&running, &rt, &screen] {
        running = false;
        for (auto& bw : rt.busWorkers) bw->workCv.notify_all();
        screen.ExitLoopClosure()();
    });
    auto bGlobalSetup = ActionButton("Setup", [openSetup] { openSetup(); });

    buildRuntime(rt, topo, screen, running, timeoutArg, autoConnectAll, openSetup, bGlobalQuit, bGlobalSetup);
```

- [ ] **Step 6: Thread the global buttons into `onMidSessionFinish`'s `applyNewBoardsLive` call**

Find:

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

Change to:

```cpp
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
```

- [ ] **Step 7: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui psb_tests 2>&1 | tail -60`
Expected: clean build.

- [ ] **Step 8: `psb_tests` unaffected**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, same count as Task 1's (this task touches no `psb_modbus_core` files beyond what Task 1 already added).

- [ ] **Step 9: Manual verification — every entry path via tmux**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology:

1. **Fresh launch, no args, no existing topology file** (rename/move `~/.psb_demo_app/topology.toml` aside first if one exists from earlier testing): confirm the mode-selection popup appears with "Single Board"/"Multi-Board Setup"/"Cancel".
2. **Choose Single Board**: confirm the quick-connect form appears (port dropdown, baud, slave ID), confirm selecting a port and clicking Connect launches the dashboard with exactly that one board — menu row is genuinely one line (global `Quit`/`Setup` folded in alongside the board's own controls), no `Remove` button, no switcher bar.
3. **Relaunch, choose Single Board again**: confirm the port/baud/slave fields are pre-filled with the previous values (from `~/.psb_demo_app/last_single.toml`).
4. **Choose Multi-Board Setup**: confirm the existing standalone wizard appears unchanged (Add Bus/Add Board/Scan all present).
5. **Runtime switching**: from a single-board dashboard, click `Setup` (now part of the merged row) — confirm the mid-session wizard opens pre-seeded with the currently-running board, add a second board via Add Board + Apply, confirm: the menu layout live-transitions to the two-level form (separate global row + switcher bar + per-board row), `Remove` now appears, and the original board is undisturbed throughout (uptime, if connected, keeps advancing).
6. **Crossing back 2→1**: click `Remove` on one of the two boards — confirm the layout live-transitions back to the single merged row, `Remove` disappears, keyboard input still reaches the remaining board's own controls immediately (send a keypress like Right/Enter right after the transition, no extra Tab/Down needed first — the same regression class Task 4 of Phase 3 already found and fixed once).
7. **Global Quit thread teardown**: with 2+ boards on 2+ buses connected, click the global `Quit` — confirm via `/proc/<pid>/status`-style thread-count check (captured just before clicking) that the process exits cleanly and promptly, not waiting out any bus's idle poll interval.
8. **Existing paths untouched (regression check)**: confirm `-p <port>`, an existing topology file with no `--setup`, `--topology <missing-file>`, and `--setup` all behave exactly as before this task — none of them should show the mode-selection popup.

- [ ] **Step 10: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire the mode-selection startup flow and global menu bar into main()"
```

---

## Task 6: Full-repo verification

**Files:** none (verification only).

- [ ] **Step 1: Clean rebuild of every touched target**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui 2>&1 | tail -40`
Expected: clean build.

- [ ] **Step 2: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests`
Expected: PASS, one more test case than the baseline this plan started from (Task 1's `lastSingleConnectPath` test — every other task in this plan adds no automated tests, matching this codebase's established precedent that FTXUI/threading-coupled UI code is verified via tmux, not Catch2).

- [ ] **Step 3: Re-run the full manual verification sequence once, end to end, against real hardware if available**

Repeat Task 5 Step 9's full sequence in one continuous pass where practical (fresh launch → single-board quick-connect → runtime-switch to multi via Setup → Remove back down to single → Quit), confirming nothing regresses when these transitions happen back to back rather than as isolated relaunches.

- [ ] **Step 4: Clean up any temporary/moved files from manual verification**

Restore any topology file moved aside in Task 5 Step 9.1 back to its original location. Remove any scratch files created during testing (e.g. under `/tmp`).

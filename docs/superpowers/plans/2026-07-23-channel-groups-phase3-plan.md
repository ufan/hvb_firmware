# Channel Groups — Phase 3 (Group View in the Sidebar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the sidebar's vertical tab bar into a top "Groups" section and the existing "Boards" section below it, each group's tab showing an aggregating Monitor-style table of its member channels (live-bound to their real owning boards, same read/write semantics as the board's own Monitor table) with a trailing "jump to source channel" link column.

**Architecture:** `tab_monitor.h`'s per-row/header cell-building logic is extracted into three reusable free functions (pure refactor, no behavior change) so a new `group_monitor.h` can build an aggregating table by calling `makeMonitorRow` against each member channel's *real* owning `BoardSession` — the same "separate widget instance, same underlying `ConfigInputs`" pattern this codebase already uses between a board's Monitor and Channel tabs — never a second poll or a duplicate data model. `board_switcher.h` grows a second, independent `Container::Tab` (for group dashboards) stacked above the existing board one, with its own `Menu`, synchronized to a shared "which section is showing" flag via each `Menu`'s `on_change` callback (verified against the vendored FTXUI source: `on_change` fires only on user-driven selection changes, never from `Render()`'s own `Clamp()`, which makes this synchronization safe). Because group rows hold live references into a specific `BoardSession`, every place a board can be added or removed while the app is running is hooked to rebuild all group dashboards fresh — cheap, since a group dashboard owns no hardware connection or worker thread, purely a read/write projection over already-polling boards.

**Tech Stack:** C++17, FTXUI (vendored), Catch2 (Tasks 1-2 have no new automated coverage — FTXUI rendering, consistent with every other `tab_*.h`/`*_screen.h` file in this codebase, verified manually/tmux-driven instead).

## Global Constraints

- **Reuses `tab_monitor.h`'s per-row logic verbatim** (Design spec, Architecture § Phase 3) — no duplicated capability-conditional cell code between a board's own Monitor table and a group's aggregating table.
- **A group's monitor table always shows every configured member channel**, rendering an offline/placeholder row (the *same* placeholder a board's own Monitor table already uses for an offline channel) for any member whose board is disconnected or no longer in the topology — membership is never silently dropped. (Design spec, Confirmed Decisions.)
- **One shared active selection** — groups and boards share one active tab; whichever entry (group or board) was clicked last is the one shown. (Design spec, Confirmed Decisions.)
- **The jump link's label is the channel's alias, falling back to `CHn`** when unset. (Design spec, Phase 3 architecture.) Offline/missing members get no link button — nothing to jump to.
- **No change to polling/data-acquisition** — groups are a pure rendering projection over already-polling boards, never a second poll or a new worker thread. (Design spec, Out of Scope.)
- **No change to a board's own Monitor table's non-alias columns** — Task 1's extraction must be behavior-preserving; verified by a before/after live comparison, not just "it compiles."

---

### Task 1: Extract `tab_monitor.h`'s per-row rendering into reusable free functions

**Files:**
- Modify: `tools/psb_demo_app/tui/tab_monitor.h`

**Interfaces:**
- Produces: `monitorHeaderLabels(const ScannedData&) -> std::vector<std::string>`, `monitorOfflineRowCells(const std::string& chLabel, Component aliasInp, const std::string& aliasFallback, size_t numCols) -> std::vector<Element>`, `monitorRowCells(const std::string& chLabel, const ScannedData&, int ch, const MonitorRow&) -> std::vector<Element>`. Consumed by Task 2's `group_monitor.h`.

This is a **pure code-motion refactor** — every line of logic already exists in `makeMonitorTab`'s `Renderer` lambda (`tools/psb_demo_app/tui/tab_monitor.h:213-372` as of this writing); this task only moves it into named functions and calls them from the same place. No new automated test exists for this file (FTXUI rendering — this codebase's established convention), so correctness is verified by a live before/after comparison on real hardware in Step 3 below, not just a successful build.

- [ ] **Step 1: Add the three extracted functions**

In `tools/psb_demo_app/tui/tab_monitor.h`, insert these three functions right after the `MonitorRow` struct and before `makeMonitorRow` (i.e., right after line 16):

```cpp
// Column labels for the Monitor table's header row. The drive/enable
// column label and the current-unit-labeled columns depend on which
// capabilities are actually present among the currently-loaded channels, so
// this is computed from live ScannedData rather than being static — reused
// unmodified by group_monitor.h's aggregating table.
inline std::vector<std::string> monitorHeaderLabels(const ScannedData& data) {
    int n = data.numChannels();
    const char* vsetEnHeader = "Vset/En";
    {
        bool anyDrive = false, anyOut = false;
        for (int ch = 0; ch < n; ++ch) {
            uint16_t caps = data.chInfo[ch].chCapFlags;
            if (caps & CH_CAP_RAW_OUTPUT_DRIVE) anyDrive = true;
            if (caps & CH_CAP_OUTPUT_ENABLE)     anyOut   = true;
        }
        if (anyDrive)      vsetEnHeader = "Vset (V)";
        else if (anyOut)   vsetEnHeader = "En";
    }
    CurrentUnit iu = currentUnitFor(data.sysInfo.currentUnitExp);
    return {
        "", "Alias", vsetEnHeader, "Status", "Vop", "V (V)",
        std::string("I (") + iu.label + ")",
        "Ru", "Rd", std::string("Limit (") + iu.label + ")",
        "Fault", "Clear", "Save",
    };
}

// A single "channel unreachable" row — the same shape used both when an
// individual channel within a connected board goes offline (Monitor's own
// use, aliasInp always non-null there) and when a group's member channel's
// owning board is disconnected or missing entirely (group_monitor.h's use,
// where aliasInp is null whenever the owning board doesn't exist at all —
// there is no live widget to show, so the alias column falls back to plain
// text instead).
inline std::vector<Element> monitorOfflineRowCells(const std::string& chLabel,
                                                    Component aliasInp,
                                                    const std::string& aliasFallback,
                                                    size_t numCols) {
    std::vector<Element> cells;
    cells.push_back(text(chLabel) | center);
    cells.push_back((aliasInp ? aliasInp->Render() : text(aliasFallback)) | center);
    cells.push_back(text("OFFLINE") | color(Color::Red) | bold | center);
    for (size_t c = 3; c < numCols; ++c)
        cells.push_back(text("--") | color(Color::Red) | dim | center);
    return cells;
}

// One channel's live cell values — capability-conditional per column
// (hasVolt/hasCurr/hasDrive/hasOut), reused verbatim by both a board's own
// Monitor table and a group's aggregating table (group_monitor.h), so a
// group mixing channels from different board models "just works" through
// this same per-channel logic, no new heterogeneity handling needed.
inline std::vector<Element> monitorRowCells(const std::string& chLabel, const ScannedData& data,
                                            int ch, const MonitorRow& row) {
    const auto& ci = data.chInfo[ch];
    const uint16_t caps = ci.chCapFlags;
    bool hasOut   = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
    bool hasDrive = (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0;
    bool hasVolt  = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
    bool hasCurr  = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

    std::vector<Element> cells;
    cells.push_back(text(chLabel) | center);
    cells.push_back(row.aliasInp->Render() | center);
    if (hasDrive)
        cells.push_back(row.vsetInp->Render() | center);
    else if (hasOut)
        cells.push_back(row.outputEnabledCyc->Render() | center);
    else
        cells.push_back(text(unsupportedMonitorCellLabel()) | center);
    cells.push_back((hasOut || hasDrive) ? row.statusBtn->Render() | center
                                          : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasDrive ? text(fmtVoltage(ci.operationalTargetVoltageRaw)) | center
                             : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasVolt ? text(fmtVoltageBare(ci.voltageRaw)) | center
                            : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasCurr ? text(fmtCurrentBare(ci.currentRaw, data.sysInfo.currentUnitExp)) | center
                            : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasDrive ? row.rampUpInp->Render() | center
                             : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasDrive ? row.rampDownInp->Render() | center
                             : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(hasCurr ? row.iLimitInp->Render() | center
                            : text(unsupportedMonitorCellLabel()) | center);
    {
        ProtectionMode mode = data.chCfg[ch].iProtMode;
        Element faultEl;
        if (!hasCurr) {
            faultEl = text(unsupportedMonitorCellLabel());
        } else if (mode == ProtectionMode::FlagOnly) {
            faultEl = text(faultStr(ci.faultHistory));
        } else if (mode == ProtectionMode::ApplyOutputAction) {
            faultEl = text(faultStr(ci.activeFault));
        } else {
            faultEl = text(faultStr(ci.activeFault)) | dim;
        }
        cells.push_back(faultEl | center);
    }
    cells.push_back(hasCurr ? row.clearFaultBtn->Render() | center
                            : text(unsupportedMonitorCellLabel()) | center);
    cells.push_back(row.saveBtn->Render() | center);
    return cells;
}
```

- [ ] **Step 2: Rewrite `makeMonitorTab`'s `Renderer` to call the extracted functions**

Replace the body of `makeMonitorTab`'s `Renderer` lambda (`tools/psb_demo_app/tui/tab_monitor.h`, currently the block from `return Renderer(tableContainer, [=, &s]() {` through its closing `});` — lines 213-372 as of this writing) with:

```cpp
    return Renderer(tableContainer, [=, &s]() {
        int n = s.data.numChannels();

        if (!s.data.valid) {
            return vbox({
                text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | bold,
                filler(),
            });
        }

        if (n == 0)
            return text(" Discovering channels... ") | dim | center;

        // Connect scan stages every channel and publishes the whole table
        // atomically (see doFullScan() in tui/main.cpp) — show one clear
        // progress message for the whole duration rather than revealing
        // rows one at a time, which read as a torn/inconsistent table
        // rather than an obviously-still-loading one.
        if (!s.data.allChannelsLoaded()) {
            return text(" Scanning channels... " + std::to_string(s.data.scanProgress) +
                        "/" + std::to_string(n) + " ") | dim | center;
        }

        auto headerLabels = monitorHeaderLabels(s.data);

        std::vector<std::vector<Element>> grid;

        // Header row
        {
            std::vector<Element> hdr;
            for (const auto& h : headerLabels)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        // Data rows
        for (int ch = 0; ch < n; ++ch) {
            char chLabel[8];
            snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

            // Channel has failed enough consecutive status polls in a row
            // (see kChannelOfflineThreshold in tui/main.cpp) to be considered
            // genuinely unresponsive — show it as an error, not silently
            // continuing to display its last-known (now stale) values.
            if (s.data.chOffline[ch]) {
                grid.push_back(monitorOfflineRowCells(chLabel, rows->at(ch).aliasInp,
                                                      "CH" + std::to_string(ch), headerLabels.size()));
                continue;
            }

            grid.push_back(monitorRowCells(chLabel, s.data, ch, rows->at(ch)));
        }

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);

        for (size_t c = 0; c < headerLabels.size(); ++c)
            table.SelectColumn(static_cast<int>(c)).Decorate(flex);

        return vbox({
            table.Render(),
            filler(),
        });
    });
```

- [ ] **Step 3: Build and verify no behavior change**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests`
Expected: SUCCESS, no new warnings.

Run: `./tools/build/psb_modbus_core/tests/psb_tests`
Expected: PASS, no regressions (this file has no direct test coverage, but a build/link regression elsewhere would show up here).

Then verify the refactor is behavior-preserving live: launch `tools/bin/psb_demo_tui` under tmux (or reuse an existing session), connect at least one board, capture its Monitor tab's rendered output (`tmux capture-pane -p`) *before* this task's edit (check out the pre-edit revision temporarily, or capture from a build made before Step 1/2, if still running) and *after*, with the board in the same connected state. The two captures must be identical except for live-changing values (uptime, V/I readings) — column headers, row layout, alias column, and cell contents for a fixed channel must match exactly. If they don't, the extraction introduced a behavior change — find and fix it before proceeding; this is a pure refactor and must produce pixel-identical output for identical input state.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/tab_monitor.h
git commit -m "refactor(psb_demo_tui): extract tab_monitor.h's per-row cell logic into reusable functions (no behavior change)"
```

---

### Task 2: `group_monitor.h` — the group's aggregating dashboard

**Files:**
- Create: `tools/psb_demo_app/tui/group_monitor.h`

**Interfaces:**
- Consumes: `psb::GroupChannelRef` (`topology_config.h`), `BoardSession` (`board_session.h`), `MonitorRow`/`makeMonitorRow`/`monitorHeaderLabels`/`monitorOfflineRowCells`/`monitorRowCells` (Task 1's `tab_monitor.h`).
- Produces: `makeGroupDashboard(const std::string& groupName, const std::vector<psb::GroupChannelRef>& members, std::vector<std::unique_ptr<BoardSession>>& boards, std::function<void(const std::string&, int)> jumpToBoard) -> Component`. Consumed by Task 4's `main.cpp` wiring.

**Critical lifetime invariant** (read before writing any code here): each member row's widgets are built once, at construction, bound by reference into whichever `BoardSession` currently owns that member's nickname in `boards` at that moment. If that `BoardSession` is later erased from `boards` (a board removed from the running session — `tools/psb_demo_app/tui/main.cpp`'s `drainPendingRemovals`), those references dangle. This task does **not** solve that by itself — Task 4 wires a `refreshGroupDashboards()` call into every place `boards` can gain or lose an entry, which tears down and rebuilds every group dashboard (this function included) fresh. A `Component` built by this function is therefore only valid until the next such rebuild — never hold on to one past that point. This mirrors `board_switcher.h`'s own by-nickname re-scan pattern for reads, but widgets cannot be re-scanned per-render the way a read can — they must be rebuilt.

- [ ] **Step 1: Create `group_monitor.h`**

Create `tools/psb_demo_app/tui/group_monitor.h`:

```cpp
#pragma once

#include "board_session.h"
#include "tab_monitor.h"
#include "topology_config.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/table.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// Builds one group's aggregating Monitor-style table — one row per member
// channel, each built via the same makeMonitorRow() a board's own Monitor
// tab uses, bound to that member's real owning BoardSession's AppState/
// ConfigInputs. Editing a cell here writes through to the real board exactly
// as editing it on the board's own tab would: both are separate widget
// instances over the same underlying data, the same pattern ConfigInputs
// already establishes between a single board's Monitor and Channel tabs
// (see widgets.h) — "a view into the same underlying board channels," never
// a second poll or a second copy of the data.
//
// A member gets real, live-bound widgets whenever its owning board exists in
// `boards` at construction time (regardless of whether it happens to be
// connected right now) — mirroring how a board's own Monitor tab keeps a
// channel's widgets even after that channel goes offline, rather than
// discarding them. Each render then decides, fresh, whether that member is
// currently online (owning board connected, data valid, channel index still
// in range) or should show the offline placeholder — so a board connecting
// or disconnecting is reflected immediately without rebuilding anything. A
// member whose board doesn't exist in `boards` at all (removed from the
// topology entirely) gets no widgets — always the static offline
// placeholder. See this file's own comment block above (and Task 4) for why
// this Component must be rebuilt, not reused, whenever `boards` changes.
//
// jumpToBoard performs the compound "Go" action described in the design
// spec: switch the shared switcher selection to the member's board and set
// that board's own tab bar to the member's Channel tab.
inline Component makeGroupDashboard(const std::string& groupName,
                                    const std::vector<psb::GroupChannelRef>& members,
                                    std::vector<std::unique_ptr<BoardSession>>& boards,
                                    std::function<void(const std::string&, int)> jumpToBoard) {
    struct MemberRow {
        psb::GroupChannelRef ref;
        BoardSession* board = nullptr;  // nullptr if the board doesn't exist at all
        MonitorRow row;                 // only meaningful when board != nullptr
        Component jumpBtn;              // only meaningful when board != nullptr
    };
    auto memberRows = std::make_shared<std::vector<MemberRow>>();
    Components rowComps;

    // Safe without a lock on rt.boardsMutex — this scan, like board_switcher.h's
    // own by-nickname status-dot lookup, only ever runs on the UI thread, and
    // every mutation of `boards` (add/remove) is also UI-thread-only, so the
    // two can never interleave.
    for (const auto& ref : members) {
        BoardSession* owner = nullptr;
        for (auto& b : boards)
            if (b->nickname == ref.boardNickname) { owner = b.get(); break; }

        MemberRow mr;
        mr.ref = ref;
        mr.board = owner;
        if (owner) {
            mr.row = makeMonitorRow(*owner->appState, owner->inputs, ref.channelIndex, owner->saveChannelAlias);
            ButtonOption bopt{};
            bopt.transform = [owner, ch = ref.channelIndex](const EntryState& es) -> Element {
                // Re-read the alias fresh every render (not baked in at
                // construction) — it can change via this same row's own
                // aliasInp, or the board's own Monitor/Channel tab, at any
                // time after this button is built.
                std::string label = owner->inputs.chAlias[ch].empty()
                    ? "CH" + std::to_string(ch) : owner->inputs.chAlias[ch];
                auto e = text("[ " + label + " ]");
                if (es.focused) e = e | inverted;
                return e;
            };
            mr.jumpBtn = Button("", [jumpToBoard, nickname = ref.boardNickname, ch = ref.channelIndex] {
                jumpToBoard(nickname, ch);
            }, bopt);
            rowComps.push_back(mr.row.row);
            rowComps.push_back(mr.jumpBtn);
        }
        memberRows->push_back(std::move(mr));
    }

    auto tableContainer = Container::Vertical(rowComps);

    return Renderer(tableContainer, [=] {
        if (members.empty()) {
            return vbox({
                text(" No channels in this group yet \xe2\x80\x94 use the Group wizard to add some ") | center | dim,
                filler(),
            });
        }

        // Header labels are capability-derived (see monitorHeaderLabels) —
        // use whichever member's board happens to be connected first as the
        // source; if none are connected right now, the generic "Vset/En"
        // fallback shows, same as a just-connecting board's own Monitor tab.
        ScannedData emptyData;
        const ScannedData* headerSource = &emptyData;
        for (const auto& mr : *memberRows) {
            if (mr.board && mr.board->connected.load() && mr.board->data.valid) {
                headerSource = &mr.board->data;
                break;
            }
        }
        auto headerLabels = monitorHeaderLabels(*headerSource);
        headerLabels.push_back("Go");

        std::vector<std::vector<Element>> grid;
        {
            std::vector<Element> hdr;
            for (const auto& h : headerLabels)
                hdr.push_back(text(h) | bold | center);
            grid.push_back(std::move(hdr));
        }

        for (const auto& mr : *memberRows) {
            std::string chLabel = mr.ref.boardNickname + " CH" + std::to_string(mr.ref.channelIndex);
            bool online = mr.board && mr.board->connected.load() && mr.board->data.valid
                        && mr.ref.channelIndex < mr.board->data.numChannels();
            if (!online) {
                auto cells = monitorOfflineRowCells(
                    chLabel, mr.board ? mr.row.aliasInp : Component{}, chLabel, headerLabels.size() - 1);
                cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }
            auto cells = monitorRowCells(chLabel, mr.board->data, mr.ref.channelIndex, mr.row);
            cells.push_back(mr.jumpBtn->Render() | center);
            grid.push_back(std::move(cells));
        }

        auto table = Table(std::move(grid));
        table.SelectAll().Separator(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);
        for (size_t c = 0; c < headerLabels.size(); ++c)
            table.SelectColumn(static_cast<int>(c)).Decorate(flex);

        return vbox({
            text(" " + groupName + " ") | bold | center,
            table.Render(),
            filler(),
        });
    });
}

} // namespace psb::tui
```

- [ ] **Step 2: Build**

Run: `cmake --build tools/build --target psb_tests`
Expected: SUCCESS (this header isn't included anywhere yet, so this only confirms it doesn't break the existing build; it can't be exercised until Task 4 wires it in).

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/group_monitor.h
git commit -m "feat(psb_demo_tui): add group_monitor.h (not yet wired into main.cpp — expected unused until Task 4)"
```

---

### Task 3: `board_switcher.h` — two-section tab bar

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`

**Interfaces:**
- Produces: `makeBoardSwitcher(boards, screen, globalQuit, globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll)` — gains a new `ScreenInteractive& screen` parameter (needed by `jumpToBoard` to `PostEvent` after mutating state). `BoardSwitcher` gains `attachGroup`, `detachGroup`, `jumpToBoard` alongside the existing `attachBoard`/`detachBoard`.
- Consumed by: Task 4's `main.cpp` wiring (which also updates `buildRuntime`'s call site for the new `screen` parameter).

This task deliberately breaks `main.cpp`'s build (its one call to `makeBoardSwitcher` won't match the new signature) — Task 4 fixes it. `psb_tests` is unaffected (never links `main.cpp`).

**Design note — why two independent `Container::Tab`s, not one shared index:** the design spec's Architecture section sketches "one flat index space, groups-first-then-boards" driving a single `Container::Tab`. Implementing that literally — passing the *same* `int*` to both the groups `Menu` and the boards `Menu` — does not work with FTXUI's actual `Menu` semantics: `MenuBase::Render()` calls `Clamp()` on every render, which unconditionally clamps the shared selector into `[0, thisMenu'sOwnSize() - 1]` (verified in the vendored source, `menu.cpp`). With a shared index space, whichever `Menu` renders *last* on a given frame would silently clamp the shared value back into its own section's range even while the other section is genuinely showing — corrupting the "one shared active selection" behavior the design actually calls for. Instead: two independent `Container::Tab`s (groups, boards), each with its own local `Menu` selector, plus one `showingGroup` flag deciding which stack's `Render()` the outer view actually shows. Each `Menu`'s `on_change` (confirmed in the vendored source to fire only from `OnEvent()` — user-driven arrow/click — never from `Render()`/`Clamp()`, so this is safe) updates `showingGroup` when the user picks an entry in that section. This still delivers the spec's functional requirement — one shared active selection, whichever was clicked last wins — with substantially less risk than fighting `Menu`'s per-widget clamping.

- [ ] **Step 1: Replace the switcher/board Menu section with two sections**

In `tools/psb_demo_app/tui/board_switcher.h`, replace the signature (lines 35-38):

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       ScreenInteractive& screen,
                                       Component globalQuit, Component globalSetup, Component globalGroup,
                                       Component globalPreferences,
                                       Component globalConnectAll, Component globalDisconnectAll) {
```

Replace the `BoardSwitcher` struct (lines 29-33) with:

```cpp
struct BoardSwitcher {
    Component root;
    std::function<void(const std::string& nickname, Component dashboard)> attachBoard;
    std::function<void(const std::string& nickname)> detachBoard;
    std::function<void(const std::string& name, Component dashboard)> attachGroup;
    std::function<void(const std::string& name)> detachGroup;
    // Switches the shared selection to `nickname`'s board and sets that
    // board's own activeTab to channelIndex's Channel tab (1 + channelIndex,
    // since tabTitles[0] is always "Monitor" — see board_session.h's own
    // activeTab convention). A no-op if nickname isn't currently attached.
    std::function<void(const std::string& nickname, int channelIndex)> jumpToBoard;
};
```

Replace the body from `auto boardNames = ...` (line 39) through `auto dashboardStack = Container::Tab({}, activeBoard.get());` (line 112), i.e. the whole board-Menu-and-dashboard-stack construction, with:

```cpp
    // ---- Boards section (bottom) ----
    auto boardNames = std::make_shared<std::vector<std::string>>();
    for (auto& b : boards) boardNames->push_back(b->nickname);
    auto boardLocalIdx = std::make_shared<int>(0);

    // ---- Groups section (top) ----
    auto groupNames = std::make_shared<std::vector<std::string>>();
    auto groupLocalIdx = std::make_shared<int>(0);

    // Which section currently owns the shared active selection — see this
    // task's own design note above for why this replaces a literal shared
    // index. Starts on boards (false) since groups start empty; buildRuntime
    // (main.cpp) may switch this after populating any groups the loaded
    // topology already defines, but there is no strong reason to default
    // there even then — landing on the boards section first is a reasonable,
    // predictable default regardless of group count.
    auto showingGroup = std::make_shared<bool>(false);

    MenuOption switcherOpt = MenuOption::Vertical();
    // Looks up e.label (the nickname) in `boards` to read that board's live
    // connection state — Menu only ever hands the transform a label/active/
    // focused triple, never an index or the BoardSession itself, so a
    // by-nickname scan is the only way in. Boards are few in practice
    // (single digits), and this mirrors detachBoard's own by-nickname
    // lookup below. `boards` is `rt.boards` from main.cpp, captured by
    // reference the same way main.cpp's own Connect All/Disconnect All
    // buttons already capture it — safe without an extra lock: mutations to
    // the vector itself (add/remove) are, like this transform's reads, only
    // ever issued from the single UI thread, so they can't interleave with
    // this scan.
    //
    // Five states, reusing the exact fields/thresholds the per-board
    // dashboard's own status dot (board_dashboard.h) already computes, so
    // the two never disagree about what "connected" or "stale" means:
    //   green  ● : connected, polling normal
    //   red    ■ : connected, but data gone stale (polling failed)
    //   yellow ◐ : connecting (static glyph — no redraw ticker exists
    //              during an in-flight connect attempt for this to animate)
    //   red    ■ : not connected/connecting, last status was an error
    //              (connection failed)
    //   gray   ○ : not connected/connecting, no error (idle / never
    //              connected) — the loop's starting value below
    //
    // e.active reflects only this Menu's own local selector (boardLocalIdx)
    // — overridden to false whenever the groups section currently owns the
    // shared selection, so at most one row across both sections ever shows
    // as "the" active one.
    switcherOpt.entries_option.transform = [&boards, showingGroup](const EntryState& e) -> Element {
        Element dot = text("\xe2\x97\x8b ") | color(Color::GrayDark);  // ○
        for (auto& b : boards) {
            if (b->nickname != e.label) continue;
            bool connected = b->connected.load();
            bool connecting = b->connecting.load();
            bool stale = connected && b->data.sysStale(kSysStaleThreshold);
            if (connected && !stale) {
                dot = text("\xe2\x97\x8f ") | color(Color::Green);   // ●
            } else if (connected && stale) {
                dot = text("\xe2\x96\xa0 ") | color(Color::Red);     // ■
            } else if (connecting) {
                dot = text("\xe2\x97\x90 ") | color(Color::Yellow);  // ◐
            } else {
                std::lock_guard<std::mutex> lk(b->statusMutex);
                if (b->statusMsg.find("Error") != std::string::npos)
                    dot = text("\xe2\x96\xa0 ") | color(Color::Red); // ■
            }
            break;
        }
        bool active = e.active && !*showingGroup;
        auto t = hbox({ text("  "), dot, text(e.label + "  ") });
        if (active)                  t = t | bold | color(Color::Cyan);
        if (e.focused && !active)    t = t | inverted;
        if (!active && !e.focused)   t = t | dim;
        return t;
    };
    switcherOpt.on_change = [showingGroup] { *showingGroup = false; };
    auto switcherBar = Menu(boardNames.get(), boardLocalIdx.get(), switcherOpt);

    MenuOption groupOpt = MenuOption::Vertical();
    groupOpt.entries_option.transform = [showingGroup](const EntryState& e) -> Element {
        bool active = e.active && *showingGroup;
        auto t = hbox({ text("  "), text(e.label + "  ") });
        if (active)                  t = t | bold | color(Color::Cyan);
        if (e.focused && !active)    t = t | inverted;
        if (!active && !e.focused)   t = t | dim;
        return t;
    };
    groupOpt.on_change = [showingGroup] { *showingGroup = true; };
    auto groupMenu = Menu(groupNames.get(), groupLocalIdx.get(), groupOpt);

    auto boardDashboardStack = Container::Tab({}, boardLocalIdx.get());
    for (auto& b : boards) boardDashboardStack->Add(b->dashboard);
    auto groupDashboardStack = Container::Tab({}, groupLocalIdx.get());
```

- [ ] **Step 2: Update `globalMenuBar`/`mainContainer`, add the `jumpToBoard` closure, and rewrite the `root` Renderer**

Replace the `globalMenuBar`/`mainContainer` construction (lines 115-124 in the pre-edit file) with:

```cpp
    // Order matches the visual/Tab order the Renderer below produces
    // (Setup, Group, Preferences, then Connect All/Disconnect All/Quit
    // pushed to the right corner) — Container children order drives
    // keyboard Tab traversal even though the actual visual arrangement is
    // decided entirely in the Renderer, so keeping the two in sync avoids
    // Tab jumping in an order that doesn't match what's on screen.
    auto globalMenuBar = Container::Horizontal(
        {globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll, globalQuit});

    // 4-slot scheme: 0=globalMenuBar, 1=groupMenu, 2=switcherBar,
    // 3=dashboardStack (whichever of boardDashboardStack/groupDashboardStack
    // is currently showing, decided by showingGroup — see the Renderer
    // below). Groups are always attached after this constructor returns
    // (main.cpp's refreshGroupDashboards, called right after this function)
    // — groupNames is provably empty here, so defaulting focus on it would
    // always be dead code. Default mirrors Phase 2's own reasoning instead
    // (a Menu with very few/zero entries unconditionally swallows
    // Event::Return and doesn't meaningfully handle Up/Down — see
    // MenuBase::OnEvent in the vendored FTXUI source): the boards Menu if
    // there's more than one board, else straight to the dashboard so keys
    // reach it immediately. A user who already has groups defined can
    // still reach them with one Tab/Shift-Tab press.
    auto mainSelected = std::make_shared<int>(boardNames->size() > 1 ? 2 : 3);

    auto mainContainer = Container::Vertical(
        {globalMenuBar, groupMenu, switcherBar, boardDashboardStack}, mainSelected.get());
```

Then replace the `root` Renderer (the block starting `auto root = Renderer(mainContainer, [switcherBar, dashboardStack, ...`, through its closing `});`, i.e. the pre-edit lines 161-182) with:

```cpp
    auto root = Renderer(mainContainer, [switcherBar, groupMenu, boardDashboardStack, groupDashboardStack,
                                         globalSetup, globalGroup, globalPreferences,
                                         globalConnectAll, globalDisconnectAll, globalQuit,
                                         boardNames, groupNames, showingGroup, mainSelected] {
        bool showSwitcherSection = !groupNames->empty() || boardNames->size() > 1;
        Component activeStack = *showingGroup ? groupDashboardStack : boardDashboardStack;
        if (!showSwitcherSection) {
            return vbox({ activeStack->Render() | flex });
        }
        Elements sideParts;
        if (!groupNames->empty()) {
            sideParts.push_back(text(" Groups ") | dim);
            sideParts.push_back(groupMenu->Render());
            sideParts.push_back(separator());
        }
        sideParts.push_back(text(" Boards ") | dim);
        sideParts.push_back(switcherBar->Render());
        return vbox({
            hbox({
                globalSetup->Render(), text(" "), globalGroup->Render(), text(" "), globalPreferences->Render(),
                filler(),
                globalConnectAll->Render(), text(" "), globalDisconnectAll->Render(), text(" "),
                globalQuit->Render(),
            }),
            separator(),
            hbox({
                vbox(std::move(sideParts)),
                separator(),
                activeStack->Render() | flex,
            }) | flex,
        });
    });
```

Now add the `attachGroup`/`detachGroup`/`jumpToBoard` closures. Replace the final block of the function — `attachBoard`/`detachBoard`'s definitions and the `return BoardSwitcher{...}` (pre-edit lines 184-222) — with:

```cpp
    auto attachBoard = [boardNames, boardDashboardStack](const std::string& nickname, Component dashboard) {
        boardNames->push_back(nickname);
        boardDashboardStack->Add(std::move(dashboard));
    };

    // Symmetric to attachBoard. FTXUI's Container::Tab indexes its active
    // child via `*selector_ % children_.size()` (confirmed in the vendored
    // source, container.cpp) — not a clamp, a modulo — so after an erase,
    // boardLocalIdx needs explicit adjustment or it can silently jump to an
    // unrelated board rather than either staying on the same one or landing
    // on a sensible neighbor. The two adjustments below cover every case:
    // if the removed tab was before the active one, decrement to keep
    // tracking the same logical board (which just shifted down one index);
    // then clamp into the new valid range (handles the removed tab being
    // the active one, especially if it was also the last slot).
    auto detachBoard = [boardNames, boardDashboardStack, boardLocalIdx](const std::string& nickname) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            boardDashboardStack->ChildAt(i)->Detach();
            boardNames->erase(boardNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*boardLocalIdx > removedIdx) --*boardLocalIdx;
            if (*boardLocalIdx >= static_cast<int>(boardNames->size()))
                *boardLocalIdx = std::max(0, static_cast<int>(boardNames->size()) - 1);
            return;
        }
    };

    auto attachGroup = [groupNames, groupDashboardStack](const std::string& name, Component dashboard) {
        groupNames->push_back(name);
        groupDashboardStack->Add(std::move(dashboard));
    };

    // Same index-shift reasoning as detachBoard, retargeted at the groups
    // section. Falls back to the boards section (showingGroup = false) if
    // the group being removed was the one currently showing, or if this
    // was the last group — mirrors detachBoard's own "force a sane visible
    // section" adjustment (Phase 2's mainSelected-forcing precedent) rather
    // than leaving the view pointed at a section that's about to render
    // empty.
    auto detachGroup = [groupNames, groupDashboardStack, groupLocalIdx, showingGroup]
                       (const std::string& name) {
        for (size_t i = 0; i < groupNames->size(); ++i) {
            if ((*groupNames)[i] != name) continue;
            bool wasShowingThis = *showingGroup && (*groupLocalIdx == static_cast<int>(i));
            groupDashboardStack->ChildAt(i)->Detach();
            groupNames->erase(groupNames->begin() + i);
            int removedIdx = static_cast<int>(i);
            if (*groupLocalIdx > removedIdx) --*groupLocalIdx;
            if (*groupLocalIdx >= static_cast<int>(groupNames->size()))
                *groupLocalIdx = std::max(0, static_cast<int>(groupNames->size()) - 1);
            if (wasShowingThis || groupNames->empty()) *showingGroup = false;
            return;
        }
    };

    auto jumpToBoard = [&boards, boardNames, boardLocalIdx, showingGroup, mainSelected, &screen]
                       (const std::string& nickname, int channelIndex) {
        for (size_t i = 0; i < boardNames->size(); ++i) {
            if ((*boardNames)[i] != nickname) continue;
            *boardLocalIdx = static_cast<int>(i);
            *showingGroup = false;
            break;
        }
        for (auto& b : boards) {
            if (b->nickname != nickname) continue;
            int maxTab = static_cast<int>(b->tabTitles.size()) - 1;
            b->activeTab = std::min(1 + channelIndex, std::max(0, maxTab));
            break;
        }
        *mainSelected = 3;  // land keyboard focus on the dashboard itself
        screen.PostEvent(Event::Custom);
    };

    return BoardSwitcher{root, attachBoard, detachBoard, attachGroup, detachGroup, jumpToBoard};
```

- [ ] **Step 3: Build `psb_tests` to confirm it's unaffected**

Run: `cmake --build tools/build --target psb_tests`
Expected: PASS (unchanged — `psb_tests` never links `main.cpp` or `board_switcher.h`).

- [ ] **Step 4: Confirm the app now fails to build (expected)**

Run: `cmake --build tools/build --target psb_demo_tui`
Expected: FAIL — `main.cpp`'s call to `makeBoardSwitcher` (via `buildRuntime`) is missing the new `screen` argument and the caller doesn't yet use `attachGroup`/`detachGroup`/`jumpToBoard`. Task 4 fixes this.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h
git commit -m "feat(psb_demo_tui): split board_switcher.h into groups+boards sections with a shared active selection (main.cpp not yet updated — expected build break until Task 4)"
```

---

### Task 4: Wire group dashboards into `main.cpp`

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `psb::tui::makeGroupDashboard` (Task 2); `BoardSwitcher::attachGroup/detachGroup/jumpToBoard`, `makeBoardSwitcher`'s new `screen` parameter (Task 3).
- Produces: `Runtime::attachedGroupNames`, `refreshGroupDashboards(Runtime&, const psb::TopologyConfig&, ScreenInteractive&)` — the single reconciliation point called from every place `rt.boards` or `topo.groups` can change while the app is running.

- [ ] **Step 1: Add the include**

In `tools/psb_demo_app/tui/main.cpp`, add right after `#include "group_wizard_screen.h"`:

```cpp
#include "group_wizard_screen.h"
#include "group_monitor.h"
```

- [ ] **Step 2: Add `Runtime::attachedGroupNames`**

In `main.cpp`'s `Runtime` struct (currently lines 53-67), add a new field right after `pendingRemovals`:

```cpp
struct Runtime {
    std::vector<std::unique_ptr<psb::tui::BusWorker>> busWorkers;
    std::vector<std::unique_ptr<psb::tui::BoardSession>> boards;
    std::mutex boardsMutex;
    psb::tui::BoardSwitcher switcher;
    std::thread animThread;
    std::vector<PendingRemoval> pendingRemovals;
    // Tracks which group dashboards are currently attached to `switcher`,
    // purely so refreshGroupDashboards() (below) knows what to detach
    // before reattaching fresh copies — see that function's own comment.
    std::vector<std::string> attachedGroupNames;
};
```

- [ ] **Step 3: Add `refreshGroupDashboards`**

Add this new function right after `removeGoneBoardsLive`'s definition (which currently ends at line 534, right before `int main(...)`):

```cpp
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
    for (const auto& name : rt.attachedGroupNames)
        rt.switcher.detachGroup(name);
    rt.attachedGroupNames.clear();

    for (const auto& g : topo.groups) {
        auto dash = psb::tui::makeGroupDashboard(g.name, g.channels, rt.boards, rt.switcher.jumpToBoard);
        rt.switcher.attachGroup(g.name, dash);
        rt.attachedGroupNames.push_back(g.name);
    }
    screen.PostEvent(Event::Custom);
}
```

- [ ] **Step 4: Update `buildRuntime`'s `makeBoardSwitcher` call and populate initial groups**

Replace the `rt.switcher = ...` line (currently lines 388-389, right before `buildRuntime`'s closing brace):

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, screen, globalQuit, globalSetup, globalGroup,
                                              globalPreferences, globalConnectAll, globalDisconnectAll);
    refreshGroupDashboards(rt, topo, screen);
}
```

- [ ] **Step 5: Hook `drainPendingRemovals`**

Replace `drainPendingRemovals`'s signature and top of body (currently lines 193-198) and its closing brace, to track whether anything was actually removed and refresh once at the end:

```cpp
void drainPendingRemovals(Runtime& rt, psb::TopologyConfig& topo,
                          ScreenInteractive& screen, std::atomic<bool>& running) {
    bool anyRemoved = false;
    for (size_t i = 0; i < rt.pendingRemovals.size(); ) {
        auto& pr = rt.pendingRemovals[i];
        if (!pr.done->load()) { ++i; continue; }
        anyRemoved = true;

        rt.switcher.detachBoard(pr.board->nickname);
```

(The rest of the loop body — bus/board bookkeeping through `rt.pendingRemovals.erase(...)` — is unchanged.) Then replace the function's closing brace (currently just `}` right after the loop, line 262) with:

```cpp
    }
    if (anyRemoved) refreshGroupDashboards(rt, topo, screen);
}
```

- [ ] **Step 6: Hook `onMidSessionFinish`**

In `onMidSessionFinish` (its `ConnectNow` branch, currently around lines 772-776), add a call right after `currentTopologyPath = midSessionWiz->topologyPath;`:

```cpp
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences, saveChannelAliasToTopology);
            topo = midSessionWiz->topo;
            currentTopologyPath = midSessionWiz->topologyPath;
            refreshGroupDashboards(rt, topo, screen);
        }
```

- [ ] **Step 7: Extend `onGroupSetupFinish`**

Replace `onGroupSetupFinish`'s body (currently lines 850-855):

```cpp
    auto onGroupSetupFinish = [showGroupSetup, groupWiz, &rt, &topo, &currentTopologyPath, &screen] {
        topo = groupWiz->topo;
        currentTopologyPath = groupWiz->topologyPath;
        refreshGroupDashboards(rt, topo, screen);
        *showGroupSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

(Adds `&rt` to the capture list — everything else is the prior body plus the one new line.)

- [ ] **Step 8: Build**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests`
Expected: SUCCESS, no new warnings.

Run: `./tools/build/psb_modbus_core/tests/psb_tests`
Expected: PASS, no regressions.

- [ ] **Step 9: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire group dashboards into main.cpp, refreshing on every board/group change"
```

---

### Task 5: Manual verification

No automated coverage exists for FTXUI rendering, live multi-board aggregation, or the sidebar's two-section split — matching every other UI feature in this codebase and Task 1/2's own notes above. Protect the real topology file first, per this session's established practice:

```bash
cp ~/.psb_demo_app/topology.toml /tmp/user_topology_backup_phase3.toml 2>/dev/null || echo "(no existing file — nothing to back up)"
```

- [ ] **Step 1: Baseline regression check — zero groups, single board**

Launch with a single-board topology (no groups defined). Verify the dashboard renders exactly as it did before this phase (no visible "Groups"/"Boards" section labels, no sidebar at all) — this is the Global Constraint carried over from Phase 2 ("pixel-identical single-board rendering... whenever fewer than two boards exist"), now conditioned on `!hasGroups` too.

- [ ] **Step 2: Baseline regression check — multi-board, zero groups**

Load the existing multi-board topology (2+ boards, no groups). Verify the sidebar shows only a "Boards" section (no "Groups" label, no divider) exactly as Phase 2 left it.

- [ ] **Step 3: Create a group, verify it appears live without restart**

With 2+ boards connected, open the Group wizard (Task 5 of the Phase 2 plan already covered creating a group and adding a channel — reuse that flow here), add a group with at least 2 member channels from different boards, click Save, then Exit. Verify: without restarting the app, a "Groups" section now appears above "Boards" in the sidebar, with the new group's name listed. Click it — verify the aggregating table shows both member channels with correct live V/I/status values matching what each channel's own board Monitor tab shows.

- [ ] **Step 4: Verify the jump link**

In the group's table, click the "Go" button on one member's row. Verify: the sidebar's selection switches to that member's own board, and that board's tab bar switches to the corresponding Channel tab (not Monitor) — both in one click, per the "compound jump" requirement.

- [ ] **Step 5: Verify the offline-member placeholder updates live**

While the group's table is showing, disconnect one member's board (via that board's own Disconnect button — no need to reopen the Group wizard). Verify the group's table immediately shows that member as an offline placeholder row (red "OFFLINE", "--" cells, no "Go" button), without needing to reopen the group wizard or restart. Reconnect it and verify the row returns to live data.

- [ ] **Step 6: Verify a board removal doesn't crash a group referencing it**

With a group still referencing a board's channel, remove that board entirely (its dashboard's own "Remove" button — requires 2+ boards, per the existing Global Constraint). Verify: the app does not crash, and the group's table now shows that member as a permanent offline placeholder (board no longer exists at all).

- [ ] **Step 7: Verify persistence across restart**

Quit and relaunch the app with the same topology file. Verify the previously-created group(s) reappear in the sidebar immediately (built from `topo.groups` at startup), matching what was last saved.

- [ ] **Step 8: Restore the real topology file**

```bash
cp /tmp/user_topology_backup_phase3.toml ~/.psb_demo_app/topology.toml 2>/dev/null || rm -f ~/.psb_demo_app/topology.toml
rm -f /tmp/user_topology_backup_phase3.toml
```

No commit for this task — it's verification only. If any step surfaces a bug, fix it with its own dedicated commit (following systematic-debugging if the root cause isn't immediately obvious) before proceeding to `finishing-a-development-branch`.

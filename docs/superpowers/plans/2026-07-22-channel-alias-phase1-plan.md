# Channel Alias Names (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every channel a user-editable alias name, shown and editable inline in both the Monitor table and its own Channel tab, persisted in the topology config file and auto-saved on edit.

**Architecture:** `BoardConfig` (topology_config.h) gains a `channelAliases` vector, serialized as a new TOML key alongside `nickname`/`slave_id`. `ConfigInputs` (widgets.h) — the same struct that already backs every other per-channel editable field shared between the Monitor and Channel tabs — gains a `chAlias[MAX_CHANNELS]` array. A new closure, `BoardSession::saveChannelAlias`, threaded down from a single closure constructed in `main()` (which has the live `topo`/`topologyPath`), writes an edited alias back into the topology and saves it immediately — mirroring how `BoardSession::connect`/`disconnect` are already populated externally.

**Tech Stack:** C++17, FTXUI (vendored), toml++ (vendored), the existing `BoardSession`/`ConfigInputs`/`TopologyConfig` machinery from prior rounds.

**This is Phase 1 of 3** (see `docs/superpowers/specs/2026-07-22-channel-alias-and-groups-design.md`). Phases 2 (group wizard) and 3 (group sidebar view) are separate plans, written after this one lands.

## Global Constraints

- Alias edits auto-save to the topology file immediately on commit (Enter) — never staged behind the Topology wizard's own Save button.
- An unset alias shows as an empty input with the canonical `CHn` name as placeholder text — never pre-filled with real `CHn` content.
- `channelAliases` loads once into each `BoardSession` at construction time (`buildRuntime`'s initial sweep, `applyNewBoardsLive`'s hot-attach) — never re-fetched on reconnect, since it's config data, not live hardware state.
- The Monitor tab's alias input and the Channel tab's alias input for the same channel bind to the *same* `ConfigInputs::chAlias[ch]` string — editing either one is reflected on the other's next render, matching how every other per-channel field (`targetV`, `ruStep`, ...) already works across both tabs.

---

## Task 1: `topology_config.h`/`.cpp` — persist per-channel aliases

**Files:**
- Modify: `tools/psb_modbus_core/topology_config.h`
- Modify: `tools/psb_modbus_core/topology_config.cpp`

**Interfaces:**
- Produces: `BoardConfig::channelAliases` (`std::vector<std::string>`, index = channel number, `""` = no alias). Serialized/deserialized as a new `channel_aliases` array-of-strings key inside each `[[bus.board]]` TOML table.

- [ ] **Step 1: Add the field**

Edit `tools/psb_modbus_core/topology_config.h`, find:

```cpp
struct BoardConfig {
    std::string nickname;
    int slaveId = 1;
};
```

Change to:

```cpp
struct BoardConfig {
    std::string nickname;
    int slaveId = 1;
    // Index = channel number; "" means no alias set for that channel. Only
    // as long as the highest-set channel needs it — never pre-sized to
    // MAX_CHANNELS on disk.
    std::vector<std::string> channelAliases;
};
```

- [ ] **Step 2: Load it**

Edit `tools/psb_modbus_core/topology_config.cpp`, find:

```cpp
                    BoardConfig board;
                    board.nickname = (*boardTbl)["nickname"].value_or(std::string(""));
                    board.slaveId = static_cast<int>((*boardTbl)["slave_id"].value_or(1));
                    bus.boards.push_back(std::move(board));
```

Change to:

```cpp
                    BoardConfig board;
                    board.nickname = (*boardTbl)["nickname"].value_or(std::string(""));
                    board.slaveId = static_cast<int>((*boardTbl)["slave_id"].value_or(1));
                    if (auto aliasArr = (*boardTbl)["channel_aliases"].as_array()) {
                        for (auto&& aliasNode : *aliasArr)
                            board.channelAliases.push_back(aliasNode.value_or(std::string("")));
                    }
                    bus.boards.push_back(std::move(board));
```

- [ ] **Step 3: Save it**

Edit `tools/psb_modbus_core/topology_config.cpp`, find:

```cpp
            for (const auto& board : bus.boards) {
                toml::table boardTbl;
                boardTbl.insert_or_assign("nickname", board.nickname);
                boardTbl.insert_or_assign("slave_id", board.slaveId);
                boardArr.push_back(std::move(boardTbl));
            }
```

Change to:

```cpp
            for (const auto& board : bus.boards) {
                toml::table boardTbl;
                boardTbl.insert_or_assign("nickname", board.nickname);
                boardTbl.insert_or_assign("slave_id", board.slaveId);
                if (!board.channelAliases.empty()) {
                    toml::array aliasArr;
                    for (const auto& alias : board.channelAliases)
                        aliasArr.push_back(alias);
                    boardTbl.insert_or_assign("channel_aliases", std::move(aliasArr));
                }
                boardArr.push_back(std::move(boardTbl));
            }
```

- [ ] **Step 4: Build to confirm no breakage**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests -j$(nproc) 2>&1 | tail -20`
Expected: clean build. `BoardConfig{"nickname", 1}`-style aggregate-init call sites elsewhere (e.g. the existing test file) still compile — C++ aggregate init allows fewer initializers than members, so `channelAliases` simply defaults to empty.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_modbus_core/topology_config.h tools/psb_modbus_core/topology_config.cpp
git commit -m "feat(psb_modbus_core): persist per-channel aliases in BoardConfig"
```

---

## Task 2: `test_topology_config.cpp` — round-trip test for aliases

**Files:**
- Modify: `tools/psb_modbus_core/tests/test_topology_config.cpp`

**Interfaces:**
- Consumes: `BoardConfig::channelAliases` (Task 1).

- [ ] **Step 1: Add a test case**

Edit `tools/psb_modbus_core/tests/test_topology_config.cpp`, find:

```cpp
TEST_CASE("TopologyConfig — load returns nullopt for missing file", "[topology_config]") {
```

Change to (inserting a new test case immediately before it):

```cpp
TEST_CASE("TopologyConfig — round trip preserves per-channel aliases", "[topology_config]") {
    psb::TopologyConfig cfg;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.baudRate = 115200;
    psb::BoardConfig board;
    board.nickname = "hvb-bench";
    board.slaveId = 1;
    board.channelAliases = {"Cell1", "", "Cell3"};
    bus.boards.push_back(board);
    cfg.buses.push_back(bus);

    const std::string path = "/tmp/psb_topology_test_aliases.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->buses.size() == 1);
    REQUIRE(loaded->buses[0].boards.size() == 1);
    REQUIRE(loaded->buses[0].boards[0].channelAliases.size() == 3);
    CHECK(loaded->buses[0].boards[0].channelAliases[0] == "Cell1");
    CHECK(loaded->buses[0].boards[0].channelAliases[1] == "");
    CHECK(loaded->buses[0].boards[0].channelAliases[2] == "Cell3");

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — a board with no aliases round-trips with an empty channelAliases", "[topology_config]") {
    auto cfg = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
    const std::string path = "/tmp/psb_topology_test_no_aliases.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->buses[0].boards.size() == 1);
    CHECK(loaded->buses[0].boards[0].channelAliases.empty());

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — load returns nullopt for missing file", "[topology_config]") {
```

- [ ] **Step 2: Run the new tests**

Run: `cd tools && cmake --build build --target psb_tests -j$(nproc) && ./build/psb_modbus_core/tests/psb_tests "[topology_config]" -v`
Expected: PASS, all `[topology_config]`-tagged tests including the two new ones.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_modbus_core/tests/test_topology_config.cpp
git commit -m "test(psb_modbus_core): cover per-channel alias round-trip in TopologyConfig"
```

---

## Task 3: `widgets.h` — `ConfigInputs::chAlias`

**Files:**
- Modify: `tools/psb_demo_app/tui/widgets.h`

**Interfaces:**
- Produces: `ConfigInputs::chAlias[MAX_CHANNELS]` — the shared live-edit state both the Monitor and Channel tab's alias `Input` widgets bind to.

- [ ] **Step 1: Add the field**

Edit `tools/psb_demo_app/tui/widgets.h`, find:

```cpp
struct ConfigInputs {
    // Monitor table editable columns
    std::string targetV [MAX_CHANNELS];  // Vset  — configured target voltage in V — DAC channels
```

Change to:

```cpp
struct ConfigInputs {
    // Monitor table editable columns
    std::string chAlias [MAX_CHANNELS];  // User-editable display name, "" = unset (shows CHn placeholder)
    std::string targetV [MAX_CHANNELS];  // Vset  — configured target voltage in V — DAC channels
```

- [ ] **Step 2: Build**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: clean build — purely additive, nothing reads or writes the new field yet.

- [ ] **Step 3: Commit**

```bash
git add tools/psb_demo_app/tui/widgets.h
git commit -m "feat(psb_demo_tui): add ConfigInputs::chAlias (not yet wired to any widget)"
```

---

## Task 4: `board_session.h` — `saveChannelAlias` member, alias-aware tab titles

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h`

**Interfaces:**
- Produces: `BoardSession::saveChannelAlias` (`std::function<void(int ch, const std::string& alias)>`, populated externally by `board_dashboard.h` in Task 7 — empty/never-called until then, same pattern as `connect`/`disconnect`). `rebuildChannelTitles`'s signature gains a `const std::string* chAlias` parameter.

- [ ] **Step 1: Add the `BoardSession` member**

Edit `tools/psb_demo_app/tui/board_session.h`, find:

```cpp
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

Change to:

```cpp
    // Set by makeBoardDashboard() to the same doConnect/doDisconnect
    // closures its own Connect/Disconnect toggle button already uses — lets
    // an external caller (main.cpp's Connect All/Disconnect All) trigger
    // identical per-board connection logic without duplicating it or
    // restructuring ownership. Empty (never called) until the dashboard is
    // built, which always happens before these could be reached.
    std::function<void()> connect;
    std::function<void()> disconnect;

    // Set by makeBoardDashboard() (Task 7) to a closure bound to this
    // board's own nickname, wrapping a single save-to-topology closure
    // main() constructs once (it has the live TopologyConfig/path this
    // board's alias edits need to land in) — lets the Monitor/Channel tabs'
    // alias Input widgets (Task 5/6) commit an edit without needing to know
    // anything about topology files themselves. Empty until the dashboard
    // is built, same as connect/disconnect above.
    std::function<void(int ch, const std::string& alias)> saveChannelAlias;
};
```

- [ ] **Step 2: Make `rebuildChannelTitles` alias-aware**

Find:

```cpp
inline void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels) {
    titles.resize(1);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}
```

Change to:

```cpp
inline void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels,
                                 const std::string* chAlias) {
    titles.resize(1);
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back(!chAlias[ch].empty() ? chAlias[ch] : "CH" + std::to_string(ch));
}
```

- [ ] **Step 3: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -20`
Expected: FAIL — `board_dashboard.h`'s existing `rebuildChannelTitles(board.tabTitles, nc);` call still passes only 2 arguments. Confirm this is the *only* error — Task 7 fixes it.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/board_session.h
git commit -m "feat(psb_demo_tui): add BoardSession::saveChannelAlias, make rebuildChannelTitles alias-aware (callers not yet updated)"
```

---

## Task 5: `tab_monitor.h` — Alias column

**Files:**
- Modify: `tools/psb_demo_app/tui/tab_monitor.h`

**Interfaces:**
- Consumes: `ConfigInputs::chAlias` (Task 3).
- Produces: `makeMonitorRow`/`makeMonitorTab` gain a new trailing parameter, `std::function<void(int, const std::string&)> saveAlias` — the per-board closure Task 7 wires up as `board.saveChannelAlias`.

- [ ] **Step 1: Add `aliasInp` to `MonitorRow` and construct it in `makeMonitorRow`**

Edit `tools/psb_demo_app/tui/tab_monitor.h`, find:

```cpp
struct MonitorRow {
    Component row;  // Container::Horizontal — focus chain
    Component statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn;
};

// Build one row — creates all widgets, returns them in a MonitorRow.
inline MonitorRow makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch) {
```

Change to:

```cpp
struct MonitorRow {
    Component row;  // Container::Horizontal — focus chain
    Component statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn;
    Component aliasInp;
};

// Build one row — creates all widgets, returns them in a MonitorRow.
inline MonitorRow makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch,
                                 std::function<void(int, const std::string&)> saveAlias) {
```

Find:

```cpp
    auto saveBtn = ActionButton("Save", [&s, &inputs, ch, refreshFull] {
        saveChannelConfig(s, inputs, ch, refreshFull);
    });

    auto rowWidgets = Container::Horizontal({
        vsetInp, outputEnabledCyc, statusBtn, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
    });

    return MonitorRow{
        CatchEvent(rowWidgets, [&s, ch](Event e) {
            if (e.is_mouse()) return false;  // let mouse events pass; parent checks bounds
            return !s.data.valid || ch >= s.data.numChannels();
        }),
        statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
    };
}
```

Change to:

```cpp
    auto saveBtn = ActionButton("Save", [&s, &inputs, ch, refreshFull] {
        saveChannelConfig(s, inputs, ch, refreshFull);
    });

    // No hardware write — just a display name, so no CommitInput try/catch
    // body needed beyond forwarding the committed string straight to the
    // save closure. Placeholder is the canonical CHn name, per the
    // confirmed "empty means unset, never pre-filled" design.
    auto aliasInp = CommitInput(&inputs.chAlias[ch], "CH" + std::to_string(ch), [&inputs, ch, saveAlias] {
        saveAlias(ch, inputs.chAlias[ch]);
    });

    auto rowWidgets = Container::Horizontal({
        aliasInp, vsetInp, outputEnabledCyc, statusBtn, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
    });

    return MonitorRow{
        CatchEvent(rowWidgets, [&s, ch](Event e) {
            if (e.is_mouse()) return false;  // let mouse events pass; parent checks bounds
            return !s.data.valid || ch >= s.data.numChannels();
        }),
        statusBtn, vsetInp, outputEnabledCyc, rampUpInp, rampDownInp, iLimitInp, clearFaultBtn, saveBtn,
        aliasInp,
    };
}
```

- [ ] **Step 2: Thread `saveAlias` through `makeMonitorTab` and add the header/cell**

Find:

```cpp
inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs) {
    auto rows = std::make_shared<std::vector<MonitorRow>>();
    Components rowComps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        auto r = makeMonitorRow(s, inputs, ch);
        rowComps.push_back(r.row);
        rows->push_back(std::move(r));
    }
```

Change to:

```cpp
inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs,
                                std::function<void(int, const std::string&)> saveAlias) {
    auto rows = std::make_shared<std::vector<MonitorRow>>();
    Components rowComps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        auto r = makeMonitorRow(s, inputs, ch, saveAlias);
        rowComps.push_back(r.row);
        rows->push_back(std::move(r));
    }
```

Find:

```cpp
        const std::vector<std::string> headers = {
            "", vsetEnHeader, "Status", "Vop", "V (V)",
            std::string("I (") + iu.label + ")",
            "Ru", "Rd", std::string("Limit (") + iu.label + ")",
            "Fault", "Clear", "Save",
        };
```

Change to:

```cpp
        const std::vector<std::string> headers = {
            "", "Alias", vsetEnHeader, "Status", "Vop", "V (V)",
            std::string("I (") + iu.label + ")",
            "Ru", "Rd", std::string("Limit (") + iu.label + ")",
            "Fault", "Clear", "Save",
        };
```

Find:

```cpp
            if (s.data.chOffline[ch]) {
                std::vector<Element> cells;
                cells.push_back(text(chLabel) | center);
                cells.push_back(text("OFFLINE") | color(Color::Red) | bold | center);
                for (size_t c = 2; c < headers.size(); ++c)
                    cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }
```

Change to:

```cpp
            if (s.data.chOffline[ch]) {
                std::vector<Element> cells;
                cells.push_back(text(chLabel) | center);
                cells.push_back(rows->at(ch).aliasInp->Render() | center);
                cells.push_back(text("OFFLINE") | color(Color::Red) | bold | center);
                for (size_t c = 3; c < headers.size(); ++c)
                    cells.push_back(text("--") | color(Color::Red) | dim | center);
                grid.push_back(std::move(cells));
                continue;
            }
```

Find:

```cpp
            std::vector<Element> cells;
            cells.push_back(text(chLabel) | center);
            // Vset slot: target-voltage input (DAC channels), on/off cycler
```

Change to:

```cpp
            std::vector<Element> cells;
            cells.push_back(text(chLabel) | center);
            cells.push_back(rows->at(ch).aliasInp->Render() | center);
            // Vset slot: target-voltage input (DAC channels), on/off cycler
```

- [ ] **Step 3: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: FAIL — `board_dashboard.h`'s existing `makeMonitorTab(*board.appState, board.inputs)` call still passes only 2 arguments. Confirm this is the *only* error (beyond Task 4's still-unresolved `rebuildChannelTitles` one) — Task 7 fixes both.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/tab_monitor.h
git commit -m "feat(psb_demo_tui): add Alias column to the Monitor table (caller not yet updated)"
```

---

## Task 6: `tab_channel.h` — alias field synced with Monitor

**Files:**
- Modify: `tools/psb_demo_app/tui/tab_channel.h`

**Interfaces:**
- Consumes: `ConfigInputs::chAlias` (Task 3) — the *same* backing field Task 5's Monitor-table alias input binds to.
- Produces: `makeChannelTab` gains a new trailing parameter, `std::function<void(int, const std::string&)> saveAlias`.

- [ ] **Step 1: Add the parameter and construct a second `Input` bound to the same field**

Edit `tools/psb_demo_app/tui/tab_channel.h`, find:

```cpp
inline Component makeChannelTab(AppState& s, ConfigInputs& inputs, int ch) {
```

Change to:

```cpp
inline Component makeChannelTab(AppState& s, ConfigInputs& inputs, int ch,
                                std::function<void(int, const std::string&)> saveAlias) {
```

Find:

```cpp
    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0", onTarget);
```

Change to (inserting the alias input's construction immediately before this line):

```cpp
    // Same backing field (inputs.chAlias[ch]) the Monitor table's own alias
    // Input binds to — a second widget instance pointed at the same
    // std::string, exactly like tgtInp/ruStepInp/rdStepInp below already
    // are relative to Monitor's vsetInp/rampUpInp/rampDownInp. Editing here
    // shows on Monitor's next render and vice versa.
    auto aliasInp = CommitInput(&inputs.chAlias[ch], "CH" + std::to_string(ch), [&inputs, ch, saveAlias] {
        saveAlias(ch, inputs.chAlias[ch]);
    });
    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0", onTarget);
```

- [ ] **Step 2: Add it to the container and the live-info panel**

Find:

```cpp
    auto container = Container::Vertical({
        visibleOutputControls, visibleTgtInp,
        visibleRuStepInp, visibleRdStepInp, visibleOutputEnabledCyc,
        visibleProtectionControls,
        recovC, delayInp, maxInp, winInp, derInp, iBandInp,
        bSave, bLoad, bFactory,
    });
```

Change to:

```cpp
    auto container = Container::Vertical({
        aliasInp,
        visibleOutputControls, visibleTgtInp,
        visibleRuStepInp, visibleRdStepInp, visibleOutputEnabledCyc,
        visibleProtectionControls,
        recovC, delayInp, maxInp, winInp, derInp, iBandInp,
        bSave, bLoad, bFactory,
    });
```

Find:

```cpp
        auto livePanel = hbox({
            text(" Live ") | bold | color(Color::Cyan),
            separator(),
            liveBar,
        });
```

Change to:

```cpp
        auto livePanel = hbox({
            text(" Alias: ") | bold | color(Color::Cyan),
            aliasInp->Render() | size(WIDTH, EQUAL, 14),
            separator(),
            text(" Live ") | bold | color(Color::Cyan),
            separator(),
            liveBar,
        });
```

- [ ] **Step 3: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: FAIL — `board_dashboard.h`'s existing `makeChannelTab(*board.appState, board.inputs, ch)` call still passes only 3 arguments. Confirm no *other* new errors beyond the ones already expected from Tasks 4 and 5 — Task 7 fixes all three at once.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/tab_channel.h
git commit -m "feat(psb_demo_tui): add alias field to the Channel tab, synced with Monitor (caller not yet updated)"
```

---

## Task 7: `board_dashboard.h` — wire it all together

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Consumes: `rebuildChannelTitles`'s new signature (Task 4), `makeMonitorTab`'s new signature (Task 5), `makeChannelTab`'s new signature (Task 6).
- Produces: `makeBoardDashboard` gains a new trailing parameter, `std::function<void(const std::string&, int, const std::string&)> saveChannelAliasToTopology` — the one closure `main()` constructs (Task 8).

- [ ] **Step 1: Add the new parameter and populate `board.saveChannelAlias`**

Edit `tools/psb_demo_app/tui/board_dashboard.h`, find:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    Component globalPreferences,
                                    std::function<size_t()> liveBoardCount) {
```

Change to:

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup,
                                    Component globalPreferences,
                                    std::function<size_t()> liveBoardCount,
                                    std::function<void(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Find:

```cpp
    // Exposes the same connect/disconnect logic the toggle button below
    // uses, for main.cpp's Connect All/Disconnect All to call directly —
    // see BoardSession::connect/disconnect's own comment.
    board.connect = doConnect;
    board.disconnect = doDisconnect;
```

Change to:

```cpp
    // Exposes the same connect/disconnect logic the toggle button below
    // uses, for main.cpp's Connect All/Disconnect All to call directly —
    // see BoardSession::connect/disconnect's own comment.
    board.connect = doConnect;
    board.disconnect = doDisconnect;
    // Binds this board's own nickname so the Monitor/Channel tabs' alias
    // Input widgets (below) never need to know anything about topology
    // files — see BoardSession::saveChannelAlias's own comment.
    board.saveChannelAlias = [saveChannelAliasToTopology, nickname = board.nickname]
                             (int ch, const std::string& alias) {
        saveChannelAliasToTopology(nickname, ch, alias);
    };
```

- [ ] **Step 2: Update the three call sites the earlier tasks left broken**

Find:

```cpp
    Components tabComponents = { makeMonitorTab(*board.appState, board.inputs) };
```

Change to:

```cpp
    Components tabComponents = { makeMonitorTab(*board.appState, board.inputs, board.saveChannelAlias) };
```

Find:

```cpp
        tabComponents.push_back(makeChannelTab(*board.appState, board.inputs, ch));
```

Change to:

```cpp
        tabComponents.push_back(makeChannelTab(*board.appState, board.inputs, ch, board.saveChannelAlias));
```

Find:

```cpp
                rebuildChannelTitles(board.tabTitles, nc);
```

Change to:

```cpp
                rebuildChannelTitles(board.tabTitles, nc, board.inputs.chAlias);
```

- [ ] **Step 3: Build to confirm the expected failure**

Run: `cd tools && cmake --build build --target psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: FAIL — `main.cpp`'s two calls to `makeBoardDashboard(...)` (inside `buildRuntime` and `applyNewBoardsLive`) still pass only 11 arguments. Confirm this is the *only* remaining error — Task 8 fixes it and resolves every earlier task's deferred failure at once.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): wire alias save-to-topology through makeBoardDashboard (main.cpp not yet updated)"
```

---

## Task 8: `main.cpp` — construct the save closure, populate aliases at connect time

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `makeBoardDashboard`'s new signature (Task 7).
- Produces: nothing new — this is the final wiring task; the build is clean after it.

- [ ] **Step 1: Construct the save closure**

Edit `tools/psb_demo_app/tui/main.cpp`, find:

```cpp
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

Change to:

```cpp
    auto bGlobalConnectAll = psb::tui::ActionButton("Connect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->connect) b->connect();
    });
    auto bGlobalDisconnectAll = psb::tui::ActionButton("Disconnect All", [&rt] {
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) if (b->disconnect) b->disconnect();
    });

    // The one place that knows both the live topo object and where it's
    // saved — every board's own saveChannelAlias (board_dashboard.h) is a
    // thin per-board wrapper around this, so the Monitor/Channel tabs never
    // need direct access to either. Looks the board up by nickname (not a
    // cached bus/board index) since topo can be wholesale replaced by a
    // mid-session Setup edit (onMidSessionFinish below) at any time,
    // invalidating any index captured earlier — the same reasoning
    // detachBoard (board_switcher.h) already applies to board lookups.
    auto saveChannelAliasToTopology = [&topo, topologyPath]
                                      (const std::string& nickname, int ch, const std::string& alias) {
        for (auto& bus : topo.buses) {
            for (auto& brd : bus.boards) {
                if (brd.nickname != nickname) continue;
                if (static_cast<int>(brd.channelAliases.size()) <= ch)
                    brd.channelAliases.resize(ch + 1);
                brd.channelAliases[ch] = alias;
                topo.save(topologyPath);
                return;
            }
        }
    };

    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalPreferences, bGlobalConnectAll, bGlobalDisconnectAll,
                saveChannelAliasToTopology);
```

- [ ] **Step 2: Thread it through `buildRuntime`'s signature and its `makeBoardDashboard` call**

Find:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalPreferences, Component globalConnectAll, Component globalDisconnectAll) {
```

Change to:

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalPreferences, Component globalConnectAll, Component globalDisconnectAll,
                  std::function<void(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Find (the per-board construction loop inside `buildRuntime`):

```cpp
            b->slaveVal = std::to_string(boardCfg.slaveId);
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); });
```

Change to:

```cpp
            b->slaveVal = std::to_string(boardCfg.slaveId);
            for (int ch = 0; ch < static_cast<int>(boardCfg.channelAliases.size()); ++ch)
                b->inputs.chAlias[ch] = boardCfg.channelAliases[ch];
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                bw->workQueue, bw->workMutex, bw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); },
                saveChannelAliasToTopology);
```

- [ ] **Step 3: Thread it through `applyNewBoardsLive`'s signature and its `makeBoardDashboard` call**

Find:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup, Component globalPreferences) {
```

Change to:

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup, Component globalPreferences,
                        std::function<void(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Find (the per-board construction inside `applyNewBoardsLive`):

```cpp
            b->slaveVal = std::to_string(boardCfg.slaveId);
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                targetBw->workQueue, targetBw->workMutex, targetBw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); });
```

Change to:

```cpp
            b->slaveVal = std::to_string(boardCfg.slaveId);
            for (int ch = 0; ch < static_cast<int>(boardCfg.channelAliases.size()); ++ch)
                b->inputs.chAlias[ch] = boardCfg.channelAliases[ch];
            b->appState = std::unique_ptr<psb::tui::AppState>(new psb::tui::AppState{
                *b->client, b->connected, b->data, b->statusMsg, b->statusMutex,
                targetBw->workQueue, targetBw->workMutex, targetBw->workCv, screen});
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalPreferences, [&rt] { return rt.boards.size(); },
                saveChannelAliasToTopology);
```

- [ ] **Step 4: Update `applyNewBoardsLive`'s call site inside `onMidSessionFinish`**

Find:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
```

Change to:

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &screen, &running, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences, saveChannelAliasToTopology]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
```

Find:

```cpp
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences);
            topo = midSessionWiz->topo;
```

Change to:

```cpp
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalPreferences, saveChannelAliasToTopology);
            topo = midSessionWiz->topo;
```

- [ ] **Step 5: Build**

Run: `cd tools && cmake --build build --target psb_modbus_core psb_tests psb_demo_cli psb_demo_tui -j$(nproc) 2>&1 | tail -30`
Expected: clean build — this resolves every deferred failure from Tasks 4, 5, 6, and 7.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): construct the alias save closure, populate aliases at connect time"
```

---

## Task 9: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Full test suite**

Run: `./build/psb_modbus_core/tests/psb_tests 2>&1 | tail -10`
Expected: PASS — the pre-existing suite plus the two new `[topology_config]` cases from Task 2.

- [ ] **Step 2: Manual tmux verification**

Following `docs/guide/client-architecture-and-pitfalls.md` §3's methodology, and protecting any real `~/.psb_demo_app/topology.toml` the same way every prior round did (back it up, diff it after testing):

1. Connect a board (single-board or multi-board mode — both must work per the design). Confirm the Monitor table shows a new "Alias" column, empty, with grey `CH0`/`CH1`/... placeholder text in each row.
2. Type an alias into one channel's Monitor-table Alias field and press Enter. Confirm: (a) the input keeps the typed text, (b) switching to that channel's own Channel tab shows the same alias in its live-info panel's alias field, (c) that channel's tab title in the tab bar now shows the alias instead of `CHn`.
3. Edit the SAME channel's alias from the Channel tab instead, press Enter, switch back to Monitor — confirm the Monitor table's field shows the updated value (two-way sync).
4. Clear an alias back to empty and press Enter — confirm the field returns to showing the grey `CHn` placeholder, and that channel's tab title reverts to `CHn`.
5. Disconnect and reconnect the same board (or restart the app) — confirm the alias is still there (persisted, loaded at connect time), without needing to revisit the Topology wizard.
6. Open the Topology wizard's own Save at any point during this — confirm it doesn't error or overwrite the alias (the wizard's `s.topo` is a separate copy seeded from `topo` when the wizard opens; this step just confirms the two save paths don't corrupt each other's data under normal use).

- [ ] **Step 3: Confirm the real topology file, if one exists, is untouched**

```bash
diff ~/.psb_demo_app/topology.toml /path/to/your/backup/user_topology_backup.toml
```

Expected: identical (no aliases from this test session bled into the real file), or no real file existed before testing began.

# Topology Group Core Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move topology/group naming and mutation policy from `psb_demo_tui` headers into `psb_modbus_core`, while leaving rendering and live UI orchestration in the TUI.

**Architecture:** `psb_modbus_core` already owns `TopologyConfig`, `GroupConfig`, `GroupChannelRef`, `AppPreferences`, TOML load/save, and platform paths. Extend it with UI-free topology/group editing helpers and naming/query helpers. The TUI keeps wizard screen state, FTXUI components, modal status strings, connected-board snapshots, and dashboard refresh logic.

**Tech Stack:** C++17, `psb_modbus_core`, TOML++, Catch2, FTXUI in TUI only.

---

## Current File Map

- Keep in core: `tools/psb_modbus_core/topology_config.h/.cpp`
- Keep in core: `tools/psb_modbus_core/app_preferences.h/.cpp`
- Create in core: `tools/psb_modbus_core/topology_rules.h/.cpp`
- Modify core build: `tools/psb_modbus_core/CMakeLists.txt`
- Move/rewrite tests: `tools/psb_modbus_core/tests/test_wizard_state.cpp`
- Move/rewrite tests: `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`
- Keep in TUI: `tools/psb_demo_app/tui/wizard_screen.h`
- Keep in TUI: `tools/psb_demo_app/tui/group_wizard_screen.h`
- Thin down in TUI: `tools/psb_demo_app/tui/wizard_state.h`
- Thin down in TUI: `tools/psb_demo_app/tui/group_wizard_state.h`
- Update references: `tools/psb_demo_app/tui/main.cpp`, `tools/psb_demo_app/tui/tab_channel.h`, `tools/psb_demo_app/tui/group_monitor.h` if needed

---

## Task 1: Add Core Topology Naming Queries

**Files:**
- Create: `tools/psb_modbus_core/topology_rules.h`
- Create: `tools/psb_modbus_core/topology_rules.cpp`
- Modify: `tools/psb_modbus_core/CMakeLists.txt`
- Test: `tools/psb_modbus_core/tests/test_topology_rules.cpp`

- [ ] **Step 1: Write failing tests**

Test these exact helpers:

```cpp
CHECK(psb::boardChannelId("hvb-left", 2) == "hvb-left/CH2");
CHECK(psb::groupNameInUse(topo, "detector"));
CHECK(psb::boardNicknameInUse(topo, "hvb-left"));
CHECK(psb::findGroupForBoardChannel(topo, "hvb-left", 0) == 0);
CHECK(psb::groupAliasInUse(topo.groups[0], "bias"));
```

- [ ] **Step 2: Run tests and confirm failure**

Run:

```bash
cmake --build tools/build --target psb_tests -j
./tools/build/psb_modbus_core/tests/psb_tests "[topology_rules]"
```

Expected: fails because `topology_rules.h` does not exist.

- [ ] **Step 3: Implement core helpers**

Move these UI-free helpers from TUI headers to `namespace psb`:

```cpp
bool boardNicknameInUse(const TopologyConfig& topo, const std::string& nickname);
bool slaveIdInUse(const BusConfig& bus, int slaveId);
bool groupNameInUse(const TopologyConfig& topo, const std::string& name);
std::string boardChannelId(const std::string& boardNickname, int channelIndex);
int findGroupForBoardChannel(const TopologyConfig& topo, const std::string& boardNickname, int channelIndex);
bool groupAliasInUse(const GroupConfig& group, const std::string& alias, int exceptChannelIdx = -1);
```

- [ ] **Step 4: Run tests and commit**

Run:

```bash
cmake --build tools/build --target psb_tests -j
./tools/build/psb_modbus_core/tests/psb_tests "[topology_rules]"
git add tools/psb_modbus_core/topology_rules.* tools/psb_modbus_core/CMakeLists.txt tools/psb_modbus_core/tests/test_topology_rules.cpp
git commit -m "feat(psb_modbus_core): add topology naming helpers"
```

---

## Task 2: Move Topology Edit Mutators Into Core

**Files:**
- Modify: `tools/psb_modbus_core/topology_rules.h/.cpp`
- Modify: `tools/psb_demo_app/tui/wizard_state.h`
- Modify: `tools/psb_modbus_core/tests/test_wizard_state.cpp`

- [ ] **Step 1: Rewrite tests against core APIs**

Replace TUI-state tests with core mutation tests:

```cpp
psb::TopologyConfig topo;
CHECK(psb::addBus(topo, "", "/dev/ttyUSB0", 115200) == "");
CHECK(psb::addBoard(topo, 0, "hvb-left", 1) == "");
CHECK(psb::addBoard(topo, 0, "hvb-left", 2) == "nickname \"hvb-left\" already in use");
```

- [ ] **Step 2: Run tests and confirm failure**

Run:

```bash
cmake --build tools/build --target psb_tests -j
./tools/build/psb_modbus_core/tests/psb_tests "[topology_rules]"
```

- [ ] **Step 3: Implement core mutators**

Add:

```cpp
std::string addBus(TopologyConfig& topo, const std::string& name, const std::string& port, int baud);
std::string removeBus(TopologyConfig& topo, int busIdx);
std::string addBoard(TopologyConfig& topo, int busIdx, const std::string& nickname, int slaveId);
std::string removeBoard(TopologyConfig& topo, int busIdx, int boardIdx);
```

Keep selection indexes and dirty/status fields in `WizardState`; after successful core mutation, the TUI sets `dirty = true`.

- [ ] **Step 4: Update TUI wrappers**

`tools/psb_demo_app/tui/wizard_state.h` should contain only:

```cpp
struct WizardState {
    psb::TopologyConfig topo;
    std::string topologyPath;
    int selectedBus = -1;
    int selectedBoard = -1;
    std::string statusMsg;
    bool dirty = false;
};
```

Call `psb::addBus`, `psb::removeBus`, `psb::addBoard`, and `psb::removeBoard` from `wizard_screen.h`.

- [ ] **Step 5: Run tests and commit**

Run:

```bash
cmake --build tools/build --target psb_tests psb_demo_tui psb_demo_cli -j
./tools/build/psb_modbus_core/tests/psb_tests
git add tools/psb_modbus_core tools/psb_demo_app/tui/wizard_state.h tools/psb_demo_app/tui/wizard_screen.h
git commit -m "refactor(psb_demo_tui): use core topology mutators"
```

---

## Task 3: Move Group Edit Mutators Into Core

**Files:**
- Modify: `tools/psb_modbus_core/topology_rules.h/.cpp`
- Modify: `tools/psb_demo_app/tui/group_wizard_state.h`
- Modify: `tools/psb_demo_app/tui/group_wizard_screen.h`
- Modify: `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`

- [ ] **Step 1: Rewrite tests against core APIs**

Use `TopologyConfig` directly:

```cpp
CHECK(psb::addGroup(topo, "detector") == "");
CHECK(psb::addChannelToGroup(topo, 0, "hvb-left", 0, "bias") == "");
CHECK(psb::addChannelToGroup(topo, 0, "hvb-left", 1, "bias") ==
      "alias \"bias\" already in use in group detector");
CHECK(psb::renameGroupChannelAliasForBoardChannel(topo, "hvb-left", 0, "guard") == "");
```

- [ ] **Step 2: Run tests and confirm failure**

Run:

```bash
cmake --build tools/build --target psb_tests -j
./tools/build/psb_modbus_core/tests/psb_tests "[topology_rules]"
```

- [ ] **Step 3: Implement core group mutators**

Add:

```cpp
std::string addGroup(TopologyConfig& topo, const std::string& name);
std::string removeGroup(TopologyConfig& topo, int groupIdx);
std::string addChannelToGroup(TopologyConfig& topo, int groupIdx, const std::string& boardNickname, int channelIndex, const std::string& alias);
std::string renameGroupChannelAlias(TopologyConfig& topo, int groupIdx, int channelIdx, const std::string& alias);
std::string renameGroupChannelAliasForBoardChannel(TopologyConfig& topo, const std::string& boardNickname, int channelIndex, const std::string& alias);
std::string removeChannelFromGroup(TopologyConfig& topo, int groupIdx, int channelIdx);
```

- [ ] **Step 4: Thin TUI group state**

`tools/psb_demo_app/tui/group_wizard_state.h` should keep only UI scratch:

```cpp
struct GroupWizardState {
    psb::TopologyConfig topo;
    std::string topologyPath;
    int selectedGroup = -1;
    int selectedChannel = -1;
    std::string statusMsg;
    bool dirty = false;
};
```

Call core mutators from `group_wizard_screen.h`; on success set `dirty = true` and clamp selection in UI code.

- [ ] **Step 5: Run tests and commit**

Run:

```bash
cmake --build tools/build --target psb_tests psb_demo_tui -j
./tools/build/psb_modbus_core/tests/psb_tests
git add tools/psb_modbus_core tools/psb_demo_app/tui/group_wizard_state.h tools/psb_demo_app/tui/group_wizard_screen.h
git commit -m "refactor(psb_demo_tui): use core group mutators"
```

---

## Task 4: Move Available-Channel Selection Policy Into Core

**Files:**
- Modify: `tools/psb_modbus_core/topology_rules.h/.cpp`
- Modify: `tools/psb_demo_app/tui/group_wizard_screen.h`
- Test: `tools/psb_modbus_core/tests/test_topology_rules.cpp`

- [ ] **Step 1: Add tests for add-channel availability**

Model connected boards without FTXUI:

```cpp
std::vector<psb::LiveBoardInfo> live{{"hvb-left", 2}, {"hvb-right", 1}};
auto available = psb::availableGroupChannels(topo, live);
CHECK(available[0].boardNickname == "hvb-left");
CHECK(available[0].channelIndex == 1);
```

- [ ] **Step 2: Add a core data type**

Add a UI-free struct:

```cpp
struct LiveBoardInfo {
    std::string nickname;
    int numChannels = 0;
};
```

and helper:

```cpp
std::vector<GroupChannelRef> availableGroupChannels(const TopologyConfig& topo,
                                                    const std::vector<LiveBoardInfo>& liveBoards);
```

- [ ] **Step 3: Replace picker filtering**

In `group_wizard_screen.h`, keep the two-level FTXUI picker but use `availableGroupChannels()` for the data set. The screen still owns selected indexes and input text.

- [ ] **Step 4: Run tests and commit**

Run:

```bash
cmake --build tools/build --target psb_tests psb_demo_tui -j
./tools/build/psb_modbus_core/tests/psb_tests
git add tools/psb_modbus_core tools/psb_demo_app/tui/group_wizard_screen.h
git commit -m "refactor(psb_demo_tui): use core group channel availability"
```

---

## Task 5: Remove Upward TUI Dependencies From Core Tests

**Files:**
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`
- Modify or remove: `tools/psb_modbus_core/tests/test_wizard_state.cpp`
- Modify or remove: `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`

- [ ] **Step 1: Remove TUI policy tests from core test target**

After the policy tests are rewritten against core APIs, core tests should no longer include:

```cpp
#include "../../psb_demo_app/tui/wizard_state.h"
#include "../../psb_demo_app/tui/group_wizard_state.h"
```

- [ ] **Step 2: Split true widget tests**

Keep FTXUI/widget-specific tests in a TUI-specific test target later, or leave only small pure helper tests that do not pull FTXUI into core tests.

- [ ] **Step 3: Verify dependency direction**

Run:

```bash
rg -n "\\.\\./\\.\\./psb_demo_app/tui" tools/psb_modbus_core/tests
```

Expected: no matches for topology/group policy tests.

- [ ] **Step 4: Run full host tests and commit**

Run:

```bash
cmake --build tools/build --target psb_tests psb_demo_tui psb_demo_cli -j
./tools/build/psb_modbus_core/tests/psb_tests
git status --short
git commit -m "test(psb_modbus_core): keep topology policy tests in core"
```

---

## Non-Goals

- Do not move FTXUI rendering into `psb_modbus_core`.
- Do not move `Runtime`, `BoardSwitcher`, or worker-thread orchestration into `psb_modbus_core`.
- Do not change the TOML file format.
- Do not change the user-facing naming rules.
- Do not add a new standalone library; extend `psb_modbus_core`.


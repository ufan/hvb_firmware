# Channel Groups — Phase 2 (Group Data Model + Wizard) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist user-defined channel groups (name + member channels, referenced by board nickname + channel index) in the topology config file, and let the user create/edit them mid-session via a new "Group" wizard modal, mirroring the existing Topology wizard.

**Architecture:** `TopologyConfig` gains a `groups` field (a sibling to `buses`) with TOML load/save, following the exact nesting style `channelAliases` already established. A new `GroupWizardState`/`group_wizard_state.h` pair (pure data + hardware-free mutators, unit-tested) mirrors `WizardState`/`wizard_state.h`. A new `group_wizard_screen.h` (FTXUI, manually verified — this codebase never unit-tests `*_screen.h` files) mirrors `wizard_screen.h`'s two-pane list/modal structure, simplified: no scan sub-flow, an "Add Channel" picker scoped to currently-connected boards. Wiring in `main.cpp` + `board_switcher.h` + `board_dashboard.h` adds a "Group" `ActionButton` immediately after "Topology", following the exact `showSetup`/`openSetup`/`onMidSessionFinish`/`Modal(...)` pattern already in place — including the `topologyPath`-reseed-on-every-open fix Phase 1 had to add after shipping without it.

**Tech Stack:** C++17, FTXUI (vendored), toml++, Catch2.

## Global Constraints

- **Offline/missing group members**: a group's member list always shows every configured member, rendering an "(offline)" marker for any member whose board is not currently connected — membership is never silently dropped. (Design spec, Confirmed Decisions.)
- **Default alias display fallback**: anywhere a channel's name is shown outside the Monitor/Channel tabs (here: the group wizard's member list and Add Channel picker), it falls back to `CHn` when no alias is set. (Design spec, Confirmed Decisions.)
- **Group channel picker scope**: the "Add Channel" picker only ever lists channels from **currently-connected** boards — never every board ever defined in the topology file. (Design spec, Confirmed Decisions.)
- **Groups reference channels by board nickname + channel index**, not bus/port. (Design spec, Confirmed Decisions and Architecture.)
- **TOML load is tolerant**: a malformed group/channel entry is skipped, not fatal — matching every other load path in `topology_config.cpp`. A topology file with no `[[group]]` key at all (every file written before this phase) must still load successfully with an empty `groups` vector.
- **Save never truncates**: any code path that calls `topo.save(path)` must be operating on a full `TopologyConfig` (buses + boards + aliases + groups), never a partial copy — the exact class of bug Phase 1's `currentTopologyPath` fix exists to prevent, now extended to groups.
- **`topologyPath`-reseed-on-every-open**: any wizard state's `topologyPath` field must be reseeded from the live tracked path immediately before the wizard/dialog opens (inside the `open*` closure), not just once at construction — this was Phase 1's own Critical bug, found and fixed after the fact; Phase 2 must not repeat it.

---

### Task 1: Group data model — `TopologyConfig` + TOML persistence

**Files:**
- Modify: `tools/psb_modbus_core/topology_config.h`
- Modify: `tools/psb_modbus_core/topology_config.cpp`
- Test: `tools/psb_modbus_core/tests/test_topology_config.cpp`

**Interfaces:**
- Produces: `psb::GroupChannelRef` (`boardNickname` : `std::string`, `channelIndex` : `int`), `psb::GroupConfig` (`name` : `std::string`, `channels` : `std::vector<GroupChannelRef>`), `psb::TopologyConfig::groups` (`std::vector<GroupConfig>`).

- [ ] **Step 1: Write the failing tests**

Append to `tools/psb_modbus_core/tests/test_topology_config.cpp`, right after the existing "a board with no aliases round-trips with an empty channelAliases" test case (after line 93, before the "load returns nullopt for missing file" test case):

```cpp
TEST_CASE("TopologyConfig — round trip preserves groups and their member channels", "[topology_config]") {
    psb::TopologyConfig cfg;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.baudRate = 115200;
    bus.boards.push_back({"hvb-bench", 1});
    bus.boards.push_back({"hvb-bench-2", 2});
    cfg.buses.push_back(bus);

    psb::GroupConfig group;
    group.name = "Battery Bank";
    group.channels.push_back({"hvb-bench", 0});
    group.channels.push_back({"hvb-bench-2", 3});
    cfg.groups.push_back(group);

    psb::GroupConfig group2;
    group2.name = "Empty Group";
    cfg.groups.push_back(group2);

    const std::string path = "/tmp/psb_topology_test_groups.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->groups.size() == 2);

    CHECK(loaded->groups[0].name == "Battery Bank");
    REQUIRE(loaded->groups[0].channels.size() == 2);
    CHECK(loaded->groups[0].channels[0].boardNickname == "hvb-bench");
    CHECK(loaded->groups[0].channels[0].channelIndex == 0);
    CHECK(loaded->groups[0].channels[1].boardNickname == "hvb-bench-2");
    CHECK(loaded->groups[0].channels[1].channelIndex == 3);

    CHECK(loaded->groups[1].name == "Empty Group");
    CHECK(loaded->groups[1].channels.empty());

    // Buses/boards from the same file must still round-trip untouched.
    REQUIRE(loaded->buses.size() == 1);
    CHECK(loaded->buses[0].boards.size() == 2);

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — a topology with no groups round-trips with an empty groups vector", "[topology_config]") {
    auto cfg = psb::TopologyConfig::singleBoard("/dev/ttyUSB0", 115200, 1, "board1");
    const std::string path = "/tmp/psb_topology_test_no_groups.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->groups.empty());

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig — a pre-Phase-2 topology file with no [[group]] key at all still loads", "[topology_config]") {
    const std::string path = "/tmp/psb_topology_test_legacy_no_group_key.toml";
    {
        std::ofstream ofs(path);
        ofs << "[[bus]]\nname = 'bus1'\nport = '/dev/ttyUSB0'\nbaud_rate = 115200\n"
               "  [[bus.board]]\n  nickname = 'board1'\n  slave_id = 1\n";
    }
    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->groups.empty());
    REQUIRE(loaded->buses.size() == 1);

    std::remove(path.c_str());
}
```

- [ ] **Step 2: Run tests to verify they fail (compile error — types don't exist yet)**

Run: `cmake --build tools/build --target psb_tests`
Expected: FAIL — `error: 'GroupConfig' is not a member of 'psb'` (or similar, for `psb::GroupChannelRef`/`cfg.groups`).

- [ ] **Step 3: Add `GroupChannelRef`/`GroupConfig`/`TopologyConfig::groups`**

In `tools/psb_modbus_core/topology_config.h`, insert the two new structs between `BusConfig` and `TopologyConfig`, and add the `groups` field:

```cpp
struct BusConfig {
    std::string name;
    std::string port;
    int baudRate = 115200;
    std::vector<BoardConfig> boards;
};

// References a channel by the board's nickname + channel index rather than
// bus/port — nicknames are already the codebase's de facto unique board
// identifier (used the same way by detachBoard in board_switcher.h), so a
// group's membership survives a board moving to a different port.
struct GroupChannelRef {
    std::string boardNickname;
    int channelIndex = 0;
};

struct GroupConfig {
    std::string name;
    std::vector<GroupChannelRef> channels;
};

// Supersedes ConfigManager (~/.psb_demo_app.toml, single board/bus only).
// See docs/superpowers/specs/2026-07-20-multi-board-topology-design.md.
struct TopologyConfig {
    std::vector<BusConfig> buses;
    std::vector<GroupConfig> groups;  // user-defined channel groups, sibling to buses
```

(The rest of `TopologyConfig` — `load`/`save`/`exists`/`singleBoard`/`defaultPath`/`lastSingleConnectPath`/`totalBoardCount` — is unchanged.)

- [ ] **Step 4: Add TOML load/save for groups**

In `tools/psb_modbus_core/topology_config.cpp`, inside `TopologyConfig::load`, insert the group-parsing block right after the `bus` array loop and before `return cfg;`:

```cpp
            cfg.buses.push_back(std::move(bus));
        }
        auto groupArr = tbl["group"].as_array();
        if (groupArr) {
            for (auto&& groupNode : *groupArr) {
                auto groupTbl = groupNode.as_table();
                if (!groupTbl) continue;
                GroupConfig group;
                group.name = (*groupTbl)["name"].value_or(std::string(""));
                auto chArr = (*groupTbl)["channel"].as_array();
                if (chArr) {
                    for (auto&& chNode : *chArr) {
                        auto chTbl = chNode.as_table();
                        if (!chTbl) continue;
                        GroupChannelRef ref;
                        ref.boardNickname = (*chTbl)["board"].value_or(std::string(""));
                        ref.channelIndex = static_cast<int>((*chTbl)["channel"].value_or(0));
                        group.channels.push_back(std::move(ref));
                    }
                }
                cfg.groups.push_back(std::move(group));
            }
        }
        return cfg;
```

In `TopologyConfig::save`, insert the group-serialization block right after `root.insert_or_assign("bus", std::move(busArr));` and before the `std::filesystem::path fsPath(path);` line:

```cpp
        root.insert_or_assign("bus", std::move(busArr));

        if (!groups.empty()) {
            toml::array groupArr;
            for (const auto& group : groups) {
                toml::table groupTbl;
                groupTbl.insert_or_assign("name", group.name);
                toml::array chArr;
                for (const auto& ch : group.channels) {
                    toml::table chTbl;
                    chTbl.insert_or_assign("board", ch.boardNickname);
                    chTbl.insert_or_assign("channel", ch.channelIndex);
                    chArr.push_back(std::move(chTbl));
                }
                groupTbl.insert_or_assign("channel", std::move(chArr));
                groupArr.push_back(std::move(groupTbl));
            }
            root.insert_or_assign("group", std::move(groupArr));
        }

        std::filesystem::path fsPath(path);
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build tools/build --target psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests "[topology_config]"`
Expected: PASS, all `[topology_config]` test cases including the 3 new ones.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_modbus_core/topology_config.h tools/psb_modbus_core/topology_config.cpp tools/psb_modbus_core/tests/test_topology_config.cpp
git commit -m "feat(psb_modbus_core): add GroupConfig/GroupChannelRef, persist groups in TopologyConfig"
```

---

### Task 2: `GroupWizardState` — pure data + mutators

**Files:**
- Create: `tools/psb_demo_app/tui/group_wizard_state.h`
- Test: `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`
- Modify: `tools/psb_modbus_core/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `psb::TopologyConfig`, `psb::GroupConfig`, `psb::GroupChannelRef` (Task 1).
- Produces: `psb::tui::GroupWizardState` (`topo`, `topologyPath`, `selectedGroup`, `selectedChannel`, `statusMsg`, `dirty`), `addGroup(GroupWizardState&, const std::string&)`, `removeGroup(GroupWizardState&, int)`, `addChannelToGroup(GroupWizardState&, int, const std::string&, int)`, `removeChannelFromGroup(GroupWizardState&, int, int)` — every mutator returns `""` on success or an error string, never throws, never touches hardware. Consumed by Task 3's `group_wizard_screen.h`.

- [ ] **Step 1: Write the failing tests**

Create `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "../../psb_demo_app/tui/group_wizard_state.h"

using namespace psb::tui;

TEST_CASE("GroupWizardState — addGroup rejects an empty name", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(addGroup(s, "") == "group name required");
    CHECK(s.topo.groups.empty());
}

TEST_CASE("GroupWizardState — addGroup rejects a duplicate name", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "Battery Bank").empty());
    CHECK(addGroup(s, "Battery Bank") == "group name \"Battery Bank\" already in use");
    CHECK(s.topo.groups.size() == 1);
}

TEST_CASE("GroupWizardState — successful addGroup marks the state dirty", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK_FALSE(s.dirty);
    addGroup(s, "Battery Bank");
    CHECK(s.dirty);
}

TEST_CASE("GroupWizardState — removeGroup rejects an out-of-range index", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(removeGroup(s, 0) == "invalid group index");
}

TEST_CASE("GroupWizardState — removeGroup drops the group and clears selection past the end", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    s.selectedGroup = 0;
    REQUIRE(removeGroup(s, 0).empty());
    CHECK(s.topo.groups.empty());
    CHECK(s.selectedGroup == -1);
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects an invalid group index", "[group_wizard_state]") {
    GroupWizardState s;
    CHECK(addChannelToGroup(s, 0, "hvb-bench", 0) == "invalid group index");
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects an empty board nickname", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    CHECK(addChannelToGroup(s, 0, "", 0) == "board required");
}

TEST_CASE("GroupWizardState — addChannelToGroup rejects a channel already in the group", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    REQUIRE(addChannelToGroup(s, 0, "hvb-bench", 0).empty());
    CHECK(addChannelToGroup(s, 0, "hvb-bench", 0) == "channel already in group");
    CHECK(s.topo.groups[0].channels.size() == 1);
}

TEST_CASE("GroupWizardState — addChannelToGroup allows the same channel index on different boards", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    REQUIRE(addChannelToGroup(s, 0, "hvb-bench", 0).empty());
    CHECK(addChannelToGroup(s, 0, "hvb-bench-2", 0).empty());
    CHECK(s.topo.groups[0].channels.size() == 2);
}

TEST_CASE("GroupWizardState — removeChannelFromGroup rejects an out-of-range channel index", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    CHECK(removeChannelFromGroup(s, 0, 0) == "invalid channel index");
}

TEST_CASE("GroupWizardState — removeChannelFromGroup drops the member channel", "[group_wizard_state]") {
    GroupWizardState s;
    addGroup(s, "Battery Bank");
    addChannelToGroup(s, 0, "hvb-bench", 0);
    REQUIRE(removeChannelFromGroup(s, 0, 0).empty());
    CHECK(s.topo.groups[0].channels.empty());
}
```

- [ ] **Step 2: Register the new test file**

In `tools/psb_modbus_core/tests/CMakeLists.txt`, add `test_group_wizard_state.cpp` right after `test_wizard_state.cpp`:

```cmake
add_executable(psb_tests
    test_connection.cpp
    test_serial_bus.cpp
    test_board_session.cpp
    test_topology_config.cpp
    test_app_preferences.cpp
    test_system_reads.cpp
    test_channel_reads.cpp
    test_writes.cpp
    test_enum_validation.cpp
    test_error_handling.cpp
    test_monitor_output.cpp
    test_tui_format.cpp
    test_tui_policy.cpp
    test_tui_modbus_settings.cpp
    test_calibration.cpp
    test_wizard_state.cpp
    test_group_wizard_state.cpp
    test_wizard_scan.cpp
)
```

- [ ] **Step 3: Run tests to verify they fail (missing header)**

Run: `cmake --build tools/build --target psb_tests`
Expected: FAIL — `fatal error: ../../psb_demo_app/tui/group_wizard_state.h: No such file or directory`. (CMake will need to be re-run to pick up the new source file: `cmake -S tools -B tools/build && cmake --build tools/build --target psb_tests` if the plain build-only invocation doesn't detect the CMakeLists.txt change.)

- [ ] **Step 4: Create `group_wizard_state.h`**

Create `tools/psb_demo_app/tui/group_wizard_state.h`:

```cpp
#pragma once

#include "topology_config.h"

#include <string>

namespace psb::tui {

// In-progress group edits, plus UI selection/status scratch — mirrors
// WizardState (wizard_state.h) exactly: hardware- and FTXUI-free so the
// mutators below are unit-testable without a bus, a board, or a terminal.
// `topo` is always seeded from the *full* live topology (buses + boards +
// aliases + groups), never a groups-only copy — group_wizard_screen.h's own
// Save writes this whole struct back to disk, and a groups-only copy would
// silently truncate the saved file's buses/boards, exactly the class of bug
// Phase 1's currentTopologyPath fix exists to prevent.
struct GroupWizardState {
    psb::TopologyConfig topo;
    std::string topologyPath;
    int selectedGroup = -1;    // index into topo.groups, -1 = none
    int selectedChannel = -1;  // index into topo.groups[selectedGroup].channels, -1 = none
    std::string statusMsg;
    bool dirty = false;
};

inline bool groupNameInUse(const psb::TopologyConfig& topo, const std::string& name) {
    for (const auto& g : topo.groups)
        if (g.name == name) return true;
    return false;
}

// Every mutator below: returns "" on success, a user-facing error message on
// failure; never throws; never touches hardware.

inline std::string addGroup(GroupWizardState& s, const std::string& name) {
    if (name.empty()) return "group name required";
    if (groupNameInUse(s.topo, name)) return "group name \"" + name + "\" already in use";
    psb::GroupConfig g;
    g.name = name;
    s.topo.groups.push_back(std::move(g));
    s.dirty = true;
    return "";
}

// Same known limitation as WizardState's removeBus/removeBoard: only clamps
// selectedGroup when it falls past the end after the erase, does not shift
// it when idx is strictly before the current selection. Harmless here too —
// group_wizard_screen.h only ever calls removeGroup(s, s.selectedGroup).
inline std::string removeGroup(GroupWizardState& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    s.topo.groups.erase(s.topo.groups.begin() + idx);
    if (s.selectedGroup >= static_cast<int>(s.topo.groups.size()))
        s.selectedGroup = static_cast<int>(s.topo.groups.size()) - 1;
    s.selectedChannel = -1;
    s.dirty = true;
    return "";
}

inline std::string addChannelToGroup(GroupWizardState& s, int groupIdx,
                                     const std::string& boardNickname, int channelIndex) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    if (boardNickname.empty()) return "board required";
    if (channelIndex < 0) return "invalid channel index";
    auto& channels = s.topo.groups[groupIdx].channels;
    for (const auto& c : channels)
        if (c.boardNickname == boardNickname && c.channelIndex == channelIndex)
            return "channel already in group";
    psb::GroupChannelRef ref;
    ref.boardNickname = boardNickname;
    ref.channelIndex = channelIndex;
    channels.push_back(std::move(ref));
    s.dirty = true;
    return "";
}

// Same known limitation as removeGroup above regarding selectedChannel shift.
inline std::string removeChannelFromGroup(GroupWizardState& s, int groupIdx, int channelIdx) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    auto& channels = s.topo.groups[groupIdx].channels;
    if (channelIdx < 0 || channelIdx >= static_cast<int>(channels.size()))
        return "invalid channel index";
    channels.erase(channels.begin() + channelIdx);
    if (s.selectedChannel >= static_cast<int>(channels.size()))
        s.selectedChannel = static_cast<int>(channels.size()) - 1;
    s.dirty = true;
    return "";
}

} // namespace psb::tui
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake -S tools -B tools/build && cmake --build tools/build --target psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests "[group_wizard_state]"`
Expected: PASS, all 11 new `[group_wizard_state]` test cases.

- [ ] **Step 6: Commit**

```bash
git add tools/psb_demo_app/tui/group_wizard_state.h tools/psb_modbus_core/tests/test_group_wizard_state.cpp tools/psb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(psb_demo_tui): add GroupWizardState — pure data + mutators for group edits"
```

---

### Task 3: `group_wizard_screen.h` — the group wizard's FTXUI Component

**Files:**
- Create: `tools/psb_demo_app/tui/group_wizard_screen.h`

**Interfaces:**
- Consumes: `psb::tui::GroupWizardState` + its mutators (Task 2); `psb::tui::ActionButton`, `psb::tui::CommitInput` (`widgets.h`, unused here but the header pulls it in transitively — no new symbol needed from it besides `ActionButton`).
- Produces: `psb::tui::LiveBoardInfo` (`nickname`, `numChannels`, `aliases`), `psb::tui::LiveBoards` (`std::vector<LiveBoardInfo>`), `psb::tui::GetLiveBoards` (`std::function<LiveBoards()>`), `psb::tui::makeGroupWizardScreen(GroupWizardState&, ScreenInteractive&, std::function<void()> onFinish, GetLiveBoards)` returning a `Component`. Consumed by Task 5's `main.cpp` wiring.

This file cannot be unit-tested (no `test_group_wizard_screen.cpp` — this codebase never unit-compiles `*_screen.h` FTXUI files; `wizard_screen.h` itself has no test file either, only `wizard_state.h` does). Its only build verification is Task 5, where `main.cpp` includes it and the whole app target compiles. Manual/tmux verification of its actual behavior happens in Task 6.

- [ ] **Step 1: Create `group_wizard_screen.h`**

Create `tools/psb_demo_app/tui/group_wizard_screen.h`:

```cpp
#pragma once

#include "group_wizard_state.h"
#include "widgets.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psb::tui {

using namespace ftxui;

// A currently-connected board, as the group wizard's "Add Channel" picker
// needs to see it — never sourced from topo.buses (which lists every board
// ever defined, connected or not); see makeGroupWizardScreen's GetLiveBoards
// parameter. aliases[ch] is "" when that channel has no alias set.
struct LiveBoardInfo {
    std::string nickname;
    int numChannels = 0;
    std::vector<std::string> aliases;
};
using LiveBoards = std::vector<LiveBoardInfo>;
using GetLiveBoards = std::function<LiveBoards()>;

// Falls back to "CHn" when no alias is set or the alias lookup can't find
// this channel (offline board) — the same display-fallback rule the Monitor/
// Channel tabs already use for tab titles (rebuildChannelTitles).
inline std::string groupChannelDisplayName(const GroupChannelRef& ref, const LiveBoards& live) {
    for (const auto& lb : live) {
        if (lb.nickname != ref.boardNickname) continue;
        if (ref.channelIndex >= 0 && ref.channelIndex < static_cast<int>(lb.aliases.size())
            && !lb.aliases[ref.channelIndex].empty())
            return lb.aliases[ref.channelIndex];
        break;
    }
    return "CH" + std::to_string(ref.channelIndex);
}

// Builds the Group wizard's Component — a group name list plus Add/Remove
// Group, a selected group's member-channel list plus Add/Remove Channel, and
// Save/Exit. Deliberately simpler than makeWizardScreen (wizard_screen.h):
// no scan sub-flow, and no ConnectNow/Back outcomes — this is always a
// mid-session-only modal (there is no pre-dashboard "launch flow" for
// groups, since a group needs boards to already be defined), so onFinish
// takes no arguments; it always means "close this modal."
//
// getLiveBoards is called fresh every time the Add Channel modal opens
// (mirroring the Topology wizard's own doScanPorts-on-open pattern), never
// cached across renders — a board can connect/disconnect at any time while
// this modal sits open.
inline Component makeGroupWizardScreen(GroupWizardState& s, ScreenInteractive& screen,
                                       std::function<void()> onFinish,
                                       GetLiveBoards getLiveBoards) {
    // ---- Group list (left pane) ----
    auto groupNames = std::make_shared<std::vector<std::string>>();
    auto rebuildGroupNames = [&s, groupNames] {
        groupNames->clear();
        for (const auto& g : s.topo.groups) groupNames->push_back(g.name);
    };
    rebuildGroupNames();
    auto groupMenu = Menu(groupNames.get(), &s.selectedGroup);

    // ---- Member channel list (right pane) ----
    // Rebuilt every render, not just on add/remove — a member's board can
    // connect/disconnect independently of any edit made here, and the
    // "(offline)" marker (Global Constraints) must reflect that live.
    auto channelLabels = std::make_shared<std::vector<std::string>>();
    auto rebuildChannelLabels = [&s, channelLabels, getLiveBoards] {
        channelLabels->clear();
        if (s.selectedGroup < 0 || s.selectedGroup >= static_cast<int>(s.topo.groups.size())) return;
        LiveBoards live = getLiveBoards();
        for (const auto& ref : s.topo.groups[s.selectedGroup].channels) {
            bool online = false;
            for (const auto& lb : live) if (lb.nickname == ref.boardNickname) { online = true; break; }
            std::string label = ref.boardNickname + " " + groupChannelDisplayName(ref, live);
            if (!online) label += " (offline)";
            channelLabels->push_back(label);
        }
    };
    rebuildChannelLabels();
    auto channelMenu = Menu(channelLabels.get(), &s.selectedChannel);

    // ---- Add Group modal ----
    auto newGroupName = std::make_shared<std::string>();
    auto showAddGroupPtr = std::make_shared<bool>(false);
    auto groupNameInp = Input(newGroupName.get(), "group name");
    auto bAddGroupConfirm = ActionButton("Add", [&s, newGroupName, rebuildGroupNames, showAddGroupPtr, &screen] {
        std::string err = addGroup(s, *newGroupName);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildGroupNames();
            *showAddGroupPtr = false;
            newGroupName->clear();
        }
        screen.PostEvent(Event::Custom);
    });
    auto bAddGroupCancel = ActionButton("Cancel", [showAddGroupPtr, &screen] {
        *showAddGroupPtr = false; screen.PostEvent(Event::Custom);
    });
    auto addGroupForm = Container::Vertical({groupNameInp, bAddGroupConfirm, bAddGroupCancel});
    auto addGroupPopup = Renderer(addGroupForm, [groupNameInp, bAddGroupConfirm, bAddGroupCancel] {
        return vbox({
            text(" Add Group ") | bold | center, separator(),
            hbox({ text("Name : "), groupNameInp->Render() }),
            separator(),
            hbox({ bAddGroupConfirm->Render(), text("  "), bAddGroupCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 40);
    });

    // ---- Add Channel modal (picker scoped to currently-connected boards,
    //      excluding channels already in the selected group — Global
    //      Constraints) ----
    auto channelPickerLabels = std::make_shared<std::vector<std::string>>();
    auto channelPickerRefs = std::make_shared<std::vector<GroupChannelRef>>();
    auto channelPickerIdx = std::make_shared<int>(-1);
    auto rebuildChannelPicker = [&s, channelPickerLabels, channelPickerRefs, channelPickerIdx, getLiveBoards] {
        channelPickerLabels->clear();
        channelPickerRefs->clear();
        if (s.selectedGroup < 0 || s.selectedGroup >= static_cast<int>(s.topo.groups.size())) return;
        const auto& existing = s.topo.groups[s.selectedGroup].channels;
        LiveBoards live = getLiveBoards();
        for (const auto& lb : live) {
            for (int ch = 0; ch < lb.numChannels; ++ch) {
                bool already = false;
                for (const auto& ref : existing)
                    if (ref.boardNickname == lb.nickname && ref.channelIndex == ch) { already = true; break; }
                if (already) continue;
                std::string label = lb.nickname + " CH" + std::to_string(ch);
                if (ch < static_cast<int>(lb.aliases.size()) && !lb.aliases[ch].empty())
                    label += " (" + lb.aliases[ch] + ")";
                channelPickerLabels->push_back(label);
                GroupChannelRef ref;
                ref.boardNickname = lb.nickname;
                ref.channelIndex = ch;
                channelPickerRefs->push_back(ref);
            }
        }
        if (*channelPickerIdx >= static_cast<int>(channelPickerLabels->size()))
            *channelPickerIdx = channelPickerLabels->empty() ? -1 : 0;
    };
    auto channelPickerMenu = Menu(channelPickerLabels.get(), channelPickerIdx.get());
    auto showAddChannelPtr = std::make_shared<bool>(false);

    auto bAddChannelConfirm = ActionButton("Add", [&s, channelPickerRefs, channelPickerIdx,
                                                    rebuildChannelPicker, rebuildChannelLabels, &screen] {
        int i = *channelPickerIdx;
        if (i < 0 || i >= static_cast<int>(channelPickerRefs->size())) return;
        const auto& ref = (*channelPickerRefs)[i];
        std::string err = addChannelToGroup(s, s.selectedGroup, ref.boardNickname, ref.channelIndex);
        s.statusMsg = err.empty() ? "" : "Error: " + err;
        if (err.empty()) {
            rebuildChannelPicker();
            rebuildChannelLabels();
        }
        screen.PostEvent(Event::Custom);
    });
    auto bAddChannelCancel = ActionButton("Cancel", [showAddChannelPtr, &screen] {
        *showAddChannelPtr = false; screen.PostEvent(Event::Custom);
    });
    auto addChannelForm = Container::Vertical({channelPickerMenu, bAddChannelConfirm, bAddChannelCancel});
    auto addChannelPopup = Renderer(addChannelForm, [channelPickerMenu, channelPickerLabels,
                                                      bAddChannelConfirm, bAddChannelCancel] {
        Element list = channelPickerLabels->empty()
            ? text("(no channels available — connect a board)") | dim
            : channelPickerMenu->Render() | frame | size(HEIGHT, LESS_THAN, 10);
        return vbox({
            text(" Add Channel ") | bold | center, separator(),
            list,
            separator(),
            hbox({ bAddChannelConfirm->Render(), text("  "), bAddChannelCancel->Render() }) | center,
        }) | border | size(WIDTH, EQUAL, 46);
    });

    // ---- List actions ----
    auto bAddGroup = ActionButton("Add Group", [showAddGroupPtr, &screen] {
        *showAddGroupPtr = true; screen.PostEvent(Event::Custom);
    });
    auto bRemoveGroup = ActionButton("Remove Group", [&s, rebuildGroupNames, &screen] {
        if (s.selectedGroup < 0) return;
        s.statusMsg = removeGroup(s, s.selectedGroup);
        rebuildGroupNames();
        screen.PostEvent(Event::Custom);
    });
    // Same "check range, not >= 0" reasoning as wizard_screen.h's
    // busInRange/boardInRange — MenuBase::Clamp() promotes an empty list's
    // -1 sentinel to 0 on every Render().
    auto groupInRange = [&s] { return s.selectedGroup >= 0 && s.selectedGroup < static_cast<int>(s.topo.groups.size()); };
    auto groupSelectable = Maybe(bRemoveGroup, groupInRange);

    auto bAddChannel = ActionButton("Add Channel", [showAddChannelPtr, rebuildChannelPicker, &screen] {
        rebuildChannelPicker();
        *showAddChannelPtr = true; screen.PostEvent(Event::Custom);
    });
    auto addChannelEnabled = Maybe(bAddChannel, groupInRange);
    auto bRemoveChannel = ActionButton("Remove Channel", [&s, rebuildChannelLabels, &screen] {
        if (s.selectedGroup < 0 || s.selectedChannel < 0) return;
        s.statusMsg = removeChannelFromGroup(s, s.selectedGroup, s.selectedChannel);
        rebuildChannelLabels();
        screen.PostEvent(Event::Custom);
    });
    auto channelInRange = [&s, groupInRange] {
        return groupInRange() && s.selectedChannel >= 0
            && s.selectedChannel < static_cast<int>(s.topo.groups[s.selectedGroup].channels.size());
    };
    auto channelSelectable = Maybe(bRemoveChannel, channelInRange);

    // ---- Save / Exit ----
    auto bSave = ActionButton("Save", [&s, &screen] {
        if (s.topo.save(s.topologyPath)) {
            s.dirty = false;
            s.statusMsg = "Saved to " + s.topologyPath;
        } else {
            s.statusMsg = "Error: could not save to " + s.topologyPath;
        }
        screen.PostEvent(Event::Custom);
    });
    auto bExit = ActionButton("Exit", [onFinish] { onFinish(); });

    Components mainChildren = {
        groupMenu, bAddGroup, groupSelectable,
        channelMenu, addChannelEnabled, channelSelectable,
        bSave, bExit,
    };
    auto mainContainer = Container::Vertical(mainChildren);

    auto root = Renderer(mainContainer, [&s, groupMenu, bAddGroup, groupSelectable,
                                         channelMenu, addChannelEnabled, channelSelectable,
                                         bSave, bExit, rebuildGroupNames, rebuildChannelLabels] {
        // Re-derive both lists from s.topo on every render — s.topo can be
        // reseeded from outside this file (main.cpp's openGroupSetup) at any
        // time, same reasoning as wizard_screen.h's own rebuildBusNames/
        // rebuildBoardNames calls at the top of its Renderer.
        rebuildGroupNames();
        rebuildChannelLabels();
        return vbox({
            text(" Group Wizard " + std::string(s.dirty ? "*" : "") + " ") | bold | center,
            separator(),
            hbox({
                vbox({ text("Groups") | bold, groupMenu->Render() | frame | flex,
                       hbox({ bAddGroup->Render(), text(" "), groupSelectable->Render() }) }) | flex | border,
                vbox({ text("Channels") | bold, channelMenu->Render() | frame | flex,
                       hbox({ addChannelEnabled->Render(), text(" "), channelSelectable->Render() }) }) | flex | border,
            }) | flex,
            separator(),
            text(" " + s.statusMsg + " ") | (s.statusMsg.rfind("Error", 0) == 0 ? color(Color::Red) : color(Color::Green)),
            separator(),
            hbox({ bSave->Render(), text("  "), bExit->Render() }) | center,
        }) | border | size(WIDTH, GREATER_THAN, 80) | size(HEIGHT, GREATER_THAN, 24);
    }) | Modal(addGroupPopup, showAddGroupPtr.get())
       | Modal(addChannelPopup, showAddChannelPtr.get());

    return root;
}

} // namespace psb::tui
```

- [ ] **Step 2: Commit**

```bash
git add tools/psb_demo_app/tui/group_wizard_screen.h
git commit -m "feat(psb_demo_tui): add group_wizard_screen.h (not yet wired into main.cpp — expected unused until Task 5)"
```

---

### Task 4: `board_switcher.h` + `board_dashboard.h` — plumb a `globalGroup` Component through

**Files:**
- Modify: `tools/psb_demo_app/tui/board_switcher.h`
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`

**Interfaces:**
- Consumes: nothing new — `Component` is already `ftxui::Component`, already in scope in both files.
- Produces: `makeBoardSwitcher(boards, globalQuit, globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll)` (new `globalGroup` parameter inserted right after `globalSetup`); `makeBoardDashboard(..., globalQuit, globalSetup, globalGroup, globalPreferences, liveBoardCount, saveChannelAliasToTopology)` (same insertion point). Consumed by Task 5's `main.cpp` call-site updates.

This task deliberately breaks the `psb_demo_tui` build — `main.cpp`'s existing calls to `makeBoardSwitcher`/`makeBoardDashboard` (and `buildRuntime`/`applyNewBoardsLive`, which forward to them) will no longer match these new signatures until Task 5 updates every call site. `psb_tests` is unaffected (it never links `main.cpp`) and must still build and pass. This mirrors the exact "expected build break until next task" pattern used for `board_switcher.h` itself in the original multi-board work (see `ed9fc07`/`dff54e6`).

- [ ] **Step 1: Insert `globalGroup` into `makeBoardSwitcher`**

In `tools/psb_demo_app/tui/board_switcher.h`, change the signature (line 35-38):

```cpp
inline BoardSwitcher makeBoardSwitcher(std::vector<std::unique_ptr<BoardSession>>& boards,
                                       Component globalQuit, Component globalSetup, Component globalGroup,
                                       Component globalPreferences,
                                       Component globalConnectAll, Component globalDisconnectAll) {
```

Change the `globalMenuBar` construction (line 121-122) to include it:

```cpp
    auto globalMenuBar = Container::Horizontal(
        {globalSetup, globalGroup, globalPreferences, globalConnectAll, globalDisconnectAll, globalQuit});
```

Add `globalGroup` to the `root` Renderer's capture list (line 161-163):

```cpp
    auto root = Renderer(mainContainer, [switcherBar, dashboardStack, globalSetup, globalGroup, globalPreferences,
                                         globalConnectAll, globalDisconnectAll, globalQuit,
                                         boardNames, activeBoard, mainSelected] {
```

Insert `globalGroup->Render()` into the multi-board global row (line 168-174), right after `globalSetup->Render()`:

```cpp
        return vbox({
            hbox({
                globalSetup->Render(), text(" "), globalGroup->Render(), text(" "), globalPreferences->Render(),
                filler(),
                globalConnectAll->Render(), text(" "), globalDisconnectAll->Render(), text(" "),
                globalQuit->Render(),
            }),
```

- [ ] **Step 2: Insert `globalGroup` into `makeBoardDashboard`**

In `tools/psb_demo_app/tui/board_dashboard.h`, change the signature (line 35-42):

```cpp
inline Component makeBoardDashboard(BoardSession& board, BusWorker& busWorker,
                                    ScreenInteractive& screen, std::atomic<bool>& running,
                                    int timeoutMs, std::function<void()> openSetup,
                                    std::function<void()> requestRemove,
                                    Component globalQuit, Component globalSetup, Component globalGroup,
                                    Component globalPreferences,
                                    std::function<size_t()> liveBoardCount,
                                    std::function<bool(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Add `globalGroup` to the `root` Renderer's capture list (line 386-388):

```cpp
    auto root = Renderer(mainContainer, [&board, &screen, menuModeC, connectedMenuSave, bConnToggle, bRemove,
                                         tabBar, tabContent, bSysCfg, globalQuit, globalSetup, globalGroup,
                                         globalPreferences, liveBoardCount] {
```

Insert `globalGroup->Render()` into the single-board merge block (line 488-501), right after `globalSetup->Render()`:

```cpp
        if (boardCount <= 1) {
            // Quit last — the same right-corner placement board_switcher.h's
            // multi-board row uses, keeping this one destructive action set
            // apart from Setup/Group/Preferences even without a dedicated
            // filler here (the row's existing pair of fillers around
            // centerGroup already pushes this whole tail block flush to the
            // right edge; adding a third filler would unbalance that split).
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalSetup->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalGroup->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalPreferences->Render());
            menuBarParts.push_back(text(" "));
            menuBarParts.push_back(globalQuit->Render());
        }
```

- [ ] **Step 3: Run `psb_tests` to confirm it's unaffected**

Run: `cmake --build tools/build --target psb_tests`
Expected: PASS (unchanged — `psb_tests` never links `main.cpp`, `board_switcher.h`, or `board_dashboard.h`).

- [ ] **Step 4: Confirm the app target now fails to build (expected)**

Run: `cmake --build tools/build --target psb_demo_tui`
Expected: FAIL — `main.cpp`'s calls to `makeBoardSwitcher`/`makeBoardDashboard` (via `buildRuntime`/`applyNewBoardsLive`) now have too few arguments. This confirms the signature change took effect; Task 5 fixes it.

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/tui/board_switcher.h tools/psb_demo_app/tui/board_dashboard.h
git commit -m "feat(psb_demo_tui): thread a globalGroup Component through board_switcher/board_dashboard (main.cpp not yet updated — expected build break until Task 5)"
```

---

### Task 5: Wire the "Group" button into `main.cpp`

**Files:**
- Modify: `tools/psb_demo_app/tui/main.cpp`

**Interfaces:**
- Consumes: `psb::tui::GroupWizardState`, `addGroup`/`removeGroup`/`addChannelToGroup`/`removeChannelFromGroup` (Task 2); `psb::tui::LiveBoardInfo`, `psb::tui::LiveBoards`, `psb::tui::makeGroupWizardScreen` (Task 3); the new `globalGroup` parameter of `makeBoardSwitcher`/`makeBoardDashboard` (Task 4).
- Produces: nothing new for later tasks — this is the final integration point for Phase 2.

- [ ] **Step 1: Add includes**

In `tools/psb_demo_app/tui/main.cpp`, add two includes right after `#include "wizard_screen.h"` (line 8):

```cpp
#include "wizard_screen.h"
#include "group_wizard_state.h"
#include "group_wizard_screen.h"
```

- [ ] **Step 2: Update `buildRuntime`'s signature and body**

Change the signature (line 277-281):

```cpp
void buildRuntime(Runtime& rt, const psb::TopologyConfig& topo, ScreenInteractive& screen,
                  std::atomic<bool>& running, int timeoutMs, bool autoConnectAll,
                  std::function<void()> openSetup, Component globalQuit, Component globalSetup,
                  Component globalGroup, Component globalPreferences,
                  Component globalConnectAll, Component globalDisconnectAll,
                  std::function<bool(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Update its `makeBoardDashboard` call (line 304-307):

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *bw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalGroup, globalPreferences, [&rt] { return rt.boards.size(); },
                saveChannelAliasToTopology);
```

Update its `makeBoardSwitcher` call (line 389-390):

```cpp
    rt.switcher = psb::tui::makeBoardSwitcher(rt.boards, globalQuit, globalSetup, globalGroup, globalPreferences,
                                              globalConnectAll, globalDisconnectAll);
```

- [ ] **Step 3: Update `applyNewBoardsLive`'s signature and body**

Change the signature (line 405-409):

```cpp
void applyNewBoardsLive(Runtime& rt, const psb::TopologyConfig& newTopo,
                        ScreenInteractive& screen, std::atomic<bool>& running,
                        int timeoutMs, std::function<void()> openSetup,
                        Component globalQuit, Component globalSetup, Component globalGroup,
                        Component globalPreferences,
                        std::function<bool(const std::string&, int, const std::string&)> saveChannelAliasToTopology) {
```

Update its `makeBoardDashboard` call (line 454-457):

```cpp
            b->dashboard = psb::tui::makeBoardDashboard(*b, *targetBw, screen, running, timeoutMs, openSetup,
                [&rt, &screen, &running, bPtr = b.get()] { removeBoardLive(rt, screen, running, bPtr); },
                globalQuit, globalSetup, globalGroup, globalPreferences, [&rt] { return rt.boards.size(); },
                saveChannelAliasToTopology);
```

- [ ] **Step 4: Declare `showGroupSetup`/`groupWiz` and define `openGroupSetup`**

In `main()`, right after `openSetup`'s definition (after line 658, before `Runtime rt;` at line 660), insert:

```cpp
    auto showGroupSetup = std::make_shared<bool>(false);
    auto groupWiz = std::make_shared<psb::tui::GroupWizardState>();

    // Mirrors openSetup exactly, including reseeding topologyPath on every
    // open (not just at construction) — the exact Critical bug Phase 1
    // shipped without, then had to fix after the fact (see the Global
    // Constraints section of this plan). groupWiz->topo is seeded from the
    // *full* live topo (not a groups-only copy) so Save
    // (group_wizard_screen.h's bSave) never truncates the file's
    // buses/boards.
    std::function<void()> openGroupSetup = [showGroupSetup, groupWiz, &topo, &currentTopologyPath, &screen] {
        groupWiz->topo = topo;
        groupWiz->topologyPath = currentTopologyPath;
        groupWiz->selectedGroup = -1;
        groupWiz->selectedChannel = -1;
        groupWiz->statusMsg.clear();
        *showGroupSetup = true;
        screen.PostEvent(Event::Custom);
    };
```

- [ ] **Step 5: Add the `bGlobalGroup` button**

Right after `bGlobalSetup`'s definition (line 672), insert:

```cpp
    auto bGlobalGroup = psb::tui::ActionButton("Group", [openGroupSetup] { openGroupSetup(); });
```

- [ ] **Step 6: Thread `bGlobalGroup` through `buildRuntime`'s call**

Update the `buildRuntime` call (line 730-732):

```cpp
    buildRuntime(rt, topo, screen, running, g_connectTimeoutMs, autoConnectAll, openSetup,
                bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences, bGlobalConnectAll, bGlobalDisconnectAll,
                saveChannelAliasToTopology);
```

- [ ] **Step 7: Thread `bGlobalGroup` through `onMidSessionFinish`'s `applyNewBoardsLive` call**

Update `onMidSessionFinish`'s capture list and body (line 734-751):

```cpp
    auto onMidSessionFinish = [showSetup, midSessionWiz, &rt, &topo, &currentTopologyPath, &screen, &running,
                               openSetup, bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences,
                               saveChannelAliasToTopology]
                              (psb::tui::WizardOutcome outcome) {
        if (outcome == psb::tui::WizardOutcome::ConnectNow) {
            removeGoneBoardsLive(rt, midSessionWiz->topo, screen, running);
            applyNewBoardsLive(rt, midSessionWiz->topo, screen, running, g_connectTimeoutMs, openSetup,
                               bGlobalQuit, bGlobalSetup, bGlobalGroup, bGlobalPreferences, saveChannelAliasToTopology);
            topo = midSessionWiz->topo;
            currentTopologyPath = midSessionWiz->topologyPath;
        }
        *showSetup = false;
        screen.PostEvent(Event::Custom);
    };
```

(Only the capture list and the `applyNewBoardsLive` call itself change; the rest of the body — including its comment block above it, not reproduced here — is unchanged.)

- [ ] **Step 8: Build `getLiveGroupBoards`, `onGroupSetupFinish`, and `groupWizardRoot`**

Right after `scanViaLiveBus`'s definition and right before `midSessionWizardRoot`'s definition (before line 793), insert:

```cpp
    // Snapshot of currently-connected boards for the group wizard's Add
    // Channel picker — Global Constraints require this scope (never every
    // board ever defined in the topology, connected or not). Called fresh
    // every time the picker opens (group_wizard_screen.h's bAddChannel),
    // never cached, so a board that connects/disconnects while the modal
    // sits open is picked up on the next open.
    auto getLiveGroupBoards = [&rt]() -> psb::tui::LiveBoards {
        psb::tui::LiveBoards result;
        std::lock_guard<std::mutex> lk(rt.boardsMutex);
        for (auto& b : rt.boards) {
            if (!b->connected.load()) continue;
            psb::tui::LiveBoardInfo info;
            info.nickname = b->nickname;
            info.numChannels = b->data.numChannels();
            for (int ch = 0; ch < info.numChannels; ++ch)
                info.aliases.push_back(b->inputs.chAlias[ch]);
            result.push_back(std::move(info));
        }
        return result;
    };

    // Syncs main()'s live topo (and the path it's tracked against) from
    // whatever the group wizard ended up with — same "sync back on finish"
    // pattern as onMidSessionFinish's topo = midSessionWiz->topo; without
    // this, a later alias edit's saveChannelAliasToTopology (which also
    // calls topo.save()) would overwrite the group wizard's own already-
    // saved-to-disk groups with an in-memory topo that never learned about
    // them.
    auto onGroupSetupFinish = [showGroupSetup, groupWiz, &topo, &currentTopologyPath, &screen] {
        topo = groupWiz->topo;
        currentTopologyPath = groupWiz->topologyPath;
        *showGroupSetup = false;
        screen.PostEvent(Event::Custom);
    };

    auto groupWizardRoot = psb::tui::makeGroupWizardScreen(*groupWiz, screen, onGroupSetupFinish, getLiveGroupBoards);
```

- [ ] **Step 9: Chain the Group modal onto the root**

Update `rootWithSetup`'s `Modal` chain (line 796-800):

```cpp
    auto rootWithSetup = Renderer(rt.switcher.root, [&rt, &topo, &screen, &running] {
        drainPendingRemovals(rt, topo, screen, running);
        return rt.switcher.root->Render();
    }) | Modal(midSessionWizardRoot, showSetup.get())
       | Modal(groupWizardRoot, showGroupSetup.get())
       | Modal(prefsDialog.root, showPreferences.get());
```

- [ ] **Step 10: Build the full app**

Run: `cmake --build tools/build --target psb_demo_tui`
Expected: SUCCESS (no errors, no warnings — the codebase builds with `-Wall -Wextra`; treat any new warning as a signature/capture mismatch to fix before moving on).

- [ ] **Step 11: Run the full test suite**

Run: `cmake --build tools/build --target psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests`
Expected: PASS, all test cases (no regressions from Tasks 1-4).

- [ ] **Step 12: Commit**

```bash
git add tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): wire a Group button + wizard modal into main.cpp, mirroring Topology's Setup wiring"
```

---

### Task 6: Manual verification

No automated coverage exists for FTXUI rendering, live multi-board aggregation, or the actual on-screen wizard flow — this matches every other UI feature in this codebase (see the design spec's own Testing section) and mirrors exactly how Phase 1's alias UI was verified.

**Protect the real topology file first**, per this session's established practice — every prior verification round in this feature backed up and restored `~/.psb_demo_app/topology.toml` around manual testing:

```bash
cp ~/.psb_demo_app/topology.toml /tmp/user_topology_backup_phase2.toml 2>/dev/null || echo "(no existing file — nothing to back up)"
```

- [ ] **Step 1: Launch and open the Group wizard**

Run `./tools/build/psb_demo_app/tui/psb_demo_tui` (or the multi-board topology entry point) under tmux, connect at least one board, and click "Group" (immediately right of "Topology" in the menu bar). Verify: the modal opens, titled "Group Wizard", with empty Groups/Channels panes and Save/Exit buttons.

- [ ] **Step 2: Add a group, add a channel, verify the member list**

Click "Add Group", type a name (e.g. "Bench Test"), click "Add". Verify the new group appears in the left pane. Select it, click "Add Channel" — verify the picker lists only channels from the currently-connected board(s), one entry per channel, showing the alias in parentheses if one was set in Phase 1. Pick one, click "Add". Verify it now appears in the right pane's member list as `"<nickname> <alias-or-CHn>"`.

- [ ] **Step 3: Verify Save writes the full topology, not just groups**

Click "Save". Verify the status line shows "Saved to <path>" in green. Read the saved file:

```bash
cat ~/.psb_demo_app/topology.toml   # or whatever path was shown
```

Confirm it contains BOTH the original `[[bus]]`/`[[bus.board]]` entries (with any `channel_aliases` from Phase 1 intact) AND a new `[[group]]` entry with a nested `[[group.channel]]` — this is the "Save never truncates" Global Constraint; if the bus/board section is missing, stop and treat it as a Critical bug (same severity as Phase 1's currentTopologyPath corruption).

- [ ] **Step 4: Verify the offline-member marker**

Disconnect the board whose channel was just added to the group (via that board's own Disconnect button). Reopen the Group wizard (Exit, then click "Group" again), select the same group. Verify the member list now shows that entry suffixed with `(offline)`, and that it's still present (not silently dropped).

- [ ] **Step 5: Verify Remove Group / Remove Channel**

With the group still selected, select the offline member and click "Remove Channel" — verify it disappears from the list and the group itself remains. Then select the group and click "Remove Group" — verify it disappears from the left pane. Click Save; re-check the file no longer contains that `[[group]]` entry.

- [ ] **Step 6: Verify the `topologyPath`-reseed-on-every-open fix**

This directly re-tests the Global Constraint carried over from Phase 1's own Critical bug. Load a **non-default** topology file (e.g. via the Topology wizard's Browse/Load, pointing at a test file, not `~/.psb_demo_app/topology.toml`). Without touching the Topology wizard again, open the Group wizard, add a group, click Save. Verify (via `cat`) that the group landed in the **non-default file just loaded**, and that `~/.psb_demo_app/topology.toml` (the real one, backed up in the preamble above) is untouched:

```bash
diff /tmp/user_topology_backup_phase2.toml ~/.psb_demo_app/topology.toml && echo "OK: real topology file untouched"
```

- [ ] **Step 7: Restore the real topology file**

```bash
cp /tmp/user_topology_backup_phase2.toml ~/.psb_demo_app/topology.toml 2>/dev/null || rm -f ~/.psb_demo_app/topology.toml
rm -f /tmp/user_topology_backup_phase2.toml
```

No commit for this task — it's verification only. If any step surfaces a bug, fix it with its own dedicated commit (following systematic-debugging if the root cause isn't immediately obvious) before proceeding to `finishing-a-development-branch`.

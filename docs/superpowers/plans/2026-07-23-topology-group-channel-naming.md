# Topology Group Channel Naming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move user-facing channel aliases from board-level display state to group membership, so board views use firmware channel indexes and group views use `group_name/alias` identities.

**Architecture:** Extend `GroupChannelRef` with an alias and keep old topology files loadable by deriving missing aliases from `CHn`. Add pure validation/mutation helpers in `group_wizard_state.h`, then update the FTXUI views with narrow options/callbacks rather than duplicating the monitor-table control logic. Board dashboards get a reverse group-membership lookup so channel tabs can edit the group alias and jump back to the group view.

**Tech Stack:** C++17, FTXUI, toml++, Catch2, existing `TopologyConfig`, `GroupWizardState`, `BoardSession`, `BoardSwitcher`, and monitor-tab helpers.

---

## File Structure

- `tools/psb_modbus_core/topology_config.h/.cpp`: add `GroupChannelRef::alias`, save/load TOML `alias`, normalize missing aliases to `CHn`.
- `tools/psb_modbus_core/tests/test_topology_config.cpp`: cover new schema and legacy loading.
- `tools/psb_demo_app/tui/group_wizard_state.h`: pure helper functions for alias uniqueness, assignment uniqueness, and alias editing.
- `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`: cover group alias constraints without FTXUI.
- `tools/psb_demo_app/tui/tab_monitor.h`: allow board monitor and group monitor to request different identity columns.
- `tools/psb_demo_app/tui/group_monitor.h`: render group aliases as row identity and `board_nickname/CHn` as jump button.
- `tools/psb_demo_app/tui/tab_channel.h`: render grouped alias controls and group-link button only when membership exists.
- `tools/psb_demo_app/tui/board_session.h`: make channel tab titles canonical `CHn`.
- `tools/psb_demo_app/tui/board_dashboard.h`: pass group membership callbacks into channel tabs.
- `tools/psb_demo_app/tui/board_switcher.h`: add jump-to-group support if not already present.
- `tools/psb_demo_app/tui/main.cpp`: replace board-alias saving with group-alias saving, build reverse membership lookup, refresh dashboards after edits.

## Task 1: Topology Schema Stores Group Channel Aliases

**Files:**
- Modify: `tools/psb_modbus_core/topology_config.h`
- Modify: `tools/psb_modbus_core/topology_config.cpp`
- Test: `tools/psb_modbus_core/tests/test_topology_config.cpp`

- [ ] **Step 1: Write failing topology tests**

Add tests covering alias round-trip and legacy missing-alias fallback:

```cpp
TEST_CASE("TopologyConfig - round trip preserves group channel aliases", "[topology_config]") {
    psb::TopologyConfig cfg;
    psb::BusConfig bus;
    bus.name = "bus1";
    bus.port = "/dev/ttyUSB0";
    bus.baudRate = 115200;
    bus.boards.push_back({"hvb-left", 1});
    cfg.buses.push_back(bus);

    psb::GroupConfig group;
    group.name = "detector";
    group.channels.push_back({"hvb-left", 0, "bias"});
    group.channels.push_back({"hvb-left", 1, "guard"});
    cfg.groups.push_back(group);

    const std::string path = "/tmp/psb_topology_group_alias_roundtrip.toml";
    std::remove(path.c_str());
    REQUIRE(cfg.save(path));

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->groups.size() == 1);
    REQUIRE(loaded->groups[0].channels.size() == 2);
    CHECK(loaded->groups[0].channels[0].alias == "bias");
    CHECK(loaded->groups[0].channels[1].alias == "guard");

    std::remove(path.c_str());
}

TEST_CASE("TopologyConfig - legacy group channel without alias loads as CHn", "[topology_config]") {
    const std::string path = "/tmp/psb_topology_legacy_group_alias.toml";
    {
        std::ofstream ofs(path);
        ofs << "[[bus]]\nname = 'bus1'\nport = '/dev/ttyUSB0'\nbaud_rate = 115200\n"
               "  [[bus.board]]\n  nickname = 'hvb-left'\n  slave_id = 1\n"
               "[[group]]\nname = 'detector'\n"
               "  [[group.channel]]\n  board = 'hvb-left'\n  channel = 2\n";
    }

    auto loaded = psb::TopologyConfig::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->groups.size() == 1);
    REQUIRE(loaded->groups[0].channels.size() == 1);
    CHECK(loaded->groups[0].channels[0].alias == "CH2");

    std::remove(path.c_str());
}
```

- [ ] **Step 2: Run tests and verify compile failure**

Run: `cmake --build tools/build --target psb_tests`

Expected: compile failure because aggregate initialization with three `GroupChannelRef` fields and `.alias` do not exist.

- [ ] **Step 3: Add alias field and default helper**

Change `GroupChannelRef` in `tools/psb_modbus_core/topology_config.h`:

```cpp
struct GroupChannelRef {
    std::string boardNickname;
    int channelIndex = 0;
    std::string alias;
};

inline std::string defaultChannelAlias(int channelIndex) {
    return "CH" + std::to_string(channelIndex);
}
```

Add `#include <string>` is already present; no extra dependency is needed.

- [ ] **Step 4: Load and save alias**

In `TopologyConfig::load`, after reading `channel`:

```cpp
ref.alias = (*chTbl)["alias"].value_or(std::string(""));
if (ref.alias.empty())
    ref.alias = defaultChannelAlias(ref.channelIndex);
```

In `TopologyConfig::save`, when serializing `group.channel`:

```cpp
chTbl.insert_or_assign("alias", ch.alias.empty() ? defaultChannelAlias(ch.channelIndex) : ch.alias);
```

- [ ] **Step 5: Update older test aggregate initializers**

Where existing tests use `group.channels.push_back({"hvb-bench", 0});`, keep them valid by adding the expected default explicitly:

```cpp
group.channels.push_back({"hvb-bench", 0, "CH0"});
```

- [ ] **Step 6: Verify and commit**

Run: `cmake --build tools/build --target psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests "[topology_config]"`

Expected: all `[topology_config]` tests pass.

Commit:

```bash
git add tools/psb_modbus_core/topology_config.h tools/psb_modbus_core/topology_config.cpp tools/psb_modbus_core/tests/test_topology_config.cpp
git commit -m "feat(psb_modbus_core): store aliases on group channels"
```

## Task 2: Group Wizard State Enforces Naming Rules

**Files:**
- Modify: `tools/psb_demo_app/tui/group_wizard_state.h`
- Test: `tools/psb_modbus_core/tests/test_group_wizard_state.cpp`

- [ ] **Step 1: Write failing tests for aliases and global assignment**

Add tests:

```cpp
TEST_CASE("GroupWizardState - aliases are unique only inside one group", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addGroup(s, "supply").empty());

    CHECK(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    CHECK(addChannelToGroup(s, 0, "hvb-left", 1, "bias") == "alias \"bias\" already in use in group detector");
    CHECK(addChannelToGroup(s, 1, "hvb-right", 0, "bias").empty());
}

TEST_CASE("GroupWizardState - board channel can be assigned to only one group", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addGroup(s, "supply").empty());

    CHECK(addChannelToGroup(s, 0, "hvb-left", 0, "bias").empty());
    CHECK(addChannelToGroup(s, 1, "hvb-left", 0, "bias") == "hvb-left/CH0 already assigned to group detector");
}

TEST_CASE("GroupWizardState - group channel alias can be renamed", "[group_wizard_state]") {
    GroupWizardState s;
    REQUIRE(addGroup(s, "detector").empty());
    REQUIRE(addChannelToGroup(s, 0, "hvb-left", 0, "CH0").empty());

    CHECK(renameGroupChannelAlias(s, 0, 0, "bias").empty());
    CHECK(s.topo.groups[0].channels[0].alias == "bias");
    CHECK(s.dirty);
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `cmake --build tools/build --target psb_tests`

Expected: failure because `addChannelToGroup` still has the old signature and `renameGroupChannelAlias` does not exist.

- [ ] **Step 3: Add helper functions**

In `group_wizard_state.h`, add:

```cpp
inline std::string boardChannelId(const std::string& boardNickname, int channelIndex) {
    return boardNickname + "/" + psb::defaultChannelAlias(channelIndex);
}

inline int findGroupForBoardChannel(const psb::TopologyConfig& topo,
                                    const std::string& boardNickname,
                                    int channelIndex) {
    for (int gi = 0; gi < static_cast<int>(topo.groups.size()); ++gi) {
        for (const auto& c : topo.groups[gi].channels)
            if (c.boardNickname == boardNickname && c.channelIndex == channelIndex)
                return gi;
    }
    return -1;
}

inline bool groupAliasInUse(const psb::GroupConfig& group,
                            const std::string& alias,
                            int exceptChannelIdx = -1) {
    for (int i = 0; i < static_cast<int>(group.channels.size()); ++i)
        if (i != exceptChannelIdx && group.channels[i].alias == alias) return true;
    return false;
}
```

- [ ] **Step 4: Change add and rename mutators**

Replace `addChannelToGroup` with:

```cpp
inline std::string addChannelToGroup(GroupWizardState& s, int groupIdx,
                                     const std::string& boardNickname,
                                     int channelIndex,
                                     const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    if (boardNickname.empty()) return "board required";
    if (channelIndex < 0) return "invalid channel index";
    std::string finalAlias = alias.empty() ? psb::defaultChannelAlias(channelIndex) : alias;
    auto assignedGroup = findGroupForBoardChannel(s.topo, boardNickname, channelIndex);
    if (assignedGroup >= 0)
        return boardChannelId(boardNickname, channelIndex) + " already assigned to group " +
               s.topo.groups[assignedGroup].name;
    auto& group = s.topo.groups[groupIdx];
    if (groupAliasInUse(group, finalAlias))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;

    group.channels.push_back({boardNickname, channelIndex, finalAlias});
    s.dirty = true;
    return "";
}

inline std::string renameGroupChannelAlias(GroupWizardState& s, int groupIdx,
                                           int channelIdx,
                                           const std::string& alias) {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(s.topo.groups.size()))
        return "invalid group index";
    auto& group = s.topo.groups[groupIdx];
    if (channelIdx < 0 || channelIdx >= static_cast<int>(group.channels.size()))
        return "invalid channel index";
    const auto& ref = group.channels[channelIdx];
    std::string finalAlias = alias.empty() ? psb::defaultChannelAlias(ref.channelIndex) : alias;
    if (groupAliasInUse(group, finalAlias, channelIdx))
        return "alias \"" + finalAlias + "\" already in use in group " + group.name;
    group.channels[channelIdx].alias = finalAlias;
    s.dirty = true;
    return "";
}
```

- [ ] **Step 5: Verify and commit**

Run: `cmake --build tools/build --target psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests "[group_wizard_state]"`

Expected: all group wizard state tests pass.

Commit:

```bash
git add tools/psb_demo_app/tui/group_wizard_state.h tools/psb_modbus_core/tests/test_group_wizard_state.cpp
git commit -m "feat(psb_demo_tui): enforce group channel naming rules"
```

## Task 3: Group Wizard Add Channel Uses Board-to-Channel Picker

**Files:**
- Modify: `tools/psb_demo_app/tui/group_wizard_screen.h`
- Modify: `tools/psb_demo_app/tui/main.cpp`

- [ ] **Step 1: Replace display names with group alias plus board channel ID**

Change `groupChannelDisplayName` to return `alias -> board/CHn`:

```cpp
inline std::string groupChannelDisplayName(const GroupChannelRef& ref) {
    std::string alias = ref.alias.empty() ? psb::defaultChannelAlias(ref.channelIndex) : ref.alias;
    return alias + " -> " + ref.boardNickname + "/" + psb::defaultChannelAlias(ref.channelIndex);
}
```

- [ ] **Step 2: Build assigned-channel filtering across all groups**

In the Add Channel picker rebuild logic, filter with:

```cpp
bool assigned = findGroupForBoardChannel(s.topo, lb.nickname, ch) >= 0;
if (assigned) continue;
```

Expected behavior: once `hvb-left/CH0` is in any group, it does not appear in the picker for any group.

- [ ] **Step 3: Replace the flat picker with board and channel menus**

Use two local selections:

```cpp
auto boardPickerLabels = std::make_shared<std::vector<std::string>>();
auto channelPickerLabels = std::make_shared<std::vector<std::string>>();
auto channelPickerRefs = std::make_shared<std::vector<GroupChannelRef>>();
auto selectedBoard = std::make_shared<int>(0);
auto selectedChannel = std::make_shared<int>(0);
auto newChannelAlias = std::make_shared<std::string>();
```

The rebuild function fills boards first, then channels for the selected board. Channel labels are canonical:

```cpp
channelPickerLabels->push_back(psb::defaultChannelAlias(ch));
channelPickerRefs->push_back({lb.nickname, ch, psb::defaultChannelAlias(ch)});
```

- [ ] **Step 4: Add alias input and call the new mutator**

Add `Input(newChannelAlias.get(), "alias")` to the modal. Before opening the modal and after channel selection changes, default an empty alias to the selected channel name:

```cpp
if (newChannelAlias->empty() && !channelPickerRefs->empty())
    *newChannelAlias = channelPickerRefs->at(*selectedChannel).alias;
```

On Add:

```cpp
const auto& ref = channelPickerRefs->at(*selectedChannel);
std::string err = addChannelToGroup(s, s.selectedGroup, ref.boardNickname, ref.channelIndex, *newChannelAlias);
```

- [ ] **Step 5: Update `main.cpp` live-board snapshot comments**

`LiveBoardInfo::aliases` is legacy. Stop showing board aliases in the picker. Keep `aliases` only if another caller still needs it; otherwise remove the field and its fill loop from `getLiveGroupBoards`.

- [ ] **Step 6: Verify and commit**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests "[group_wizard_state]"`

Manual check: open Group wizard, Add Channel, verify board list on the left, available `CHn` list on the right, alias input below, and already assigned channels hidden.

Commit:

```bash
git add tools/psb_demo_app/tui/group_wizard_screen.h tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): pick group channels by board and alias"
```

## Task 4: Monitor Table Supports Board and Group Identity Modes

**Files:**
- Modify: `tools/psb_demo_app/tui/tab_monitor.h`
- Modify: `tools/psb_demo_app/tui/group_monitor.h`

- [ ] **Step 1: Add monitor table mode options**

Add near `MonitorRow`:

```cpp
enum class MonitorIdentityMode {
    BoardChannel,
    GroupAlias,
};

struct MonitorRenderOptions {
    MonitorIdentityMode identityMode = MonitorIdentityMode::BoardChannel;
    std::string identityHeader;
};
```

- [ ] **Step 2: Split header generation**

Change `monitorHeaderLabels` to accept options:

```cpp
inline std::vector<std::string> monitorHeaderLabels(const ScannedData& data,
                                                    MonitorRenderOptions opts = {}) {
    ...
    std::vector<std::string> labels;
    labels.push_back(opts.identityHeader);
    if (opts.identityMode == MonitorIdentityMode::GroupAlias)
        labels.push_back("Name");
    labels.push_back(vsetEnHeader);
    ...
    return labels;
}
```

Board mode therefore removes `Name`; group mode uses one identity column for alias and no hardware ID column.

- [ ] **Step 3: Split row cell generation**

Change row helpers to make the identity string explicit and include alias input only in group mode:

```cpp
inline std::vector<Element> monitorRowCells(const std::string& identityLabel,
                                            const ScannedData& data,
                                            int ch,
                                            const MonitorRow& row,
                                            MonitorRenderOptions opts = {}) {
    std::vector<Element> cells;
    cells.push_back(text(identityLabel) | center);
    if (opts.identityMode == MonitorIdentityMode::GroupAlias)
        cells.push_back(row.aliasInp->Render() | center);
    ...
}
```

Apply the same option to `monitorOfflineRowCells`.

- [ ] **Step 4: Update board monitor calls**

In `makeMonitorTab`, call:

```cpp
auto headerLabels = monitorHeaderLabels(s.data, {MonitorIdentityMode::BoardChannel, "CH"});
grid.push_back(monitorRowCells(chLabel, s.data, ch, rows->at(ch), {MonitorIdentityMode::BoardChannel, "CH"}));
```

Expected: board Monitor columns are `CH`, `Vset/En`, `Status`, etc.; no `Name`.

- [ ] **Step 5: Update group monitor calls**

In `makeGroupDashboard`, call:

```cpp
auto groupOpts = MonitorRenderOptions{MonitorIdentityMode::GroupAlias, "Name"};
auto headerLabels = monitorHeaderLabels(*headerSource, groupOpts);
headerLabels.push_back("Go");
```

Group row identity is:

```cpp
std::string alias = mr.ref.alias.empty() ? psb::defaultChannelAlias(mr.ref.channelIndex) : mr.ref.alias;
```

The `Go` button label is:

```cpp
std::string label = mr.ref.boardNickname + "/" + psb::defaultChannelAlias(mr.ref.channelIndex);
```

- [ ] **Step 6: Verify and commit**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests`

Manual check: board Monitor has no `Name`; group Monitor has alias identity and `Go` button text `board/CHn`.

Commit:

```bash
git add tools/psb_demo_app/tui/tab_monitor.h tools/psb_demo_app/tui/group_monitor.h
git commit -m "style(psb_demo_tui): separate board and group monitor identities"
```

## Task 5: Board Channel Tabs Use Canonical Titles and Group Membership Controls

**Files:**
- Modify: `tools/psb_demo_app/tui/board_session.h`
- Modify: `tools/psb_demo_app/tui/tab_channel.h`
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`
- Modify: `tools/psb_demo_app/tui/main.cpp`

- [ ] **Step 1: Make channel tab titles canonical**

Change `rebuildChannelTitles` in `board_session.h` to ignore aliases:

```cpp
inline void rebuildChannelTitles(std::vector<std::string>& titles, int numChannels,
                                 const std::string* = nullptr) {
    titles.clear();
    titles.push_back("Monitor");
    for (int ch = 0; ch < numChannels; ++ch)
        titles.push_back("CH" + std::to_string(ch));
}
```

- [ ] **Step 2: Add group membership value type**

In `tab_channel.h` or a small header if reuse is needed:

```cpp
struct GroupMembership {
    bool grouped = false;
    std::string groupName;
    int groupIndex = -1;
    int memberIndex = -1;
    std::string alias;
};

using GetGroupMembership = std::function<GroupMembership(const std::string&, int)>;
using SaveGroupAlias = std::function<bool(const std::string&, int, const std::string&)>;
using JumpToGroup = std::function<void(const std::string&, int)>;
```

- [ ] **Step 3: Change channel tab signature**

Change `makeChannelTab` to receive `boardNickname`, `getGroupMembership`, `saveGroupAlias`, and `jumpToGroup`. The alias input binds to a local shared string that is refreshed from membership on render:

```cpp
auto groupAlias = std::make_shared<std::string>();
auto aliasInp = CommitInput(groupAlias.get(), "CH" + std::to_string(ch),
    [boardNickname, ch, groupAlias, saveGroupAlias] {
        if (saveGroupAlias)
            saveGroupAlias(boardNickname, ch, *groupAlias);
    });
```

- [ ] **Step 4: Render group controls conditionally**

In the live panel:

```cpp
auto membership = getGroupMembership ? getGroupMembership(boardNickname, ch) : GroupMembership{};
if (membership.grouped && *groupAlias != membership.alias)
    *groupAlias = membership.alias;
```

Render alias and group link only when grouped:

```cpp
Element groupEl = emptyElement();
if (membership.grouped) {
    groupEl = hbox({
        text(" Alias: ") | bold | color(Color::Cyan),
        aliasInp->Render() | size(WIDTH, EQUAL, 14),
        text(" "),
        groupLinkBtn->Render(),
    });
}
```

The link button calls:

```cpp
jumpToGroup(membership.groupName, membership.memberIndex);
```

- [ ] **Step 5: Thread callbacks through board dashboard**

Change `makeBoardDashboard` parameters to include:

```cpp
GetGroupMembership getGroupMembership,
SaveGroupAlias saveGroupAlias,
JumpToGroup jumpToGroup,
```

Pass them into every `makeChannelTab`.

- [ ] **Step 6: Implement callbacks in main**

Add helpers in `main.cpp`:

```cpp
auto findMembership = [&topo](const std::string& boardNickname, int ch) -> psb::tui::GroupMembership {
    for (int gi = 0; gi < static_cast<int>(topo.groups.size()); ++gi) {
        auto& group = topo.groups[gi];
        for (int mi = 0; mi < static_cast<int>(group.channels.size()); ++mi) {
            auto& ref = group.channels[mi];
            if (ref.boardNickname == boardNickname && ref.channelIndex == ch)
                return {true, group.name, gi, mi, ref.alias};
        }
    }
    return {};
};
```

For saving:

```cpp
auto saveGroupChannelAliasToTopology = [&topo, &currentTopologyPath, &screen]
    (const std::string& boardNickname, int ch, const std::string& alias) -> bool {
    for (auto& group : topo.groups) {
        for (auto& ref : group.channels) {
            if (ref.boardNickname != boardNickname || ref.channelIndex != ch) continue;
            ref.alias = alias.empty() ? psb::defaultChannelAlias(ch) : alias;
            bool ok = topo.save(currentTopologyPath);
            screen.PostEvent(ftxui::Event::Custom);
            return ok;
        }
    }
    return false;
};
```

- [ ] **Step 7: Verify and commit**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests`

Manual check: board channel tabs are `CHn`; grouped channels show alias edit and group link; ungrouped channels do not.

Commit:

```bash
git add tools/psb_demo_app/tui/board_session.h tools/psb_demo_app/tui/tab_channel.h tools/psb_demo_app/tui/board_dashboard.h tools/psb_demo_app/tui/main.cpp
git commit -m "feat(psb_demo_tui): show group alias controls on board channels"
```

## Task 6: Replace Board Alias Save Path With Group Alias Save Path

**Files:**
- Modify: `tools/psb_demo_app/tui/board_dashboard.h`
- Modify: `tools/psb_demo_app/tui/main.cpp`
- Modify: `tools/psb_demo_app/tui/board_session.h`

- [ ] **Step 1: Stop rebuilding tab titles after alias edits**

Remove alias-triggered `rebuildChannelTitles(...)` calls from the old board alias save closure. Channel titles are canonical and no longer depend on aliases.

- [ ] **Step 2: Keep legacy board aliases loaded but unused for new identity**

Leave the existing `board.channelAliases` load into `ConfigInputs::chAlias` only until every current caller has been removed. Then delete the fill loops:

```cpp
for (int ch = 0; ch < n; ++ch)
    b->inputs.chAlias[ch] = boardCfg.channelAliases[ch];
```

Expected: board `channel_aliases` no longer affect board tab titles, group row labels, or group picker labels.

- [ ] **Step 3: Remove old board alias save callback where dead**

If no callers remain, remove:

```cpp
std::function<void(int ch, const std::string& alias)> saveChannelAlias;
```

from `BoardSession`, and remove `saveChannelAliasToTopology` from `main.cpp`/`buildRuntime`/`applyNewBoardsLive`.

If any monitor control still requires a save callback for group alias rows, replace it with a group-aware callback local to `group_monitor.h`.

- [ ] **Step 4: Verify no references remain**

Run:

```bash
rg -n "saveChannelAlias|channelAliases|chAlias|rebuildChannelTitles\\([^\\n]*chAlias" tools/psb_demo_app/tui tools/psb_modbus_core
```

Expected: `channelAliases` remains only in topology compatibility and tests; `saveChannelAlias` is gone if no longer required; `chAlias` is gone unless still needed for temporary widget backing.

- [ ] **Step 5: Verify and commit**

Run: `cmake --build tools/build --target psb_demo_tui psb_tests && ./tools/build/psb_modbus_core/tests/psb_tests`

Commit:

```bash
git add tools/psb_demo_app/tui tools/psb_modbus_core
git commit -m "refactor(psb_demo_tui): retire board-level alias identity"
```

## Task 7: Final TUI Verification

**Files:**
- No required source changes unless verification finds a defect.

- [ ] **Step 1: Build final binaries**

Run:

```bash
cmake --build tools/build --target psb_demo_tui psb_tests
./tools/build/psb_modbus_core/tests/psb_tests
```

Expected: build succeeds; all tests pass.

- [ ] **Step 2: Manual single-board verification**

Run the TUI against `last_single.toml` or single-board quick connect:

```bash
tmux new-session -d -s codex_group_naming_single './tools/build/psb_demo_app/tui/psb_demo_tui'
```

Expected:

- app top menu remains top-level
- no Connect All/Disconnect All buttons in single-board mode
- board Monitor has no `Name` column
- channel tab titles are `CHn`
- ungrouped channel tabs show no group-link button

- [ ] **Step 3: Manual multi-board/group verification**

Run the TUI with a topology containing at least one group:

```bash
tmux new-session -d -s codex_group_naming_multi './tools/build/psb_demo_app/tui/psb_demo_tui --topology ~/.psb_demo_app/topology.toml'
```

Expected:

- group dashboard row identity is group alias
- group dashboard `Go` button labels are `board_nickname/CHn`
- clicking `Go` opens the corresponding board channel tab
- grouped board channel tab shows alias editor and group link
- editing alias in group dashboard or board channel tab updates the same `group.channel.alias` in topology
- Add Channel picker is board -> channel and hides globally assigned board channels

- [ ] **Step 4: Clean up tmux sessions**

Run:

```bash
tmux kill-session -t codex_group_naming_single
tmux kill-session -t codex_group_naming_multi
```

Expected: no verifier sessions left running.

- [ ] **Step 5: Commit verification-only fixes if needed**

If verification required fixes:

```bash
git add tools/psb_demo_app/tui tools/psb_modbus_core
git commit -m "fix(psb_demo_tui): polish group channel naming flow"
```

If no fixes were needed, do not create an empty commit.

## Self-Review

- Spec coverage: topology schema, uniqueness rules, board dashboard, group dashboard, group wizard, navigation, and tests are each covered by a task.
- Placeholder scan: no open implementation placeholders remain.
- Type consistency: group alias ownership is consistently `GroupChannelRef::alias`; board hardware identity is consistently `boardNickname + "/" + defaultChannelAlias(channelIndex)`.

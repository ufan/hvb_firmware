# Channel Alias Names & Channel Groups — Design

## Context

`psb_demo_tui` currently identifies every channel only by its canonical
index-based name (`CH0`, `CH1`, ...) and shows every board's channels in one
fixed per-board Monitor table. Requested addition, in three parts:

1. **Channel aliases** — a user-editable display name per channel, more
   recognizable than `CHn`, shown alongside the canonical name.
2. **Channel groups** — user-defined, user-named collections of channels
   (potentially spanning multiple boards) rendered as their own aggregating
   monitor table — "a view into the same underlying board channels," not a
   second copy of the data or a second poll.
3. **Sidebar integration** — a new "Group" button opens a group-setup
   wizard (mirroring the existing Topology wizard); the vertical tab bar
   splits into a groups section (top) and the existing boards section
   (bottom).

Both aliases and groups must work identically in single-board and
multi-board mode, and persist in the same topology config file the rest of
the app already reads/writes (`TopologyConfig`, `topology_config.h`).

This is comparable in scope to the original multi-board-topology work
(`docs/superpowers/specs/2026-07-20-multi-board-topology-design.md`), which
was split into three phase plans under one shared design. This spec follows
the same structure: one design, three phase plans at implementation time.

- **Phase 1 — Channel aliases**: data model + inline-editable alias in the
  Monitor table and Channel tab, auto-saved.
- **Phase 2 — Group data model + wizard**: persisted groups, a new "Group"
  button and setup wizard (add/remove groups and member channels).
- **Phase 3 — Group view in the sidebar**: the two-section tab bar, the
  aggregating group monitor table, and the jump-to-source-channel link
  column.

## Confirmed Decisions

- **Alias save trigger**: auto-save to the topology file immediately on
  commit (Enter) — the same "commit on Enter" UX every other per-channel
  editable field in this codebase already uses (via `CommitInput`), though
  the alias field itself is a plain `Input` since no hardware write is
  involved — not staged behind the Topology wizard's own Save button.
- **Tab-bar selection model**: groups and boards share **one** active
  selection (today's single `Container::Tab` index), not two independently
  browsable stacks — whichever entry, group or board, was clicked last is
  the one shown.
- **Offline/missing group members**: a group's monitor table always shows
  every member channel it's configured with, rendering an offline/
  placeholder row (mirroring how a board's own Monitor table already shows
  an offline channel) for any member whose board is currently disconnected
  or no longer in the topology — membership itself is never silently
  dropped by this. The group wizard's own member list follows the same
  rule when reopened.
- **Default alias display**: an unset alias shows as an empty input with
  the canonical `CHn` name as placeholder text — never pre-filled with real
  `CHn` content — so "no alias set" stays visually distinct from "alias is
  literally CH0." Anywhere else a channel's name is shown (tab titles, the
  group view's link-button labels), it falls back to `CHn` when no alias
  is set.
- **Group channel picker scope**: the group wizard's "Add Channel" picker
  only ever lists channels from **currently-connected** boards, per the
  original request — it is not a picker over every board ever defined in
  the topology file, connected or not.
- **Groups reference channels by board nickname + channel index**, not bus/
  port — nicknames are already the codebase's de facto unique board
  identifier (used the same way by `detachBoard`), so group membership
  survives a board moving to a different port.

## Architecture

### Data model & persistence (`tools/psb_modbus_core/topology_config.h/.cpp`)

```cpp
struct BoardConfig {
    std::string nickname;
    int slaveId = 1;
    std::vector<std::string> channelAliases;  // index = channel number; "" = no alias
};

struct GroupChannelRef {
    std::string boardNickname;
    int channelIndex = 0;
};

struct GroupConfig {
    std::string name;
    std::vector<GroupChannelRef> channels;
};

struct TopologyConfig {
    std::vector<BusConfig> buses;
    std::vector<GroupConfig> groups;   // new, sibling to buses
    ...                                // load/save/singleBoard/etc. unchanged in shape
};
```

TOML shape mirrors the existing bus/board nesting (toml++, same tolerant-
load style as today — a malformed group/channel entry is skipped, not
fatal):

```toml
[[bus.board]]
nickname = "hv-up"
slave_id = 2
channel_aliases = ["Cell1", "", "Cell3"]

[[group]]
name = "Battery Bank"
  [[group.channel]]
  board = "hv-up"
  channel = 0
```

`channelAliases` loads once into each `BoardSession` at construction time
(`buildRuntime`'s initial sweep and `applyNewBoardsLive`'s hot-attach, both
in `main.cpp` — the same two places `nickname`/`portVal`/etc. are already
populated from `BoardConfig`), not re-fetched on every connect/reconnect —
it's config data, not live hardware state that changes with the connection
lifecycle.

### Phase 1 — Channel aliases

A new `std::string chAlias[MAX_CHANNELS]` joins `ConfigInputs`
(`tools/psb_demo_app/tui/widgets.h`) — the same struct that already backs
`targetV`/`ruStep`/every other per-channel editable field, and the
mechanism `tab_monitor.h` and `tab_channel.h` already rely on to keep two
separate widget instances (one per tab) showing consistent content for the
same underlying channel.

- **`MonitorRow`/`makeMonitorRow`** (`tab_monitor.h`): gains a new
  `aliasInp` — a plain `Input` (no hardware write involved, so no
  `CommitInput`) bound to `inputs.chAlias[ch]`, added to the row's focus
  chain (`rowWidgets`). A new "Alias" column is inserted into the fixed
  header/cell sequence the Monitor table already builds.
- **`tab_channel.h`**: gains a second `Input` instance bound to the *same*
  `inputs.chAlias[ch]` string, placed in the live-info header panel.
  Editing either tab's field shows on the other on next render — same
  mechanism `targetV`'s two widget instances already use.
- **Auto-save on commit**: the alias `Input`'s commit handler (FTXUI
  `InputOption`'s `on_enter`, matching how other committed fields in this
  codebase confirm edits) writes `inputs.chAlias[ch]` into the owning
  board's `BoardConfig::channelAliases[ch]` inside the live `topo` object
  and calls `topo.save(topologyPath)` immediately — mirroring the Topology
  wizard's own `bSave` handler, just triggered per-edit instead of by a
  button click. A save failure surfaces via the existing status-message
  convention (not a popup), matching every other status-message use in
  this codebase.
- **Tab titles**: `rebuildChannelTitles` (`board_session.h`) gains an
  alias lookup — a channel tab's title shows its alias if set, else falls
  back to `"CHn"`.

### Phase 2 — Group data model + wizard

New files mirroring the existing wizard's data/UI split
(`wizard_state.h` / `wizard_screen.h`):

- **`group_wizard_state.h`**: pure data + hardware-free mutators (no FTXUI,
  no I/O), same shape and testability as `WizardState`:

```cpp
struct GroupWizardState {
    psb::TopologyConfig topo;   // seeded from the live topo, same pattern as WizardState
    std::string topologyPath;
    int selectedGroup = -1;
    std::string statusMsg;
    bool dirty = false;
};

std::string addGroup(GroupWizardState&, const std::string& name);
std::string removeGroup(GroupWizardState&, int idx);
std::string addChannelToGroup(GroupWizardState&, int groupIdx,
                              const std::string& boardNickname, int channelIndex);
std::string removeChannelFromGroup(GroupWizardState&, int groupIdx, int channelIdx);
```

Each mutator returns `""` on success or a human-readable error string on
failure (empty name, duplicate group name, channel already in group, etc.)
— exact validation rules finalized during plan-writing by mirroring
`wizard_state.h`'s existing `addBus`/`addBoard` validation style.

- **`group_wizard_screen.h`**: `makeGroupWizardScreen(GroupWizardState&,
  ScreenInteractive&, onFinish, liveBoards)` — a smaller sibling of
  `makeWizardScreen`: a left-pane `groupMenu` (group names) + a right-pane
  list of the selected group's member channels, an "Add Group" modal (name
  field), an "Add Channel" modal (a `Menu` populated by scanning
  `liveBoards` — **currently-connected boards only** — one entry per
  `"nickname CHn"`, showing the channel's alias if set), Remove buttons
  gated with `Maybe()` on selection, and Save/Exit. Same visual chrome
  (bordered two-pane layout, status line, save-success/error message
  convention) as the Topology wizard.
- **Wiring** (`main.cpp` + `board_switcher.h`): a `"Group"` `ActionButton`
  added immediately after `"Topology"` in `globalMenuBar`'s children and
  both existing render sites (the multi-board global row and the
  single-board dashboard's merged row) — mirrors `bGlobalSetup`/
  `showSetup`/`onMidSessionFinish`/`Modal(...)` exactly: its own
  `showGroupSetup` flag, a `groupWizardRoot` built once, and a second
  `Modal(groupWizardRoot, showGroupSetup.get())` chained onto the root.
- **Existing-but-now-offline members**: reopening the wizard still shows
  every configured member of a group, including ones whose board isn't
  currently connected (greyed out / marked offline, per the Confirmed
  Decisions) — they're just not addable fresh from the Add Channel picker,
  which only lists currently-connected boards.

### Phase 3 — Group view in the sidebar

- **`board_switcher.h` restructure**: the single flat `boardNames` `Menu`
  becomes two — a `groupNames` `Menu` (from `topo.groups`) stacked above
  the existing `boardNames` `Menu` — both mapping into **one** shared
  selection index and **one** `Container::Tab`, per the Confirmed
  Decisions. Concretely: one flat index space ordered groups-first-then-
  boards; `Container::Tab`'s children are `[group1Dashboard,
  group2Dashboard, ..., board1Dashboard, board2Dashboard, ...]` in that
  same order; clicking either Menu sets the same underlying index. The
  boards menu's existing connection-status-dot `transform` is unchanged;
  the groups menu gets no status dot of its own (a group isn't itself
  connected/disconnected — its member rows each carry their own status,
  same as today's Monitor table).
- **Group monitor view** (new `group_monitor.h`, reusing `tab_monitor.h`'s
  per-row rendering): builds one row per member channel by looking up that
  channel's owning `BoardSession` via the group's `boardNickname`, reusing
  `makeMonitorRow`'s existing per-cell logic verbatim — the Monitor table's
  capability-conditional cells (`hasVolt`/`hasCurr`/`hasDrive`, checked per
  channel already, not per board) mean a group mixing channels from
  different board models "just works" through the same code path, no new
  heterogeneity handling needed. One new trailing column: a link
  `ActionButton` labeled with the channel's alias (falling back to `CHn`)
  that, on click, performs a compound jump — switches the shared active
  index to that channel's board *and* sets that board's own `activeTab` to
  the target channel's Channel tab. An offline/missing member (per the
  Confirmed Decisions) renders the same placeholder row a board's own
  Monitor table uses for an offline channel, with no link button (nothing
  to jump to).
- **Attach/detach bookkeeping**: `board_switcher.h`'s existing index-shift
  logic (`detachBoard`, today only for the boards side) grows a parallel
  group-side counterpart for group add/remove, keeping the shared index
  consistent when a group is added or removed via the wizard while the
  dashboard is live. Flagged as the highest-mechanical-risk piece of Phase
  3 — its own dedicated plan task with explicit before/after index-math
  test cases.

## Error Handling

- Alias auto-save failure (unwritable topology path): status message only,
  matching every other save-failure path in this codebase — no popup, no
  blocking the UI.
- Group wizard validation errors (duplicate name, empty name, channel
  already in group, no board selected): returned as strings from the pure
  mutator functions, displayed in the wizard's existing status-line
  convention — same UX as the Topology wizard's own validation errors.
- A group referencing a board nickname that no longer exists anywhere in
  the topology (e.g., the board itself was later removed, not just
  disconnected) is handled identically to "board currently disconnected" —
  the offline/placeholder row, never a crash or a dropped group.

## Testing

Consistent with this codebase's established practice: `GroupWizardState`'s
pure mutators get unit tests (mirroring `test_topology_config.cpp` for the
new `TopologyConfig` fields, and `wizard_state.h`'s own test coverage style
for the group mutators) — this is real, hardware/FTXUI-free logic. The rest
(FTXUI rendering, live multi-board aggregation, the sidebar's two-section
split, the compound jump-to-channel navigation) is manual/tmux-driven,
matching every other UI feature in this codebase, protecting the real
`~/.psb_demo_app/topology.toml` the same way every prior round did.

## Out of Scope

- Renaming a board's own nickname from inside a group view.
- Drag-reordering groups or their member channels (add/remove only, no
  reordering UI).
- Any change to a board's own Monitor table's non-alias columns.
- Any change to polling/data-acquisition — groups are a pure rendering
  projection over already-polling boards, never a second poll or a new
  worker thread.

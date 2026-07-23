# Topology and Group Channel Naming

Date: 2026-07-23
Status: Draft

## Scope

This design updates `psb_demo_tui` and `tools/psb_modbus_core` topology
configuration so board/channel views and group/channel views use distinct,
stable names. It supersedes the board-level channel alias behavior introduced
for the first group dashboard pass.

Firmware protocol behavior is out of scope. The board firmware still reports a
channel index, and that index remains the canonical hardware-facing channel
identity.

## Motivation

The current TUI uses one "Name" field for both board channels and group
members. That blurs two different user models:

- Topology view is hardware-facing. Users first define which boards exist and
  how to reach them.
- Group view is operator-facing. Users then arrange board channels into named
  groups and assign meaningful channel aliases inside each group.

When a single board has multiple channel groups, the shared alias model makes
the board view look like a group view and makes duplicate or misplaced buttons
hard to interpret.

## Vocabulary

- **Board nickname**: user-defined board name in the topology. It must be
  unique within the topology.
- **Board channel ID**: canonical hardware-facing channel identity:
  `board_nickname/CHn`.
- **Group name**: user-defined group name. It must be unique within the
  topology.
- **Group channel alias**: user-defined operator-facing channel name within a
  group. It must be unique only inside that group.
- **Group channel ID**: canonical operator-facing channel identity:
  `group_name/alias`.

`board_nickname/CHn` and `group_name/alias` are both globally unique, but they
serve different perspectives. The same alias text may be reused in two
different groups.

## Data Model

`GroupChannelRef` gains an alias field:

```cpp
struct GroupChannelRef {
    std::string boardNickname;
    int channelIndex = 0;
    std::string alias;
};
```

The topology TOML stores the alias on each group channel:

```toml
[[group]]
name = "detector"

  [[group.channel]]
  board = "hvb-left"
  channel = 0
  alias = "bias"
```

Legacy topology files without `alias` remain valid. When loading a missing or
empty alias, the TUI treats it as `CHn`. On the next save, the alias is written
explicitly.

Existing board-level `channel_aliases` are legacy display data. They may still
load for compatibility, but new group-channel identity is owned by
`GroupChannelRef::alias`, not by `BoardConfig::channelAliases`.

## Constraints

The topology editor and save path must enforce:

- board nicknames are unique
- group names are unique
- aliases are unique within their own group
- a board channel can belong to at most one group at a time
- a group channel always references a board channel
- a board channel may be ungrouped

The group wizard must hide board channels already assigned to any group until
they are removed from that group.

## TUI Behavior

### Board Dashboard

The board Monitor table removes the `Name` column. Board rows use channel
index labels (`CH0`, `CH1`, etc.) as the canonical board-view identity.

Channel tabs are always named by channel index, never alias. A channel tab
shows group alias controls only when that board channel is assigned to a
group:

- editable group alias field
- link button to the corresponding group channel

Ungrouped board channels do not show a group-link button.

### Group Dashboard

The group Monitor table removes the hardware ID column. The first visible
identity for each row is the group channel alias.

The last column is the board-channel jump action, and its button label is
`board_nickname/CHn`. Clicking it jumps to the board dashboard channel tab.

Alias edits made in the group dashboard update the corresponding
`GroupChannelRef::alias` and save through the current topology path.

### Group Wizard

The Add Channel dialog uses a two-level selection:

1. board
2. available channel under that board

Only unassigned board channels appear. After a board channel is selected, the
user specifies the group channel alias before adding it. The alias input
defaults to `CHn` and validates uniqueness within the selected group.

The existing group member list displays `alias -> board_nickname/CHn` so both
the operator-facing and hardware-facing identities are visible.

## Navigation

The runtime maintains a reverse lookup from `board_nickname/CHn` to its group
membership. Board channel tabs use that lookup to decide whether to show alias
and group-link controls.

Group dashboard rows keep the existing jump-to-board behavior, but the button
label changes from alias-like text to the board channel ID.

## Testing

Automated coverage should include:

- topology load/save round trip for group channel aliases
- loading legacy group channels without aliases
- duplicate board nickname rejection
- duplicate group name rejection
- duplicate alias rejection inside one group
- alias reuse allowed across different groups
- a board channel cannot be assigned to two groups
- board channel tab titles stay `CHn`

Manual TUI verification should cover:

- board Monitor has no `Name` column
- group Monitor has no hardware ID column and uses alias identity
- group dashboard jump buttons show `board_nickname/CHn`
- Add Channel dialog shows board-to-channel hierarchy
- already assigned channels disappear from the picker
- grouped channel tab shows alias edit and group link
- ungrouped channel tab has no group-link button

# PSB Demo TUI User Guide

A quick-start reference for `psb_demo_tui`, the interactive terminal dashboard for
live monitoring and control of one or more Jianwei voltage-control boards over
Modbus RTU. This is an **engineering / bring-up / demo tool**, not the factory
calibration tool — it has no calibration-mode UI. For calibrating a board,
see [`calibration-guide.md`](calibration-guide.md).

*(中文版本: [demo-tui-guide.zh.md](demo-tui-guide.zh.md))*

---

## 1. Build and launch

```bash
cmake -S tools -B tools/build
cmake --build tools/build --target psb_demo_tui -j
tools/bin/psb_demo_tui
```

The TUI starts with a mode selector:

| Mode | Use it for | Saved file |
|---|---|---|
| `Single Board` | Quick bring-up of one board on one serial port. | `~/.psb_demo_app/last_single.toml` |
| `Multi-Board Topology` | A named topology of buses, boards, and optional channel groups. | `~/.psb_demo_app/topology.toml`, or a file selected in the topology wizard |

App-level runtime preferences, such as connection timeout and poll interval,
are saved separately in `~/.psb_demo_app/preferences.toml` from the
Preferences button.

---

## 2. Screen layout

```
┌ [Topology] [Group] [Preferences]       PSB Demo TUI (vX.Y.Z)                              [Quit] ┐
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Groups       │ PSB │ 2 Channels │ [Normal ▾] │ [ Save ]   ● 20m 34s | T:24.1C H:41.2% [Disconnect] │
│  detector    ├───────────────────────────────────────────────────────────────────────────────────┤
│ Boards       │ Monitor    CH0    CH1                                                            │
│  board1      ├───────────────────────────────────────────────────────────────────────────────────┤
│              │                                                                                   │
│              │              (selected board tab, or selected group dashboard)                     │
│              │                                                                                   │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│ OK: Target V                                  FW:0x0102  Proto:1.0  /dev/ttyUSB0 @115200 #1 [Setting] │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

| Region | Contents |
|---|---|
| App menu bar | Top-level actions: Topology, Group, Preferences, optional multi-board connect/disconnect actions, centered app title/version, Quit |
| Sidebar | Group dashboards first, then board dashboards; hidden when there is only one board and no groups |
| Board menu bar | Board-local title, channel count, Working Mode selector, Save button, connection indicator, uptime, temperature/humidity, Connect/Disconnect/Abort toggle |
| Tab bar | `Monitor` + one `CHn` tab per channel the selected board reports |
| Main content | The selected board Monitor table, selected board channel tab, or selected group dashboard |
| Status bar | Last operation's result message; board dashboards also show firmware/protocol version, connection string, and `[Setting]` |

Navigation: **Tab** moves focus between controls, arrow keys move within a
row and cycle selector values, **Enter** commits a text field or a cycler,
**Space**/**←**/**→** also cycle a selector. Mouse click works everywhere.
**Esc** closes whichever modal is open.

---

## 3. Connecting

In `Single Board` mode, click **[ Connect ]** on the board menu bar to open
the Connection Settings modal:

```
┌ Connection Settings ─────────────┐
│ Port  : [/dev/ttyUSB0 ▾] [Rescan]│
│ Baud  : [115200]                 │
│ Slave : [1]                      │
│                                   │
│      [ Connect ]  [ Cancel ]     │
└───────────────────────────────────┘
```

- **Rescan** re-lists available serial ports.
- Fill in Baud/Slave, then **[ Connect ]**. The button reads **[ Abort ]**
  while a connection attempt is in flight, and **[ Disconnect ]** once
  connected — it's the same button throughout.
- The connection indicator dot: hollow gray = offline, yellow hourglass =
  connecting, breathing green = connected.
- On disconnect, all channel tabs collapse back to just `Monitor` — they
  reappear after the next successful connect and channel scan.

In multi-board topology mode, each board has the same board-local connection
button. The app menu also exposes `Connect All` / `Disconnect All` when more
than one board is configured.

---

## 4. Menu bars

The app menu bar is always a dedicated top-level bar. It does not collapse into
the selected board's menu, even in single-board mode.

### App menu bar

| Element | Behavior |
|---|---|
| `[Topology]` | Opens the topology wizard for single-board or multi-board hardware configuration |
| `[Group]` | Opens the group wizard for operator-facing channel groups |
| `[Preferences]` | Edits app runtime preferences such as timeout and poll interval |
| `[Connect All]` / `[Disconnect All]` | Multi-board mode only; starts or stops every board session |
| Center title | App title and TUI version |
| `[Quit]` | Exits the application |

### Board menu bar

| Element | Behavior |
|---|---|
| `N Channels` | Channel count reported by the board (`--` until connected) |
| Working Mode `[Normal ▾]` / `[Automatic ▾]` | Click, Space, or ←/→ to cycle — **commits immediately on every change**, unlike the per-channel cyclers described below. See [`operating-mode-guide.md`](operating-mode-guide.md) for what each mode actually changes. |
| `[ Save ]` | Same as `sys save` in the factory tool: persists op-config + system config for **all** channels to NVS. Dim/inactive while disconnected. |
| Center group | Connection dot, human-readable uptime, board temperature, humidity — only shown when connected |
| `[ Connect / Disconnect / Abort ]` | See §3 |

---

## 5. Board Monitor tab

One row per board channel, all key control fields in one table. Board view uses
the firmware-reported channel index (`CH0`, `CH1`, ...) as the channel's
canonical name.

```
┌────┬────────┬────────┬────────┬────────┬─────────┬─────┬─────┬───────┬───────┬───────┬──────┐
│    │  Vset  │ Status │  Vop   │  V (V) │  I (nA) │ Ru  │ Rd  │ Limit │ Fault │ Clear │ Save │
╞════╪════════╪════════╪════════╪════════╪═════════╪═════╪═════╪═══════╪═══════╪═══════╪══════╡
│CH0 │ +500.0 │ [ ON ] │ +500.0 │ +499.8 │  +12.3  │ 5.0 │ 5.0 │ 1000  │  --   │ Clear │ Save │
│CH1 │  +0.0  │ [ OFF ]│  +0.0  │  +0.0  │  +0.0   │ 5.0 │ 5.0 │ 1000  │  --   │ Clear │ Save │
└────┴────────┴────────┴────────┴────────┴─────────┴─────┴─────┴───────┴───────┴───────┴──────┘
```

| Column | Meaning | Editable? |
|---|---|---|
| `Vset` | Configured target voltage (V) | Type a value, **Enter** to write |
| `Status` | `[ OFF ]` / `[ ON ]` / `[ RAMP ]` — see click behavior below | Click to toggle |
| `Vop` | Operational target voltage (live, ramps toward `Vset`) | read-only |
| `V (V)` | Measured voltage | read-only |
| `I (nA)` | Measured current | read-only |
| `Ru` / `Rd` | Ramp-up / ramp-down step (V per interval) | Type, **Enter** to write |
| `Limit` | Current-protection threshold (nA) | Type, **Enter** to write |
| `Fault` | See mode-dependent semantics below | read-only |
| `Clear` | Clears a fault — target depends on protection mode, see below | Click |
| `Save` | Persists this channel's op-config to NVS (`param save`, this channel only) | Click |

A column shows a dimmed `--` instead of a control when the channel lacks the
underlying capability (e.g. no current measurement → no `I (nA)`, `Limit`,
`Fault`, or `Clear`).

**Status button click behavior** — a single click either enables or
gracefully disables, depending on current state:

| Current state | Click does |
|---|---|
| Off, target voltage is 0 | Nothing (no-op) |
| Off, target voltage is non-zero | Enable |
| Ramping | Nothing (no-op — wait for it to settle) |
| On (output drive nonzero) | Disable, gracefully |

**Fault column** is mode-dependent — it shows different things depending on
that channel's current-protection mode (set on the channel tab, §6):

| Protection mode | Fault column shows | Clear button targets |
|---|---|---|
| `Apply-Action` | The **active** fault (protection actually disabled the output) | Active fault block |
| `FlagOnly` | **Fault history** (the only trace left — the output was never touched) | Fault history |
| `Disabled` | Active fault, dimmed (still meaningful for non-current faults) | Active fault block |

Fault codes: `CL` current, `MI` measurement, `HW` hardware, `IL` interlock,
`RE` retry-exhausted, `CI` config-invalid, `ST` stale. Multiple codes can
appear together (e.g. `CL RE` after auto-retry gives up).

---

## 6. Per-channel tab (`CHn`)

```
 Live │ Vop:+500.0 V  V:+499.8 V  I:+12.3 nA  Status:ON  Retries:0 │ Control │ [Enable] [Disable] [Kill] │ Name: [bias] [detector/bias]

┌ Startup Policy ───────────┐  ┌ Recovery Policy ────────────┐
│ Vset    : +500.0      V  │  │ Policy : [AutoRetry ▾]      │
│ Ramp up : 5.0        V/s │  │ Max    : 3    Win: 60    s │
│ Ramp dn : 5.0        V/s │  │ Delay  : 10               s │
└───────────────────────────┘  │ Derate : 0               LSB│
┌ Protection Policy ────────┐  │ Band   : 10                %│
│ Limit  : 1000        nA  │  └─────────────────────────────┘
│ Mode   : [Apply-Action ▾]│  ┌ Setting ────────────────────┐
│ Action : [Dis-Graceful ▾]│  │ [Save] [Load] [Factory]    │
│              [ClrActive] │  └─────────────────────────────┘
│              [ClrHist]   │
└───────────────────────────┘
```

### Live panel
Read-only: operational target, measured V/I, status badge (`OFF` / `ON` /
`RAMP` / `ON RAMP` / `FAULT` / `COOL` / `STALE`), and the current auto-retry
count in the sliding window. `[Enable]` / `[Disable]` / `[Kill]` send
`Enable` / `DisableGraceful` / `DisableImmediate` immediately on click. Hidden
if the channel has no output capability.

### Startup Policy panel
- `Vset`, `Ramp up`, `Ramp dn` — text fields, **Enter** to commit each.

### Name control
Shown at the end of the Live row when this board channel belongs to a group.
Editing the name changes the group-channel alias and syncs to the group
dashboard. The adjacent group/name button jumps back to that group channel.
Ungrouped board channels do not show a group jump button.

### Protection Policy panel
Shown only if the channel has both voltage and current measurement
capability. `Limit` is a text field (Enter to commit). `Mode` and `Action`
are cyclers: click, Space, or ←/→ steps through the options, but **unlike
the board menu-bar mode selector, these do not auto-commit** — press **Enter**
after cycling to actually write the change. `[ClrActive]` / `[ClrHist]`
clear the active fault block / fault history directly, independent of the
Monitor tab's mode-dependent Clear button.

### Recovery Policy panel
`Policy` cycles through `ManualLatch` / `AutoRetry` / `AutoDerate` /
`NeverRetry` (same commit-on-Enter rule as Mode/Action above). `Max`, `Win`,
`Delay`, `Derate`, `Band` are text fields, each committed independently on
Enter. See [`operating-mode-guide.md`](operating-mode-guide.md) §4 for what
each field controls — the UI here is a direct mirror of that model, with no
extra behavior of its own.

### Setting panel
`[Save]` / `[Load]` / `[Factory]` — per-channel op-config persistence
(`param save|load|factory-reset`, this channel only). Distinct from the
board menu-bar `[ Save ]`, which covers every channel plus system config at
once.

---

## 7. System Config popup

Opened via **[Setting]** on the status bar (bottom-right):

```
┌ System Config ────────────────┐
│ Working Mode  : [Normal ▾]    │
│ Startup Policy: [Load NVS ▾]  │
│                                │
│   [Save]  [Load]  [Factory]   │
│                                │
│  Modbus (next boot)           │
│ Slave Address : [1]           │
│ Baud Rate     : [115200 ▾]    │
│      [ Save Modbus ]          │
│                                │
│           [ Reset ]           │
│           [ Close ]           │
└────────────────────────────────┘
```

| Control | Effect |
|---|---|
| Working Mode | Same selector as the board menu bar (shared state) |
| Startup Policy | `Load NVS Config` or `Factory Default` — what the board loads on the *next* boot |
| `[Save]` / `[Load]` / `[Factory]` | System-wide, all channels — same as the board menu-bar Save plus load/factory-reset |
| Slave Address / Baud Rate + `[ Save Modbus ]` | Writes new Modbus comm settings. **Takes effect only after a reset** — you'll need to reconnect at the new address/baud afterward. |
| `[ Reset ]` | Sends a software reset and disconnects. Reconnect manually once the board has rebooted. |
| `[ Close ]` / Esc | Closes the popup without further action |

---

## 8. Status bar

| Element | Meaning |
|---|---|
| Message | Result of the last write, colored green (`OK: ...`) or red (`Error: ...`) |
| `FW:` / `Proto:` | Firmware version and protocol version, shown on board dashboards only when connected |
| Connection string | `<port> @<baud> #<slave>`, `Connecting...`, or `offline`, shown on board dashboards |
| `[Setting]` | Opens the System Config popup (§7), shown on board dashboards |

Group dashboards have the same message area but no firmware/protocol or board
connection string because a group may contain channels from multiple boards.

---

## 9. Topology mechanism

Topology is the hardware-facing configuration. It answers: which serial buses
exist, which boards sit on each bus, and what nickname identifies each board.

| Concept | Meaning |
|---|---|
| Bus | One serial port plus baud rate. Boards on the same bus share one serialized Modbus transaction path. |
| Board | One Modbus slave on a bus, identified in the UI by a user-defined nickname. |
| Channel | One firmware-reported channel index on a board, displayed as `CHn` in board view. |

The normal configuration sequence is topology first, groups second:

1. Use `Single Board` for quick one-board work, or `Multi-Board Topology` for a saved topology.
2. In the topology wizard, define buses and boards before defining groups.
3. Connect the boards. Channel tabs appear only after a board connects and reports its channel count.
4. Use the group wizard to build operator-facing channel groups from those board channels.

Single-board mode and multi-board mode use the same topology runtime. The
difference is only persistence and UI: single-board mode writes a small
`last_single.toml`; multi-board mode uses the topology file.

---

## 10. Group mechanism

Groups are operator-facing channel views over the topology. A group does not
create another hardware channel and does not poll independently; each group row
points back to one underlying board channel.

The group dashboard monitor table uses the group channel name as its first
column. The final `Go` column is labeled with the hardware identity
`board_nickname/CHn`; clicking it jumps to the matching board channel tab.
Editing the group channel name in the group dashboard syncs back to the board
channel tab. Editing it from the board channel tab syncs back to the group
dashboard.

The group wizard's Add Channel dialog is organized as a two-level picker:
board, then channel. Channels already assigned to any group are hidden until
they are removed from their current group.

---

## 11. Naming rules

| Name | Scope | Rule |
|---|---|---|
| Board nickname | Whole topology | Required for practical use; must be unique. |
| Board channel ID | Whole topology | `board_nickname/CHn`, where `CHn` is the firmware channel index. |
| Group name | Whole topology | Required; must be unique. |
| Group channel name | One group | Must be unique inside that group. The same text may be reused in another group. |
| Group channel ID | Whole topology | `group_name/name`; globally unique because group names are unique and names are unique within each group. |

Additional rules:

- A group channel always references an existing board channel.
- A board channel may be ungrouped.
- A board channel may belong to at most one group.
- The default group channel name is `CHn` when the user has not customized it.
- Duplicate group-channel names in the same group are rejected with an error
  message and no rename/add action is applied.

---

## 12. Topology config file format

Default paths:

| File | Purpose |
|---|---|
| `~/.psb_demo_app/topology.toml` | Default multi-board topology. |
| `~/.psb_demo_app/last_single.toml` | Last single-board quick-connect settings. |
| `~/.psb_demo_app/preferences.toml` | App runtime preferences such as timeout and poll interval. |

Example topology:

```toml
[[bus]]
name = "rack-a"
port = "/dev/ttyUSB0"
baud_rate = 115200

  [[bus.board]]
  nickname = "hvb-left"
  slave_id = 1

  [[bus.board]]
  nickname = "hvb-right"
  slave_id = 2

[[bus]]
name = "rack-b"
port = "/dev/ttyUSB1"
baud_rate = 115200

  [[bus.board]]
  nickname = "lvb-aux"
  slave_id = 1

[[group]]
name = "detector"

  [[group.channel]]
  board = "hvb-left"
  channel = 0
  alias = "bias"

  [[group.channel]]
  board = "hvb-right"
  channel = 1
  alias = "guard"
```

Format rules:

| Field | Rule |
|---|---|
| `[[bus]]` | Required top-level array. A topology file without any bus entries is ignored. |
| `bus.name` | Optional; defaults to `bus1`, `bus2`, ... by file order when missing. |
| `bus.port` | Serial device path. Missing/empty can be saved, but the bus cannot connect usefully until a real port is selected. |
| `bus.baud_rate` | Optional; defaults to `115200`. |
| `[[bus.board]]` | Nested under a bus. Each entry defines one Modbus slave. |
| `board.nickname` | Board UI name and stable reference used by groups. Keep it unique across the whole topology. |
| `board.slave_id` | Optional; defaults to `1`. |
| `board.channel_aliases` | Legacy/display alias array indexed by board channel. Group channel names are stored under `[[group.channel]]` instead. |
| `[[group]]` | Optional top-level array. Topologies without groups are valid. |
| `group.name` | Group UI name. Keep it unique across the whole topology. |
| `[[group.channel]]` | Nested under a group. Each entry references one board channel. |
| `group.channel.board` | Must match a board nickname. |
| `group.channel.channel` | Zero-based board channel index from firmware. |
| `group.channel.alias` | Operator-facing name in that group. Missing or empty aliases load as `CHn` and are written explicitly on the next save. |

Malformed TOML, missing files, or files without a `[[bus]]` array fail to load.
Use the topology and group wizards when possible; they apply the uniqueness and
membership checks before writing the file.

---

## 13. Typical workflow

Bring channel 0 up to 500 V, watch it, then take it back down:

1. `[ Connect ]` → fill in port → `[ Connect ]` in the modal.
2. Select the `Monitor` tab (or `CH0` for the full control/protection view).
3. Type `500.0` into `Vset` for CH0, press **Enter**.
4. Click the `Status` button (`[ OFF ]` → enables since target is now
   non-zero). It shows `[ RAMP ]` while ramping, then `[ ON ]` once
   `Vop` reaches `Vset`.
5. Watch `V (V)` / `I (nA)` update on the configured poll interval.
6. If `Fault` shows a code: check the channel tab's Protection Policy panel
   for the mode, then use the matching Clear button (§5) once the
   underlying condition is resolved.
7. Click `Status` again to gracefully disable, or use `CH0`'s `[Disable]` /
   `[Kill]` for a graceful vs. immediate stop.

---

## 14. Relationship to other tools

| Tool | Binary | Purpose |
|---|---|---|
| Demo TUI (this guide) | `psb_demo_tui` | Interactive monitoring/control, engineering + demo use |
| Demo CLI | `psb_demo_cli` | Scriptable one-shot commands; can use `--topology`, `--board`, or quick single-board `--port`/`--baud`/`--id`; `--save` writes the connection used to the topology path |
| Factory TUI | `psb_factory_tui` | Production calibration — see [`calibration-guide.md`](calibration-guide.md) |

Building or maintaining a client (this TUI, the Qt/QML GUI, or your own)?
See [`client-architecture-and-pitfalls.md`](client-architecture-and-pitfalls.md)
for the threading/polling architecture and every non-obvious bug found
hardening it against real hardware.

---

## 15. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Connect fails immediately | Wrong port, board not powered, or another process (e.g. `psb_demo_cli`, `psb_factory_tui`) already holds the serial port. Only one tool can hold the port at a time. |
| A field reverts after you type a new value | You navigated away or the field lost focus before pressing **Enter** — nothing was sent. Re-enter the value and press Enter. |
| Cycler shows a new value but nothing happened on the board | You cycled the Mode/Action/Policy selector but never pressed Enter — cyclers other than the board menu-bar mode selector don't auto-commit (see §6). |
| Channel tabs disappear | The connection dropped. Reconnect; channel tabs reappear after a successful channel scan. |
| Single-board port/baud/slave do not match a previous multi-board setup | Single-board quick-connect and multi-board topology are deliberately stored in separate files: `last_single.toml` and `topology.toml`. |
| Changed slave address or baud but board stopped responding | Those changes only take effect after a reset (§7) — reconnect using the *new* settings once it reboots. |

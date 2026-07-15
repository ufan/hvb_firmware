# PSB Demo TUI User Guide

A quick-start reference for `psb_demo_tui`, the interactive terminal dashboard for
live monitoring and control of a Jianwei voltage-control board over Modbus
RTU. This is an **engineering / bring-up / demo tool**, not the factory
calibration tool — it has no calibration-mode UI. For calibrating a board,
see [`calibration-guide.md`](calibration-guide.md).

*(中文版本: [demo-tui-guide.zh.md](demo-tui-guide.zh.md))*

---

## 1. Build and launch

```bash
cmake -S tools -B tools/build
cmake --build tools/build --target psb_demo_tui -j
tools/bin/psb_demo_tui [-p PORT] [-b BAUD] [-i SLAVE_ID] [-t TIMEOUT_MS] [-s POLL_S]
```

| Flag | Meaning | Default |
|---|---|---|
| `-p` | Serial port, e.g. `/dev/ttyUSB0`. If given, connects automatically at launch. | connect manually via the UI |
| `-b` | Baud rate | 115200 |
| `-i` | Modbus slave id | 1 |
| `-t` | Connection timeout, ms | 3000 |
| `-s` | Background poll interval, seconds | 1 |

If `~/.psb_demo_app.toml` exists (written by `psb_demo_cli --save`, the
companion CLI tool), its `[connection]` section supplies the defaults for any
flag you don't pass. **The TUI itself never writes this file** — connection
settings you type into the Connect modal apply only to the current session
and are not remembered on the next launch unless you also pass `-p`/`-b`/`-i`
or maintain the TOML file some other way.

---

## 2. Screen layout

```
┌ PSB │ 2 Channels │ [Normal ▾] │ [ Save ]      ● 1234s | T:24.1C H:41.2%      [ Connect ] [ Quit ] ┐
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│  Monitor    CH0    CH1                                                                             │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                      │
│                         (active tab: Monitor table, or a per-channel panel)                         │
│                                                                                                      │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│  OK: Target V                                    FW:0x0102  Proto:1.0    /dev/ttyUSB0 @115200 #1  [Setting] │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

| Region | Contents |
|---|---|
| Menu bar | Title, channel count, Working Mode selector, Save button, connection indicator + uptime/temp/humidity (when connected), Connect/Disconnect/Abort toggle, Quit |
| Tab bar | `Monitor` + one `CHn` tab per channel the board reports |
| Tab content | The Monitor table, or the selected channel's detail panels |
| Status bar | Last operation's result message, firmware/protocol version, connection string, `[Setting]` button |

Navigation: **Tab** moves focus between controls, arrow keys move within a
row and cycle selector values, **Enter** commits a text field or a cycler,
**Space**/**←**/**→** also cycle a selector. Mouse click works everywhere.
**Esc** closes whichever modal is open.

---

## 3. Connecting

Click **[ Connect ]** (top-right of the menu bar) to open the Connection
Settings modal:

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
- Launching with `-p` skips the modal and connects immediately.
- On disconnect, all channel tabs collapse back to just `Monitor` — they
  reappear after the next successful connect and channel scan.

---

## 4. Menu bar

| Element | Behavior |
|---|---|
| `N Channels` | Channel count reported by the board (`--` until connected) |
| Working Mode `[Normal ▾]` / `[Automatic ▾]` | Click, Space, or ←/→ to cycle — **commits immediately on every change**, unlike the per-channel cyclers described below. See [`operating-mode-guide.md`](operating-mode-guide.md) for what each mode actually changes. |
| `[ Save ]` | Same as `sys save` in the factory tool: persists op-config + system config for **all** channels to NVS. Dim/inactive while disconnected. |
| Center group | Connection dot, uptime, board temperature, humidity — only shown when connected |
| `[ Connect / Disconnect / Abort ]` | See §3 |
| `[ Quit ]` | Exits the application |

---

## 5. Monitor tab

One row per channel, all key control fields in one table:

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
 Live │ Vop: +500.0 V   V: +499.8 V   I: +12.3 nA   Status: ON   Retries: 0

┌ Control ─────────────────┐  ┌ Protection Policy ─────────┐
│ [Enable][Disable][Kill]  │  │ Limit  : 1000        nA    │
│ Vset    : +500.0      V  │  │ Mode   : [Apply-Action ▾]  │
│ Ramp up : 5.0        V/s │  │ Action : [Dis-Graceful ▾]  │
│ Ramp dn : 5.0        V/s │  │ [ClrActive]  [ClrHist]     │
└───────────────────────────┘  └─────────────────────────────┘
┌ Setting ──────────────────┐  ┌ Recovery Policy ────────────┐
│ [Save] [Load] [Factory]  │  │ Policy : [AutoRetry ▾]      │
└───────────────────────────┘  │ Max    : 3    Win: 60    s │
                                │ Delay  : 10               s │
                                │ Derate : 0               LSB│
                                │ Band   : 10                %│
                                └─────────────────────────────┘
```

### Live panel
Read-only: operational target, measured V/I, status badge (`OFF` / `ON` /
`RAMP` / `ON RAMP` / `FAULT` / `COOL` / `STALE`), and the current auto-retry
count in the sliding window.

### Control panel
- `[Enable]` / `[Disable]` / `[Kill]` — send `Enable` / `DisableGraceful` /
  `DisableImmediate` immediately on click. Hidden if the channel has no
  output capability.
- `Vset`, `Ramp up`, `Ramp dn` — text fields, **Enter** to commit each.

### Protection Policy panel
Shown only if the channel has both voltage and current measurement
capability. `Limit` is a text field (Enter to commit). `Mode` and `Action`
are cyclers: click, Space, or ←/→ steps through the options, but **unlike
the menu-bar mode selector, these do not auto-commit** — press **Enter**
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
menu-bar `[ Save ]`, which covers every channel plus system config at once.

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
| Working Mode | Same selector as the menu bar (shared state) |
| Startup Policy | `Load NVS Config` or `Factory Default` — what the board loads on the *next* boot |
| `[Save]` / `[Load]` / `[Factory]` | System-wide, all channels — same as the menu-bar Save plus load/factory-reset |
| Slave Address / Baud Rate + `[ Save Modbus ]` | Writes new Modbus comm settings. **Takes effect only after a reset** — you'll need to reconnect at the new address/baud afterward. |
| `[ Reset ]` | Sends a software reset and disconnects. Reconnect manually once the board has rebooted. |
| `[ Close ]` / Esc | Closes the popup without further action |

---

## 8. Status bar

| Element | Meaning |
|---|---|
| Message | Result of the last write, colored green (`OK: ...`) or red (`Error: ...`) |
| `FW:` / `Proto:` | Firmware version and protocol version (connected only) |
| Connection string | `<port> @<baud> #<slave>`, `Connecting...`, or `offline` |
| `[Setting]` | Opens the System Config popup (§7) |

---

## 9. Typical workflow

Bring channel 0 up to 500 V, watch it, then take it back down:

1. `[ Connect ]` → fill in port → `[ Connect ]` in the modal.
2. Select the `Monitor` tab (or `CH0` for the full control/protection view).
3. Type `500.0` into `Vset` for CH0, press **Enter**.
4. Click the `Status` button (`[ OFF ]` → enables since target is now
   non-zero). It shows `[ RAMP ]` while ramping, then `[ ON ]` once
   `Vop` reaches `Vset`.
5. Watch `V (V)` / `I (nA)` update on the poll interval (`-s`, default 1 s).
6. If `Fault` shows a code: check the channel tab's Protection Policy panel
   for the mode, then use the matching Clear button (§5) once the
   underlying condition is resolved.
7. Click `Status` again to gracefully disable, or use `CH0`'s `[Disable]` /
   `[Kill]` for a graceful vs. immediate stop.

---

## 10. Relationship to other tools

| Tool | Binary | Purpose |
|---|---|---|
| Demo TUI (this guide) | `psb_demo_tui` | Interactive monitoring/control, engineering + demo use |
| Demo CLI | `psb_demo_cli` | Scriptable one-shot commands; writes `~/.psb_demo_app.toml` with `--save` |
| Factory TUI | `psb_factory_tui` | Production calibration — see [`calibration-guide.md`](calibration-guide.md) |

---

## 11. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Connect fails immediately | Wrong port, board not powered, or another process (e.g. `psb_demo_cli`, `psb_factory_tui`) already holds the serial port. Only one tool can hold the port at a time. |
| A field reverts after you type a new value | You navigated away or the field lost focus before pressing **Enter** — nothing was sent. Re-enter the value and press Enter. |
| Cycler shows a new value but nothing happened on the board | You cycled the Mode/Action/Policy selector but never pressed Enter — cyclers other than the menu-bar mode selector don't auto-commit (see §6). |
| Channel tabs disappear | The connection dropped — `reconcileDisconnectedTabs` collapses back to just `Monitor`. Reconnect. |
| Port/baud/slave typed at launch don't come back next time | The TUI never writes `~/.psb_demo_app.toml` — pass `-p`/`-b`/`-i` again, or maintain that file via `psb_demo_cli --save`. |
| Changed slave address or baud but board stopped responding | Those changes only take effect after a reset (§7) — reconnect using the *new* settings once it reboots. |

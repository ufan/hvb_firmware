# Factory Calibration Guide — psb_factory_tui

Quick-start reference for calibrating a Jianwei voltage-control board using
the **factory TUI** (`psb_factory_tui`), the host-side tool intended for
production/technician use. The Qt factory GUI exists in this repo but is not
yet release-ready — use the TUI described here.

*(中文版本: [calibration-guide.zh.md](calibration-guide.zh.md))*

---

## 1. What calibration does

Each channel converts raw hardware codes to physical units through a linear
coefficient pair, `y = x × k/D + b`, on three independent axes:

| Axis | Coefficient fields | D | Meaning |
|---|---|---|---|
| Output | `out_cal_k` / `out_cal_b` | 10000 | `raw_dac = target × k/10000 + b` |
| Voltage measurement | `v_cal_k` / `v_cal_b` | 1000000 | `measured_v = raw_adc_v × k/1000000 + b` |
| Current measurement | `i_cal_k` / `i_cal_b` | 1000000 | `measured_i = raw_adc_i × k/1000000 + b` |

The measurement axes use a much larger divisor because they scale down a
small, attenuated raw ADC reading (gain ≈ 0.001–0.01); unity gain isn't
representable there (max is 65535/1000000 ≈ 0.0655). The output axis is
near-unity, so it uses the smaller divisor. See
`docs/guide/parameter-reference.md` for the full derivation.

**Factory (pre-calibration) defaults** — the board ships with these; your job
is to replace them with real per-unit numbers:

| Field | Default | Note |
|---|---|---|
| `out_cal_k` / `out_cal_b` | 32768 / 0 | Nominal 1:1-ish DAC gain, uncalibrated |
| `v_cal_k` / `v_cal_b` | 1 / 0 (jw_hvb board: 2387 / 0) | Deliberately near-zero until calibrated |
| `i_cal_k` / `i_cal_b` | 1 / 0 (jw_hvb board: 14901 / 0) | Deliberately near-zero until calibrated |

---

## 2. What you need

- Board powered and connected via USB-serial Modbus RTU adapter (e.g.
  `/dev/ttyUSB0`, 115200 8N1, slave id 1 — all defaults).
- `psb_factory_tui` built and available (see §3).
- A calibrated external reference: a DMM for the voltage axis, a reference
  current source or precision load for the current axis. The board's own
  ADC readings are what you're calibrating — don't use them as ground truth.
- Calibrate one channel at a time; the firmware only allows one channel's
  calibration output active at once.

---

## 3. Build and launch

```bash
cmake -S tools -B tools/build       # BUILD_FACTORY is ON by default
cmake --build tools/build --target psb_factory_tui -j
tools/bin/psb_factory_tui -p /dev/ttyUSB0
```

```
Usage: psb_factory_tui -p <port> [-b <baud>] [-i <slaveId>]
  -p, --port   Serial port to connect to (required, e.g. /dev/ttyUSB0)
  -b, --baud   Baud rate (default 115200)
  -i, --id     Modbus slave id (default 1)
```

The connection is established once at startup. There's no `connect` command
inside the tool — if the port or wiring is wrong, fix it and relaunch. To
leave the session, type `exit` (or `quit`); there's no `disconnect` either,
since a dropped session can't reconnect from inside the REPL.

---

## 4. Command reference

Commands are used at the `factory>` prompt after launch.

### Root-level (works in any operating mode)

| Command | Effect |
|---|---|
| `info` | Protocol/firmware/variant/mode/uptime summary |
| `ch <n>` | Select the active channel (shared by all per-channel commands below) |
| `target <V>` | Set configured target voltage on the active channel |
| `output on\|off\|immed\|zero` | Enable / graceful-disable / immediate-disable / force-zero |
| `fault clear\|clear-history` | Clear active fault block / clear fault history |
| `measure` | Read Vmeas, Imeas, target, status on the active channel |
| `sys status` | Mode + per-channel one-line overview |
| `sys mode normal\|auto` | Set operating mode |
| `sys save` / `sys load` | Save / load all configuration (op-config + cal) to/from NVS |
| `sys factory-reset` | Reset all configuration to firmware defaults |
| `sys reset` | Software reset the board, then disconnect |

### Calibration submenu (`cal ...`, requires unlock)

| Command | Effect |
|---|---|
| `cal unlock` | Two-step unlock + enter Calibration Mode (atomic) |
| `cal ch <n>` | Select active channel (same selector as root `ch`) |
| `cal enable` / `cal disable` | Enable calibration output / disable + zero DAC |
| `cal dac <code>` | Write raw DAC code (0–65535, or lower if fixture-limited) |
| `cal sample` | Trigger a fresh sample, print `dac` / `adc_v` / `adc_i` |
| `cal read` | Print the last snapshot without triggering a new sample |
| `cal coeff out\|meas-v\|meas-i <k> <b>` | Write a coefficient pair |
| `cal coeff show` | Print current coefficients for the active channel |
| `cal commit` | Persist the active channel's coefficients to NVS |
| `cal status` | Mode + per-channel output/dac/adc overview |
| `cal safe` | Disable calibration output and zero DAC on **all** channels |
| `cal watch adc\|measure\|status\|all [interval]` | Live-updating monitor; any key stops it |
| `cal exit-cal` | Exit Calibration Mode, restore the previous operating mode |

---

## 5. Calibration session walkthrough

![Calibration session](calibration-session-en.png)

1. **Unlock**: `cal unlock` — two-step unlock and entry happen as one atomic
   command; the tool prints a short in-session command summary on success.
2. **Per channel**, repeat:
   - `cal ch <n>` to select it, then `cal enable` to energize the output.
   - For at least two DAC codes (more points give a better fit): `cal dac
     <code>`, then `cal sample` to trigger and print the raw readings. Record
     the external reference meter's reading alongside each sample.
   - Compute `k` and `b` per axis offline (spreadsheet, script — see §6).
   - Write them back: `cal coeff out <k> <b>`, `cal coeff meas-v <k> <b>`,
     `cal coeff meas-i <k> <b>`. Confirm with `cal coeff show`.
   - `cal disable` (required before commit — commit is rejected while output
     is enabled or the DAC code is non-zero), then `cal commit`.
3. **Exit**: `cal exit-cal` returns to the operating mode that was active
   before you entered calibration.

---

## 6. Worked example: computing k and b

All three axes use the same two-point linear fit. Given two `(x, y)` pairs,
where `x` is what the tool reports and `y` is your reference measurement,
converted to the register's raw units:

```
k = D × (y2 − y1) / (x2 − x1)
b = y1 − x1 × k / D
```

Round `k` and `b` to integers before writing them back (`k` is `uint16_t`,
`b` is `int16_t`).

**Output axis worked example** (`x` = commanded DAC code, `y` = DMM-measured
output voltage, converted to raw target units at 0.1 V/LSB):

| Point | DAC code (x) | DMM reading | y (raw, ×0.1 V) |
|---|---|---|---|
| 1 | 32768 | 995.0 V | 9950 |
| 2 | 49152 | 1490.0 V | 14900 |

```
k = 10000 × (49152 − 32768) / (14900 − 9950) = 10000 × 16384 / 4950 ≈ 33108
b = 32768 − 9950 × 33108 / 10000 ≈ −174
```

→ `cal coeff out 33108 -174`

**Voltage-measurement axis worked example** (`x` = `adc_v` from `cal sample`,
`y` = the same DMM readings as above, in raw units):

| Point | adc_v (x) | y (raw, ×0.1 V) |
|---|---|---|
| 1 | 4,140,000 | 9950 |
| 2 | 6,205,000 | 14900 |

```
k = 1000000 × (14900 − 9950) / (6205000 − 4140000) ≈ 2397
b = 9950 − 4140000 × 2397 / 1000000 ≈ 26
```

→ `cal coeff meas-v 2397 26`

**Current-measurement axis worked example** (`x` = `adc_i` from `cal sample`,
`y` = reference current source reading, in raw units at 0.1 nA/LSB):

| Point | adc_i (x) | Reference | y (raw, ×0.1 nA) |
|---|---|---|---|
| 1 | 335,000 | 500.0 nA | 5000 |
| 2 | 1,342,000 | 2000.0 nA | 20000 |

```
k = 1000000 × (20000 − 5000) / (1342000 − 335000) ≈ 14896
b = 5000 − 335000 × 14896 / 1000000 ≈ 10
```

→ `cal coeff meas-i 14896 10`

**The numbers above are illustrative** — substitute your fixture's actual
`cal sample` output and reference-meter readings. Note how close the fitted
`k` values land to the jw_hvb board's factory defaults (2387, 14901) —
that's expected; a healthy board needs only a small correction, not a wildly
different gain.

---

## 7. Post-calibration verification

After committing and exiting calibration mode, verify the result using the
root-level commands (no unlock needed):

![Post-calibration verification](calibration-verify-en.png)

```
factory> ch 0
factory> target 500.0
factory> output on
factory> measure
CH0:
  Vmeas:  499.8 V  (raw=4998)
  Imeas:  0.012 uA  (raw=120)
  Target: 500.0 V
  Status: 0x0003 [ON]
factory> output off
```

Compare `Vmeas`/`Imeas` against your reference meter at a few points across
the operating range — not just the two points used for the fit — to confirm
the correction holds across the full span, not only at the calibration
points.

---

## 8. Persistence

| Command | Scope | Effect |
|---|---|---|
| `cal commit` | Cal config, active channel only | Save that channel's coefficients to NVS |
| `sys save` | All channels, op-config + cal | Save everything currently in RAM to NVS |
| `sys load` | All channels, op-config + cal | Reload from NVS (no-op for anything never saved) |
| `sys factory-reset` | All channels + system | Erase NVS, revert everything to firmware defaults |

`cal commit` only persists calibration coefficients — it does not touch
operational parameters (target voltage, ramp rate, current limit, etc.). If
you also changed those during this session, run `sys save` before
disconnecting, or they're lost on the next reboot.

---

## 9. Safety rules (enforced by firmware, not by the tool)

- Calibration Mode requires the two-step unlock (`cal unlock` does both
  steps for you).
- Only one channel may have calibration output enabled at a time — `cal
  enable` on a second channel fails while another is still active.
- A non-zero DAC code requires calibration output already enabled.
- `cal commit` is rejected while output is enabled or the DAC code is
  non-zero — always `cal disable` first.
- Hardware/interlock faults block any non-zero calibration output,
  regardless of mode.
- An inactivity watchdog (default 300 s, `CONFIG_VC_CAL_WATCHDOG_TIMEOUT_S`)
  auto-exits Calibration Mode if no `cal` command is issued — any `cal`
  command, including `cal watch`, resets the timer.
- Calibration Mode is volatile and never persists across a reboot; you
  always start Normal or Automatic mode on power-up.

---

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Error: not connected` | The tool failed its startup connection. Check `-p`/`-b`/`-i` and relaunch — there's no in-session reconnect. |
| `Error: no active channel (use 'cal ch <n>')` | Run `ch <n>` (or `cal ch <n>`) before any per-channel command. |
| `cal enable` fails | Another channel still has calibration output active — `cal disable` it, or run `cal safe` to clear all channels. |
| `cal commit` fails | Output still enabled or DAC code non-zero — run `cal disable` first. |
| Calibration Mode exited on its own | Inactivity watchdog fired (default 300 s of no `cal` commands) — run `cal unlock` again. |
| `cal dac <code>` rejected above some value | The build's `CONFIG_VC_CAL_MAX_RAW_DAC` ceiling (project/fixture safety limit) — check with your firmware build config, not adjustable at runtime. |

---

## Quick reference

```
tools/bin/psb_factory_tui -p /dev/ttyUSB0

cal unlock
  cal ch <n>
  cal enable
    cal dac <code>
    cal sample                       # repeat for >= 2 points
  cal coeff out     <k> <b>
  cal coeff meas-v   <k> <b>
  cal coeff meas-i   <k> <b>
  cal coeff show
  cal disable
  cal commit
cal exit-cal

ch <n>
target <V>
output on
measure
output off
```

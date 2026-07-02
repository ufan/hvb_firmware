# Shell Command Reference — Jianwei Voltage-Control Board

## Overview

The firmware exposes three shell command trees via the Zephyr shell (USART3 console, 115200 8N1):

| Tree | Purpose | Config | Source |
|------|---------|--------|--------|
| `vc` | Voltage control domain | `CONFIG_VC_SHELL` | `lib/voltage_control/vc_shell.c` |
| `mb` | Modbus adapter config | `CONFIG_MODBUS_ADAPTER_SHELL` | `lib/modbus_adapter/modbus_adapter_shell.c` |
| `ss` | System status | `CONFIG_SYS_STATUS_SHELL` | `lib/sys_status/sys_status_shell.c` |

---

## `vc` — Voltage Control

### `vc status`

Show a one-liner for the system and each channel.

```
uart:~$ vc status
SYS: mode=NORMAL channels=2 active=0x0003 fault=none
CH0: V=0 I=0 target=5000 fault=none
CH1: V=0 I=0 target=0 fault=none
```

### `vc mode <normal|auto>`

Set the operating mode. Calibration mode is entered exclusively via `vc cal unlock` (not via `vc mode`).

```
vc mode normal
vc mode auto
```

### `vc param <save|load|reset>`

Apply param action to system and all channels (writes to NVS flash via Zephyr Settings).

```
vc param save       # persist to NVS
vc param load       # reload from NVS
vc param reset      # factory reset
```

### `vc watch [<ch>] [<interval_ms>]`

Live monitoring loop. Press any key to stop.

```
vc watch            # monitor all channels, default interval
vc watch 0          # monitor channel 0
vc watch 0 200      # channel 0, 200ms interval
```

### `vc sys status`

Detailed system snapshot.

```
uart:~$ vc sys status
Protocol:    3.0
Variant:     1
Caps:        0x0007
Channels:    2
Active:      0x0003
Mode:        NORMAL
Status:      0x0000
Fault:       none
```

### `vc sys config`

Show system configuration.

```
uart:~$ vc sys config
System Configuration
  mode:           NORMAL
  startup_policy: 0
```

### `vc sys param <save|load|reset>`

System-only param action.

```
vc sys param save
vc sys param load
vc sys param reset
```

### `vc sys set <field> <value>`

Set a system config field. Available fields: `mode`, `startup_policy`.

```
vc sys set mode           0       # set to NORMAL mode
vc sys set startup_policy 0       # 0=load from NVS, 1=factory reset
```

### `vc ch <n> status`

Detailed channel snapshot.

```
uart:~$ vc ch 0 status
Channel 0 Status
  caps:         [O,R,V,I]
  enabled:      yes
  target:       5000
  voltage:      4998
  current:      120
  status_bits:  0x0003
  fault:        none
  fault_hist:   none
```

### `vc ch <n> config`

Show channel configuration.

```
uart:~$ vc ch 0 config
Channel 0 Configuration
  target:       5000
  ramp_up:      step=50000 interval=10
  ramp_down:    step=50000 interval=10
  recovery:     MANUAL
  retry:        delay=0 max=0 window=0
  safe_band:    10%
  protection:   mode=OFF action=IMMEDIATE limit=32767
  derate_step:  0
```

### `vc ch <n> enable`

Enable channel output (rejected while active fault blocks).

```
vc ch 0 enable
```

### `vc ch <n> disable [graceful]`

Disable channel output. Default: immediate. Add `graceful` to use graceful disable.

```
vc ch 0 disable
vc ch 0 disable graceful
```

### `vc ch <n> target <voltage>`

Set the configured target voltage (mV).

```
vc ch 0 target 5000       # set target to 5V
vc ch 0 target 0          # set target to 0V
```

### `vc ch <n> fault <clear|clear-history>`

Clear active fault block or fault history.

```
vc ch 0 fault clear
vc ch 0 fault clear-history
```

### `vc ch <n> param <save|load|reset>`

Channel-only param action.

```
vc ch 0 param save
vc ch 0 param load
vc ch 0 param reset
```

### `vc ch <n> set <field> <value>`

Set a channel config field. Available fields:

| Shell Field | Description | Unit |
|-------------|-------------|------|
| `target` | Configured target voltage | mV |
| `ramp_up_step` | Ramp-up step size | mV/interval |
| `ramp_up_int` | Ramp-up interval | ×100 ms |
| `ramp_dn_step` | Ramp-down step size | mV/interval |
| `ramp_dn_int` | Ramp-down interval | ×100 ms |
| `recovery` | Recovery policy mode | enum (0–3) |
| `retry_delay` | Delay between retry attempts | seconds |
| `retry_max` | Max retry attempts | count |
| `retry_window` | Retry count reset window | seconds |
| `safe_band` | Current safe band for clear | percent |
| `prot_mode` | Current protection mode | enum (0–2) |
| `prot_action` | Protection output action | enum |
| `i_limit` | Current limit threshold | raw ADC counts |
| `derate_step` | Auto-derate step size | mV |

```
vc ch 0 set target       5000
vc ch 0 set ramp_up_step 100
vc ch 0 set ramp_up_int  5
vc ch 0 set ramp_dn_step 100
vc ch 0 set ramp_dn_int  5
vc ch 0 set recovery     0      # 0=Manual, 1=Retry, 2=Derate, 3=Never
vc ch 0 set retry_delay  5      # seconds
vc ch 0 set retry_max    3
vc ch 0 set retry_window 60     # seconds
vc ch 0 set safe_band    10     # percent
vc ch 0 set prot_mode    2      # 0=Off, 1=Flag, 2=Apply
vc ch 0 set prot_action  3      # 0=None, 2=Graceful, 3=Immediate, 4=ForceZero
vc ch 0 set i_limit      30000  # raw ADC counts
vc ch 0 set derate_step  500    # mV per derate
```

---

## `vc cal` — Calibration Commands

### `vc cal unlock`

Two-step calibration unlock (0xCA1B → 0xA11B) followed immediately by mode transition into calibration mode. Prints a session guide on success. The unlock flag self-clears once calibration mode is active.

```
vc cal unlock
```

Example output:
```
Calibration session started.
  vc cal status              -- session overview
  vc cal max_dac <ch> <lim>  -- set safety DAC cap first
  vc cal output <ch> on      -- enable output
  vc cal dac <ch> <code>     -- set raw DAC code
  vc cal sample <ch>         -- read raw ADC (blocking)
  vc cal set <ch> <fld> <v>  -- adjust cal coefficients
  vc cal commit <ch>         -- save to NVS
  vc cal exit                -- end session
```

### `vc cal exit`

Exit calibration mode, returning to the previous operating mode.

```
vc cal exit
```

### `vc cal output <ch> <on|off>`

Enable/disable calibration output. Only one channel may have cal output active at a time.

```
vc cal output 0 on
vc cal output 0 off
```

### `vc cal dac <ch> <code>`

Write raw 16-bit DAC code (0–65535). Requires cal output enabled and code ≤ `max_dac` ceiling.

```
vc cal dac 0 32767
vc cal dac 0 0
```

### `vc cal sample <ch>`

Trigger an ADC sample capture (blocking). Waits for the snapshot to complete, then prints `dac=`, `raw_v=`, and `raw_i=` values directly in the shell. Raw ADC values are also available via Modbus input registers `CH_RAW_ADC_VOLTAGE` (offset 12–13) and `CH_RAW_ADC_CURRENT` (offset 14–15).

```
vc cal sample 0
```

Example output:
```
  dac=1000
  raw_v=1023
  raw_i=0
hint: vc cal set 0 <field> <val>  or  vc cal commit 0
```

### `vc cal max_dac <ch> <limit>`

Set the calibration DAC ceiling. Default is 65535. Must be ≥ current DAC code.

```
vc cal max_dac 0 50000
```

### `vc cal config <ch>`

Show calibration coefficients.

```
uart:~$ vc cal config 0
Channel 0 Calibration Config
  out_cal:  k=10000 b=0
  v_cal:    k=1 b=0
  i_cal:    k=1 b=0
```

`v_cal`/`i_cal` default to `k=1` — the smallest representable measurement
gain, an intentionally near-zero placeholder until a real per-unit
calibration is written (see below; unity gain is not representable on these
two axes).

### `vc cal set <ch> <field> <value>`

Set a calibration coefficient field. Only writable in calibration mode. Available fields:

| Shell Field | Description | Formula |
|-------------|-------------|---------|
| `out_cal_k` | DAC gain (÷10000) | DAC = target × k/10000 + b |
| `out_cal_b` | DAC offset | |
| `v_cal_k` | Voltage measurement gain (÷1000000) | Measured = raw_adc × k/1000000 + b |
| `v_cal_b` | Voltage measurement offset | |
| `i_cal_k` | Current measurement gain (÷1000000) | Measured = raw_adc × k/1000000 + b |
| `i_cal_b` | Current measurement offset | |
| `max_dac` | Safety DAC ceiling (uint16) | Same as `vc cal max_dac <ch> <limit>` |

```
vc cal set 0 out_cal_k 10000
vc cal set 0 out_cal_b 0
vc cal set 0 v_cal_k   2387
vc cal set 0 v_cal_b   0
vc cal set 0 i_cal_k   14901
vc cal set 0 i_cal_b   0
```

### `vc cal status`

Show the current calibration session state for all channels: active mode, per-channel output enabled flag, raw DAC readback, and raw ADC values.

```
uart:~$ vc cal status
Cal status  mode=CAL
  CH0: out=ON dac=1000 raw_v=1023 raw_i=0
  CH1: out=OFF dac=0 raw_v=0 raw_i=0
```

### `vc cal watch [<ch>] [<interval_ms>]`

Continuous raw DAC/ADC monitor. Default interval is 1000 ms. Prints one line per channel per tick. Press any key to stop.

```
vc cal watch            # watch all channels, 1s interval
vc cal watch 0          # watch channel 0, 1s interval
vc cal watch 0 500      # watch channel 0, 500ms interval
```

### `vc cal commit <ch>`

Persist calibration coefficients to NVS. Rejected if cal output is enabled or DAC code is non-zero.

```
vc cal commit 0
```

---

## `mb` — Modbus Adapter

### `mb status`

Show current Modbus adapter configuration.

```
uart:~$ mb status
Modbus Adapter
  slave address: 1
  baud rate:     0 (115200 Hz)
```

### `mb set slave <1-247>`

Set Modbus slave address. Applied immediately — Modbus server restarts with new address. Shell console (separate UART) is unaffected.

```
mb set slave 10
```

### `mb set baud <0|1>`

Set baud rate. 0 = 115200, 1 = 9600. Applied immediately.

```
mb set baud 0     # 115200
mb set baud 1     # 9600
```

### `mb save`

Persist current adapter config to NVS.

```
mb save
```

### `mb load`

Reload adapter config from NVS. Applied immediately.

```
mb load
```

### `mb factory`

Reset adapter config to Kconfig build-time defaults. Clears NVS entry, applies immediately.

```
mb factory
```

---

## `ss` — System Status

### `ss`

Display uptime, board temperature, humidity, and firmware version.

```
uart:~$ ss
uptime:  3600 s
temp:    25.5 C
humid:   45.0 %RH
fw:      0.1
```

### `ss reset`

Perform a software reset of the system.

```
ss reset
```

---

## Enum Reference

### Operating Mode
| Shell | Value |
|-------|-------|
| `normal` | 0 |
| `auto` | 1 |
| `cal` | 2 |

### Recovery Policy Mode (`recovery`)
| Shell | Value | Name |
|-------|-------|------|
| `0` | 0 | Manual Latch |
| `1` | 1 | Auto Retry |
| `2` | 2 | Auto Derate + Retry |
| `3` | 3 | Never Retry |

### Protection Mode (`prot_mode`)
| Shell | Value | Name |
|-------|-------|------|
| `0` | 0 | Disabled |
| `1` | 1 | Flag Only |
| `2` | 2 | Apply Output Action |

### Protection Output Action (`prot_action`)
| Shell | Value | Name |
|-------|-------|------|
| `0` | 0 | None |
| `2` | 2 | Disable Graceful |
| `3` | 3 | Disable Immediate |
| `4` | 4 | Force Output Zero |

### Baud Rate Code
| Shell | Value | Rate |
|-------|-------|------|
| `0` | 0 | 115200 |
| `1` | 1 | 9600 |

### System Config Fields
| Shell Field | Value | Description |
|-------------|-------|-------------|
| `mode` | 0/1/2 | Operating mode |
| `startup_policy` | 0/1 | 0=load op-config from NVS, 1=factory reset op-config |

## Fault Bitmask Display

Faults shown as comma-separated abbreviations:

| Letter | Fault | Bit |
|--------|-------|-----|
| `I` | Current | 0x0002 |
| `MEAS` | Measurement | 0x0004 |
| `HW` | Hardware | 0x0008 |
| `LOCK` | Interlock | 0x0010 |
| `RETRY` | Retry Exhausted | 0x0020 |
| `CFG` | Config Invalid | 0x0040 |
| `STALE` | Measurement Stale | 0x0080 |

## Capability Display

Capabilities shown as comma-separated letters:

| Letter | Bit | Capability |
|--------|-----|------------|
| `O` | 0x0001 | Output Enable |
| `R` | 0x0002 | Raw Output Drive (DAC) |
| `V` | 0x0004 | Voltage Measurement (ADC) |
| `I` | 0x0008 | Current Measurement (ADC) |
| `H` | 0x0010 | Hardware Status |

## Safety Notes

- `vc cal unlock` performs unlock and mode entry in one step; `vc mode cal` is not a valid path. 30s inactivity watchdog auto-exits calibration mode.
- `vc cal output on` only one channel at a time; cross-channel enforced by controller.
- `vc cal dac` with non-zero code requires cal output enabled.
- `vc cal commit` rejected while cal output enabled or DAC code non-zero.
- `vc ch <n> enable` rejected while active fault blocks present.
- Hard safety faults (HW/Interlock) block any non-zero calibration output.
- `mb set slave/baud` restarts Modbus server; any connected Modbus clients must reconnect.
- Modbus config persistence is separate from VC domain config (stored under `mb/cfg` NVS key).

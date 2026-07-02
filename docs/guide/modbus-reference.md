# Modbus Register Reference — Jianwei Voltage-Control Board

## Protocol Overview

- **Protocol version:** 3.0
- **Transport:** Modbus RTU over RS-485 (USART6)
- **Default slave address:** 1 (configurable 1–247)
- **Default baud:** 115200 (configurable: 9600)
- **Parity:** None (8N1)
- **All registers:** 16-bit (UINT16 or INT16)
- **32-bit values:** Split across consecutive HI (MSW) / LO (LSW) register pairs

### Backing Store

All Modbus registers are dispatched through a centralized **Register Catalog** (`lib/reg_store/reg_catalog.c`) — a descriptor-based catalog with owner vtables. Each register is keyed by a semantic ID (`REG_ID(module, instance, field)`). Reads are non-blocking; writes block until the VC runtime worker thread processes the command. This decouples the protocol adapter from domain logic.

## Register Block Layout

```
Offset  Block           Type        Size
0       System          Input       40
0       System          Holding     40
40      Channel 0       Input       40
40      Channel 0       Holding     40
80      Channel 1       Input       40
80      Channel 1       Holding     40
120     Channel 2       Input       40
120     Channel 2       Holding     40
...
640     Channel 15      Input       40
640     Channel 15      Holding     40
680     Extension       Holding     80
```

Channel base address formula: `CH_BASE(ch) = 40 + ch × 40`

Extension block base: `EXT_BASE = 40 + 16 × 40 = 680`

## System Input Block (FC04, offsets 0–39)

| Offset | Register | Type | Description |
|--------|----------|------|-------------|
| 0 | Protocol Major | uint16 | Major version (3) |
| 1 | Protocol Minor | uint16 | Minor version (0) |
| 2 | Variant ID | uint16 | Board variant identifier |
| 3 | Capability Flags | uint16 | `0x0001`=AUTO, `0x0002`=ENV_SENSOR, `0x0004`=CAL |
| 4 | Supported Channels | uint16 | Number of addressable channels |
| 5 | Active Channel Mask | uint16 | Bitmask of present channels |
| 6 | Board Temperature | int16 | Deci-°C (e.g. 255 = 25.5°C) |
| 7 | Board Humidity | uint16 | Deci-%RH (e.g. 450 = 45.0%) |
| 8 | Uptime HI | uint16 | Uptime seconds, high word |
| 9 | Uptime LO | uint16 | Uptime seconds, low word |
| 10 | FW Version HI | uint16 | Firmware version, major |
| 11 | FW Version LO | uint16 | Firmware version, minor |
| 12 | Operating Mode | uint16 | 0=Normal, 1=Automatic, 2=Calibration |
| 13 | System Status | uint16 | System status bitmask |
| 14 | Fault Cause | uint16 | System fault bitmask |
| 15–39 | Reserved | — | — |

## System Holding Block (FC03/FC06, offsets 0–39)

| Offset | Register | R/W | Description |
|--------|----------|-----|-------------|
| 0 | Operating Mode | R/W | Set operating mode (0=N, 1=A, 2=C) |
| 1 | Startup Channel Policy | R/W | 0=load from NVS, 1=factory reset op-config |
| 2 | Slave Address | R/W | Modbus slave address (1–247) |
| 3 | Baud Rate Code | R/W | 0=115200, 1=9600 |
| 4–38 | Reserved | — | — |
| 39 | Param Action | W | 1=Save, 2=Load, 3=Factory Reset |

### Changing Slave Address / Baud Rate

```
1. Write new value to SYS_SLAVE_ADDRESS (offset 2) and/or SYS_BAUD_RATE_CODE (offset 3)
2. Write 1 (SAVE) to SYS_PARAM_ACTION (offset 39) to persist + apply
3. Reconnect using new settings (Modbus server restarts immediately)
```

## Channel Input Block (FC04, base CH_BASE(ch))

| Offset | Register | Type | Capability Guard | Description |
|--------|----------|------|-------------------|-------------|
| 0 | Status Bits | uint16 | — | Channel status bitmask |
| 1 | Active Fault Cause | uint16 | — | Active blocking fault bitmask |
| 2 | Fault History Cause | uint16 | — | Recorded fault history bitmask |
| 3 | Last Prot Out Action | uint16 | — | Last protection output action applied |
| 4 | Auto Retry Count | uint16 | — | Auto-retry attempt counter |
| 5 | Auto Cooldown Remaining | uint16 | — | Cooldown seconds remaining |
| 6 | Last Fault Timestamp HI | uint16 | — | Last fault timestamp, high word |
| 7 | Last Fault Timestamp LO | uint16 | — | Last fault timestamp, low word |
| 8 | Oper Target Voltage | int16 | — | Operational target voltage (mV) |
| 9 | Capability Flags | uint16 | — | Channel capability bitmask |
| 10 | Measured Voltage | int16 | VOLTAGE_MEASUREMENT | Calibrated voltage (×100 mV) |
| 11 | Measured Current | int16 | CURRENT_MEASUREMENT | Calibrated current (×0.1 nA) |
| 12 | Raw ADC Voltage HI | int32 | VOLTAGE_MEASUREMENT | Raw ADC voltage, high word |
| 13 | Raw ADC Voltage LO | int32 | VOLTAGE_MEASUREMENT | Raw ADC voltage, low word |
| 14 | Raw ADC Current HI | int32 | CURRENT_MEASUREMENT | Raw ADC current, high word |
| 15 | Raw ADC Current LO | int32 | CURRENT_MEASUREMENT | Raw ADC current, low word |
| 16–39 | Reserved | — | — | — |

## Channel Holding Block (FC03/FC06, base CH_BASE(ch))

### Commands (offset 0–2)

| Offset | Register | R/W | Description |
|--------|----------|-----|-------------|
| 0 | Output Action | W | 1=Enable, 2=Disable Graceful, 3=Disable Immediate |
| 1 | Fault Command | W | 1=Clear Active, 2=Clear History |
| 2 | Param Action | W | 1=Save, 2=Load, 3=Factory Reset |

### Operational Config (offset 3–16)

| Offset | Register | R/W | Capability Guard | Description |
|--------|----------|-----|-------------------|-------------|
| 3 | Target Voltage | R/W | RAW_OUTPUT_DRIVE | Configured target (mV) |
| 4 | Ramp Up Step | R/W | RAW_OUTPUT_DRIVE | mV per interval |
| 5 | Ramp Up Interval | R/W | RAW_OUTPUT_DRIVE | ×100 ms |
| 6 | Ramp Down Step | R/W | RAW_OUTPUT_DRIVE | mV per interval |
| 7 | Ramp Down Interval | R/W | RAW_OUTPUT_DRIVE | ×100 ms |
| 8 | Recovery Policy Mode | R/W | — | 0=Manual, 1=Retry, 2=Derate, 3=Never |
| 9 | Auto Retry Delay | R/W | — | Seconds |
| 10 | Auto Retry Max Count | R/W | — | Max attempts |
| 11 | Auto Retry Window | R/W | — | Seconds |
| 12 | Current Safe Band % | R/W | — | 0–100% below limit for clear eligibility |
| 13 | Current Protection Mode | R/W | CURRENT_MEASUREMENT | 0=Off, 1=Flag Only, 2=Apply Action |
| 14 | Current Prot Out Action | R/W | CURRENT_MEASUREMENT | Protection action (see enum) |
| 15 | Current Limit Threshold | R/W | CURRENT_MEASUREMENT | ×0.1 nA (compared against Measured Current, same unit) |
| 16 | Auto Derate Step | R/W | OUTPUT+V_MEAS | mV per derate step |
| 17–19 | Reserved | — | — | — |

### Calibration Coefficients (offset 20–25)

Readable in any mode. Writable only in Calibration mode.

| Offset | Register | R/W | Guard | Description |
|--------|----------|-----|-------|-------------|
| 20 | Output Cal K | R/W/CAL | RAW_OUTPUT_DRIVE | DAC gain (÷10000, default 10000) |
| 21 | Output Cal B | R/W/CAL | RAW_OUTPUT_DRIVE | DAC offset (default 0) |
| 22 | Measured V Cal K | R/W/CAL | VOLTAGE_MEASUREMENT | Voltage gain (÷1000000; unity not representable) |
| 23 | Measured V Cal B | R/W/CAL | VOLTAGE_MEASUREMENT | Voltage offset (×100 mV) |
| 24 | Measured I Cal K | R/W/CAL | CURRENT_MEASUREMENT | Current gain (÷1000000; unity not representable) |
| 25 | Measured I Cal B | R/W/CAL | CURRENT_MEASUREMENT | Current offset (×0.1 nA) |

### Calibration Session (offset 30–34)

Writable only in Calibration mode.

| Offset | Register | R/W | Guard | Description |
|--------|----------|-----|-------|-------------|
| 30 | Cal Output Enable | W/CAL | RAW_OUTPUT_DRIVE | 1=On, 0=Off |
| 31 | Cal DAC Code | W/CAL | RAW_OUTPUT_DRIVE | Raw DAC value (0–65535) |
| 32 | Cal Sample Cmd | W/CAL | V\_or\_I\_MEAS | Write 1 to trigger ADC sample |
| 33 | Cal Commit Cmd | W/CAL | Any cal cap | Write 1 to persist to NVS |
| 34 | Cal Max Raw DAC Limit | R/W/CAL | RAW_OUTPUT_DRIVE | DAC ceiling (default 65535) |

## Extension Holding Block (FC03/FC06, offsets 680–759)

The extension block starts at absolute Modbus address 680 (0-based).

| Offset | Register | Description |
|--------|----------|-------------|
| 680 | Cal Unlock | Write `0xCA1B` then `0xA11B` to unlock calibration mode |
| 681 | Cal Exit | Write `1` to exit calibration mode |
| 682–759 | Reserved | — |

## Modbus Error Codes

| Code | Name | Condition |
|------|------|-----------|
| 1 | Illegal Function | Unimplemented function code |
| 2 | Illegal Address | Unsupported channel, capability-guarded register, or out-of-range address |
| 3 | Illegal Value | Invalid field value, unsafe state, or invalid command |
| 4 | Device Failure | Storage error or internal failure |

## Enum Values

### Operating Mode
| Value | Name |
|-------|------|
| 0 | Normal |
| 1 | Automatic |
| 2 | Calibration |

### Baud Rate Code
| Value | Rate |
|-------|------|
| 0 | 115200 |
| 1 | 9600 |

### Output Action
| Value | Name |
|-------|------|
| 0 | None |
| 1 | Enable |
| 2 | Disable Graceful |
| 3 | Disable Immediate |
| 4 | Force Output Zero (protection only) |

### Protection Mode
| Value | Name |
|-------|------|
| 0 | Disabled |
| 1 | Flag Only (record history) |
| 2 | Apply Output Action |

### Recovery Policy Mode
| Value | Name |
|-------|------|
| 0 | Manual Latch |
| 1 | Auto Retry |
| 2 | Auto Derate + Retry |
| 3 | Never Retry |

### Param Action
| Value | Name |
|-------|------|
| 1 | Save (persist to NVS) |
| 2 | Load (reload from NVS) |
| 3 | Factory Reset |

### Fault Cause Bitmask
| Bit | Name | Description |
|-----|------|-------------|
| 0x0002 | Current | Current exceeded limit |
| 0x0004 | Measurement | ADC measurement fault |
| 0x0008 | Hardware | Hardware failure |
| 0x0010 | Interlock | Safety interlock |
| 0x0020 | Retry Exhaust | Auto-retry limit reached |
| 0x0040 | Config Invalid | Invalid configuration |
| 0x0080 | Stale | Measurement data stale |

### Status Bits
| Bit | Name |
|-----|------|
| 0x0001 | Output Active (voltage non-zero or state enabling/ramping) |
| 0x0002 | Output Enabled |
| 0x0004 | Ramping |
| 0x0008 | Fault Latched |
| 0x0010 | Fault History Present |
| 0x0020 | Retry Cooldown |

### Channel Capability Flags
| Bit | Name |
|-----|------|
| 0x0001 | Output Enable (separate enable GPIO) |
| 0x0002 | Raw Output Drive (DAC) |
| 0x0004 | Voltage Measurement (ADC) |
| 0x0008 | Current Measurement (ADC) |
| 0x0010 | Hardware Status |

### Calibration Formula

```
calibrated = raw × k / D + b
```

`D` depends on the axis: **10000 for Output Cal K** (identity `k = 10000`,
`b = 0`), **1000000 for Measured V/I Cal K** (unity gain not representable —
see `docs/guide/parameter-reference.md` for why the measurement axes use a
finer scale).

## mbpoll Examples

All `-r` values are 1-indexed Modbus register addresses. Add 1 to the 0-based offset for the `-r` argument.

```bash
# Read system snapshot (input registers 1–15, Modbus addresses)
mbpoll -a 1 -b 115200 -t 4 -r 1 -c 15 /dev/ttyUSB0

# Read channel 0 input snapshot (base 40 + offsets 0–15 = addresses 41–56)
mbpoll -a 1 -b 115200 -t 4 -r 41 -c 16 /dev/ttyUSB0

# Read slave address and baud rate (holding registers, offsets 2–3 = addresses 3–4)
mbpoll -a 1 -b 115200 -t 3 -r 3 -c 2 /dev/ttyUSB0

# Set slave address to 10 (holding offset 2 = address 3), baud to 9600 (offset 3 = address 4), then save (offset 39 = address 40)
mbpoll -a 1 -b 115200 -t 6 -r 3 10 /dev/ttyUSB0
mbpoll -a 1 -b 115200 -t 6 -r 4 1 /dev/ttyUSB0
mbpoll -a 1 -b 115200 -t 6 -r 40 1 /dev/ttyUSB0
# → reconnect with: mbpoll -a 10 -b 9600 ...

# Set channel 0 target voltage to 5000 mV (holding offset 3 = address 44)
mbpoll -a 1 -b 115200 -t 6 -r 44 5000 /dev/ttyUSB0

# Enable channel 0 output (holding offset 0 = address 41)
mbpoll -a 1 -b 115200 -t 6 -r 41 1 /dev/ttyUSB0

# Calibration unlock (extension block, absolute offset 680 = address 681)
mbpoll -a 1 -b 115200 -t 6 -r 681 51867 /dev/ttyUSB0    # 0xCA1B
mbpoll -a 1 -b 115200 -t 6 -r 681 41243 /dev/ttyUSB0    # 0xA11B
# Then enter calibration mode: mbpoll -r 1 2  (set operating mode = 2)

# Calibration exit (extension block, absolute offset 681 = address 682)
mbpoll -a 1 -b 115200 -t 6 -r 682 1 /dev/ttyUSB0

# Read calibration coefficients (channel 0 holding, offsets 20–25 = addresses 61–66)
mbpoll -a 1 -b 115200 -t 3 -r 61 -c 6 /dev/ttyUSB0
```

# Modbus-RTU Interface - Requirement and Design Reference

Date: 2026-06-15
Status: Draft for the voltage-control architecture

## 1. Scope

This document defines the Modbus-RTU protocol adapter interface for the Jianwei voltage-control board firmware architecture. It specifies transport, function codes, register layout, register encoding, numeric enum values, and Modbus exception mapping.

Product-domain behavior is specified separately in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`. Domain terminology is specified in `UBIQUITOUS_LANGUAGE.md` and `CONTEXT.md`.

The shared register-offset header `include/regmap/hvb_regs.h` is the single source of truth for register layout. Both firmware and host application can include or parse this header.

Protocol major version `2` is not backward-compatible with the older register map. Host tools must read the protocol version before interpreting registers. Protocol minor version `1` adds Calibration Mode and its calibration-only registers.

## 2. Architecture Boundary

```text
RS-485 Modbus RTU
  -> Zephyr Modbus serial server
  -> Modbus adapter
  -> Domain Runtime Library facade
  -> Domain Runtime Library
```

The Modbus adapter owns register layout, function-code access rules, register packing, command readback, and Modbus exception mapping. It must not own product validation, ramping, limit detection, safe-state override, persistence policy, UART framing, CRC, or RS-485 DE timing.

Firmware should use Zephyr's native Modbus serial support and devicetree configuration for UART and RS-485 DE handling.

## 3. Transport Requirements

| Parameter | Value |
|---|---|
| Protocol | Modbus-RTU |
| Physical layer | RS-485 half-duplex for current HVB variant |
| Data format | 8N1 |
| Default baud rate | 115200 bps |
| Alternate baud rate | 9600 bps |
| Default slave address | 1 |
| Broadcast address | 0, write-only, no response |
| Current HVB UART | USART6: TX PG14, RX PG9 |
| Current HVB RS-485 DE GPIO | PG11, active-high transmit, via devicetree `de-gpios` |

## 4. Function Codes

| FC | Name | Required behavior |
|---|---|---|
| 0x03 | Read Holding Registers | Read configuration, writable state, and self-clearing command readback |
| 0x04 | Read Input Registers | Read telemetry, capabilities, status, and fault history |
| 0x06 | Write Single Holding Register | Write a single 16-bit setting or command |

All registers are single 16-bit (UINT16 or INT16). FC10 (Write Multiple Holding Registers) is optional for host convenience but not required; a host may write registers individually with FC06. The adapter does not enforce atomicity across multiple registers because all fields are single 16-bit.

Unsupported function codes return exception `0x01` Illegal Function. Coils and discrete inputs are not part of this protocol.

## 5. Data Conventions

All register addresses are zero-based Modbus PDU addresses.

| Type | Registers | Encoding |
|---|---:|---|
| UINT16 | 1 | Unsigned 16-bit integer |
| INT16 | 1 | Two's-complement signed 16-bit integer |

All registers are single 16-bit. Every register can be read or written independently with FC03 / FC04 / FC06.

Voltage and current values are in raw LSBs. The physical value is `raw × scale`, where scale is a compile-time constant defined by the board variant. The variant documents its scale factors. Host tools map `variant_id` to the correct scales.

| Variant | voltage_scale | current_scale | Example |
|---|---|---|---|
| HVB (id=1) | 100 mV/LSB | 1 nA/LSB | raw 20000 = 2000 V, raw 5000 = 5 μA |

32-bit quantities (uptime, timestamps) are split across two consecutive 16-bit registers with `_HI` / `_LO` suffixes: `value = (HI << 16) | LO`.

Protocol v2 fixed units for non-voltage/current quantities:

| Quantity | Unit |
|---|---|
| Time interval | seconds x10 where named interval, seconds where named delay/window |
| Calibration K | x10000, UINT16, default 10000 |
| Calibration B | x1000, INT16, default 0 |
| Safe-band percentage | UINT16 percent, range 0-50, default 10 |

Signed 32-bit raw ADC values are split across two consecutive 16-bit registers with `_HI` / `_LO` suffixes: `value = sign_extend((HI << 16) | LO)`.

## 6. Address Space

```text
0      System block                 input + holding, 40 registers
40     Channel 0 block              input + holding, 40 registers
80     Channel 1 block              input + holding, 40 registers
120    Channel 2 block              reserved for future variants
160    Channel 3 block              reserved for future variants
200    Reserved extension block     holding only, 80 registers
```

```text
channel_base = 40 + channel_index * 40
absolute_addr = channel_base + local_offset
```

Initial implementation rules:

| Rule | Behavior |
|---|---|
| Channel availability | Fixed by firmware/hardware variant configuration |
| SYS_MOD DIP selection | Not used initially |
| Unsupported channel access | Return exception `0x02` Illegal Data Address |
| Future SYS_MOD support | May change addressable channel mask at boot before protocol services start |

There is no distinction between supported-but-inactive and unsupported in protocol behavior.

## 7. Domain Fields Exposed By Modbus

| Term | Meaning |
|---|---|
| Configured Target Voltage | Domain field exposed in channel holding registers |
| Operational Target Voltage | Domain Snapshot exposed in channel input registers |
| Output Drive Level | Domain status exposed by channel status bits |
| Output Enable | Domain status exposed by channel status bits |
| Active Fault Block | Domain status exposed in channel input registers |
| Fault History | Domain status exposed in channel input registers |

## 8. Enumerations And Bit Fields

### 8.1 Operating Mode

| Value | Name | Meaning |
|---:|---|---|
| 0 | Normal | Domain Operating Mode value; see domain behavior spec |
| 1 | Automatic | Domain Operating Mode value; see domain behavior spec |
| 2 | Calibration | Volatile factory/service mode; requires Calibration Unlock and is not persisted |

Runtime semantics are defined in the domain behavior spec.

### 8.2 Output Action

| Value | Name | Host Output Action | Protection Output Action | Meaning |
|---:|---|---|---|---|
| 0 | None | valid | valid | No action |
| 1 | Enable | valid | invalid | Domain Output Action; see domain behavior spec |
| 2 | Disable Graceful | valid | valid | Domain Output Action; see domain behavior spec |
| 3 | Disable Immediate | valid | valid | Domain Output Action; see domain behavior spec |
| 4 | Force Output Zero | invalid | valid | Domain Output Action; see domain behavior spec |
| 5 | Clamp | invalid | voltage protection only | Domain Output Action; see domain behavior spec |

Output Action semantics are defined in the domain behavior spec. The Modbus adapter validates context-specific enum values.

### 8.3 Channel Fault Command

| Value | Command | Meaning |
|---:|---|---|
| 0 | None | Readback value after command execution |
| 1 | Clear Active Fault Block | Domain fault command; see domain behavior spec |
| 2 | Clear Fault History | Domain fault command; see domain behavior spec |

The adapter maps domain rejection of an unsafe clear to exception `0x03`.

### 8.4 Parameter Action

| Value | Action |
|---:|---|
| 0 | None, readback value after execution |
| 1 | Save persistent parameters for this block's scope |
| 2 | Load persistent parameters for this block's scope |
| 3 | Restore factory defaults for this block's scope |
| 255 | Software reset |

Parameter action scope and persistence behavior are defined in the domain behavior spec. The adapter maps storage failure to exception `0x04`.

### 8.5 Protection Mode

| Value | Name | Meaning |
|---:|---|---|
| 0 | Disabled | Domain Protection Mode value; see domain behavior spec |
| 1 | Flag Only | Domain Protection Mode value; see domain behavior spec |
| 2 | Apply Output Action | Domain Protection Mode value; see domain behavior spec |

### 8.6 Recovery Policy Mode

| Value | Mode | Meaning |
|---:|---|---|
| 0 | Manual latch | Domain Recovery Policy Mode value; see domain behavior spec |
| 1 | Auto retry | Domain Recovery Policy Mode value; see domain behavior spec |
| 2 | Auto derate retry | Domain Recovery Policy Mode value; see domain behavior spec |
| 3 | Never retry | Domain Recovery Policy Mode value; see domain behavior spec |

Recovery behavior is defined in the domain behavior spec.

### 8.7 Channel Status Bits

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Output Drive Level is nonzero |
| 1 | 0x0002 | Output Enable is active |
| 2 | 0x0004 | Ramping is active |
| 3 | 0x0008 | Active Fault Block is present |
| 4 | 0x0010 | Fault History is present |
| 5 | 0x0020 | Automatic cooldown active |
| 6 | 0x0040 | Automatic retry budget exhausted |
| 7 | 0x0080 | Channel unsupported by this variant |

### 8.8 Fault Cause Bits

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Voltage limit event |
| 1 | 0x0002 | Current limit event |
| 2 | 0x0004 | Measurement invalid |
| 3 | 0x0008 | Output hardware fault |
| 4 | 0x0010 | Variant safety interlock |
| 5 | 0x0020 | Automatic retry exhausted |
| 6 | 0x0040 | Configuration invalid for automatic start |

### 8.9 System Capability Flags

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Automatic mode supported |
| 1 | 0x0002 | Environment sensor present |
| 2 | 0x0004 | Calibration Mode supported |

### 8.10 Channel Capability Flags

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Output Enable control present |
| 1 | 0x0002 | Raw output drive present |
| 2 | 0x0004 | Voltage measurement present |
| 3 | 0x0008 | Current measurement present |
| 4 | 0x0010 | Hardware status/fault evidence present |

`CH_CAPABILITY_FLAGS` is the primary per-channel discovery register for physical/provider capabilities. Host tools should read it before showing capability-specific controls or accessing capability-specific registers. Product policy features such as Automatic Mode or Calibration Mode are not channel capability bits. Calibration register availability is derived from `SYS_CAP_CALIBRATION_MODE` plus the relevant raw output and measurement capability bits. Capability-specific registers whose offsets are defined by the protocol but unsupported by the current channel return `0x02` Illegal Data Address for both reads and writes.

### 8.11 Calibration Sample Status

| Value | Name | Meaning |
|---:|---|---|
| 0 | No valid sample | No captured raw sample is available since Calibration Mode entry or channel disable |
| 1 | Sample valid | All supported raw measurement paths for the channel succeeded |
| 2 | Sample busy | A raw sample command is in progress |
| 3 | Sample error | One or more supported raw measurement paths failed; raw values from that sample should not be trusted |

Unsupported measurement paths are not sample failures. Host tools use `CH_CAPABILITY_FLAGS` to decide which raw readback registers are meaningful and supported for the channel.

### 8.12 Calibration Unlock

Calibration Unlock is a volatile global guard for entering Calibration Mode. It is not cryptographic authentication.

| Step | Register | Value |
|---:|---|---:|
| 1 | Calibration Unlock | `0xCA1B` |
| 2 | Calibration Unlock | `0xA11B` |
| 3 | System Operating Mode | `2` |

A wrong Calibration Unlock value clears unlock progress. Successful entry to Calibration Mode clears unlock progress. The Calibration Unlock register always reads as `0`.

## 9. Input Registers - FC04

Input registers expose read-only Domain Snapshots maintained by the Domain Runtime Library from the latest channel evidence. Reading input registers does not require synchronous hardware sampling during the Modbus request.

### 9.1 System Input Block - base 0

| Offset | Abs | Name | Type | Description |
|---:|---:|---|---|---|
| 0 | 0 | Protocol Major | UINT16 | Value 2 |
| 1 | 1 | Protocol Minor | UINT16 | Value 1 when Calibration Mode registers are supported |
| 2 | 2 | Variant ID | UINT16 | Board/product variant identifier |
| 3 | 3 | System Capability Flags | UINT16 | See system capability flags |
| 4 | 4 | Supported Channel Count | UINT16 | Number of channels addressable by this variant |
| 5 | 5 | Active Channel Mask | UINT16 | Bit N set means channel N is addressable |
| 6 | 6 | Board Temperature | INT16 | degC x10, environmental reading if available, else 0 |
| 7 | 7 | Board Humidity | UINT16 | %RH x10, environmental reading if available, else 0 |
| 8 | 8 | Uptime HI | UINT16 | Seconds since boot, high word |
| 9 | 9 | Uptime LO | UINT16 | Seconds since boot, low word |
| 10 | 10 | Firmware Version High | UINT16 | Version encoding defined by release policy |
| 11 | 11 | Firmware Version Low | UINT16 | Version encoding defined by release policy |
| 12 | 12 | Active Operating Mode | UINT16 | Current domain operating mode |
| 13 | 13 | System Status | UINT16 | Global status flags, product-defined |
| 14 | 14 | System Fault Cause | UINT16 | Global fault summary |
| 15-39 | 15 | Reserved | UINT16 | Read as 0 |

### 9.2 Channel Input Block - base 40 + channel * 40

| Offset | Name | Type | Description |
|---:|---|---|---|
| 0 | Channel Status Bits | UINT16 | See channel status bits |
| 1 | Active Fault Cause | UINT16 | Fault bits currently blocking operation |
| 2 | Fault History Cause | UINT16 | Fault bits observed since last history clear |
| 3 | Last Protection Output Action | UINT16 | Last protection output action applied |
| 4 | Auto Retry Count | UINT16 | Retries inside current sliding retry window |
| 5 | Auto Cooldown Remaining | UINT16 | Seconds until retry is allowed, else 0 |
| 6 | Last Fault Timestamp HI | UINT16 | Uptime when last fault event was recorded, high word |
| 7 | Last Fault Timestamp LO | UINT16 | Uptime when last fault event was recorded, low word |
| 8 | Operational Target Voltage | INT16 | Current runtime target, raw LSBs |
| 9 | Channel Capability Flags | UINT16 | See channel capability flags |
| 10 | Measured Voltage | INT16 | Calibrated measured output voltage, raw LSBs; requires voltage measurement capability |
| 11 | Measured Current | INT16 | Calibrated measured output current, raw LSBs; requires current measurement capability |
| 12 | Raw ADC Voltage HI | INT32_HI | Calibration Mode only, captured raw voltage ADC code |
| 13 | Raw ADC Voltage LO | INT32_LO | Calibration Mode only, captured raw voltage ADC code |
| 14 | Raw ADC Current HI | INT32_HI | Calibration Mode only, captured raw current ADC code |
| 15 | Raw ADC Current LO | INT32_LO | Calibration Mode only, captured raw current ADC code |
| 16 | Calibration Sample Status | UINT16 | Calibration Mode only, see calibration sample status enum |
| 17 | Raw DAC Readback | UINT16 | Calibration Mode only, last written native DAC code |
| 18-39 | Reserved | UINT16 | Read as 0 |

## 10. Holding Registers - FC03 / FC06

Holding registers expose writable configuration and self-clearing commands. Command registers read as 0 after execution. All registers are single 16-bit; FC06 writes each register independently.

### 10.1 System Holding Block - base 0

| Offset | Abs | Name | Access | Type | Range | Description |
|---:|---:|---|---|---|---|---|
| 0 | 0 | Operating Mode | RW | UINT16 | 0-2 | Normal, Automatic, or Calibration |
| 1 | 1 | Slave Address | RW | UINT16 | 0-247 | Effective after save and restart |
| 2 | 2 | Baud Rate Code | RW | UINT16 | 0-1 | 0 = 115200, 1 = 9600; effective after save and restart |
| 3 | 3 | Recovery Policy Mode | RW | UINT16 | 0-3 | System-wide recovery policy |
| 4 | 4 | Auto Retry Delay | RW | UINT16 | seconds | Cooldown before retry |
| 5 | 5 | Auto Retry Max Count | RW | UINT16 | count | Maximum retries in sliding retry window |
| 6 | 6 | Auto Retry Window | RW | UINT16 | seconds | Sliding window used to count retries |
| 7 | 7 | Voltage Safe Band % | RW | UINT16 | 0-50 | Voltage retry band around target, default 10 |
| 8 | 8 | Current Safe Band % | RW | UINT16 | 0-50 | Current retry below limit, default 10 |
| 9-38 | 9 | Reserved | R | UINT16 | - | Read as 0, reject writes |
| 39 | 39 | System Param Action | RW | UINT16 | enum | Save/load/factory reset/software reset for system parameters |

### 10.2 Channel Holding Block - base 40 + channel * 40

| Offset | Name | Access | Type | Range | Description |
|---:|---|---|---|---|---|
| 0 | Channel Output Action | RW | UINT16 | context-valid | Self-clearing host output action |
| 1 | Channel Fault Command | RW | UINT16 | 0-2 | Self-clearing fault command |
| 2 | Channel Param Action | RW | UINT16 | enum | Save/load/factory reset/software reset for this channel |
| 3 | Configured Target Voltage | RW | INT16 | variant-defined | Host/configured target voltage, raw LSBs |
| 4 | Ramp Up Step | RW | UINT16 | variant-defined | Step size per ramp-up step, raw LSBs |
| 5 | Ramp Up Interval | RW | UINT16 | seconds x10 | Delay per ramp-up step |
| 6 | Ramp Down Step | RW | UINT16 | variant-defined | Step size per ramp-down step, raw LSBs |
| 7 | Ramp Down Interval | RW | UINT16 | seconds x10 | Delay per ramp-down step |
| 8 | Voltage Protection Mode | RW | UINT16 | 0-2 | Disabled, Flag Only, or Apply Output Action; requires voltage measurement capability |
| 9 | Voltage Protection Output Action | RW | UINT16 | context-valid | Protection action; Clamp allowed for voltage only; requires voltage measurement capability |
| 10 | Voltage Limit Threshold | RW | INT16 | variant-defined | Voltage threshold, raw LSBs; requires voltage measurement capability |
| 11 | Current Protection Mode | RW | UINT16 | 0-2 | Disabled, Flag Only, or Apply Output Action; requires current measurement capability |
| 12 | Current Protection Output Action | RW | UINT16 | context-valid | Clamp invalid for current protection; requires current measurement capability |
| 13 | Current Limit Threshold | RW | INT16 | variant-defined | Current threshold, raw LSBs; requires current measurement capability |
| 14 | Auto Derate Step | RW | UINT16 | variant-defined | Per-channel target reduction per retry, raw LSBs |
| 15 | Save Target Policy | RW | UINT16 | 0-1 | 0 saves safe target default, 1 saves configured target |
| 16 | Output Calibration K | RW | UINT16 | x10000 | Output path slope; requires raw output drive capability |
| 17 | Output Calibration B | RW | INT16 | x1000 | Output path offset; requires raw output drive capability |
| 18 | Measured Voltage Calibration K | RW | UINT16 | x10000 | Voltage measurement slope; requires voltage measurement capability |
| 19 | Measured Voltage Calibration B | RW | INT16 | x1000 | Voltage measurement offset; requires voltage measurement capability |
| 20 | Measured Current Calibration K | RW | UINT16 | x10000 | Current measurement slope; requires current measurement capability |
| 21 | Measured Current Calibration B | RW | INT16 | x1000 | Current measurement offset; requires current measurement capability |
| 22 | Calibration Output Enable | RW | UINT16 | 0-1 | Calibration Mode only; requires raw output drive capability; readable state, enables raw output gate for this channel |
| 23 | Raw DAC Code | RW | UINT16 | variant-defined | Calibration Mode only; requires raw output drive capability; native DAC code |
| 24 | Calibration Sample Command | RW | UINT16 | command | Calibration Mode only; captures supported raw ADC samples, reads as 0 after execution |
| 25 | Calibration Commit Command | RW | UINT16 | command | Calibration Mode only; persists this channel's calibration coefficients, reads as 0 after execution |
| 26 | Calibration Max Raw DAC Limit | RW | UINT16 | variant-defined | Calibration Mode only; requires raw output drive capability; optional temporary limit at or below variant max |
| 27-39 | Reserved | R | UINT16 | - | Read as 0, reject writes |

## 11. Adapter Requirements

The Modbus adapter must translate register access into protocol-neutral domain operations without implementing product behavior itself.

The Modbus register map is a stable protocol address dictionary. Board and channel capabilities are static build-composed facts derived from devicetree/Kconfig and the variant model; they do not change register offsets. Capability-specific registers whose offsets are defined by the protocol but unsupported by the current board/channel return `0x02` Illegal Data Address for both reads and writes instead of pretending the feature exists with disabled values.

Channel blocks are arranged so the front core/discovery ranges are safe for broad reads. For supported channels, channel input offsets `0..9` are always readable. Channel holding offsets `0..2` are command/core registers. Capability-specific tail registers may be unsupported for a channel; a block read that includes any unsupported capability-specific tail register returns `0x02` Illegal Data Address for the whole request. Reserved registers remain readable as `0` and reject writes with `0x02`.

| Adapter requirement | Behavior |
|---|---|
| Domain command mapping | Writable command registers call domain APIs and read back 0 after execution |
| Domain snapshot mapping | Input registers expose cached Domain Snapshots maintained by the Domain Runtime Library |
| Range validation | Adapter validates protocol enum/range shape before calling domain where possible |
| Domain rejection mapping | Domain invalid value or unsafe state maps to exception `0x03` unless a more specific mapping applies |
| Reserved registers | Writes return exception `0x02`; reads return 0 where specified |
| Unsupported channels | Access returns exception `0x02` |
| Unsupported capability-specific registers | Reads and writes return exception `0x02` unless a later protocol version defines more specific behavior |
| Self-clearing commands | Command registers read back as 0 after execution |
| Calibration-only access | Raw ADC/DAC/debug registers reject access outside Calibration Mode according to exception mapping |
| Calibration coefficient writes | Coefficient registers are readable in all modes and writable only in Calibration Mode |
| Calibration unlock | `CAL_UNLOCK` writes update volatile unlock progress and always read back 0 |
| Calibration mode persistence | System save must not persist Calibration Mode as boot operating mode |

Product behavior for operating modes, recovery, safe bands, fault handling, target changes, persistence, and calibration is defined in `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`.

Calibration command registers use value `1` to execute the command unless a future minor version assigns additional command values. Value `0` is accepted as no-op and reads back as `0`. Other values return exception `0x03` Illegal Data Value.

## 12. Exception Mapping

| Exception | Name | Use |
|---:|---|---|
| 0x01 | Illegal Function | Unsupported function code |
| 0x02 | Illegal Data Address | Undefined register, reserved write, unsupported channel |
| 0x03 | Illegal Data Value | Out-of-range value, invalid enum, unsafe fault clear |
| 0x04 | Slave Device Failure | Internal failure, storage failure, hardware/service unavailable |

Broadcast writes are processed without a response. Broadcast reads are ignored according to Modbus behavior.

## 13. Initial HVB Variant Profile

| Field | Initial value |
|---|---|
| Variant ID | 1 |
| voltage_scale | 100 mV/LSB |
| current_scale | 1 nA/LSB |
| Physical channels | 2, fixed by firmware/hardware configuration |
| SYS_MOD selection | Not used initially |
| Default operating mode | Normal |
| Calibration Mode | Supported in protocol 2.1, volatile, unlock required |
| Default recovery policy | Manual latch |
| Default safe-band percentages | 10 for voltage, 10 for current |
| OTA block | Reserved, not implemented initially |
| RS-485 transport | Zephyr Modbus serial over USART6 with PG11 DE GPIO |

## 14. Reserved Extension Block

Protocol `2.1` assigns the first extension holding register for Calibration Unlock.

| Offset | Abs | Name | Access | Type | Range | Description |
|---:|---:|---|---|---|---|---|
| 0 | 200 | Calibration Unlock | RW | UINT16 | command | Global two-step guard for entering Calibration Mode; reads as 0 |
| 1-79 | 201 | Reserved | R | UINT16 | - | Read as 0, reject writes |

Planned extension candidates include OTA, variant descriptor strings, extended calibration tables, and protocol bridge diagnostics.

## 15. Open Decisions

| Decision | Why it matters |
|---|---|
| Exact Variant ID values | Host tools need stable identification |
| Firmware version encoding | Host tools need to display versions consistently |
| Whether automatic mode behavior ships in the first firmware slice | Registers can exist before behavior is fully enabled, but host behavior must be clear |
| Variant-specific output/measurement calibration defaults | Factory reset needs authoritative defaults |
| Production factory handoff registers | Deferred until calibration-complete enforcement is designed |

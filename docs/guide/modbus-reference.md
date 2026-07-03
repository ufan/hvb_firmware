# Modbus Register Reference — Jianwei Voltage-Control Board

This is the authoritative wire-protocol reference for the Jianwei
voltage-control board: transport, register map, encoding, and enumerations.
It's written for **third-party developers implementing their own host
tooling** (not reusing `tools/hvb_modbus_core`) — no familiarity with this
repo's C++ code is assumed. If you *are* working in this repo, the register
offsets below are generated from `include/reg_store/reg_map.h` and
`include/reg_store/modbus_view.def`, which remain the single source of
truth; this document should never drift from them.

Related documents, once you have a working connection:
[`calibration-guide.md`](calibration-guide.md) (factory calibration
workflow), [`operating-mode-guide.md`](operating-mode-guide.md) (Normal vs.
Automatic mode, protection/recovery semantics),
[`parameter-reference.md`](parameter-reference.md) (field-by-field defaults
and derivations). This document does not repeat product *behavior* — only
the wire format needed to talk to it.

---

## 1. Protocol overview

| | |
|---|---|
| Protocol version | **3.0** (`Protocol Major`=3, `Protocol Minor`=0 — read and check before interpreting anything else) |
| Transport | Modbus RTU |
| Physical layer | RS-485 half-duplex (current variants) |
| Serial format | 8N1 |
| Baud rate | 115200 default, 9600 alternate (runtime-selectable, see §5) |
| Slave address | 1 default, runtime-selectable 1–247; 0 is the broadcast address |
| Function codes | 0x03 (Read Holding), 0x04 (Read Input), 0x06 (Write Single Holding) — that's all; no FC10, no coils/discrete inputs |

**A major version mismatch means incompatible registers — do not proceed.**
Protocol 3 is a breaking change from protocol 2 (different offsets, removed
and added registers throughout); a client built against this document must
refuse to talk to a board reporting `Protocol Major` ≠ 3. A minor-version
bump is additive only (new registers in previously-reserved space); it's
safe to ignore fields you don't recognize.

---

## 2. Transport framing

Standard Modbus RTU framing applies — nothing board-specific here, but
spelled out for implementers who haven't built an RTU client before:

- Frame: `[slave addr: 1B] [function code: 1B] [data: N bytes] [CRC16: 2B]`
- CRC16 is the standard Modbus CRC (poly `0xA001`, init `0xFFFF`, LSB-first
  in the frame — i.e. CRC low byte transmitted before CRC high byte). Any
  standard Modbus CRC16 implementation works; it is not board-specific.
- Register values are big-endian on the wire (high byte first), standard
  Modbus convention.
- A frame boundary is a silence of ≥3.5 character times on the wire — the
  usual RTU inter-frame gap. If you're using an off-the-shelf RTU
  master library (e.g. `libmodbus`, `pymodbus`, ModbusLib), it already
  handles this; you only need the register map below.
- **RS-485 half-duplex**: the board's driver-enable timing is handled
  firmware-side: don't add your own extra inter-request delay beyond normal
  RTU turnaround unless your USB-RS485 adapter needs it.
- **Broadcast** (slave address 0): write-only, no response is sent. Reads
  addressed to 0 are not meaningful and should not be sent.

Everything below describes the **PDU** (data field) contents — register
addresses, encoding, and semantics — which is transport-agnostic.

---

## 3. Data conventions

- **All registers are single 16-bit words** (`UINT16` or `INT16`,
  two's-complement for signed). There is no register wider than 16 bits at
  the wire level.
- **32-bit values** (uptime, firmware version, fault timestamp, raw ADC
  codes) are split across two consecutive registers, `_HI` then `_LO`, at
  addresses `N` and `N+1`: `value = (HI << 16) | LO`. Raw ADC values are
  signed 32-bit (sign-extend after combining).
- **Voltage**: raw register value × **0.1 V/LSB** (i.e. ×100 mV). Raw 5000
  = 500.0 V. This applies uniformly to every voltage-flavored register
  (configured target, operational target, measured voltage, ramp steps).
- **Current**: raw register value × **0.1 nA/LSB**. Raw 10000 = 1000.0 nA.
  This applies to current threshold and measured-current registers.
- **Time intervals**: raw × 0.1 = seconds (i.e. raw is deciseconds — "×10"
  in field names below means the raw value is ten times the second count).
  Delay/window fields (auto-retry delay, window) are already in whole
  seconds with no ×10 factor — check the per-field description, the
  convention differs between "interval" and "delay/window" fields.
- **Percentages**: plain `UINT16`, e.g. 10 = 10%. Not scaled.
- **Calibration coefficients**: see §9's dedicated formula — the divisor
  differs between the output axis and the two measurement axes, and is not
  the simple ×0.1 pattern used elsewhere.
- **Reserved registers**: always read as `0`, always reject writes with
  exception `0x02`. Never assume a meaning for a reserved offset, even if
  it happens to read back a non-zero value on some firmware build — that
  would be a firmware bug, not a documented behavior to depend on.

---

## 4. Address space

```
0      System block            input + holding, 40 registers
40     Channel 0 block         input + holding, 40 registers
80     Channel 1 block         input + holding, 40 registers
120    Channel 2 block         input + holding, 40 registers  (reserved on 2-channel variants)
...
640    Channel 15 block        input + holding, 40 registers  (protocol max; reserved unless present)
680    Extension block         holding only, 80 registers
```

```
CH_BASE(ch) = 40 + ch × 40
EXT_BASE    = 40 + 16 × 40 = 680
```

The protocol always reserves address space for 16 channels regardless of
what any particular board variant actually populates. **Read `Supported
Channels` and `Active Channel Mask` (§6) before assuming any channel beyond
0 exists** — accessing an unsupported channel's block returns exception
`0x02` for every register in it, not zeroed/dummy data.

---

## 5. Getting started — recommended bring-up sequence

A minimal client implementation should, in order:

1. Connect at 115200 8N1, slave address 1 (the universal defaults — override
   only if you already know the board was reconfigured).
2. **FC04 read System Input offsets 0–1** (`Protocol Major`/`Minor`).
   Refuse to continue if `Major` ≠ 3.
3. **FC04 read System Input offsets 2–5** (`Variant ID`, `System Capability
   Flags`, `Supported Channels`, `Active Channel Mask`). Use these to decide
   which channel blocks and which system-level features (Automatic mode,
   environment sensor, Calibration mode) are meaningful for this board —
   don't hardcode "2 channels" even though that's the only shipping variant
   today.
4. **Per active channel, FC04 read Channel Input offset 9** (`Capability
   Flags`) before touching any capability-gated register (§10) on that
   channel. A register whose guard capability is absent returns exception
   `0x02` for both read and write — this is not a soft "returns zero"
   fallback.
5. From here, poll or write as needed. Use the **poll category** noted per
   register (§6/§9/§10) to decide polling cadence: `REALTIME` fields change
   continuously and are safe to poll fast (~100 ms+); `CONFIG` fields only
   change when you or another client writes them — poll slowly or read
   on-demand; `FIXED` fields never change after boot — read once; `COMMAND`
   registers are write-only triggers — never poll them.

### Self-clearing commands are synchronous

Command registers (Output Action, Fault Command, Param Action, the
calibration session commands) execute **before the FC06 write response is
sent** — the board's command dispatch blocks the write until the underlying
action completes. A command register always reads back `0` immediately
after a successful write; you do not need to poll-wait for it to clear.
If a write to a command register returns a Modbus exception, the command
was rejected outright and had no effect.

---

## 6. System Input Block (FC04, offsets 0–39)

| Offset | Register | Type | Poll | Description |
|---:|---|---|---|---|
| 0 | Protocol Major | UINT16 | FIXED | Always check this first — see §1 |
| 1 | Protocol Minor | UINT16 | FIXED | Additive-only within a major version |
| 2 | Variant ID | UINT16 | FIXED | Board/product variant identifier (§14) |
| 3 | Capability Flags | UINT16 | FIXED | `0x0001`=Automatic mode, `0x0002`=env sensor, `0x0004`=Calibration mode — see §12 |
| 4 | Supported Channels | UINT16 | FIXED | Number of channels this variant addresses |
| 5 | Active Channel Mask | UINT16 | REALTIME | Bit N set ⇒ channel N addressable right now |
| 6 | Board Temperature | INT16 | REALTIME | ×0.1 °C; 0 if no environment sensor |
| 7 | Board Humidity | UINT16 | REALTIME | ×0.1 %RH; 0 if no environment sensor |
| 8–9 | Uptime HI/LO | UINT32 | REALTIME | Seconds since boot |
| 10–11 | FW Version HI/LO | UINT32 | FIXED | Packed 32-bit; current firmware sets HI=major, LO=minor as separate 16-bit values, not a semantic version string |
| 12 | Active Operating Mode | UINT16 | REALTIME | See §11.1 |
| 13 | System Status | UINT16 | REALTIME | Global status bitmask (product-reserved; no bits currently defined) |
| 14 | Fault Cause | UINT16 | REALTIME | Global fault summary, same bit layout as channel fault cause (§13) |
| 15–39 | Reserved | — | — | Read as 0, reject writes |

---

## 7. System Holding Block (FC03/FC06, offsets 0–39)

| Offset | Register | Access | Poll | Description |
|---:|---|---|---|---|
| 0 | Operating Mode | RW | CONFIG | 0=Normal, 1=Automatic, 2=Calibration — see §11.1. Entering Calibration requires the unlock sequence (§8) first |
| 1 | Startup Channel Policy | RW | CONFIG | 0=load channel config from NVS at boot, 1=apply factory-default op-config at boot |
| 2 | Slave Address | RW | CONFIG | 1–247. Writing this does **not** apply immediately — see procedure below |
| 3 | Baud Rate Code | RW | CONFIG | 0=115200, 1=9600. Same apply-on-reset caveat as Slave Address |
| 4–38 | Reserved | — | — | Read as 0, reject writes |
| 39 | Param Action | RW | COMMAND | 1=Save, 2=Load, 3=Factory Reset, 255=Software Reset — see §11.4 |

### Changing slave address or baud rate

```
1. FC06 write new value to offset 2 (Slave Address) and/or offset 3 (Baud Rate Code)
2. FC06 write 1 (Save) to offset 39 (Param Action)
3. The change is persisted immediately but the live Modbus interface keeps
   running at the OLD address/baud until the next reset or power cycle.
4. Reset (Param Action = 255, or power-cycle), then reconnect using the NEW
   address/baud.
```

Writing offset 2/3 without a subsequent Save is lost on reset — these two
registers by themselves only stage the value; `Param Action = Save` is what
actually persists it to NVS.

---

## 8. Extension Holding Block (FC03/FC06, offsets 680–759)

| Offset | Abs | Register | Access | Description |
|---:|---:|---|---|---|
| 0 | 680 | Calibration Unlock | RW | Two-step unlock — see below. Always reads back 0 |
| 1 | 681 | Calibration Exit | RW | Write 1 to exit Calibration mode and restore the operating mode that was active before entry |
| 2–79 | 682–759 | Reserved | R | Read as 0, reject writes |

**Calibration Unlock sequence** (a volatile guard, not cryptographic
authentication — it exists to prevent a stray/accidental write from putting
a board into Calibration mode):

```
1. FC06 write 0xCA1B to offset 680
2. FC06 write 0xA11B to offset 680
3. FC06 write 2 (Calibration) to System Holding offset 0 (Operating Mode)
```

Any wrong value at step 1 or 2 resets unlock progress to zero — you must
restart from step 1. Successfully entering Calibration mode (step 3) also
resets unlock progress, so re-entering after an exit requires unlocking
again. Calibration mode itself is volatile: it is never the mode restored
on the next boot, regardless of what was active when the board was
power-cycled while calibrating.

---

## 9. Channel Input Block (FC04, base `CH_BASE(ch)`, offsets 0–39)

| Offset | Register | Type | Poll | Guard | Description |
|---:|---|---|---|---|---|
| 0 | Status Bits | UINT16 | REALTIME | — | See §13.1 |
| 1 | Active Fault Cause | UINT16 | REALTIME | — | Fault bits currently blocking operation; see §13.2 |
| 2 | Fault History Cause | UINT16 | REALTIME | — | Fault bits observed since the last history clear |
| 3 | Last Protection Output Action | UINT16 | REALTIME | — | See §11.2 |
| 4 | Auto Retry Count | UINT16 | REALTIME | — | Retries counted inside the current sliding retry window |
| 5 | Auto Cooldown Remaining | UINT16 | REALTIME | — | Seconds until a retry is next allowed, 0 if not cooling down |
| 6–7 | Last Fault Timestamp HI/LO | INT32 | REALTIME | — | Board uptime (seconds) when the last fault was recorded |
| 8 | Operational Target Voltage | INT16 | REALTIME | — | Live target, ramps toward Configured Target Voltage; ×0.1 V |
| 9 | Capability Flags | UINT16 | FIXED | — | Read this before touching any guarded register on this channel — see §12.2 |
| 10 | Measured Voltage | INT16 | REALTIME | VOLTAGE_MEASUREMENT | Calibrated, ×0.1 V |
| 11 | Measured Current | INT16 | REALTIME | CURRENT_MEASUREMENT | Calibrated, ×0.1 nA |
| 12–13 | Raw ADC Voltage HI/LO | INT32 | REALTIME | VOLTAGE_MEASUREMENT | Uncalibrated ADC code — only meaningful in Calibration mode |
| 14–15 | Raw ADC Current HI/LO | INT32 | REALTIME | CURRENT_MEASUREMENT | Uncalibrated ADC code — only meaningful in Calibration mode |
| 16–39 | Reserved | — | — | — | Read as 0, reject writes |

## 10. Channel Holding Block (FC03/FC06, base `CH_BASE(ch)`, offsets 0–39)

### Commands (offsets 0–2)

| Offset | Register | Poll | Description |
|---:|---|---|---|
| 0 | Output Action | COMMAND | See §11.2 for valid values and host/protection context rules |
| 1 | Fault Command | COMMAND | 1=Clear Active Fault Block, 2=Clear Fault History — see §11.3 |
| 2 | Param Action | COMMAND | 1=Save, 2=Load, 3=Factory Reset for this channel only. **255 (Software Reset) is invalid here** — that's a system-block-only value; sending it at the channel level returns exception `0x03` |

### Operational configuration (offsets 3–16)

| Offset | Register | Guard | Description |
|---:|---|---|---|
| 3 | Configured Target Voltage | RAW_OUTPUT_DRIVE | ×0.1 V. What the channel ramps toward when enabled |
| 4 | Ramp Up Step | RAW_OUTPUT_DRIVE | ×0.1 V per interval |
| 5 | Ramp Up Interval | RAW_OUTPUT_DRIVE | ×0.1 s per step (deciseconds) |
| 6 | Ramp Down Step | RAW_OUTPUT_DRIVE | ×0.1 V per interval |
| 7 | Ramp Down Interval | RAW_OUTPUT_DRIVE | ×0.1 s per step |
| 8 | Recovery Policy Mode | — | 0=ManualLatch, 1=AutoRetry, 2=AutoDerateRetry, 3=NeverRetry — see §11.5 |
| 9 | Auto Retry Delay | — | Seconds (no ×10 — whole seconds), cooldown before a retry attempt |
| 10 | Auto Retry Max Count | — | Max retries allowed inside the sliding window before latching `RETRY_EXHAUST` |
| 11 | Auto Retry Window | — | Seconds (no ×10), sliding-window width for counting retries |
| 12 | Current Safe Band % | — | `UINT16` percent. Firmware does not range-clamp this field; 0–50 is the conventional/documented range (see `parameter-reference.md`) and values above 100 make the clear-eligibility formula (`threshold × (100−pct)/100`) go negative |
| 13 | Current Protection Mode | CURRENT_MEASUREMENT | 0=Disabled, 1=FlagOnly, 2=ApplyOutputAction — see §11.6 |
| 14 | Current Protection Output Action | CURRENT_MEASUREMENT | Same enum as offset 0, evaluated in **Protection** context (§11.2) |
| 15 | Current Limit Threshold | CURRENT_MEASUREMENT | ×0.1 nA, compared directly against Measured Current |
| 16 | Auto Derate Step | RAW_OUTPUT_DRIVE + VOLTAGE_MEASUREMENT | ×0.1 V subtracted from target per retry attempt under AutoDerateRetry |
| 17–19 | Reserved | — | Read as 0, reject writes |

### Calibration coefficients (offsets 20–25)

Readable in any mode. **Writable only in Calibration mode** — a write
attempt outside Calibration mode returns exception `0x03`.

| Offset | Register | Guard | Description |
|---:|---|---|---|
| 20 | Output Cal K | RAW_OUTPUT_DRIVE | `UINT16`, ÷10000 — see §9 formula. Default 32768 |
| 21 | Output Cal B | RAW_OUTPUT_DRIVE | `INT16`, raw offset in DAC-code units. Default 0 |
| 22 | Measured Voltage Cal K | VOLTAGE_MEASUREMENT | `UINT16`, ÷1000000 — see §9 formula |
| 23 | Measured Voltage Cal B | VOLTAGE_MEASUREMENT | `INT16`, raw offset in ×0.1 V units |
| 24 | Measured Current Cal K | CURRENT_MEASUREMENT | `UINT16`, ÷1000000 — see §9 formula |
| 25 | Measured Current Cal B | CURRENT_MEASUREMENT | `INT16`, raw offset in ×0.1 nA units |
| 26–29 | Reserved | — | Read as 0, reject writes |

**Calibration formula:** `calibrated = raw × K / D + B`, where `raw` is the
uncalibrated ADC code (measurement axes) or the target value being converted
to a DAC code (output axis), and `D` is **10000 for the output axis** or
**1000000 for both measurement axes**. The measurement axes need the finer
divisor because they scale down a small, attenuated raw ADC gain — unity
gain (1.0×) isn't representable there (max representable is 65535/1000000 ≈
0.0655); see [`parameter-reference.md`](parameter-reference.md) for the full
derivation. Full calibration walkthrough:
[`calibration-guide.md`](calibration-guide.md).

### Calibration session (offsets 30–33)

**Writable only in Calibration mode.**

| Offset | Register | Guard | Description |
|---:|---|---|---|
| 30 | Cal Output Enable | RAW_OUTPUT_DRIVE | 1=on, 0=off. Firmware allows only one channel board-wide to have this set at a time — enabling a second channel while another is still enabled is rejected |
| 31 | Cal DAC Code | RAW_OUTPUT_DRIVE | Raw DAC code, 0–65535 (or lower if a build-time ceiling is configured — that ceiling is **not** a Modbus register; it's compiled into firmware and isn't discoverable over the wire) |
| 32 | Cal Sample Command | VOLTAGE_MEASUREMENT or CURRENT_MEASUREMENT | Write 1 to trigger a fresh ADC sample on every measurement path this channel supports; reads back 0 once complete (synchronous, see §5) |
| 33 | Cal Commit Command | any cal-capable | Write 1 to persist this channel's calibration coefficients (offsets 20–25) to NVS |
| 34–39 | Reserved | — | Read as 0, reject writes — **note:** an earlier protocol draft placed a "Cal Max Raw DAC Limit" register at offset 34; it was never shipped as a Modbus register (the DAC ceiling is firmware-internal session state only) |

---

## 11. Command and mode enumerations

### 11.1 Operating Mode

| Value | Name |
|---:|---|
| 0 | Normal |
| 1 | Automatic |
| 2 | Calibration |

Behavioral differences between Normal and Automatic are in
[`operating-mode-guide.md`](operating-mode-guide.md); Calibration is
covered in [`calibration-guide.md`](calibration-guide.md).

### 11.2 Output Action

| Value | Name | Valid as Host command (channel offset 0) | Valid as Protection action (channel offset 14) |
|---:|---|:---:|:---:|
| 0 | None | yes | yes |
| 1 | Enable | yes | **no** |
| 2 | Disable Graceful | yes | yes |
| 3 | Disable Immediate | yes | yes |
| 4 | Force Output Zero | **no** | yes |

Writing a value invalid for its context (e.g. `Enable` at the protection
action offset 14, or `Force Output Zero` as a host command at offset 0)
returns exception `0x03`. This context split is enforced by firmware, not
just convention — do not assume both offsets accept the same value set.

### 11.3 Channel Fault Command

| Value | Name |
|---:|---|
| 0 | None (readback value after execution) |
| 1 | Clear Active Fault Block |
| 2 | Clear Fault History |

Clearing an Active Fault Block that's still unsafe (e.g. current still
above the safe band) is rejected with exception `0x03` rather than
silently no-oping.

### 11.4 Param Action

| Value | Name | Valid at |
|---:|---|---|
| 0 | None (readback value after execution) | system + channel |
| 1 | Save | system + channel |
| 2 | Load | system + channel |
| 3 | Factory Reset | system + channel |
| 255 | Software Reset | **system only** — invalid at channel offset 2, returns exception `0x03` |

Software Reset acknowledges the write (so the FC06 response is transmitted)
before the board reboots.

### 11.5 Recovery Policy Mode

| Value | Name |
|---:|---|
| 0 | Manual Latch |
| 1 | Auto Retry |
| 2 | Auto Derate Retry |
| 3 | Never Retry |

Only meaningful in Automatic operating mode; see
[`operating-mode-guide.md`](operating-mode-guide.md) §4.

### 11.6 Protection Mode

| Value | Name |
|---:|---|
| 0 | Disabled |
| 1 | Flag Only (records fault history, output untouched) |
| 2 | Apply Output Action (records fault history **and** disables output per offset 14) |

---

## 12. Capability flags

### 12.1 System Capability Flags (System Input offset 3)

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Automatic operating mode supported |
| 1 | 0x0002 | Environment sensor present (board temperature/humidity meaningful) |
| 2 | 0x0004 | Calibration mode supported |

### 12.2 Channel Capability Flags (Channel Input offset 9)

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Output Enable control present |
| 1 | 0x0002 | Raw output drive present (`RAW_OUTPUT_DRIVE` guard used above) |
| 2 | 0x0004 | Voltage measurement present (`VOLTAGE_MEASUREMENT` guard) |
| 3 | 0x0008 | Current measurement present (`CURRENT_MEASUREMENT` guard) |
| 4 | 0x0010 | Hardware status/fault evidence present |

**Any register tagged with a guard above returns exception `0x02` for both
reads and writes if the channel lacks that capability bit** — this is not a
"reads zero" fallback; treat it exactly like accessing an unsupported
channel. Always check capability flags once at connect time (per channel)
and cache the result rather than re-deriving it from exception codes at
runtime.

---

## 13. Status and fault bit fields

### 13.1 Channel Status Bits (Channel Input offset 0)

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | 0x0001 | Output drive level is nonzero |
| 1 | 0x0002 | Output enable is active |
| 2 | 0x0004 | Ramping is in progress |
| 3 | 0x0008 | Active Fault Block present (output may be forcibly disabled, depending on protection mode) |
| 4 | 0x0010 | Fault History present |
| 5 | 0x0020 | Automatic-mode retry cooldown active |
| 6 | 0x0040 | Measurement stale (no fresh sample since the last expected update) |

### 13.2 Fault Cause Bits (Channel Input offset 1/2, System Input offset 14)

| Bit | Mask | Meaning |
|---:|---:|---|
| 1 | 0x0002 | Current limit exceeded |
| 2 | 0x0004 | Measurement invalid |
| 3 | 0x0008 | Output hardware fault |
| 4 | 0x0010 | Variant safety interlock |
| 5 | 0x0020 | Automatic retry budget exhausted |
| 6 | 0x0040 | Configuration invalid for automatic start |
| 7 | 0x0080 | Measurement data stale |

Bit 0 (`0x0001`) is not currently assigned — earlier protocol drafts used it
for a voltage-limit fault; only current protection ships today. Multiple
bits can be set simultaneously (e.g. `0x0022` = current fault + retries
exhausted, the signature of an auto-retry policy giving up).

---

## 14. Variant profile — HVB (id=1)

| Field | Value |
|---|---|
| Variant ID | 1 |
| Physical channels | 2, fixed by hardware |
| Voltage scale | 0.1 V/LSB (100 mV/LSB) |
| Current scale | 0.1 nA/LSB |
| Default operating mode | Normal |
| Default recovery policy | Manual Latch |
| Default current safe band | 10% |
| Default calibration coefficients | Output K=32768 B=0; Voltage/Current K/B are board-specific, set during factory calibration — see [`calibration-guide.md`](calibration-guide.md) |

Other variant IDs (e.g. a future multi-channel LVB variant) will use the
same protocol structure with different `Variant ID`, `Supported Channels`,
and possibly different capability flags per channel — never hardcode
channel count or capability assumptions from this table; always derive them
from `Supported Channels`/`Active Channel Mask`/`Capability Flags` at
runtime (§5).

---

## 15. Exception codes

| Code | Name | Returned when |
|---:|---|---|
| 0x01 | Illegal Function | Function code other than 0x03/0x04/0x06 |
| 0x02 | Illegal Data Address | Reserved register, unsupported channel, or a capability-guarded register on a channel lacking that capability |
| 0x03 | Illegal Data Value | Out-of-range value, invalid enum, wrong-context Output Action, unsafe fault clear, calibration write outside Calibration mode, channel-level Software Reset |
| 0x04 | Slave Device Failure | Internal failure — e.g. NVS write failure on a Save/commit |

---

## 16. Worked byte-level examples

Slave address 1 throughout. CRC bytes shown are correct for the exact frame
given — recompute if you change any byte.

**Read the first 15 System Input registers (protocol version through fault
cause), FC04, start address 0, count 15:**

```
Request : 01 04 00 00 00 0F B0 0E
Response: 01 04 1E 00 03 00 00 00 01 00 07 00 02 00 03 00 F5 01 C2
          00 00 02 58 00 00 01 02 00 00 00 00 00 00 2B 7C
```
(Byte count `0x1E` = 30 = 15 registers × 2 bytes. Decoding a few: Protocol
Major=3, Minor=0, Variant ID=1, Capability Flags=0x0007, Supported
Channels=2, Active Channel Mask=0x0003, Board Temp=0x00F5=24.5°C, Board
Humidity=0x01C2=45.0%RH, Uptime=0x00000258=600s, FW Version=0x00000102.)

**Write System Holding offset 0 (Operating Mode) = 1 (Automatic), FC06:**

```
Request : 01 06 00 00 00 01 48 0A
Response: 01 06 00 00 00 01 48 0A   (FC06 echoes the request verbatim on success)
```

**Write Channel 0 Configured Target Voltage (holding offset 3, absolute
address 40+3=43=0x002B) to raw 5000 (= 500.0 V), FC06:**

```
Request : 01 06 00 2B 13 88 F4 94
Response: 01 06 00 2B 13 88 F4 94
```

**Attempted write to a reserved register (System Holding offset 4,
absolute address 4) — rejected:**

```
Request : 01 06 00 04 00 01 09 CB
Response: 01 86 02 C3 A1
```
(Function code `0x86` = `0x06 | 0x80` marks an exception response; `0x02`
is the exception code, Illegal Data Address.)

---

## 17. Building your own client — checklist

- [ ] Verify `Protocol Major` = 3 before trusting any offset in this
      document.
- [ ] Read `Supported Channels`/`Active Channel Mask` before assuming any
      channel exists; read each channel's `Capability Flags` before
      touching a guarded register on it.
- [ ] Treat exception `0x02` on a guarded register as "not supported here,"
      not as an error to retry.
- [ ] Don't poll `COMMAND`-category registers — they're write-only triggers
      and read back 0 immediately after a successful write (§5).
- [ ] Apply the correct scale per field: 0.1 V/LSB for voltage, 0.1 nA/LSB
      for current, the calibration-specific divisor for Cal K (§10), plain
      integer percent for safe band — don't assume one universal scale.
- [ ] Respect the Output Action host/protection context split (§11.2) — the
      same enum, two different valid subsets depending on which offset
      you're writing.
- [ ] Remember Slave Address / Baud Rate writes need an explicit `Param
      Action = Save` and a reset before they take effect (§7).

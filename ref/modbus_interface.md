# Modbus-RTU Interface — Firmware Design Reference
**High Voltage Power Supply — Multi-Channel (1 / 2 / 4)**
Synthesized from modbus_registers.pdf source via four AI-generated drafts.
Generated: 2026-06-05

---

## 1. Physical Layer

| Parameter                | Value                                                             |
|--------------------------|-------------------------------------------------------------------|
| Protocol                 | Modbus-RTU                                                        |
| Physical layer           | RS-485 half-duplex                                                |
| Data format              | 8N1 — 8 data bits, no parity, 1 stop bit                          |
| Default baud rate        | 115200 bps                                                        |
| Alternate baud rate      | 9600 bps                                                          |
| Default slave address    | 1 (configurable; 0 = broadcast)                                   |
| 32-bit word order        | Big-endian — high word at lower register address                  |
| RS-485 direction control | `RS485_DIR` GPIO: HIGH = transmit, LOW = receive                  |
| MCU UART                 | `USART6` — TX: `PG14`, RX: `PG9`                                  |
| RS-485 direction GPIO    | `RS485_DIR` on `PG12` (active-high assumed; confirm vs schematic) |

### Supported Function Codes

| FC | Name | Usage |
|----|------|-------|
| 0x03 | Read Holding Registers | Read configuration, calibration, and OTA registers |
| 0x04 | Read Input Registers | Read telemetry and status |
| 0x06 | Write Single Holding Register | Write single configuration parameter |
| 0x10 | Write Multiple Holding Registers | Write 32-bit value pairs, OTA packet blocks, grouped settings |

Coils (FC01/FC05/FC0F) and discrete inputs (FC02) are not defined in this protocol.
Return exception `0x01` (Illegal Function) if these are requested.

---

## 2. Conventions

### 2.1 Register Access Marks

| Mark | Meaning |
|------|---------|
| R | Readable via Modbus FC04 (input) or FC03 (holding) |
| W | Writable via Modbus FC06 / FC10 |
| RW | Readable and writable |

### 2.2 Data Types

| Type | Registers | Range |
|------|-----------|-------|
| UINT16 | 1 | 0 to 65 535 |
| INT16 | 1 | −32 768 to +32 767 |
| UINT32 | 2 | 0 to 4 294 967 295 |
| INT32 | 2 | −2 147 483 648 to +2 147 483 647 |

**32-bit registers** occupy two consecutive 16-bit Modbus registers.
High word is placed at the lower register address; low word at the higher address.
Both words must be written atomically using FC10; a single FC06 write to one word
leaves the 32-bit value in an undefined state.

Signed values use two's complement representation.

### 2.3 Scale Factor Convention

All physical values are stored as integers multiplied by a fixed scale factor:

```
register_value  = physical_value × scale_factor
physical_value  = register_value / scale_factor
```

| Scale | Meaning | Example |
|-------|---------|---------|
| 1 | Direct integer | Current in nA: 5 000 000 → 5 000 000 nA |
| 10 | One decimal place | Voltage: 12 345 → 1234.5 V |
| 1000 | Three decimal places | Calibration offset b |
| 10000 | Four decimal places | Calibration slope k |

### 2.4 Address Convention

All register addresses in this document are **zero-based Modbus PDU addresses**.
External tools that use the 4xxxx / 3xxxx notation must subtract the object-type base
before placing the address in the RTU PDU field.

---

## 3. Address Map

### 3.1 Address Space Layout

```
Reg addr 0x0000 ─────────────────────────────────────────────
                │  Master / System Block    (offsets 0–39)
Reg addr 0x0028 ─────────────────────────────────────────────
                │  Channel 0 Block          (offsets 40–79)
Reg addr 0x0050 ─────────────────────────────────────────────
                │  Channel 1 Block          (offsets 80–119)
Reg addr 0x0078 ─────────────────────────────────────────────
                │  Channel 2 Block          (offsets 120–159)  [protocol-defined]
Reg addr 0x00A0 ─────────────────────────────────────────────
                │  Channel 3 Block          (offsets 160–199)  [protocol-defined]
Reg addr 0x00C8 ─────────────────────────────────────────────
                │  OTA Upgrade Block        (offsets 200–270)
                └─────────────────────────────────────────────
```

Both the Input register space (FC04) and the Holding register space (FC03/FC06/FC10)
follow the same block layout.

### 3.2 Base Address Table

| Block | Input reg base | Holding reg base | Notes |
|-------|----------------|------------------|-------|
| Master / System | 0 | 0 | Board telemetry, comms settings, global commands |
| Channel 0 | 40 | 40 | First HV channel |
| Channel 1 | 80 | 80 | Second HV channel |
| Channel 2 | 120 | 120 | Protocol-defined; inactive on 2-channel hardware |
| Channel 3 | 160 | 160 | Protocol-defined; inactive on 2-channel hardware |
| OTA Upgrade | — | 200 | OTA packet buffer and control (holding only) |

Channel absolute address formula:

```
channel_base            = 40 + channel_index × 40
absolute_register_addr  = channel_base + local_offset
```

Example: Channel 1 target voltage (local offset 2) is at absolute address 82.

### 3.3 Active Channel Configuration

Active channels are determined at boot by sampling the `SYS_MOD[1:0]` DIP switch.
GPIO pins: `SYS_MOD0` on `PI5`, `SYS_MOD1` on `PI6`. Schematic note: DIP ON = logic 0.

| SYS_MOD1 | SYS_MOD0 | Active Channels | Channel bases active |
|----------|----------|-----------------|----------------------|
| 0 | 0 | Channel 0 only | 40 |
| 0 | 1 | Channel 0 + Channel 1 | 40, 80 |
| 1 | 0 | Reserved — do not use | — |
| 1 | 1 | Channels 0 through 3 | 40, 80, 120, 160 |

Firmware must reject register accesses to inactive channel blocks with exception
`0x02` (Illegal Data Address). Choose one policy (reject or silent ignore) and apply it
consistently across both input and holding register spaces.

---

## 4. Input Registers — FC04

### 4.1 Master Block (base = 0)

| Offset | Abs addr | Name              | Type   | Scale | Unit | Range   | Notes                                           |
|--------|----------|-------------------|--------|-------|------|---------|-------------------------------------------------|
| 0–5    | 0–5      | (reserved)        | —      | —     | —    | —       | Read as 0                                       |
| 6      | 6        | Temperature       | INT16  | 10    | °C   | 0–1000  | Board temp from SHT31; physical = reg/10        |
| 7      | 7        | Humidity          | UINT16 | 10    | %RH  | 0–1000  | Board RH from SHT31; physical = reg/10          |
| 8–9    | 8–9      | Timestamp         | UINT32 | 1     | s    | —       | Uptime since power-on; high word at offset 8    |
| 10     | 10       | FW Version (high) | UINT16 | —     | —    | —       | Firmware version word 0 (encoding TBD; see §12) |
| 11     | 11       | FW Version (low)  | UINT16 | —     | —    | —       | Firmware version word 1                         |
| 12     | 12       | OTA Status        | UINT16 | —     | —    | 0–4     | See §4.3                                        |
| 13     | 13       | OTA Error Code    | UINT16 | —     | —    | bitmask | See §4.3; bits OR together                      |
| 14     | 14       | OTA Packet Seq    | UINT16 | —     | —    | 0–65535 | Last acknowledged OTA packet index              |

### 4.2 Per-Channel Input Registers (ch_base = 40 + channel × 40)

| Offset | Name               | Type   | Scale | Unit | Range   | Notes                                                         |
|--------|--------------------|--------|-------|------|---------|---------------------------------------------------------------|
| 0–1    | Output Voltage     | INT32  | 10    | V    | 0–20000 | Current HV output; physical = reg/10 V; high word at offset 0 |
| 2–3    | Output Current     | INT32  | 1     | nA   | —       | Current HV output in nanoamperes; high word at offset 2       |
| 4      | Voltage Limit Flag | UINT16 | —     | —    | 0–1     | 0 = normal, 1 = voltage limit triggered (latched)             |
| 5      | Current Limit Flag | UINT16 | —     | —    | 0–1     | 0 = normal, 1 = current limit triggered (latched)             |
| 6      | Channel Status     | UINT16 | —     | —    | 0–1     | 0 = channel OFF, 1 = channel ON                               |

### 4.3 OTA Status and Error Enumerations

**OTA Status values (offsets 12 in master and per-channel blocks):**

| Value | Meaning                                                  |
|-------|----------------------------------------------------------|
| 0     | Idle — no upgrade in progress                            |
| 1     | Waiting — entered upgrade mode, ready to receive packets |
| 2     | In progress — receiving and flashing packets             |
| 3     | Success — upgrade complete, CRC verified                 |
| 4     | Failed — see OTA Error Code bitmask                      |

**OTA Error Code bitmask (offset 13):**

| Bit | Value | Meaning                                    |
|-----|-------|--------------------------------------------|
| 0   | 1     | Flash write or erase error                 |
| 1   | 2     | CRC verification failure                   |
| 2   | 4     | Invalid upgrade target selection           |
| 3   | 8     | Firmware file size exceeds flash limit     |
| 4   | 16    | Application failed to launch after upgrade |

Multiple bits may be set simultaneously (ORed).

---

## 5. Holding Registers — FC03 / FC06 / FC10

### 5.1 Master Block (base = 0)

| Offset | Abs addr | Name          | Access | Type   | Range     | Notes                                                                            |
|--------|----------|---------------|--------|--------|-----------|----------------------------------------------------------------------------------|
| 0–16   | 0–16     | (reserved)    | —      | —      | —         | Reject writes with exception 0x02; read as 0                                     |
| 17     | 17       | Slave Address | RW     | UINT16 | 0–247     | 0 = broadcast (no response); 1 = factory default; effective after save + restart |
| 18     | 18       | Baud Rate     | RW     | UINT16 | 0–1       | 0 = 115200 bps, 1 = 9600 bps; effective after save + restart                     |
| 19     | 19       | Param Action  | RW     | UINT16 | 1/2/3/255 | 1 = save, 2 = load, 3 = factory reset, 255 = SW reset                            |

### 5.2 Per-Channel Holding Registers — User Settings (ch_base = 40 + channel × 40)

| Offset | Name                | Access | Type   | Scale | Unit | Range     | Notes                                                                                  |
|--------|---------------------|--------|--------|-------|------|-----------|----------------------------------------------------------------------------------------|
| 0      | HW Switch           | RW     | UINT16 | -     | -    | -         | 0 = off, 1 = on;  **volatile** — always resets to 0 on power cycle; never saved to NVM |
| 1      | SW Switch           | RW     | UINT16 | —     | —    | 0–1       | 0 = off, 1 = on; **volatile** — always resets to 0 on power cycle; never saved to NVM  |
| 2      | Set Voltage         | RW     | UINT16 | 10    | V    | 0–20000   | Target output voltage; physical = reg/10 V; raw max 20000 = 2000.0 V; see §12          |
| 3      | Ramp-Up Step        | RW     | UINT16 | 10    | V    | 0–5000    | Voltage increment per ramp step; physical = reg/10 V                                   |
| 4      | Ramp-Up Interval    | RW     | UINT16 | 10    | s    | 0–600     | Wait time per ramp-up step; physical = reg/10 s                                        |
| 5      | Ramp-Down Step      | RW     | UINT16 | 10    | V    | 0–5000    | Voltage decrement per ramp step; physical = reg/10 V                                   |
| 6      | Ramp-Down Interval  | RW     | UINT16 | 10    | s    | 0–600     | Wait time per ramp-down step; physical = reg/10 s                                      |
| 7      | Voltage Limit Mode  | RW     | UINT16 | —     | —    | 0–5       | See §5.2.1                                                                             |
| 8–9    | Voltage Limit Value | RW     | UINT32 | 10    | V    | —         | Max allowed output voltage; high word at offset 8                                      |
| 10     | Current Limit Mode  | RW     | UINT16 | —     | —    | 0–5       | See §5.2.2                                                                             |
| 11–12  | Current Limit Value | RW     | UINT32 | 1     | nA   | —         | Max allowed output current; high word at offset 11                                     |
| 13–17  | (reserved)          | —      | —      | —     | —    | —         | Reject writes; read as 0                                                               |
| 18     | Save Voltage on NVM | RW     | UINT16 | —     | —    | 0–1       | 0 = store 0 as target when saving; 1 = store current Set Voltage                       |
| 19     | User Param Action   | RW     | UINT16 | —     | —    | 1/2/3/255 | 1 = save, 2 = load, 3 = factory reset, 255 = SW reset                                  |

#### 5.2.1 Voltage Limit Mode (offset 7) Behavior

| Value | Action when measured output voltage exceeds Voltage Limit Value            |
|-------|----------------------------------------------------------------------------|
| 0     | Disabled                                                                   |
| 1     | Clamp: set target voltage equal to Voltage Limit Value; continue operating |
| 2     | SW disable: clear SW Switch; ramp voltage down using ramp-down parameters  |
| 3     | SW disable: clear SW Switch; immediately set DAC to 0                      |
| 4     | HW disable: deassert HV_R/S GPIO; immediately set DAC to 0                 |
| 5     | Flag only: set Voltage Limit Flag; no automatic action                     |

#### 5.2.2 Current Limit Mode (offset 10) Behavior

| Value | Action when measured output current exceeds Current Limit Value           |
|-------|---------------------------------------------------------------------------|
| 0     | Disabled                                                                  |
| 1     | SW disable: clear SW Switch; ramp voltage down using ramp-down parameters |
| 2     | SW disable: same behavior as mode 1 (source duplicate; treat identically) |
| 3     | SW disable: clear SW Switch; immediately set DAC to 0                     |
| 4     | HW disable: deassert HV_R/S GPIO; immediately set DAC to 0                |
| 5     | Flag only: set Current Limit Flag; no automatic action                    |

### 5.3 Per-Channel Holding Registers — Debug / Calibration (same ch_base)

Calibration applies a linear correction to the raw signal value:

```
corrected = (raw × k / 10000) + (b / 1000)
```

| Offset | Name                   | Access | Type   | Scale | Range     | Notes                                                 |
|--------|------------------------|--------|--------|-------|-----------|-------------------------------------------------------|
| 20     | Output Voltage Cal k   | RW     | INT16  | 10000 | 1–65535   | DAC output slope; see §12 for type ambiguity          |
| 21     | Output Voltage Cal b   | RW     | INT16  | 1000  | 1–65535   | DAC output offset                                     |
| 22     | Measured Voltage Cal k | RW     | INT16  | 10000 | 1–65535   | ADC voltage slope                                     |
| 23     | Measured Voltage Cal b | RW     | INT16  | 1000  | 0–65535   | ADC voltage offset                                    |
| 24     | Measured Current Cal k | RW     | INT16  | 10000 | 1–65535   | ADC current slope                                     |
| 25     | Measured Current Cal b | RW     | INT16  | 1000  | 0–65535   | ADC current offset                                    |
| 26–38  | (reserved)             | —      | —      | —     | —         | Reject writes; read as 0                              |
| 39     | User Param Action      | RW     | UINT16 | —     | 1/2/3/255 | 1 = save, 2 = load, 3 = factory reset, 255 = SW reset |

Default factory calibration values: k = 10000 (representing 1.0000×), b = 0 (no offset).

### 5.4 OTA Upgrade Block (base = 200, holding registers only)

| Offset | Abs addr | Name                   | Access | Type       | Notes                                                          |
|--------|----------|------------------------|--------|------------|----------------------------------------------------------------|
| 0–63   | 200–263  | Upgrade Packet Data    | RW     | UINT16[64] | 64 registers = 128 bytes of firmware payload per packet        |
| 64     | 264      | Packet Sequence Number | RW     | UINT16     | Zero-based index of the packet being written                   |
| 65     | 265      | Upgrade Target Select  | RW     | UINT16     | 5 = upgrade channel sub-board; 6 = upgrade main board          |
| 66     | 266      | File CRC Checksum      | RW     | UINT16     | CRC16 of the complete firmware binary (algorithm TBD; see §12) |
| 67     | 267      | File Version (high)    | RW     | UINT16     | New firmware version word 0                                    |
| 68     | 268      | File Version (low)     | RW     | UINT16     | New firmware version word 1                                    |
| 69     | 269      | Upgrade Operation      | RW     | UINT16     | 254 = enter upgrade mode; 253 = clear flag and reboot          |
| 70     | 270      | Total Packet Count     | RW     | UINT16     | Total number of packets comprising the firmware image          |

---

## 6. Firmware Behavior Requirements

### 6.1 HV Channel Control Logic

**Enable sequence (recommended order):**
```
1. Write HW Switch (ch offset 0) = 1
   → asserts HV_R/S GPIO HIGH
2. Write Set Voltage (ch offset 2) = target_V × 10
   → stores target register value
3. Write SW Switch (ch offset 1) = 1
   → triggers ramp-up toward target voltage
```

**Graceful disable sequence:**
```
1. Write SW Switch (ch offset 1) = 0
2. Firmware ramps voltage down at ramp-down step / interval
3. HV_R/S is deasserted LOW when output voltage reaches 0
```

**Immediate disable sequence:**
```
1. Write HW Switch (ch offset 0) = 0
2. Firmware immediately sets DAC to 0 and deasserts HV_R/S LOW
```

**Effective output enable logic:**
```
output_active = hw_switch_is_on AND sw_switch_is_on AND NOT latched_fault
```

On a latched HW fault (limit mode 4), the firmware must override the SW Switch
state and drive HV_R/S to the safe (de-energized) state, regardless of register values.

### 6.2 Voltage Ramping

The firmware must implement a stepped voltage DAC controller.
When setting a new target voltage, the firmware must:
1. Calculate the difference: ΔV = |V_target - V_current|
2. Determine step direction (up or down)

Then,
**Ramp-up:**
```
each step:
    current_dac_target += RampUpStep / 10  (V)
    update DAC output
    wait RampUpInterval / 10  (seconds)
repeat until current_dac_target >= Set Voltage
```

Or,**Ramp-down:**
```
each step:
    current_dac_target -= RampDownStep / 10  (V)
    update DAC output
    wait RampDownInterval / 10  (seconds)
repeat until current_dac_target <= final_target (0 or new lower target)
```

If either step = 0 or interval = 0, apply instantaneous DAC change without inter-step delay.

### 6.3 Calibration Model

Three independent linear corrections must be applied. Use fixed-point arithmetic where
possible to avoid floating-point rounding divergence between runs.

```c
// Output path: set voltage (V) → corrected DAC voltage (V)
float k_out  = ch_holding[20] / 10000.0f;   // default: 1.0000
float b_out  = ch_holding[21] / 1000.0f;    // default: 0.000
float dac_v  = set_voltage * k_out + b_out;

// Measurement: ADC raw voltage → displayed voltage (V)
float k_mv   = ch_holding[22] / 10000.0f;
float b_mv   = ch_holding[23] / 1000.0f;
float disp_v = adc_raw_v * k_mv + b_mv;

// Measurement: ADC raw current → displayed current (nA)
float k_mi   = ch_holding[24] / 10000.0f;
float b_mi   = ch_holding[25] / 1000.0f;
float disp_i = adc_raw_i * k_mi + b_mi;
```

### 6.4 Limit Protection — Evaluation Order and Priority

On every ADC measurement cycle, evaluate protection limits in the following order.
**Current limit is evaluated first** because overcurrent is typically more safety-critical
than overvoltage in high-voltage supply applications.

```
Step 1 — Current limit:
    if (measured_I > Current_Limit_Value) AND (Current_Limit_Mode != 0):
        set Current_Limit_Flag = 1
        execute action defined by Current_Limit_Mode (§5.2.2)

Step 2 — Voltage limit:
    if (measured_V > Voltage_Limit_Value) AND (Voltage_Limit_Mode != 0):
        set Voltage_Limit_Flag = 1
        execute action defined by Voltage_Limit_Mode (§5.2.1)
```

Limit flags **latch**: once set, they remain set until explicitly cleared by the host
(write 0 to the flag input register — note: host cannot write input registers; the
flag clears only on host command via a defined mechanism, or on device restart).
Firmware must not auto-clear flags on recovery.

### 6.5 Parameter Persistence (NVM)

The `User Param Action` register exists at three locations:
- Master offset 19
- Channel user-settings offset 19
- Channel calibration offset 39

All three accept the same command set:

| Value | Action                                       |
|-------|----------------------------------------------|
| 1     | Save current RAM parameters to NVM (flash)   |
| 2     | Reload parameters from NVM into RAM          |
| 3     | Restore factory defaults into NVM and RAM    |
| 255   | Software reset (NVIC_SystemReset equivalent) |

**Volatility and persistence rules:**

| Parameter                                | Persisted?  | Notes                                                                |
|------------------------------------------|-------------|----------------------------------------------------------------------|
| SW Switch (ch offset 1)                  | Never       | Always initializes to 0 on power-up                                  |
| Set Voltage (ch offset 2)                | Conditional | Only if Save Voltage on NVM (ch offset 18) = 1; otherwise 0 is saved |
| Ramp settings (offsets 3–6)              | Yes         | Saved on param action = 1                                            |
| Limit mode and value (offsets 7–12)      | Yes         | Saved on param action = 1                                            |
| Calibration coefficients (offsets 20–25) | Yes         | Saved on param action = 1                                            |
| Slave address and baud rate              | Yes         | Effective only after save + restart                                  |
| Limit flags (input reg offsets 4–5)      | Never       | Runtime only                                                         |

Command registers should be **self-clearing**: firmware returns 0 on subsequent reads
after executing the command, rather than echoing the last written value. This prevents
Modbus hosts that retry writes from triggering the command a second time.

### 6.6 RS-485 Direction Control (Half-Duplex)

Correct half-duplex timing is critical to prevent bus contention:

1. Assert `RS485_DIR` HIGH before writing the first byte to the UART TX FIFO.
2. Begin UART transmission.
3. Wait for the **UART TX complete** interrupt/flag (not TX buffer empty — the final
   byte must fully propagate through the transmitter shift register and the MAX3485).
4. Deassert `RS485_DIR` LOW to re-enable receive mode.

Failing to wait for TX complete before releasing the bus will corrupt the last bytes
of the response frame on the line.

### 6.7 Slave Address and Baud Rate Changes

Changes to Slave Address and Baud Rate take effect **only after** a parameter
save (param action = 1) followed by a device restart (hardware reset or param action = 255).

Firmware must continue to respond on the previous address and baud rate until the
restart occurs. The host must reconfigure its port after restart.

Slave address 0 is the Modbus broadcast address: the device accepts and processes
broadcast writes but **must not** send a response frame.

### 6.8 Register Access Rules

- **Input Registers (0x04)** are updated live from ADC readings. No caching — read ADC on each request.
- **Holding Registers (0x03)** represent configuration. Changes take effect immediately upon write unless noted.
- Parameter save (reg 19=1) writes current holding register values to non-volatile storage (Flash/EEPROM).
- Parameter load (reg 19=2) reads from non-volatile storage back into RAM registers.
- Factory reset (reg 19=3) restores all holding registers to default values.

---

## 7. Register Write Validation

Firmware must validate every holding-register write before applying it.

| Check                                                | Action on failure                                                         |
|------------------------------------------------------|---------------------------------------------------------------------------|
| Address is within a defined block                    | Return exception `0x02` (Illegal Data Address)                            |
| Write attempt to read-only (input reg) area          | Return exception `0x02`                                                   |
| Write to reserved or undefined offset                | Return exception `0x02`                                                   |
| Value is within documented range                     | Return exception `0x03` (Illegal Data Value)                              |
| Command register value not in {1, 2, 3, 255}         | Return exception `0x03`                                                   |
| 32-bit register pair not written atomically via FC10 | Return exception `0x03` or process partial safely; document chosen policy |
| Channel is inactive per SYS_MOD switch               | Return exception `0x02`                                                   |
| OTA packet write while not in upgrade mode           | Return exception `0x03` or `0x04`                                         |

Most writes should update RAM immediately. Parameters are not persisted to NVM until
a param action = 1 command is executed.

### Modbus Exception Codes

| Code | Name                 | Condition                                               |
|------|----------------------|---------------------------------------------------------|
| 0x01 | Illegal Function     | Unsupported or unimplemented function code              |
| 0x02 | Illegal Data Address | Register address out of range or inactive channel       |
| 0x03 | Illegal Data Value   | Value outside allowed range; invalid enum               |
| 0x04 | Slave Device Failure | Internal error (e.g., NVM write failed, hardware fault) |

---

## 8. OTA Firmware Upgrade

### 8.1 Host Upgrade Sequence

```
Step 1  Write upgrade target select    → addr 265 = 6
Step 2  Write total packet count       → addr 270 = N
Step 3  Write file version             → addr 267 (high word), addr 268 (low word)
Step 4  Write file CRC checksum        → addr 266
Step 5  Enter upgrade mode             → addr 269 = 254
Step 6  Poll OTA Status via FC04       → addr 12, wait until value = 1 (waiting)

For each packet i = 0 to N-1:
  Step 7a  Write packet data  (FC10)   → addr 200–263  (64 registers = 128 bytes)
  Step 7b  Write packet seq   (FC06)   → addr 264 = i
  Step 7c  Poll OTA Packet Seq FC04    → addr 14, wait until value = i (firmware ACK)

Step 8  Poll OTA Status                → addr 12, wait until value = 3 (success) or 4 (failed)
Step 9a If success: clear and reboot   → addr 269 = 253
Step 9b If failed:  read error mask    → FC04 addr 13 bitmask indicates cause
```

Write the 64-register packet data window as a **single FC10 block** for efficiency
and to reduce the risk of partial writes. Firmware validates the packet sequence
number in step 7b and rejects out-of-order packets.

### 8.2 OTA State Machine

```
  IDLE ──[write 254 to addr 269]──► WAITING (status = 1)
         WAITING ──[first packet received]──► IN_PROGRESS (status = 2)
                    IN_PROGRESS ──[all N packets received, CRC OK]──► SUCCESS (status = 3)
                    IN_PROGRESS ──[error]────────────────────────────► FAILED  (status = 4)
  SUCCESS ──[write 253 to addr 269]──► [reboot → IDLE]
  FAILED  ──[write 253 to addr 269]──► [reboot → IDLE]
```

---

## 9. Suggested Firmware Data Model

### 9.1 Main Controller State Fields

| Field name        | C type      | Source register |
|-------------------|-------------|-----------------|
| `temperature_x10` | int16_t     | FC04 addr 6     |
| `humidity_x10`    | uint16_t    | FC04 addr 7     |
| `uptime_s`        | uint32_t    | FC04 addr 8–9   |
| `fw_version[2]`   | uint16_t[2] | FC04 addr 10–11 |
| `ota_status`      | uint16_t    | FC04 addr 12    |
| `ota_error_mask`  | uint16_t    | FC04 addr 13    |
| `ota_pkt_seq`     | uint16_t    | FC04 addr 14    |
| `slave_address`   | uint16_t    | FC03 addr 17    |
| `baud_rate_code`  | uint16_t    | FC03 addr 18    |

### 9.2 Per-Channel State Fields

| Field name          | C type   | Source register             |
|---------------------|----------|-----------------------------|
| `voltage_v_x10`     | int32_t  | FC04 ch offset 0–1          |
| `current_na`        | int32_t  | FC04 ch offset 2–3          |
| `v_limit_triggered` | uint16_t | FC04 ch offset 4            |
| `i_limit_triggered` | uint16_t | FC04 ch offset 5            |
| `channel_on`        | uint16_t | FC04 ch offset 6            |
| `temperature_x10`   | uint16_t | FC04 ch offset 7            |
| `sw_switch`         | uint16_t | FC03 ch offset 1 (volatile) |
| `set_voltage_x10`   | uint16_t | FC03 ch offset 2            |
| `ramp_up_step_x10`  | uint16_t | FC03 ch offset 3            |
| `ramp_up_time_x10`  | uint16_t | FC03 ch offset 4            |
| `ramp_dn_step_x10`  | uint16_t | FC03 ch offset 5            |
| `ramp_dn_time_x10`  | uint16_t | FC03 ch offset 6            |
| `v_limit_mode`      | uint16_t | FC03 ch offset 7            |
| `v_limit_val`       | uint32_t | FC03 ch offset 8–9          |
| `i_limit_mode`      | uint16_t | FC03 ch offset 10           |
| `i_limit_val`       | uint32_t | FC03 ch offset 11–12        |
| `save_voltage_flag` | uint16_t | FC03 ch offset 18           |
| `cal_v_out_k`       | int16_t  | FC03 ch offset 20           |
| `cal_v_out_b`       | int16_t  | FC03 ch offset 21           |
| `cal_v_meas_k`      | int16_t  | FC03 ch offset 22           |
| `cal_v_meas_b`      | int16_t  | FC03 ch offset 23           |
| `cal_i_meas_k`      | int16_t  | FC03 ch offset 24           |
| `cal_i_meas_b`      | int16_t  | FC03 ch offset 25           |

---

## 10. Implementation Checklist

| Task                                             | Notes                                                           |
|--------------------------------------------------|-----------------------------------------------------------------|
| Implement Modbus RTU framing                     | CRC16 poly 0xA001 (reflected), init 0xFFFF                      |
| Implement FC03, FC04, FC06, FC10                 | Minimum required set                                            |
| Return exception 0x01 for FC01, FC02, FC05, FC0F | These are not used                                              |
| Register dispatch table                          | Separate input and holding maps; do not mix spaces              |
| Per-channel base address resolver                | `base = 40 + ch_index × 40`                                     |
| Active channel gating at boot                    | Read SYS_MOD DIP once at startup; cache result                  |
| Reject inactive channel accesses                 | Return 0x02 for both input and holding spaces                   |
| Range validation on all writes                   | Especially command enums and limit values                       |
| 32-bit write atomicity                           | Require FC10 for all UINT32 / INT32 pairs                       |
| Scaled conversion helpers                        | Keep raw register integers in RAM; convert at boundaries        |
| NVM parameter manager                            | Implement save / load / factory-reset with defaults table       |
| SW Switch volatility                             | Initialize to 0 at boot; exclude from NVM save path             |
| Set Voltage conditional save                     | Check `save_voltage_flag` before persisting                     |
| Command register self-clearing                   | Read-back returns 0 after command execution                     |
| RS-485 direction timing                          | Assert before TX; deassert after TX-complete (not buf-empty)    |
| Baud / address reconfiguration                   | Apply new values only after save + restart                      |
| Limit flag latching                              | Set on threshold crossing; do not auto-clear                    |
| Current limit evaluated before voltage limit     | Evaluation order per §6.4                                       |
| OTA state machine                                | Deterministic states; validate seq, CRC, target before flashing |
| Calibration default values                       | k = 10000, b = 0 as factory defaults                            |

---

## 11. Register Quick Reference Card

### Input Registers (FC04)

```
Master block (base 0):
  0–5   (reserved)
  6     temperature        [INT16,  ÷10,  °C]        raw 0–1000
  7     humidity           [UINT16, ÷10,  %RH]       raw 0–1000
  8–9   timestamp          [UINT32, ×1,   s]          high word at 8
  10    fw_version_high    [UINT16]
  11    fw_version_low     [UINT16]
  12    ota_status         [UINT16] 0=idle,1=wait,2=prog,3=ok,4=fail
  13    ota_error_code     [UINT16] bitmask
  14    ota_pkt_seq        [UINT16]

Per-channel (base = 40 + N×40,  N = 0..3):
  +0–1  voltage            [INT32,  ÷10,  V]          high word at +0
  +2–3  current            [INT32,  ×1,   nA]         high word at +2
  +4    v_limit_flag       [UINT16] 0/1 (latched)
  +5    i_limit_flag       [UINT16] 0/1 (latched)
  +6    channel_status     [UINT16] 0=off, 1=on
  +7    temperature        [UINT16, ÷10,  °C]
```

### Holding Registers (FC03 / FC06 / FC10)

```
Master block (base 0):
  17    slave_addr         [UINT16] 0–247
  18    baud_rate          [UINT16] 0=115200, 1=9600
  19    param_action       [UINT16] 1/2/3/255

Per-channel user settings (base = 40 + N×40):
  +1    sw_switch          [UINT16] 0/1  *** volatile ***
  +2    set_voltage        [UINT16, ÷10, V]    raw 0–20000
  +3    ramp_up_step       [UINT16, ÷10, V]    raw 0–5000
  +4    ramp_up_interval   [UINT16, ÷10, s]    raw 0–600
  +5    ramp_dn_step       [UINT16, ÷10, V]    raw 0–5000
  +6    ramp_dn_interval   [UINT16, ÷10, s]    raw 0–600
  +7    v_limit_mode       [UINT16] 0–5
  +8–9  v_limit_val        [UINT32, ÷10, V]    high word at +8
  +10   i_limit_mode       [UINT16] 0–5
  +11–12 i_limit_val       [UINT32, ×1,  nA]   high word at +11
  +18   save_voltage       [UINT16] 0/1
  +19   param_action       [UINT16] 1/2/3/255

Per-channel calibration (same base):
  +20   cal_v_out_k        [INT16,  ÷10000]    default=10000
  +21   cal_v_out_b        [INT16,  ÷1000]     default=0
  +22   cal_v_meas_k       [INT16,  ÷10000]    default=10000
  +23   cal_v_meas_b       [INT16,  ÷1000]     default=0
  +24   cal_i_meas_k       [INT16,  ÷10000]    default=10000
  +25   cal_i_meas_b       [INT16,  ÷1000]     default=0
  +39   param_action       [UINT16] 1/2/3/255

OTA upgrade block (holding base 200):
  200–263  pkt_data        [UINT16×64]  128 bytes of firmware payload
  264      pkt_seq         [UINT16]
  265      target_sel      [UINT16]     6=main
  266      file_crc        [UINT16]
  267      file_ver_high   [UINT16]
  268      file_ver_low    [UINT16]
  269      ota_op          [UINT16]     254=enter, 253=exit+reboot
  270      total_pkts      [UINT16]
```

---

## 12. Open Issues — Design Decisions Pending

The following points are ambiguous in the source specification and must be confirmed
with the hardware/protocol owner before finalizing the firmware implementation.

| #  | Item                                     | Description                                                                                                                                                                                                   | Recommended resolution                                                                      |
|----|------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| 1  | Serial frame format                      | Parity and stop bits not specified in source. Document assumes 8N1 based on schematic context.                                                                                                                | Confirm 8N1; add to protocol table if correct                                               |
| 2  | `Set Voltage` register range             | Offset 2 typed UINT16, raw range 0–20000, physical range 0–2000.0 V. If the device supports voltages above 6553.5 V (UINT16 max / scale 10), the register must be widened to UINT32.                          | Confirm max output voltage; change to UINT32 if > 2000 V                                    |
| 3  | Calibration register type                | Offsets 20–25 declared INT16, but documented raw range extends to 65535, which exceeds INT16 positive max (32767). Likely intended as UINT16 fixed-point coefficients. Negative calibration slope is unusual. | Confirm whether negative k is needed; if not, use UINT16                                    |
| 4  | Firmware version encoding                | No encoding format specified for the 32-bit version field (offsets 10–11).                                                                                                                                    | Agree on a format (e.g., major.minor.patch.build packed as 4 bytes) and document            |
| 5  | OTA CRC algorithm                        | CRC polynomial, initialization value, and byte/word endianness inside the 16-bit registers are unspecified.                                                                                                   | Agree on algorithm (e.g., CRC16-CCITT, poly 0x1021, init 0xFFFF) and confirm with host tool |
| 6  | OTA packet payload byte order            | Within each UINT16 register, byte order (MSB-first or LSB-first) is unspecified.                                                                                                                              | Document and confirm — host tool and firmware must agree                                    |
| 7  | OTA target channel selection             | Target select = 5 means "channel upgrade" but which channel is not specified. Is it set via transparent mode (master offset 15), or is the channel index embedded in the target select value?                 | Clarify upgrade channel selection mechanism                                                 |
| 8  | `RS485_DIR` active polarity              | Schematic lists `RS485_DIR` on `PG12`; polarity (HIGH = TX or HIGH = RX) depends on MAX3485 DE/RE wiring.                                                                                                     | Verify against schematic; document the correct polarity in firmware                         |
| 9  | Inactive channel access policy           | Source allows two policies: reject with 0x02 or accept silently without action.                                                                                                                               | Choose one; document it; implement consistently                                             |
| 10 | Modbus inter-character and frame timeout | Standard Modbus RTU requires 3.5-character silence to delimit frames. Confirm the exact timeout values to use, especially at 115200 bps where 3.5 chars ≈ 304 µs.                                             | Use standard Modbus 3.5T rule; confirm acceptable tolerance                                 |

# Multi-Channel High Voltage Supply — Design Report
**For MCU Firmware Reference**
Generated: 2026-06-04

---

## 1. System Overview

The board is a two-channel, isolated high-voltage (HV) power supply with closed-loop voltage and current monitoring, controlled by an STM32F429BIT6 MCU via SPI/GPIO, with RS-485 external communication.

Each HV channel is fully galvanically isolated from the MCU domain via:
- Isolated DC-DC converter (UWF1212S-3WR3, 12V → ±12V isolated)
- Digital signal isolators (CMT8261N1)

The block diagram is as follows:
```
             [RS-485 Host]
                |
             MAX3485 (half-duplex)
                |
               UART
                |
  SWD ── STM32F429BIT6  (VCC_+3V3, 8MHz crystal)
                ├── SPI2 → CMT8261N1 (isolator) → AD5541 DAC → 10Hz LPF → DW-P202 HV Module (HV1)
                ├── SPI1 ← CMT8261N1 (isolator) ← ADS1232 ADC ← AD8220 V/I sense ← HV Output (HV1)
                ├── GPIO → CMT8261N1 → HV1_R/S, GAIN[0:1], A[0:1]
                │
                ├── SPI2 → CMT8261N1 (isolator) → AD5541 DAC → 10Hz LPF → DW-P202 HV Module (HV2)
                ├── SPI1 ← CMT8261N1 (isolator) ← ADS1232 ADC ← AD8220 V/I sense ← HV Output (HV2)
                ├── GPIO → CMT8261N1 → HV2_R/S, GAIN[0:1], A[0:1]
                │
                ├── I2C1 → SHT31A (Temp/Humidity, addr 0x44)
                └── GPIO ← SYS_MOD[0:1] (DIP switch, mode select)
```

---

## 2. Power Architecture

### 2.1 Main Board Power
- **Input**: 12V DC, 0.5A via J4 (XH-2A, 2-pin)
- **Reverse polarity**: D4 B540C-13-F Schottky diode (input protection)
- **Transient protection**: D5 SMBJ12CA TVS diode
- **Overcurrent**: F1 SMD1812P110TF (PTC resettable fuse)
- **VCC_12V**: Directly from input after protection
- **VCC_+3V3**: U28 MP1470GJ buck converter (12V→3.3V)
    - Inductor: L15 SWPA5020S4R7MT 4.7µH

| Rail     | Source                 | Voltage         | Purpose                                                               |
|----------|------------------------|-----------------|-----------------------------------------------------------------------|
| DC_12V   | External J4 (XH-2A)    | 12 V / 0.5 A    | System input; protected by fuse F1, TVS D5, reverse-polarity diode D4 |
| VCC_12V  | After L13 ferrite bead | 12 V (filtered) | Feeds all downstream converters                                       |
| VCC_+3V3 | U28 MP1470GJ buck      | 3.3 V           | MCU, level shifters (VDD1 side), RS-485, SHT31A                       |

### 2.2 Per-Channel Isolated Power (HV1 / HV2 identical)
All HV-side rails are **galvanically isolated** from the main 12V domain.

| Rail          | IC               | Type                      | Purpose                               |
|---------------|------------------|---------------------------|---------------------------------------|
| HV1_VCC_+12V  | U9 UWF1212S-3WR3 | Isolated DC-DC (±12V, 3W) | Isolated supply for HV-side circuitry |
| HV1_VCC_+6VA  | U10 GM1205ACPZ   | LDO (12V→6V)              | Op-amp supply (+rail)                 |
| HV1_VCC_+5VA  | U12 GM1205ACPZ   | LDO (12V→5V)              | DAC AVDD, ADC AVDD, Vref              |
| HV1_VCC_−2V5A | U11 LM27761DSGR  | Charge pump (5V→−2.5V)    | Op-amp supply (−rail)                 |
| HV1_VCC_+3V3  | U13 MP1470GJ     | Buck (12V→3.3V)           | ADC DVDD, isolator HV-side VDD        |

---

## 3. High Voltage Channel Architecture
HV1 and HV2 are near-identical.

### 3.1 HV Generator
- **Part**: DW-P202-0.5CS1 (M1 / M2 for each channel)
- **Type**: Compact programmable HV DC-DC module
- **Control pins**:
  - Pin 8 `R/S`: Run/Stop — digital GPIO from MCU (through isolator, LOW=Off, HIGH=On)
  - Pin 6 `Vadj`: 0-5V Analog voltage input — sets HV output level proportionally (driven by DAC chain, 0V=0Vout, 5V=2000Vout)
  - Pin 5 `GND`: Signal ground (HV module logic ground = GND1/GND2 isolated)
- **Output**: `Vout+` (pin 2) referenced to `HGND` (pin 1)

### 3.2 Output Voltage Measurement
Differential sense across HV output using a resistive divider:
- Series divider: R16 + R17 = 2× 200MΩ (SR2512, 1% 100ppm) on Vout+
- Series input resistors: R12–R15 = 4× 100KΩ (SR1206, 0.1% 10ppm)
- Instrumentation amp: **U7 / U20 AD8220** (`G = 1 + 49.9K/Rg`)
  - Rg not populated → gain depends on board configuration (unity by default)
  - Reference: HV_Vref_+5VA
  - Supply: +6VA and −2V5A
  - Output: `HV1_V` / `HV2_V` → ADC AINP2 (AINN2 connected to GND1/GND2)

### 3.3 Output Current Measurement
- Shunt: R26 = 200KΩ
- Instrumentation amp: **U8 / U21 AD8220** (same configuration as voltage amp)
  - Rg not populated → gain depends on board configuration (unity by default)
  - Reference: HV_Vref_+5VA
  - Supply: +6VA and −2V5A
  - Output: `HV1_I` / `HV2_I` → ADC AINP1 (AINN1 connected to GND1/GND2)

---

## 4. DAC Chain (per channel)

Each channel uses an `AD5541ARZ` DAC with a 5 V precision reference. The raw DAC output passes through an op-amp buffer/filter, then drives the HV generator module.

| Function          | HV1 | HV2 | Part            |
|-------------------|----:|----:|-----------------|
| DAC               |  U1 | U14 | `AD5541ARZ`     |
| DAC reference     |  U3 | U16 | `ADR4550BRZ`    |
| DAC buffer/filter |  U2 | U15 | `ADA4522-1ARMZ` |

### 4.1 DAC — AD5541ARZ
- **Resolution**: 16-bit
- **Interface**: SPI (write-only, 3-wire: `/CS`, `DIN`, `SCLK`)
- **Reference voltage**: 5V from ADR4550BRZ → full-scale output = 5V
- **LSB**: 5V / 65536 ≈ 76.3 µV
- **Supply**: HV1_VCC_+5VA

**SPI Transfer Protocol (AD5541):**
- 16-clock cycle write, MSB first
- `/CS` low for entire 16-bit frame
- No MISO (write-only)
- Output updates on rising edge of 16th SCLK (or `/CS` rising edge)

### 4.2 Voltage Reference — ADR4550BRZ
- **Output**: 5.000V precision reference
- **Used by**: DAC (U1/U14) AVDD/REF and ADC (U4/U17) REFP

### 4.3 10Hz Low-Pass Filter — ADA4522-1ARMZ
- **Type**: Active Sallen-Key or inverting integrator LPF
- **Cutoff**: 10Hz (designed to smooth DAC output, suppress switching noise)
- **Components**: R1=8.45KΩ, R2=13.3KΩ, R3=10KΩ, R4=100Ω; C1=C5=1µF
- **Output**: `HV1_DAC_Vadj` / `HV2_DAC_Vadj` → directly to HV module `Vadj` pin
- **Supply**: +6VA and −2V5A

---

## 5. ADC Chain (per channel)

### 5.1 ADC — ADS1232IPWR
- **Resolution**: 24-bit delta-sigma
- **Channels**: 2 differential (AINP1/AINN1 = current, AINP2/AINN2 = voltage)
- **Data rate**: 10 SPS (SPEED=0) or 80 SPS (SPEED=1) (pull up to 3.3V by default)
- **Programmable gain**: GAIN0, GAIN1 pins → GPIO from MCU
- **Channel select**: A0, A1 pins → GPIO from MCU
- **Reference**: HV_Vref_+5VA (REFP); REFN = GND1/GND2
- **Crystal**: Y1/Y2 SX3M4.9152A10F20TNN (4.9152MHz TCXO, SMD 3225)
- **Supply**: DVDD = HV_VCC_+3V3; AVDD = HV_VCC_+5VA


**ADS1232 GAIN setting:**
| GAIN1 | GAIN0 | PGA Gain |
|-------|-------|----------|
| 0     | 0     | 1        |
| 0     | 1     | 2        |
| 1     | 0     | 64       |
| 1     | 1     | 128      |

**ADS1232 Channel / A-pin mapping:**
| A1 | A0 | Active Input                      |
|----|----|-----------------------------------|
| 0  | 0  | AINP1/AINN1 (current sense)       |
| 0  | 1  | AINP2/AINN2 (voltage sense)       |
| 1  | 0  | TEMP (internal temperature diode) |
| 1  | 1  | —                                 |

---

## 6. Digital Isolation (per channel)

**IC: CMT8261N1** — 6-channel unidirectional digital isolator (two per HV channel)

### Channel 1 Isolator U5 (MCU → HV1 and HV1 → MCU):
| MCU-side signal       | Direction | HV1-side signal               |
|-----------------------|-----------|-------------------------------|
| MCU_HV1_R/S           | →         | HV1_R/S                       |
| MCU_HV1_DAC_SPI2_MOSI | →         | HV1_DAC_SPI2_MOSI             |
| MCU_HV1_DAC_SPI2_SCK  | →         | HV1_DAC_SPI2_SCK              |
| MCU_HV1_DAC_SPI2_CS   | →         | HV1_DAC_SPI2_CS               |
| MCU_HV1_ADC_GAIN0     | →         | HV1_ADC_GAIN0                 |
| MCU_HV1_ADC_SPI1_MISO | ←         | HV1_ADC_SPI1_MISO (DRDY/DOUT) |

### Channel 1 Isolator U6:
| MCU-side signal       | Direction | HV1-side signal           |
|-----------------------|-----------|---------------------------|
| MCU_HV1_ADC_SPI1_SCLK | →         | HV1_ADC_SPI1_SCLK         |
| MCU_HV1_ADC_SPI1_CS   | →         | HV1_ADC_SPI1_CS (= /PDWN) |
| MCU_HV1_ADC_GAIN1     | →         | HV1_ADC_GAIN1             |
| MCU_HV1_ADC_A0        | →         | HV1_ADC_A0                |
| MCU_HV1_ADC_A1        | →         | HV1_ADC_A1                |

Channel 2 uses U18/U19 with identical signal mapping (HV2 domain).

---

## 7. MCU and Digital Interfaces

- **Package**: LQFP208
- **Core**: ARM Cortex-M4F, up to 180MHz
- **Clock**: 8MHz external crystal (Y3, 3225 package) → internal PLL
- **Flash**: 2048 KB
- **Debug**: SWD via J2 (1.25mm 6-pin, SWDIO/SWCLK/USART1_TX/USART1_RX)
- **Boot**: BOOT0/BOOT1 via R78/R79 (1KΩ pull-down to GND)
- **Supply**: VCC_+3V3 with multiple 100nF + 10µF decoupling caps

| Function             | Signals                          |
|----------------------|----------------------------------|
| HV1 DAC write        | MCU_HV1_DAC_SPI2_CS/SCK/MOSI     |
| HV2 DAC write        | MCU_HV2_DAC_SPI2_CS/SCK/MOSI     |
| HV1 ADC read         | MCU_HV1_ADC_SPI1_CS/SCLK/MISO    |
| HV2 ADC read         | MCU_HV2_ADC_SPI1_CS/SCLK/MISO    |
| HV1 run/stop         | MCU_HV1_R/S                      |
| HV2 run/stop         | MCU_HV2_R/S                      |
| HV1 ADC gain         | MCU_HV1_ADC_GAIN0/1              |
| HV2 ADC gain         | MCU_HV2_ADC_GAIN0/1              |
| HV1 ADC channel sel  | MCU_HV1_ADC_A0/A1                |
| HV2 ADC channel sel  | MCU_HV2_ADC_A0/A1                |
| Temp/humidity        | I2C1_SDA / I2C1_SCL / I2C1_ALERT |
| Status LED           | SYS_RUN (via D3 + R80)           |
| System mode          | SYS_MOD0 / SYS_MOD1              |
| RS-485 (via MAX3485) | UART_RX / UART_TX                |
| RS-485 direction     | RS485_DIR                        |
| UART debug port      | USART1_RX / USART1_TX            |
| SWD debug port       | SWDIO / SWCLK                    |

> **Note**: Each HV channel has its own dedicated SPI signals (separate nets, separate CMT8261N1 isolators). HV1 and HV2 do NOT share any SPI bus lines.

---

## 8. RS-485 Interface

- **Transceiver**: MAX3485 (U29), half-duplex, 3.3V compatible
- **Direction control**: `RS485_DIR` GPIO (HIGH = transmit, LOW = receive)
- **Isolation transformer**: T1 SDCW3216-2-900TF (pulse transformer on bus side)
- **ESD protection**: D7/D8/D9 UDD32C05L01 TVS diodes on A/B bus lines
- **Overcurrent protection**: F2/F3 SMD1206P050TF (500mA PTC fuses)
- **Termination**: R89 = 120Ω
- **Bias resistors**: R88/R90 = 4.7KΩ
- **Connector**: J5 MX15EDGK-3.5-D3P (screw terminal, RS485-A, RS485-B, GND)
- **Aux connector**: J6 HEADER 5x2/SM (10-pin)

---

## 9. Temperature & Humidity Sensor

- **Part**: U30 SHT31A-DIS-B2.5kS
- **Interface**: I2C
- **I2C Address**: `0x44`
- **Alert pin**: I2C1_ALERT → MCU GPIO (interrupt-capable)
- **Pull-ups**: R96/R97 = 10KΩ (SDA/SCL), R98 = 10KΩ (ADDR)
- **Supply**: VCC_+3V3

---

## 10. System Mode Selection

Via BM1 DSHP02TSGER (2-position DIP switch), read by MCU at boot:

| SYS_MOD1 | SYS_MOD0 | Mode                |
|----------|----------|---------------------|
| 0        | 0        | 1-channel operation |
| 0        | 1        | 2-channel operation |
| 1        | 0        | Reserved            |
| 1        | 1        | 4-channel operation |

Switch logic: "ON" = 0 (active-low via pull-down R94/R95 = 10KΩ).

---

## 11. Firmware Design Notes

### 11.1 Startup Sequence
1. Read `SYS_MOD[1:0]` to determine active channel count
2. Initialize SPI1 (ADC), SPI2 (DAC), I2C1 (SHT31), USART1 (RS485)
3. Assert `HV1_R/S = 0` and `HV2_R/S = 0` (HV off)
4. Write DAC code = 0 to both channels (Vadj = 0V)
5. Initialize ADS1232 on both channels (set SPEED, GAIN, channel)
6. Enable HV channels as required by SYS_MOD

### 11.2 HV Voltage Set (per channel)
```
Target_Vadj [V] → DAC_code = round(Target_Vadj / 5.0 * 65535)
SPI2 write 16-bit code to AD5541 (assert CS low, 16 clocks, CS high)
DAC output settles through 10Hz LPF (≈100ms settling to 99%)
HV output follows Vadj per DW-P202 transfer function
```

### 11.3 HV Voltage & Current Read (per channel)
```
Set A1:A0 = 01 → select voltage channel (AINP2/AINN2 = HV_V)
Wait DRDY/DOUT goes LOW (indicates conversion complete)
Assert /PDWN (CS) low, clock out 24 bits on SCLK
Result is two's complement; convert to voltage:
  V_input = (ADC_code / 2^23) * Vref * (1/PGA_gain)
Apply resistor divider scale factor to get actual HV output

Set A1:A0 = 00 → select current channel (AINP1/AINN1 = HV_I)
Read similarly; apply shunt resistance (100mΩ) and amp gain
```

### 11.4 ADS1232 Timing Constraints
- DRDY assertion to SCLK first edge: no minimum specified, but start within 1 conversion period
- SCLK frequency: max 4MHz in serial mode
- Between readings: deassert /PDWN between conversions
- Speed pin: configure once at init (0 = 10SPS recommended for noise)

### 11.5 RS-485 Direction Control
```
TX:  RS485_DIR = HIGH → send UART byte(s) → RS485_DIR = LOW (after last stop bit)
RX:  RS485_DIR = LOW  → receive
Typical turnaround: assert DIR change after final stop bit + 1 bit-time guard
```

### 11.6 SHT31 Polling (I2C1, address 0x44)
- Use single-shot mode or periodic mode
- Monitor `I2C1_ALERT` interrupt for threshold violations
- Standard measurement sequence: send command 0x2C06 → wait 15ms → read 6 bytes

---

## 12. Component List (BOM Summary)

### 12.1 ICs

| Ref              | Part Number       | Description                      | Qty |
|------------------|-------------------|----------------------------------|-----|
| U27              | STM32F429BIT6     | ARM Cortex-M4, LQFP208           | 1   |
| U1, U14          | AD5541ARZ         | 16-bit SPI DAC, SOIC-8           | 2   |
| U2, U15          | ADA4522-1ARMZ     | Zero-drift op-amp (10Hz LPF)     | 2   |
| U3, U16          | ADR4550BRZ        | 5V precision voltage reference   | 2   |
| U4, U17          | ADS1232IPWR       | 24-bit delta-sigma ADC, TSSOP-24 | 2   |
| U5, U6, U18, U19 | CMT8261N1         | 6-ch digital isolator            | 4   |
| U7, U8, U20, U21 | AD8220            | Instrumentation amplifier        | 4   |
| U9, U22          | UWF1212S-3WR3     | Isolated DC-DC 12V→±12V 3W       | 2   |
| U10, U23         | GM1205ACPZ        | LDO regulator (+6V output)       | 2   |
| U11, U24         | LM27761DSGR       | Negative charge pump (−2.5V)     | 2   |
| U12, U25         | GM1205ACPZ        | LDO regulator (+5V output)       | 2   |
| U13, U26         | MP1470GJ          | 3A buck converter (12V→3.3V)     | 2   |
| U28              | MP1470GJ          | 3A buck converter (main 3.3V)    | 1   |
| U29              | MAX3485           | RS-485 transceiver, half-duplex  | 1   |
| U30              | SHT31A-DIS-B2.5kS | Temp+Humidity sensor, I2C        | 1   |
| M1, M2           | DW-P202-0.5CS1    | HV generator module              | 2   |

### 12.2 Inductors & Transformers

| Ref     | Part Number       | Value       | Description                               |
|---------|-------------------|-------------|-------------------------------------------|
| L3, L9  | PCAQ3225A-142S    | —           | Common-mode choke (isolated DC-DC input)  |
| L4, L10 | PCAQ3225A-142S    | —           | Common-mode choke (isolated DC-DC output) |
| L5, L11 | UPZ2012E331-2R5TF | 330µH       | EMI ferrite bead                          |
| L14     | UPZ2012E331-2R5TF | 330µH       | EMI ferrite bead (main 12V)               |
| L1, L7  | GZ1608D601TF      | 600Ω@100MHz | Ferrite bead (HV module power)            |
| L6, L12 | FTC303018D4R7MBCA | 4.7µH 3.4A  | Inductor (3.3V buck)                      |
| L13     | UPZ2012E331-2R5TF | —           | EMI bead (main VCC_12V input)             |
| L15     | SWPA5020S4R7MT    | 4.7µH 2.2A  | Inductor (main 3.3V buck)                 |
| T1      | SDCW3216-2-900TF  | —           | RS-485 pulse transformer                  |

### 12.3 Key Precision Resistors

| Ref                   | Value  | Tolerance/TCR | Package     | Function                   |
|-----------------------|--------|---------------|-------------|----------------------------|
| R16, R17, R54, R55    | 200MΩ  | 1% 100ppm     | SR2512      | HV output divider (top)    |
| R20, R58              | 499KΩ  | 0.1% 25ppm    | SR1206      | Instrumentation amp gain   |
| R12–R15, R50–R53      | 100KΩ  | 0.1% 10ppm    | SR1206      | HV divider series input    |
| R21, R27, R59, R65    | 100KΩ  | 0.1%          | SR0402      | In-amp reference resistors |
| R26, R64              | 200KΩ  | 0.1%          | SR1206      | In-amp reference resistors |
| RT0402BRD07100KL (×2) | 100mΩ  | —             | 0402 4-term | Current sense shunt        |
| R31, R69              | 60.4KΩ | 0.1%          | SR0402      | +6V LDO set resistor       |
| R37, R75              | 49.9KΩ | 0.1%          | SR0402      | +5V LDO set resistor       |

### 12.4 Oscillators & Crystals

| Ref    | Part Number         | Frequency | Description       |
|--------|---------------------|-----------|-------------------|
| Y3     | Crystal 3225        | 8MHz      | MCU system clock  |
| Y1, Y2 | SX3M4.9152A10F20TNN | 4.9152MHz | ADS1232 ADC clock |

### 12.5 Protection Components

| Ref | Part Number | Description |
|-----|-------------|-------------|
| D4 | B540C-13-F | Schottky (reverse polarity) |
| D5 | SMBJ12CA | TVS 12V bidirectional |
| D6 | LED | Status indicator |
| D1, D2 | UDD32C03L01 | Schottky, HV output protection |
| D7, D8, D9 | UDD32C05L01 | TVS, RS-485 ESD protection |
| F1 | SMD1812P110TF | PTC fuse, 1.1A main input |
| F2, F3 | SMD1206P050TF | PTC fuse, 500mA RS-485 |
| BM1 | DSHP02TSGER | 2-position DIP switch (SYS_MOD) |

### 12.6 Connectors

| Ref | Part Number | Description |
|-----|-------------|-------------|
| J4 | XH-2A | DC 12V power input (2-pin) |
| J5 | MX15EDGK-3.5-D3P | RS-485 terminal (3-pin screw) |
| J6 | HEADER 5×2/SM | RS-485 aux 10-pin header |
| J2 | 1.25-6P | SWD/debug connector |
| TP4, TP6 | 1440-0 | HV1 high-voltage output terminals |
| TP18, TP20 | 1440-0 | HV2 high-voltage output terminals |

---

## 13. Signal Naming Convention

All signals follow the pattern: `MCU_<CH>_<PERIPHERAL>_<SIGNAL>`

- `CH`: `HV1` or `HV2`
- `PERIPHERAL`: `DAC_SPI2`, `ADC_SPI1`
- `SIGNAL`: `CS`, `SCK`, `MOSI`, `MISO`, `GAIN0`, `GAIN1`, `A0`, `A1`, `R/S`

On the isolated HV side the `MCU_` prefix is dropped: `HV1_DAC_SPI2_CS`, etc.

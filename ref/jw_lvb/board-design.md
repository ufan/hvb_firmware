# JW LVB Hardware Design Note

Source: `schematic.pdf`

PDF metadata:

- Title: `ADC60010-22012.pdf`
- KiCad PDF, one A1 schematic sheet
- Created: 2025-01-14 20:40:13 CST
- Modified: 2025-03-27 13:07:21 CST
- Drawing number visible in the sheet: `BY/ADC60010-22012-YL-001A0`
- Product/model text visible in the title block: `ADC60010-22012`

Note: the PDF contains Chinese text, but the embedded PDF text layer encodes several Chinese strings incorrectly. The extraction below is based on readable vector labels plus visual inspection of rendered schematic crops. Where this note gives firmware-facing pin assignments, the current `boards/jianwei/jw_lvb/jw_lvb.dts` mapping is treated as authoritative.

## Board Function

JW LVB is a 10-channel isolated low-voltage board. Each channel accepts an AC input, creates an isolated local low-voltage supply, switches a relay output, and reports current and voltage feedback to the controller.

The controller is `U54 STM32F103ZET6`. It drives channel enable nets `EN1` through `EN10`, samples analog feedback nets `V1` through `V10` and `I1` through `I10`, and exposes service communication through an isolated RS485 interface.

The repeated channel labels are:

- Power rails: `+12V1` through `+12V10`, with related protected/decoupled rails `+12VD1` through `+12VD10`
- Local signal rails: channel-local `+5Vn` rails, plus a board-side `+5V`
- Logic rail: shared `+3.3V`
- Relay enables: `EN1` through `EN10`
- Analog feedback: `V1` through `V10` and `I1` through `I10`
- Outputs: `OUT1` through `OUT10`

## Power Architecture

### AC Input and Channel Supplies

Each channel is powered from its own AC input path. The line input includes an `MF73T-1 5/7` NTC inrush limiter before the channel AC/DC converter. The repeated AC/DC modules are `LD60-23B12R2`, one per channel:

- `U2`, `U6`, `U12`, `U16`, `U22`, `U26`, `U32`, `U36`, `U42`, `U46`

Each module creates a channel-local 12 V domain. The schematic names these domains with `+12Vn` and `+12VDn`. The `+12VDn` rails are the decoupled/protected local rails used around the relay driver, current sensor primary path, and channel-side analog circuitry. The channel supply has bulk and local capacitors, including 1 uF, 100 uF, 4.7 uF, 0.1 uF, and 1000 pF parts depending on the local subcircuit.

The channel 12 V rail is not a shared board-wide 12 V rail. The naming pattern and repeated AC/DC modules indicate that each channel has its own isolated supply island.

### Channel 5 V Rails

Most channel signal-conditioning blocks include `SPX1117M3-L-5-0/TR` regulators:

- `U3`, `U8`, `U13`, `U18`, `U23`, `U28`, `U33`, `U37`, `U43`, `U47`

These regulators derive local 5 V rails from the local channel 12 V domains. The schematic labels these as `+5V`, `+5V1`, `+5V2`, etc. These 5 V rails power channel-side sensing and signal-conditioning ICs such as `20A-ACS712`, `NSI1312D-DSPR`, and local analog stages.

### Board-Side 5 V and 3.3 V Rails

The board-side logic supply path uses:

- `U52 LD05-23B05R2`: isolated AC/DC 5 V supply
- `U51 SPX1117M3-L-3-3/TR`: 3.3 V LDO regulator

`U52` generates the board-side `+5V`, and `U51` regulates this down to the shared `+3.3V` logic rail. `+3.3V` powers `U54 STM32F103ZET6`, SWD pull/bias networks, RS485 logic-side pull/bias networks, and MCU-side analog/reference circuitry.

The MCU has many local `0.1U/50V` decoupling capacitors around its VDD/VSS pin groups. The crystal/reset/analog-reference area includes `C178`, `C181`, `C188`, `C189`, `C190`, `C191`, `C192`, and nearby parts.

## Galvanic Isolation Model

The board is built around several galvanic boundaries. These boundaries matter for firmware and test because a signal name can be repeated across domains while the electrical reference is different.

### Channel Power Isolation

Each channel starts from AC line/neutral and uses an `LD60-23B12R2` module to produce an isolated local 12 V domain. This separates each channel's low-voltage circuitry from the AC input side and from other channel domains.

### Voltage Measurement Isolation

Each channel uses one `NSI1312D-DSPR` isolated amplifier:

- `E1` through `E10`

The `NSI1312D-DSPR` crosses from the channel-side voltage measurement network to the MCU-side analog measurement domain. Its input side is referenced to the channel/local measured domain. Its output side is referenced to the MCU ADC domain.

### Current Measurement Isolation

Each channel uses one `20A-ACS712` Hall-effect current sensor:

- `U1`, `U5`, `U11`, `U15`, `U21`, `U25`, `U31`, `U35`, `U41`, `U44`

The load/current path passes through the ACS712 primary conductor pins. The analog output is produced magnetically by the Hall sensor and is isolated from the primary current conductor by the IC package construction.

### Relay Control Isolation

The MCU does not directly drive relay coils. Each `ENn` signal drives a `TLP291` optocoupler input through a 1 k resistor. The optocoupler output then drives an `SS8050LT1` transistor stage on the channel/relay side.

This keeps the MCU enable signal electrically separated from the local relay coil drive domain.

### RS485 Isolation

`U53 RSM3485CHT` is an isolated RS485 transceiver module. It crosses between the MCU logic domain and the external RS485 bus domain:

- MCU-side nets: `SCL_TXDB`, `SCL_RXDB`, `COM1_CTRL`
- Bus-side nets: `RS485_A`, `RS485_B`, `485G`

The module integrates signal isolation and isolated bus transceiver behavior. The bus side then passes through protection/filtering components before reaching the external RS485 lines.

## Channel Output and Relay Drive Path

Each channel has an MCU enable net `ENn`. The repeated relay drive chain is:

1. `STM32F103ZET6` GPIO drives `ENn`.
2. `ENn` passes through a 1 k input resistor, for example `R23` on channel 1.
3. The resistor drives a `TLP291` optocoupler LED input.
4. The optocoupler output switches a transistor driver, `SS8050LT1`.
5. The transistor energizes a `307H-1AH-F-C-DC12V` relay coil.
6. The relay contact switches the channel output path, labelled `OUTn`.
7. A `FR107` diode is placed across the relay coil path for flyback suppression.

Representative channel 1 labels:

- `EN1`
- `R23 1k`
- `U10 TLP291`
- `V7 SS8050LT1`
- `K2 307H-1AH-F-C-DC12V`
- `V8 FR107`
- `OUT1`

The relay-drive isolation means firmware should treat `ENn` as a logical request to energize the isolated output stage. The actual relay coil and contact circuit sit across the optocoupler boundary.

Firmware pin mapping note: the current `jw_lvb.dts` intentionally maps logical `EN1` to `PF14` and logical `EN3` to `PG1`. This preserves a validated CH1/CH3 PCB crossing workaround, so firmware-facing documentation should follow the DTS mapping rather than the raw schematic label placement.

## Current Measurement Path

The current measurement path is built around the `20A-ACS712` per channel.

### Primary Current Path

The ACS712 primary pins are labelled `IP+`, `IP+`, `IP-`, and `IP-`. In each repeated cell, the channel current path passes through these pins. This gives a galvanically isolated Hall measurement of AC or DC current through the channel.

### Sensor Supply and Filter

The ACS712 logic/output side uses:

- `VCC` powered from the channel/local 5 V rail
- `GND`
- `FILTER` pin with a capacitor to set output bandwidth/noise
- `VIOUT` as the analog current-sense output

The schematic shows local decoupling and filter capacitors near every ACS712, commonly `0.1U` and `1000P` values.

### Signal Conditioning

The current-sense analog output is routed into a repeated `RS8412XK` op-amp stage. The op amp blocks use resistor networks and `BAV99` clamp diodes before the signal reaches the MCU analog nets `I1` through `I10`.

For the representative channel shown around `U1`, ACS712 `VIOUT` passes through `R3 20K` into a node biased by `R2 20K` to `+5V`, with `C2 1000P` filtering. This equal-resistor network halves the current-dependent signal before the `RS8412XK` buffer. Using the ACS712-20A nominal sensitivity of 100 mV/A, the ADC-facing sensitivity is therefore approximately 50 mV/A before per-unit calibration.

The firmware-facing MCU pin map assigns these current nets:

| Net | MCU pin | GPIO/ADC-capable pin |
| --- | ---: | --- |
| `I7` | 19 | `PF7` |
| `I9` | 21 | `PF9` |
| `I10` | 22 | `PF10` |
| `I5` | 26 | `PC0` |
| `I8` | 29 | `PC3` |
| `I6` | 36 | `PA2` |
| `I3` | 40 | `PA4` |
| `I1` | 42 | `PA6` |
| `I2` | 44 | `PC4` |
| `I4` | 47 | `PB1` |

## Voltage Measurement Path

The voltage measurement path is separate from the ACS712 current path. It uses one `NSI1312D-DSPR` isolated amplifier per channel:

- `E1` through `E10`

### Channel-Side Input

The NSI1312 input side has pins named:

- `VDD1`
- `VIN`
- `SHDN`
- `GND1`

The surrounding schematic shows channel-side resistor/capacitor networks connected to the measured local voltage domain. These networks scale and filter the measured channel voltage before it enters `VIN`.

### MCU-Side Output

The NSI1312 output side has pins named:

- `VO+`
- `VO-`
- `VCC`
- `GND2`

This side is referenced to the low-voltage measurement domain used by the MCU ADC path. The output is differential, then routed through conditioning/filtering before reaching the MCU voltage feedback nets `V1` through `V10`.

The firmware-facing MCU pin map assigns these voltage nets:

| Net | MCU pin | GPIO/ADC-capable pin |
| --- | ---: | --- |
| `V7` | 18 | `PF6` |
| `V9` | 20 | `PF8` |
| `V5` | 27 | `PC1` |
| `V10` | 28 | `PC2` |
| `V8` | 35 | `PA1` |
| `V6` | 37 | `PA3` |
| `V3` | 41 | `PA5` |
| `V1` | 43 | `PA7` |
| `V2` | 45 | `PC5` |
| `V4` | 46 | `PB0` |

Firmware should treat voltage feedback as isolated-amplifier output data, not as a direct measurement of the channel high-side node. Calibration needs to include the divider/filter, NSI1312 transfer function, op-amp/filter path if present, and STM32 ADC scaling.

The representative voltage path around `E1 NSI1312D-DSPR` includes `R13 5.1K`, `R14 30K`, `R17 5.1K`, `C20 1000P`, differential output resistors `R16/R19 10K`, and the `RS8412XK` conversion/buffer stage. The schematic annotation near this stage marks the nominal scaling as `IN 12V` to `OUT 2.14V`. Firmware factory defaults should therefore be treated as a nominal full-path conversion, not as NSI1312 gain alone.

## Default Measurement Calibration Derivation

The firmware calibration equation is:

```text
calibrated = raw_adc * k * 10^k_exp + b
```

Voltage registers use a fixed 0.1 V/LSB unit. With the schematic's `12V -> 2.14V` measurement-path annotation and a 12-bit STM32 ADC referenced to approximately 3.3 V:

```text
ADC_LSB = 3.3V / 4096 = 0.8057mV/count
raw_at_12V = 2.14V / ADC_LSB = 2656.19 counts
desired_at_12V = 120 LSB
voltage_gain = 120 / 2656.19 = 0.045177 LSB/count
```

The nominal voltage default is therefore:

```text
VC_DEFAULT_MEASURED_V_CAL_K     = 45177
VC_DEFAULT_MEASURED_V_CAL_K_EXP = -6
```

Current registers use the board-specific 0.1 A/LSB unit. With ACS712-20A sensitivity of 100 mV/A and the equal 20K/20K network halving the signal:

```text
effective_sensitivity = 50mV/A
counts_per_amp = 50mV / 0.8057mV = 62.06 counts/A
desired_per_amp = 10 LSB/A
current_gain = 10 / 62.06 = 0.16113 LSB/count
```

The nominal current default is therefore:

```text
VC_DEFAULT_MEASURED_I_CAL_K     = 16113
VC_DEFAULT_MEASURED_I_CAL_K_EXP = -5
```

The DTS `calib-current-b` values are post-gain offsets. The schematic-nominal
zero-current value is about `-250`, from ACS712 `VCC/2` followed by the equal
20K/20K network. The committed per-channel defaults are live-board zero-load
offsets in the same post-gain unit, adjusted so the latest `jw_lvb` firmware
reports approximately zero current with no external load.

## Protection Design and Mechanisms

### AC Input Inrush Limiting

Each channel AC input line includes an `MF73T-1 5/7` NTC. This limits inrush current into the AC/DC module and local bulk capacitors during power-up.

### Relay Coil Flyback

Each relay coil has a `FR107` diode in the coil drive path. This gives the inductive current a path when the transistor switches off, limiting voltage stress on the `SS8050LT1` driver and the optocoupler output side.

### Analog Clamp Protection

The analog conditioning stages use many `BAV99` dual switching diodes. These appear around op-amp/ADC-facing analog nets and provide clamp paths for overvoltage or transient excursions before the MCU ADC pins.

### RS485 Bus Protection

The RS485 interface includes:

- `L1 485G`: common-mode choke/filter in the bus path
- `V41 SMC24`: TVS/ESD protection for the RS485 bus
- `R166 120`: bus termination
- `485G`: isolated bus-side reference

This protects and filters the external differential bus before it reaches `U53 RSM3485CHT`.

### Local Decoupling and Bulk Storage

Each isolated power island has local decoupling. The board uses a mix of 0.1 uF high-frequency bypass capacitors, 1000 pF analog filter capacitors, 1 uF/4.7 uF local capacitors, and 100 uF or larger bulk capacitors around converter outputs and regulators.

### MCU Boot, Reset, and Clock Support

The MCU support circuitry includes:

- `R161 10k` on `BOOT0`
- `Y1 X53F-8M` 8 MHz crystal/resonator
- `R163 1M` crystal feedback resistor
- `C182`, `C183` 22 pF crystal load capacitors
- Reset circuit labelled `REST`, including `NC3`, `R169`, `C190`, and local capacitors

The reset symbol is visually shown as a user/service reset point in the MCU support area.

## User and Service Interfaces

### RS485

The external communication interface is isolated RS485 through `U53 RSM3485CHT`.

MCU side:

- `PA9` pin 101: `SCL_TXDB`
- `PA10` pin 102: `SCL_RXDB`
- `PG3` pin 88: `COM1_CTRL`

Bus side:

- `RS485_A`
- `RS485_B`
- `485G`

The schematic also contains a Chinese annotation that decodes as an RS485 control switch label. The electrical control net is `COM1_CTRL`.

### SWD Debug

Connector `X1` is a 4-pin SWD service header:

| Header pin | Signal |
| ---: | --- |
| 1 | `GND` |
| 2 | `SWCLK` |
| 3 | `SWDIO` |
| 4 | `+3.3V` |

The MCU pin assignments are:

- `PA13` pin 105: `SWDIO`
- `PA14` pin 109: `SWCLK`

`R168` and `R170` are 10 k bias resistors in the SWD header area.

### Reset and Boot

The reset circuit is labelled `REST` and connects to `NRST`. `BOOT0` is pulled by `R161 10k`. These are service-oriented controls for programming, recovery, and production test.

### Button and LED Notes

The rendered schematic clearly shows the reset/service control area, but the PDF extraction did not expose distinct user `KEY`, `BUTTON`, or `LED` labels. Diodes are labelled with `V` designators in this schematic, so `Vn` references should not be interpreted as indicator LEDs without checking the original KiCad source or assembly drawing.

## Firmware-Relevant Hardware Summary

- MCU: `STM32F103ZET6`
- Channel enable outputs: `EN1` through `EN10`
- Voltage ADC feedback: `V1` through `V10`, produced through `NSI1312D-DSPR` isolated amplifier paths
- Current ADC feedback: `I1` through `I10`, produced through `20A-ACS712` Hall current sensor paths
- Communication: isolated RS485 using `SCL_TXDB`, `SCL_RXDB`, and `COM1_CTRL`
- Debug/programming: SWD on `PA13/PA14`
- Reset/boot: `NRST`/`REST`, `BOOT0`

## Extraction Limits

This document is a schematic-derived design note. It is suitable for firmware bring-up, hardware review, and calibration planning. It is not a replacement for the original KiCad design database.

Known limits:

- Chinese labels in the PDF text layer are partly mojibake.
- Some analog component values and exact channel-to-reference groupings should be verified against the KiCad source before production release.
- User LED/button assignments need confirmation from the original design files or board inspection.

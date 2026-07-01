# JW_HVB Expansion Port — Hardware Iteration Reference

Generated: 2026-07-01
Based on: `ref/pin_map.md`, `ref/board_design.md`, `boards/jianwei/jw_hvb/jw_hvb.dts`

---

## 1. Scope

Add an expansion port to the jw_hvb board hosting:

| Component | Model | Interface | Notes |
|---|---|---|---|
| Environmental sensor | BME280 | I2C (addr 0x76/0x77) | Temperature, humidity, pressure |
| OLED display | Waveshare SSD1331 | SPI (write-only) | 96×64 RGB OLED, 262K color |
| Rotary encoder | generic quadrature | 2× GPIO + 1× SW | With push-button |
| Buttons | 4× tactile | GPIO inputs | User input |

---

## 2. Power Analysis

### 2.1 Existing Rail: VCC_+3V3 (U28 MP1470GJ, 3A buck)

| Consumer | Typ (mA) | Max (mA) |
|---|---|---|
| STM32F429BIT6 @168 MHz | 100 | 200 |
| SHT31A (I2C1, addr 0x44) | 0.6 | 3.4 |
| MAX3485 RS-485 transceiver | 1 | 2 |
| CMT8261N1 isolators (4×, MCU side) | 20 | 40 |
| Pull-ups, LED, misc | 5 | 15 |
| **Existing subtotal** | **~127** | **~260** |
| | | |
| BME280 (measuring) | 0.5 | 3.4 |
| SSD1331 OLED (typ brightness) | 25 | 50 |
| Rotary encoder pull-ups | <0.5 | <1 |
| 4× button pull-ups | <1 | <2 |
| **New subtotal** | **~27** | **~56** |
| | | |
| **Grand total** | **~154** | **~317** |

### 2.2 Voltage Compatibility

| Component | Vdd range | 3.3 V compatible? |
|---|---|---|
| BME280 | 1.71–3.6 V | Yes |
| SSD1331 module (Waveshare) | 3.3 V / 5 V | Yes (3.3 V version) |
| Rotary encoder | passive | Yes |
| Tactile buttons | passive | Yes |

**Verdict**: No modification to power architecture needed. VCC_+3V3 has ~2.7 A headroom. All devices operate at 3.3 V logic levels.

---

## 3. Currently Consumed Pins

Extracted from `boards/jianwei/jw_hwb/jw_hvb.dts` and `ref/pin_map.md`.

```
PA04  SPI1_NSS   (HV2 DAC CS)
PA05  SPI1_SCK   (HV2 DAC SCLK)
PA07  SPI1_MOSI  (HV2 DAC MOSI)
PA13  SWDIO      (debug)
PA14  SWCLK      (debug)

PB02  BOOT1      (boot strap)
PB08  I2C1_SCL   (SHT31)
PB09  I2C1_SDA   (SHT31)
PB12  SPI2_NSS   (HV1 DAC CS)
PB13  SPI2_SCK   (HV1 DAC SCLK)
PB15  SPI2_MOSI  (HV1 DAC MOSI)

PC04  HV2_R/S    (HV2 run/stop)
PC10  USART3_TX  (console)
PC11  USART3_RX  (console)

PD09  HV1_R/S    (HV1 run/stop)

PE00  I2C1_ALERT (SHT31 alert)
PE07  HV1_ADC_A1
PE08  HV1_ADC_A0
PE10  HV1_ADC_GAIN1
PE11  HV1_ADC_CS  (/PDWN)
PE12  HV1_ADC_SCLK
PE13  HV1_ADC_MISO (DRDY)

PF03  HV2_ADC_A0
PF04  HV2_ADC_GAIN1
PF06  HV2_ADC_CS  (/PDWN)
PF07  HV2_ADC_SCLK
PF08  HV2_ADC_MISO (DRDY)
PF10  HV2_ADC_GAIN0

PG09  USART6_RX  (RS-485)
PG11  RS485_DIR  (RS-485 DE/RE)
PG14  USART6_TX  (RS-485)

PH12  HV1_ADC_GAIN0

PI05  SYS_MOD0   (DIP switch)
PI07  SYS_MOD1   (DIP switch)
PI12  SYS_RUN    (status LED)
PI14  HV2_ADC_A1

BOOT0 boot strap
NRST  reset
```

**Total: 36 MCU pins consumed** (excluding VDD/VSS/VCAP).

---

## 4. Available Pins

GPIOA–GPIOI are enabled in board DTS (status = "okay"). GPIOJ/GPIOK exist in the SoC but are **not enabled** in the board DTS — they may not be broken out on the PCB. Pin counts below exclude used pins listed in Section 3.

| Port | Free pins | Count |
|---|---|---|
| GPIOA | PA0,PA1,PA2,PA3,PA6,PA8,PA9,PA10,PA11,PA12,PA15 | 11 |
| GPIOB | PB0,PB1,PB3,PB4,PB5,PB6,PB7,PB10,PB11,PB14 | 10 |
| GPIOC | PC0,PC1,PC2,PC3,PC5,PC6,PC7,PC8,PC9,PC12,PC13,PC14,PC15 | 13 |
| GPIOD | PD0,PD1,PD2,PD3,PD4,PD5,PD6,PD7,PD8,PD10,PD11,PD12,PD13,PD14,PD15 | 15 |
| GPIOE | PE1,PE2,PE3,PE4,PE5,PE6,PE9,PE14,PE15 | 9 |
| GPIOF | PF0,PF1,PF2,PF5,PF9,PF11,PF12,PF13,PF14,PF15 | 10 |
| GPIOG | PG0,PG1,PG2,PG3,PG4,PG5,PG6,PG7,PG8,PG10,PG12,PG13,PG15 | 13 |
| GPIOH | PH0,PH1,PH2,PH3,PH4,PH5,PH6,PH7,PH8,PH9,PH10,PH11,PH13,PH14,PH15 | 15 |
| GPIOI | PI0,PI1,PI2,PI3,PI4,PI6,PI8,PI9,PI10,PI11 | 10 |
| **Total** | | **~104** |

> **Note**: Not all STM32F429BI LQFP208 pins may be physically routed to headers on the jw_hvb PCB. Verify with board layout before finalizing expansion connector pinout.

---

## 5. Solutions Comparison

### 5.1 BME280 — Interface Options

#### Option A: Share I2C1 (0 extra pins) ★ Recommended

| Net | MCU pin | Bus | Shared with |
|---|---|---|---|
| I2C1_SCL | PB8 | I2C1 | SHT31 (0x44) |
| I2C1_SDA | PB9 | I2C1 | SHT31 (0x44) |

- **Pros**: Zero additional pins. Already-enabled bus. No DT change to I2C1 controller node. BME280 addr `0x76`/`0x77` does not collide with SHT31 at `0x44`. Zephyr I2C API serializes transactions per bus — no race between sensor reads. Kconfig: just add `CONFIG_BME280=y`.
- **Cons**: Both sensors share I2C1 bandwidth. SHT31 single-shot takes ~15 ms; BME280 forced mode takes ~8 ms. Combined worst-case poll latency ~23 ms — negligible for environmental sensing at 1 Hz. Bus fault from one device can stall the other (I2C has no reset mechanism on STM32F4 v1 peripheral).
- **Verdict**: Best choice. Adds zero pins. Bandwidth is not a concern for <10 Hz sensor polling.

#### Option B: Dedicated I2C2 (2 extra pins)

| Net | MCU pin | Bus |
|---|---|---|
| I2C2_SCL | PB10 | I2C2 (AF4) |
| I2C2_SDA | PB11 | I2C2 (AF4) |

- **Pros**: Electrical isolation from SHT31. No shared bandwidth. Independent bus recovery.
- **Cons**: 2 extra pins consumed. PB10/PB11 would displace rotary encoder SW and BTN1 from the recommended PB cluster. I2C2 must be enabled in DT. Zephyr `stm32-i2c-v1` handles multiple instances fine.
- **Verdict**: Overkill for two low-data-rate sensors. Only justified if galvanic separation or redundant sensor paths are required.

#### Option C: SPI (4 extra pins)

| Net | MCU pin | Bus |
|---|---|---|
| BME_CS | any GPIO | — |
| SPIx_SCK | SPI pin | SPIx |
| SPIx_MOSI | SPI pin | SPIx |
| SPIx_MISO | SPI pin | SPIx |

- **Pros**: Higher throughput (10 MHz vs 400 kHz I2C). No address collision risk.
- **Cons**: 4 extra pins. Must share or dedicate an SPI bus (all 3 free SPIs have pin conflicts — see §5.2). BME280 throughput needs are trivial (~24 bytes per read). Overcomplicates the design.
- **Verdict**: Not justified. I2C is the natural interface for BME280 in this design.

| Criterion | A: Shared I2C1 | B: Dedicated I2C2 | C: SPI |
|---|---|---|---|
| Pins | **0** | 2 | 4 |
| Firmware complexity | Low | Low | Medium |
| Bus isolation | Shared with SHT31 | Isolated | Isolated |
| Bandwidth risk | Negligible | None | None |
| PCB trace impact | None (existing) | 2 new traces | 4 new traces |

---

### 5.2 SSD1331 OLED — Interface Options

#### Option A: SPI3 on PB3/PB5 (5 pins) ★ Recommended

| Signal | MCU pin | AF |
|---|---|---|
| SCLK | **PB3** | SPI3_SCK (AF6) |
| MOSI | **PB5** | SPI3_MOSI (AF6) |
| CS | **PB0** | GPIO |
| DC | **PB1** | GPIO |
| RST | **PB4** | GPIO |

- **Pros**: Dedicated SPI bus — no timing sharing with DACs or ADCs. All 5 pins cluster on GPIOB (PB0–PB5) for clean routing. SPI3 is fully free (not used by any existing device). Follows `cs-gpios` pattern already used for AD5541 DACs on SPI1/SPI2. 45 Mbit/s capable (STM32F4 SPI), far exceeding SSD1331 max (~10 MHz).
- **Cons**: Consumes 5 pins. SPI3 consumes an APB1 clock gate (minor power).
- **Verdict**: SPI3 is the only SPI peripheral with both SCK and MOSI pins fully free. Best choice.

#### Option B: Bit-bang GPIO (5 pins, any port)

| Signal | MCU pin | Mode |
|---|---|---|
| SCLK | Any free GPIO | GPIO output |
| MOSI | Any free GPIO | GPIO output |
| CS | Any free GPIO | GPIO output |
| DC | Any free GPIO | GPIO output |
| RST | Any free GPIO | GPIO output |

- **Pros**: Total pin placement freedom — pick any 5 free GPIOs on any port. Follows the ADS1232 bit-bang pattern already established in this design. No hardware SPI peripheral consumed.
- **Cons**: Lower throughput — GPIO bit-bang peaks at ~2–4 MHz vs SPI hardware at 45 Mbit/s. SSD1331 needs ~96×64×16 bits = 12.3 KB per full-screen refresh. At 2 MHz GPIO, that's ~50 ms per refresh — still acceptable for a UI panel. CPU spin-waits during transfer (no DMA); for a 50 ms transfer once per second this is negligible.
- **Verdict**: Viable fallback if SPI3 pins are unavailable on PCB. Performance difference is not user-visible for a status display.

#### Option C: Share SPI1 or SPI2 with existing DAC (3 extra pins)

| Net | MCU pin | Shared bus |
|---|---|---|
| OLED_CS | GPIO | — |
| OLED_DC | GPIO | — |
| OLED_RST | GPIO | — |
| SCLK | PA5 or PB13 | SPI1 (HV2 DAC) or SPI2 (HV1 DAC) |
| MOSI | PA7 or PB15 | SPI1 (HV2 DAC) or SPI2 (HV1 DAC) |

- **Pros**: Saves 2 pins (SCLK, MOSI reused). No new SPI peripheral needed.
- **Cons**: HV DAC timing is safety-critical — an OLED frame write (~12 KB) could stall a DAC update for ~50 ms. The 10 Hz LPF after DAC smooths this, but it introduces coupling between UI and HV control paths. Requires `cs-gpios` array management. Architecturally unclean: mixing display and safety-critical HV control on same SPI bus.
- **Verdict**: Not recommended. Safety-critical HV DAC timing should not share a bus with a display.

#### Option D: SPI6 (5 pins)

| Signal | MCU pin | AF |
|---|---|---|
| SCLK | PG13 | SPI6_SCK (AF5) |
| MOSI | PG14 | SPI6_MOSI (AF5) — **conflict** |

- **Cons**: PG14 is already USART6_TX (RS-485 Modbus). Cannot be remapped without breaking Modbus. Non-starter.
- **Verdict**: Rejected.

| Criterion | A: SPI3 PB3/PB5 | B: Bit-bang GPIO | C: Share SPI1/2 | D: SPI6 |
|---|---|---|---|---|
| Pins | 5 | 5 | 3 | 5 |
| Max throughput | 45 Mbit/s | ~2-4 Mbit/s | 45 Mbit/s | 45 Mbit/s |
| Bus contention | None | None | **Shares with DAC** | None |
| Safety isolation | Clean | Clean | **Risky** | N/A |
| Firmware complexity | Medium (SPI3 init) | Low (GPIO toggle) | Medium (CS sharing) | — |
| Pin cluster | PB0-PB5 (same port) | Any port | Scattered | Scattered |
| DMA capable | Yes | No | Yes | — |
| Conflict with existing | None | None | None if SPI2 CS timing OK | PG14 = USART6 |

---

### 5.3 Rotary Encoder — Interface Options

#### Option A: 2 GPIO + Interrupt (3 pins) ★ Recommended

| Signal | MCU pin | Mode |
|---|---|---|
| ENC_A | **PB6** | GPIO input, interrupt on both edges |
| ENC_B | **PB7** | GPIO input |
| ENC_SW | **PB10** | GPIO input, interrupt on falling edge |

- **Pros**: Simple. Any 3 free GPIOs work. Software state machine decodes quadrature — typical implementation is ~30 lines of C. Interrupt on ENC_A edge reads ENC_B level to determine direction. No hardware timer consumed.
- **Cons**: ISR overhead per encoder tick. At 24 PPR detents × 4 edges = 96 interrupts per revolution. Human knob turning max ~5 rev/s → ~480 interrupts/s — negligible on 168 MHz Cortex-M4F. Debounce needed (hardware RC or software timer).
- **Verdict**: Good enough. Interrupt rate is trivial. Software debounce is simple.

#### Option B: TIM4 QDEC Mode (2 pins, hardware decode)

| Signal | MCU pin | AF |
|---|---|---|
| ENC_A | **PB6** | TIM4_CH1 (AF2) |
| ENC_B | **PB7** | TIM4_CH2 (AF2) |
| ENC_SW | **PB10** | GPIO input |

- **Pros**: Hardware quadrature decode — no ISR per count edge. TIM4 counter directly holds position. Zephyr has `st,stm32-qdec` binding on TIM4. Filter/debounce in hardware (`st,input-filter-level`). Zero CPU overhead for counting.
- **Cons**: Consumes TIM4 peripheral (TIM4 is free on jw_hvb). PB6/PB7 must be mapped to AF2, tying these specific pins. If PB6/PB7 are unavailable on PCB, must use PD12/PD13 as TIM4 CH1/CH2 alternative. Zephyr QDEC driver must be enabled (`CONFIG_QDEC=y`, `CONFIG_QDEC_STM32=y`).
- **Verdict**: Preferred if PB6/PB7 are confirmed routed. Lower power (no ISR wake), cleaner code. GPIO fallback if QDEC driver has issues.

| Criterion | A: GPIO + ISR | B: TIM4 QDEC |
|---|---|---|
| Pins | 3 | 3 |
| CPU overhead | ~480 ISR/s | 0 |
| Hardware cost | None | Consumes TIM4 |
| Debounce | Software | Hardware filter |
| Pin flexibility | Any GPIOs | Must be PB6/PB7 or PD12/PD13 |
| Firmware complexity | Medium (state machine) | Low (counter read) |
| Zephyr support | Manual | `st,stm32-qdec` driver |

---

### 5.4 Four Buttons — Interface Options

#### Option A: 4 Individual GPIOs (4 pins) ★ Recommended

| Button | MCU pin |
|---|---|
| BTN1 | **PB11** |
| BTN2 | **PB14** |
| BTN3 | **PC0** |
| BTN4 | **PC1** |

- **Pros**: Simple. Each button independently interrupt-capable (wake from sleep). No analog noise concerns. Zephyr `gpio-keys` binding directly supports this pattern. Independent debounce per pin.
- **Cons**: 4 pins consumed.
- **Verdict**: With ~104 free pins, 4 GPIOs is not a scarce resource. Best choice.

#### Option B: ADC Resistor Ladder (1 pin)

| Signal | MCU pin |
|---|---|
| BTN_ADC | Any ADC input (e.g., PA0/ADC123_IN0) |

- **Pros**: Only 1 pin. Classic low-pin-count technique. STM32F4 ADC is 12-bit — enough to distinguish 4 buttons + idle.
- **Cons**: Requires calibration. Analog noise couples into button detection. Cannot detect simultaneous presses reliably. Each button press draws current through divider. ADC must be enabled (`CONFIG_ADC=y`). Slower polling vs interrupt-driven GPIO. More complex firmware (voltage threshold table).
- **Verdict**: Saving 3 pins is not worth the analog complexity on this board. Only justified if pin count is truly critical.

#### Option C: I2C GPIO Expander (0 extra MCU pins)

| Net | MCU pin |
|---|---|
| I2C1_SCL | PB8 (shared) |
| I2C1_SDA | PB9 (shared) |

- **Pros**: Zero MCU pins. Supports up to 8+ buttons with interrupt output. Clean.
- **Cons**: Adds BOM cost ($0.30–0.80 for PCA9536/TCA9534). Adds I2C bus traffic. Adds firmware driver dependency. Power draw (~1 µA standby). Over-engineered for 4 buttons.
- **Verdict**: Not warranted. 4 GPIOs are cheaper and simpler than an expander chip.

| Criterion | A: 4 GPIOs | B: ADC ladder | C: I2C expander |
|---|---|---|---|
| Pins | 4 | **1** | **0** |
| BOM cost | 4× pull-up R | 5× R + calibration | +$0.30–0.80 IC |
| Simultaneous press | Yes | No | Yes |
| Interrupt wake | Yes (per pin) | No (poll ADC) | Yes (INT pin) |
| Noise immunity | High | Low (analog) | High |
| Firmware complexity | Low | Medium | Medium |
| PCB area | 4 traces | 1 trace + 4 R | IC + 2 traces |

---

### 5.5 Overall Architecture — Top-Level Comparison

#### Solution 1: Balanced (Recommended) — 12 pins

| Device | Interface | Pins |
|---|---|---|
| BME280 | I2C1 shared | 0 |
| SSD1331 OLED | SPI3 HW | 5 |
| Rotary encoder | GPIO ISR | 3 |
| 4 buttons | 4× GPIO | 4 |
| **Total** | | **12** |

- Clean separation: display on SPI3, sensors on I2C1, HMI on GPIO.
- All pins cluster on GPIOB + GPIOC for easy routing.
- Follows established patterns (DAC=SPI, ADC=GPIO bit-bang, sensor=I2C).

#### Solution 2: Pin-Minimal — 8 pins

| Device | Interface | Pins |
|---|---|---|
| BME280 | I2C1 shared | 0 |
| SSD1331 OLED | Bit-bang GPIO | 5 |
| Rotary encoder | GPIO ISR | 3 |
| 4 buttons | ADC ladder | 1 |
| **Total** | | **9** |

- Saves 3 pins vs Solution 1 by using ADC ladder for buttons.
- Sacrifices: slower display refresh (bit-bang), analog noise on buttons, no simultaneous key detection.
- Only justified if expansion connector is size-constrained (e.g., 10-pin FFC instead of 16-pin header).

#### Solution 3: Pin-Rich (Clean Isolation) — 14 pins

| Device | Interface | Pins |
|---|---|---|
| BME280 | I2C2 dedicated | 2 |
| SSD1331 OLED | SPI3 HW | 5 |
| Rotary encoder | TIM4 QDEC | 3 |
| 4 buttons | 4× GPIO | 4 |
| **Total** | | **14** |

- Maximum isolation: every device on its own bus.
- Costs 2 extra pins vs Solution 1 for I2C2.
- Only justified if BME280/SHT31 bus sharing proves problematic in testing.

| Criterion | Sol 1: Balanced | Sol 2: Pin-minimal | Sol 3: Pin-rich |
|---|---|---|---|
| Total pins | **12** | **9** | **14** |
| Display speed | HW SPI (fast) | Bit-bang (slow) | HW SPI (fast) |
| Encoder decode | SW ISR | SW ISR | HW QDEC (zero CPU) |
| Button detection | Individual | ADC (analog) | Individual |
| Sensor isolation | Shared I2C1 | Shared I2C1 | Dedicated I2C2 |
| Pin clustering | Good (PB+PC) | Any port | Good |
| Recommended for | **Default** | Space-constrained | Max reliability |

---

## 6. Recommended Configuration (Solution 1)

Pin selections are optimized for:
- Physical adjacency (same port cluster → easier PCB routing on expansion board)
- Hardware peripheral availability (SPI3 for display, optional TIM4_QDEC for encoder)
- No conflict with existing or planned usage

### 6.1 BME280 — Share I2C1 (0 extra pins)

BME280 at `0x76` or `0x77` coexists with SHT31 at `0x44` on the same I2C1 bus.

| Net | MCU pin | Already used by |
|---|---|---|
| I2C1_SCL | PB8 | SHT31 |
| I2C1_SDA | PB9 | SHT31 |

No firmware/DT change to the I2C1 bus — add a second child node under `&i2c1`.

### 6.2 SSD1331 OLED — SPI3 (5 pins)

| Signal | MCU pin | AF / mode | Notes |
|---|---|---|---|
| SCLK | **PB3** | AF6 (SPI3_SCK) | Hardware SPI clock |
| MOSI (DIN) | **PB5** | AF6 (SPI3_MOSI) | Hardware SPI data |
| CS | **PB0** | GPIO output (active low) | Software chip select |
| DC | **PB1** | GPIO output | Data/Command select |
| RST | **PB4** | GPIO output (active low) | Hardware reset |

All 5 pins are on GPIOB, adjacent (PB0–PB5). SPI3 is unused in the current design. The SSD1331 is write-only (no MISO needed), matching the AD5541 DAC pattern already used on SPI1/SPI2.

### 6.3 Rotary Encoder (3 pins)

| Signal | MCU pin | Type | Notes |
|---|---|---|---|
| ENC_A | **PB6** | GPIO input, interrupt | Also TIM4_CH1 (AF2) for QDEC |
| ENC_B | **PB7** | GPIO input, interrupt | Also TIM4_CH2 (AF2) for QDEC |
| ENC_SW | **PB10** | GPIO input, interrupt | Push-button |

PB6/PB7 optionally map to TIM4 QDEC for hardware quadrature decoding (preferred if QDEC driver works; fall back to GPIO ISR).

### 6.4 Four Buttons (4 pins)

| Button | MCU pin | Type |
|---|---|---|
| BTN1 | **PB11** | GPIO input, pull-up |
| BTN2 | **PB14** | GPIO input, pull-up |
| BTN3 | **PC0** | GPIO input, pull-up |
| BTN4 | **PC1** | GPIO input, pull-up |

All buttons: active low with external or internal pull-up (internal pull-ups on STM32F4 are ~40 kΩ; add external 10 kΩ if noise is a concern).

### 6.5 Pin Summary

```
Port B cluster:
  PB0  → OLED_CS
  PB1  → OLED_DC
  PB3  → OLED_SCLK  (SPI3_SCK)
  PB4  → OLED_RST
  PB5  → OLED_MOSI  (SPI3_MOSI)
  PB6  → ENC_A
  PB7  → ENC_B
  PB8  → I2C1_SCL    (shared: SHT31 + BME280)
  PB9  → I2C1_SDA    (shared: SHT31 + BME280)
  PB10 → ENC_SW
  PB11 → BTN1
  PB14 → BTN2

Port C cluster:
  PC0  → BTN3
  PC1  → BTN4
```

| Device | New MCU pins |
|---|---|
| BME280 | **0** (shares I2C1) |
| SSD1331 OLED | **5** |
| Rotary encoder | **3** |
| 4 buttons | **4** |
| **Total** | **12** |

---

## 7. Hardware Design Details

### 7.1 BME280 Environmental Sensor

#### Module Selection

| Module | Dimensions | Notes |
|---|---|---|
| GY-BME280 | 15 × 11.5 mm | Common breakout, 3.3V/5V via onboard LDO, I2C+SPI |
| CJMCU-280 | 13 × 10 mm | Compact, 3.3V only, I2C only |
| Custom PCB | — | Bare BME280 IC (LGA-8 2.5×2.5×0.93 mm) |

Recommendation: **GY-BME280** module (widely available, well-documented).

#### Pinout (GY-BME280)

| Module pin | Connect to | Notes |
|---|---|---|
| VCC | VCC_+3V3 | Module has onboard 3.3V LDO; can also bypass LDO |
| GND | GND | |
| SCL | I2C1_SCL (PB8) | Shared with SHT31 |
| SDA | I2C1_SDA (PB9) | Shared with SHT31 |
| CSB | VCC_+3V3 | Tie HIGH to select I2C mode |
| SDO | GND or VCC_+3V3 | GND → addr 0x76; VCC → addr 0x77 |

#### I2C Address Selection

Place a solder jumper on the expansion PCB to select SDO level:

```
SDO ──┬── 2-pin header (jumper open → pull-up to VCC = 0x77)
       │
       └── 2-pin header (jumper closed → pull-down to GND = 0x76, default)
```

Default: **0x76** (SDO = GND). Ship with jumper closed or a 10 kΩ pull-down to GND.

#### I2C Bus Pull-up Check

Existing SHT31 on jw_hvb uses R96/R97 = **10 kΩ** pull-ups on SCL/SDA (see `ref/board_design.md` §9). Adding BME280 on the same bus:

- STM32F4 I2C1 fast-mode (400 kHz) requires Rp(min) ≈ 1.3 kΩ, Rp(max) ≈ 3.6 kΩ at 3.3V
- Current 10 kΩ is too weak for 400 kHz — the bus is currently running at **standard-mode (100 kHz)** per DT (`clock-frequency = <I2C_BITRATE_STANDARD>`)
- At 100 kHz with 10 kΩ: rise time ≈ 10k × 30pF × 3 ≈ 0.9 µs — marginal but works for SHT31
- Adding BME280 adds ~5 pF bus capacitance → still OK at 100 kHz
- **No change needed.** If you want 400 kHz in future, replace R96/R97 with 2.2 kΩ.

#### Decoupling

| Component | Value | Package | Placement |
|---|---|---|---|
| C_BME280 | 100 nF X7R | 0402 or 0603 | ≤5 mm from module VCC pin |

---

### 7.2 SSD1331 OLED Display

#### Module Selection

| Module | P/N | Dimensions | Resolution | Colors |
|---|---|---|---|---|
| Waveshare 0.95" OLED | SKU 14731 | 31.5 × 27.0 mm | 96×64 | 65K/262K |
| Generic SSD1331 OLED | — | ~26 × 20 mm | 96×64 | 65K |

**Recommendation**: Waveshare 0.95inch RGB OLED (HAT) — well-documented pinout, 3.3V compatible, mounting holes provided.

#### Module Pinout (Waveshare)

| Pin | Name | Connect to | Notes |
|---|---|---|---|
| 1 | VCC | VCC_+3V3 | Module accepts 3.3V directly |
| 2 | GND | GND | |
| 3 | DIN | SPI3_MOSI (PB5) | Data input (MOSI) |
| 4 | CLK | SPI3_SCLK (PB3) | SPI clock |
| 5 | CS | OLED_CS (PB0) | Active-low chip select |
| 6 | DC | OLED_DC (PB1) | Data/Command: LOW=cmd, HIGH=data |
| 7 | RST | OLED_RST (PB4) | Active-low reset |

#### Electrical Characteristics

| Parameter | Value | Notes |
|---|---|---|
| VCC supply | 3.3 V ±5% | Module has onboard regulator |
| I_VCC (typical) | 25 mA | All pixels white, max brightness |
| I_VCC (sleep) | <10 µA | Display off via SW command |
| V_IH (logic HIGH) | ≥0.7 × VCC | 2.31 V min |
| V_IL (logic LOW) | ≤0.3 × VCC | 0.99 V max |
| SPI max clock | 10 MHz | Mode 0 (CPOL=0, CPHA=0) |
| Reset low pulse | ≥3 µs | After VCC stable ≥100 ms |

#### Hardware Reset Circuit

Add an RC delay on the expansion PCB for reliable power-on reset:

```
VCC_+3V3 ─── 10kΩ ──┬── OLED_RST (PB4)
                      │
                    100nF
                      │
                     GND
```

This holds RST low for ~1 ms after power-up. PB4 GPIO can also drive RST for software-triggered reset through a series 1kΩ resistor.

#### Decoupling

| Component | Value | Package | Placement |
|---|---|---|---|
| C_OLED1 | 100 nF X7R | 0402 | ≤5 mm from module VCC pin |
| C_OLED2 | 10 µF X5R | 0805 | ≤10 mm from module VCC pin |

---

### 7.3 Rotary Encoder

#### Part Selection

| Part | Manufacturer | PPR | Detents | Shaft | Notes |
|---|---|---|---|---|---|
| EC11E15244A2 | ALPS | 15 PPR | 30 | 20 mm flatted | Good feel, common |
| PEC11R-4220F-S0024 | Bourns | 24 PPR | 24 | 20 mm flatted | Higher resolution |
| KY-040 module | Generic | 20 PPR | 20 | module | Inexpensive breakout with RC circuit |

Quadrature output: Channel A leads Channel B by 90° for clockwise rotation.

#### Reference Circuit

```
VCC_+3V3                  VCC_+3V3
   │                          │
  10kΩ                       10kΩ
   │                          │
   ├── ENC_A (PB6)            ├── ENC_B (PB7)
   │                          │
  10nF                       10nF
   │                          │
  GND                        GND
   │                          │
   ├── ENC_A terminal (C)     ├── ENC_B terminal (C)
   │                          │
  Encoder COM ── GND         Encoder COM ── GND


VCC_+3V3
   │
  10kΩ
   │
   ├── ENC_SW (PB10)
   │
  10nF (optional)
   │
  GND
   │
   └── Encoder SW terminal (NO)
```

- **Pull-up**: 10 kΩ external. Can also use STM32 internal pull-up (~40 kΩ) — skip external R if using internal.
- **Debounce RC**: 10 kΩ × 10 nF = 100 µs time constant. For 24 PPR at 5 rev/s, max pulse rate = 120 Hz, period = 8.3 ms. 100 µs debounce is 1.2% of pulse width — safe. If using TIM4 QDEC, enable hardware filter (`st,input-filter-level = <5>` for ~500 ns filtering).
- **Alternative**: If using internal pull-ups, place only 10 nF caps (no external R).

#### Mechanical

- Panel-mount encoder with nut. Shaft diameter typically 6 mm.
- Standard knob cap fits 6 mm D-shaft.
- Module placement: edge of expansion PCB with shaft protruding through enclosure.

---

### 7.4 Tactile Buttons

#### Reference Circuit

```
VCC_+3V3
   │
  10kΩ                              VCC_+3V3
   │                                   │
   ├── BTNx (PB11/PB14/PC0/PC1)       10kΩ
   │                                   │
  10nF (optional)                      ├── BTNx
   │                                   │
  GND                                100nF
   │                                   │
  Tactile SW ── GND                   GND
                                       │
  (active-low, NO)                    Tactile SW ── GND
```

- **Pull-up**: 10 kΩ external recommended (STM32 internal ~40 kΩ is OK for bench testing but vulnerable to EMI in deployed environment).
- **Debounce**: 100 nF across switch (10 kΩ × 100 nF = 1 ms time constant) provides clean hardware debounce. Software debounce still recommended as backup.
- **ESD protection**: If buttons are user-facing and exposed to touch, add a TVS array:
  - USBLC6-2SC6 (SOT-23-6, 2 channels) per 2 buttons
  - Or discrete: BAT54S dual Schottky + 100 nF on each button line

#### Recommended Switch

| Part | Manufacturer | Size | Actuation force | Travel |
|---|---|---|---|---|
| PTS645SM43SMTR92 | C&K | 6×6 mm | 160 gf | 0.25 mm |
| B3F-1000 | Omron | 6×6 mm | 100 gf | 0.25 mm |
| KSC421J | C&K | 6.2×6.2 mm | 200 gf | 0.35 mm |

Any SPST-NO tactile switch with 6×6 mm footprint, 4–5 mm height.

---

### 7.5 Pin Electrical Characteristics (STM32F429BIT6 Side)

All recommended pins below are 5 V-tolerant except where noted.

| MCU pin | Signal | 5V-tolerant | Max output speed | PU/PD | Notes |
|---|---|---|---|---|---|
| PB0 | OLED_CS | Yes | 100 MHz | PU/PD | GPIO output |
| PB1 | OLED_DC | Yes | 100 MHz | PU/PD | GPIO output |
| PB3 | OLED_SCLK | Yes | 100 MHz | PU | SPI3_SCK, also JTDO (disabled in SWD mode) |
| PB4 | OLED_RST | Yes | 100 MHz | PU | GPIO output, also NJTRST (disabled in SWD mode) |
| PB5 | OLED_MOSI | Yes | 100 MHz | PU | SPI3_MOSI |
| PB6 | ENC_A | Yes | 100 MHz | PU/PD | GPIO input, also TIM4_CH1 |
| PB7 | ENC_B | Yes | 100 MHz | PU/PD | GPIO input, also TIM4_CH2 |
| PB8 | I2C1_SCL | Yes | 100 MHz | PU/PD | I2C1 (FT — 5V tolerant), open-drain required |
| PB9 | I2C1_SDA | Yes | 100 MHz | PU/PD | I2C1 (FT — 5V tolerant), open-drain required |
| PB10 | ENC_SW | Yes | 100 MHz | PU/PD | GPIO input |
| PB11 | BTN1 | Yes | 100 MHz | PU/PD | GPIO input |
| PB14 | BTN2 | Yes | 100 MHz | PU/PD | GPIO input |
| PC0 | BTN3 | Yes | 100 MHz | PU/PD | GPIO input |
| PC1 | BTN4 | Yes | 100 MHz | PU/PD | GPIO input |

> **Note**: PB3 and PB4 are JTAG pins (JTDO/NJTRST). In SWD mode (used by jw_hvb via PA13/PA14 SWDIO/SWCLK), these pins are available as GPIO. Ensure the debug probe does not assert JTAG mode. This is the default for STM32F4 when SWD is used.

---

### 7.6 Signal Integrity & PCB Layout Guidelines

#### I2C Bus (PB8, PB9)

- Trace impedance: not critical at 100 kHz
- Keep SCL and SDA traces together, length-matched within 20 mm
- Total bus capacitance: keep <400 pF (SHT31 ≈ 5 pF, BME280 ≈ 5 pF, traces ≈ 2 pF/cm)
- Pull-ups (R96/R97) are on the jw_hvb main board (10 kΩ). Do NOT add duplicate pull-ups on the expansion PCB.

#### SPI Bus (PB3, PB5)

- Target: 8–10 MHz operation
- Trace impedance: 50 Ω nominal (not critical at these speeds)
- Keep SCK and MOSI trace lengths matched within 10 mm
- Maximum trace length: 100 mm
- For traces >50 mm: add 22 Ω series termination resistor near MCU-side connector on SCK line to damp reflections
- No MISO on this bus (SSD1331 is write-only)

#### GPIO Lines (PB0, PB1, PB4, PB6, PB7, PB10, PB11, PB14, PC0, PC1)

- No special routing requirements
- Keep >2 mm from any switching HV traces (if expansion PCB is near HV section)
- Buttons: use ground pour around switch pads for ESD protection

#### Power Distribution

- VCC_+3V3 trace width: ≥0.5 mm for 100 mA capacity
- GND: use ground plane on bottom layer of expansion PCB
- Place a 10 µF bulk capacitor at the connector entry point
- Each device gets a local 100 nF decoupling capacitor

#### General

- 2-layer PCB sufficient for all signals
- Expansion PCB ground plane must connect to jw_hvb GND at the connector (at least 2 GND pins)
- If expansion PCB is >50 mm from jw_hvb, consider adding a ferrite bead (e.g., BLM18PG121SN1) on VCC_+3V3 at the expansion PCB entry

---

### 7.7 Expansion PCB Reference Schematic Checklist

```
□ Connector: 16-pin 2×8 header (see §8)
□ Jumper: BME280 SDO address select (0x76 default)
□ BME280: 100 nF decoupling cap at module VCC
□ SSD1331: 100 nF + 10 µF decoupling at module VCC
□ SSD1331: RC reset circuit (10kΩ + 100nF) on RST line
□ Encoder: 10kΩ pull-ups on ENC_A, ENC_B, ENC_SW
□ Encoder: 10 nF debounce caps (optional, if not using internal pull-ups + SW debounce)
□ Buttons: 10kΩ pull-ups + 100 nF debounce caps on each BTNx
□ Buttons: TVS ESD protection if user-exposed (e.g., USBLC6-2SC6)
□ Power: 10 µF bulk cap at connector VCC entry
□ Test points: GND test point (for scope probe) near connector
```

---

### 7.8 Bill of Materials (Expansion PCB)

| Ref | Qty | Value / Part | Package | Notes |
|---|---|---|---|---|
| U1 | 1 | GY-BME280 module | 15×11.5 mm | BME280 environmental sensor breakout |
| U2 | 1 | Waveshare SSD1331 0.95" OLED | 31.5×27 mm | Or generic SSD1331 module |
| SW_ENC | 1 | EC11E15244A2 or PEC11R-4220F | Panel mount | Rotary encoder with push switch |
| SW1–SW4 | 4 | PTS645SM43SMTR92 | 6×6 mm SMD | Tactile switch, SPST NO |
| J1 | 1 | 2×8 pin header, 2.54 mm | TH/SMD | To jw_hvb (see §8 for mating) |
| R1–R3 | 3 | 10 kΩ ±5% | 0402 or 0603 | Encoder pull-ups (omit if using MCU internal) |
| R4–R7 | 4 | 10 kΩ ±5% | 0402 or 0603 | Button pull-ups |
| R8 | 1 | 10 kΩ ±5% | 0402 or 0603 | OLED RST RC circuit |
| R9 | 1 | 1 kΩ ±5% | 0402 or 0603 | OLED RST series resistor (SW reset) |
| R10 | 1 | 10 kΩ ±5% | 0402 or 0603 | BME280 SDO pull-down (addr 0x76) |
| C1 | 1 | 100 nF X7R | 0402 | BME280 decoupling |
| C2 | 1 | 100 nF X7R | 0402 | OLED decoupling |
| C3 | 1 | 10 µF X5R | 0805 | OLED bulk decoupling |
| C4 | 1 | 10 µF X5R | 0805 | Connector VCC entry bulk |
| C5 | 1 | 100 nF X7R | 0402 | OLED RST RC delay cap |
| C6–C9 | 4 | 100 nF X7R | 0402 | Button debounce caps (optional) |
| C10–C11 | 2 | 10 nF X7R | 0402 | Encoder debounce caps (optional) |
| D1–D2 | 2 | USBLC6-2SC6 | SOT-23-6 | ESD protection for 4 buttons (optional) |
| JP1 | 1 | 2-pin header, 2.54 mm | TH | BME280 SDO address jumper |

---

## 8. Expansion Connector Specification

### 8.1 Recommended Connectors

| Option | Series | Pitch | Pins | Mating height | Notes |
|---|---|---|---|---|---|
| A | Standard pin header | 2.54 mm | 2×8 | 8.5 mm | Simplest, breadboard-friendly |
| B | JST SH (SM16B-SRSS-TB) | 1.00 mm | 16 (single row) | 4.5 mm | Compact, locking, good for production |
| C | Molex PicoBlade 53398 | 1.25 mm | 16 | 4.2 mm | Compact, locking |
| D | Harwin M20 | 2.54 mm | 2×8 | 8.5 mm | Industrial, shrouded, polarized |

**Recommendation for development**: Option A (standard 2.54 mm pin header). Cheap, easy to probe.
**Recommendation for production**: Option B (JST SH 1.0 mm). Compact, locking, vibration-resistant.

### 8.2 Connector Pinout

| Pin | Signal | MCU pin | Direction | Option A pin | Option B pin |
|---|---|---|---|---|---|
| 1 | VCC_+3V3 | — | Power out | Row A, Col 1 | Pin 1 |
| 2 | GND | — | — | Row A, Col 2 | Pin 2 |
| 3 | I2C1_SCL | PB8 | Bidir (OD) | Row A, Col 3 | Pin 3 |
| 4 | I2C1_SDA | PB9 | Bidir (OD) | Row A, Col 4 | Pin 4 |
| 5 | SPI3_SCLK | PB3 | Output | Row A, Col 5 | Pin 5 |
| 6 | SPI3_MOSI | PB5 | Output | Row A, Col 6 | Pin 6 |
| 7 | OLED_CS | PB0 | Output | Row A, Col 7 | Pin 7 |
| 8 | OLED_DC | PB1 | Output | Row A, Col 8 | Pin 8 |
| 9 | OLED_RST | PB4 | Output | Row B, Col 1 | Pin 9 |
| 10 | ENC_A | PB6 | Input | Row B, Col 2 | Pin 10 |
| 11 | ENC_B | PB7 | Input | Row B, Col 3 | Pin 11 |
| 12 | ENC_SW | PB10 | Input | Row B, Col 4 | Pin 12 |
| 13 | BTN1 | PB11 | Input | Row B, Col 5 | Pin 13 |
| 14 | BTN2 | PB14 | Input | Row B, Col 6 | Pin 14 |
| 15 | BTN3 | PC0 | Input | Row B, Col 7 | Pin 15 |
| 16 | BTN4 | PC1 | Input | Row B, Col 8 | Pin 16 |

**Mating connectors** (for expansion PCB side):

| Option | Connector | Part number |
|---|---|---|
| A | 2×8 pin header female | e.g., Samtec SSW-108-01-G-D (TH) or equivalent |
| A | 16-pin IDC ribbon | 2×8 IDC socket + ribbon cable |
| B | JST SH receptacle | SHR-16V-S-B, plus SHF-001T-0.8BS crimp contacts |
| C | Molex PicoBlade receptacle | 51021-1600, plus 50079-8100 crimp contacts |

---

## 9. Fallback Pin Options

| Original | Fallback | Reason |
|---|---|---|
| PB3 (SPI3_SCK) | PC10 (SPI3_SCK, AF6) | Conflicts with USART3_TX unless moved |
| PB5 (SPI3_MOSI) | PC12 (SPI3_MOSI, AF6) | — |
| PB0 (OLED_CS) | Any free GPIO | Software CS is portable |
| PB1 (OLED_DC) | Any free GPIO | — |
| PB4 (OLED_RST) | Any free GPIO | — |
| PB6 (ENC_A) | PD12 (TIM4_CH1, AF2) | If PB6 is unavailable |
| PB7 (ENC_B) | PD13 (TIM4_CH2, AF2) | If PB7 is unavailable |

**SPI4/SPI5 alternative** (if SPI3 pins are blocked):
- SPI4: PE12 (SCK), PE14 (MISO), PE6 (MOSI) — but PE12 is used by HV1 ADC
- SPI5: PF7 (SCK), PF8 (MISO), PF9 (MOSI) — but PF7/PF8 are used by HV2 ADC
- SPI6: PG12 (MISO), PG13 (SCK), PG14 (MOSI) — PG14 is USART6_TX

**If all hardware SPI buses are blocked**: Bit-bang SSD1331 on any 5 free GPIOs (follows the ADS1232 pattern already established in the design).

---

## 10. Firmware Impact

### 10.1 Devicetree Changes

```dts
// 1. BME280 on existing I2C1 bus
&i2c1 {
    bme280@76 {
        compatible = "bosch,bme280";
        reg = <0x76>;
    };
};

// 2. Enable SPI3
&spi3 {
    pinctrl-0 = <&spi3_sck_pb3 &spi3_mosi_pb5>;
    pinctrl-names = "default";
    cs-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>;
    status = "okay";
    // SSD1331 display (write-only, no kernel driver in Zephyr —
    // use display API or custom driver)
};

&pinctrl {
    spi3_sck_pb3:  spi3_sck_pb3  { pinmux = <STM32_PINMUX('B', 3, AF6)>; };
    spi3_mosi_pb5: spi3_mosi_pb5 { pinmux = <STM32_PINMUX('B', 5, AF6)>; };
};

// 3. Rotary encoder (GPIO-based)
/ {
    rotary_encoder {
        compatible = "gpio-keys";
        enc_a: enc_a { gpios = <&gpiob 6 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>; };
        enc_b: enc_b { gpios = <&gpiob 7 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>; };
        enc_sw: enc_sw { gpios = <&gpiob 10 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>; };
    };
};

// 4. Buttons
/ {
    buttons {
        compatible = "gpio-keys";
        btn1: btn1 { gpios = <&gpiob 11 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; };
        btn2: btn2 { gpios = <&gpiob 14 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; };
        btn3: btn3 { gpios = <&gpioc 0  (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; };
        btn4: btn4 { gpios = <&gpioc 1  (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; };
    };
};
```

### 10.2 Kconfig Additions

```kconfig
CONFIG_I2C=y          # already enabled for SHT31
CONFIG_SENSOR=y       # already enabled
CONFIG_BME280=y       # new
CONFIG_SPI=y          # already enabled
CONFIG_SPI_STM32=y    # already enabled
CONFIG_DISPLAY=y      # new (if using Zephyr display API)
```

### 10.3 No Impact on Existing Functionality

- I2C1 bus: SHT31 at 0x44 + BME280 at 0x76 coexist on same bus
- SPI1/SPI2: HV DACs unaffected (dedicated buses)
- USART3/USART6: console and RS-485 unaffected
- All GPIO bit-bang ADS1232 ADCs unaffected
- SWD debug interface unaffected

---

## 11. Decision Rationale

1. **BME280 on I2C1 (not separate SPI)**: Saves 4 pins, uses the already-enabled I2C1 bus. Different I2C address from SHT31 eliminates bus contention. Both sensors have compatible I2C speeds (standard 100 kHz / fast 400 kHz).

2. **SPI3 for SSD1331 (not SPI4/SPI5/SPI6)**: SPI3 is the only completely free SPI peripheral where both SCK and MOSI pins are available without conflict. SPI4/SPI5 pins overlap with ADS1232 ADC GPIOs; SPI6 pin overlaps with USART6.

3. **PB cluster for expansion**: All 10 new signals (excluding I2C which is pre-routed) cluster on GPIOB (PB0,PB1,PB3,PB4,PB5,PB6,PB7,PB10,PB11,PB14) + 2 on GPIOC (PC0,PC1). This minimizes expansion PCB trace length and simplifies routing.

4. **No GPIOJ/K**: These ports exist in the STM32F429BI SoC but are not enabled in the jw_hvb board DTS, suggesting they are not broken out to accessible pads/headers.

---

## 12. Open Questions for PCB Designer

- [ ] Are PB0–PB7, PB10, PB11, PB14, PC0, PC1 physically routed to test points, vias, or accessible pads on the jw_hvb PCB?
- [ ] Is VCC_+3V3 accessible at a convenient header or test point near the planned expansion connector location?
- [ ] Does the board have a spare I2C1 bus tap point (other than the SHT31 pads)?
- [ ] What connector pitch and form factor are preferred for the expansion port?
- [ ] Should the expansion sensor PCB have its own 3.3 V LDO (fed from VCC_12V) for noise isolation, or is VCC_+3V3 directly acceptable?

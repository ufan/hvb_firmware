# MCU Requirements — 16-Channel HVB Controller

## 1. Firmware Resource Estimate (16 channels)

Based on current `hvb_controller` build (STM32F429, 2ch) with per-channel scaling
analysis of `struct vc_channel` (138 B), `vc_catalog_channel_regs[40]` (24 B each),
and `vc_channel_buffer` (24 B each). All other structures are shared/constant.

| Resource | Current 2ch | 16ch Estimate | Delta |
|----------|-------------|---------------|-------|
| Flash (text + data) | 92.3 KB | **~110 KB** | +18 KB |
| Static RAM (data + bss) | 20.1 KB | **~23 KB** | +2.9 KB |
| Thread stacks (BSS) | 9.5 KB | 9.5 KB | 0 |
| **Total RAM** | ~30 KB | **~32 KB** | +2 KB |
| CPU utilization | <1% @168 MHz | <5% @48 MHz | — |

**Minimum safe MCU (firmware only, +20% headroom):**

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| Flash | **128 KB** | 256 KB |
| SRAM | **48 KB** | 64 KB |
| Core | Cortex-M3 48 MHz | Cortex-M4 72 MHz |
| FPU | Not required (all integer math) | — |

---

## 2. GPIO Requirements by ADC/DAC Interface

### 2.1 Current baseline (2ch, bit-banged ADS1232 + SPI AD5541)

Per channel: 7 ADC GPIOs (DRDY, SCLK, PWDN, A0, A1, GAIN0, GAIN1) + 1 DAC CS
+ 1 HV enable. 2ch total: 23 peripheral GPIOs.

### 2.2 Assumptions for 16-channel redesign

| Component | Per-channel analog needs | Chip count (16ch) |
|-----------|-------------------------|--------------------|
| ADC | Voltage + Current (2 differential inputs) | 8 chips (4-analog-input each) or 4 chips (8-input each) |
| DAC | 1 voltage output | 16 single-ch AD5541 (SPI) or 2–4 multi-ch I²C DACs |
| HV enable | 1 digital output per channel | 16 discrete GPIOs |

- **SPI bus sharing**: multiple devices share SCLK/MOSI/MISO; each needs a
  unique CS.
- **I²C bus sharing**: devices share SCL/SDA; address conflicts limit ~6–8
  identical parts per bus. Additional buses or a mux (TCA9548A) for 16 devices.
- **DRDY**: dedicated pin per ADC chip, or single shared interrupt line +
  software polling to identify source. Polling eliminates DRDY pins at the cost
  of periodic I²C/SPI reads.

### 2.3 SPI ADC + SPI DAC (pin-maximizing)

| Subsystem | Signal | Qty | GPIOs |
|-----------|--------|-----|-------|
| **ADC** (8 chips) | CS (per chip) | 8 | 8 |
| | DRDY (per chip) | 8 | 8 |
| | Shared SPI (SCLK, MOSI, MISO) | 3 | 3 |
| | Shared RESET | 1 | 1 |
| **DAC** (16 chips) | CS (per chip) | 16 | 16 |
| | Shared SPI (same bus as ADC) | — | 0 |
| **HV enable** | Per channel | 16 | 16 |
| **System** | USART6 Modbus (TX, RX, DE) | 3 | 3 |
| | USART3 Debug (TX, RX) | 2 | 2 |
| | I²C1 SHT31 (SCL, SDA) | 2 | 2 |
| | SYS_MOD0/1 DIP switches | 2 | 2 |
| | SYS_RUN LED | 1 | 1 |
| | SWD (SWDIO, SWCLK) | 2 | 2 |
| **Total (no HMI)** | | | **64** |

### 2.4 I²C ADC + I²C DAC (pin-minimizing)

| Subsystem | Signal | Qty | GPIOs |
|-----------|--------|-----|-------|
| **ADC** (8 chips) | I²C bus 1 (SCL, SDA) | 2 | 2 |
| | I²C bus 2 — extra bus for addr space | 2 | 2 |
| | DRDY (shared interrupt + poll) | 1 | 1 |
| **DAC** | I²C bus 3 — multi-channel DACs¹ | 2 | 2 |
| **HV enable** | Per channel | 16 | 16 |
| **System** | USART6 Modbus (TX, RX, DE) | 3 | 3 |
| | USART3 Debug (TX, RX) | 2 | 2 |
| | I²C SHT31 (share I²C bus 1 or 2) | — | 0 |
| | SYS_MOD0/1 | 2 | 2 |
| | SYS_RUN LED | 1 | 1 |
| | SWD | 2 | 2 |
| **Total (no HMI)** | | | **33** |

¹ E.g., MCP4728 (4-ch, I²C, 2 addr pins → 8 addresses per bus).

### 2.5 Mixed: I²C ADC + SPI DAC (keep existing AD5541)

| Subsystem | Signal | Qty | GPIOs |
|-----------|--------|-----|-------|
| **ADC (I²C)** | I²C buses ×2 | 4 | 4 |
| | DRDY (shared) | 1 | 1 |
| **DAC (SPI)** | CS ×16 | 16 | 16 |
| | Shared SPI (SCLK, MOSI) | 2 | 2 |
| **HV enable** | ×16 | 16 | 16 |
| **System** | Same as above | — | 12 |
| **Total (no HMI)** | | | **51** |

### 2.6 Local HMI (display + buttons + knobs)

| Subsystem | Signal | Qty | GPIOs |
|-----------|--------|-----|-------|
| **Display** | SPI TFT: CS, DC, RST, shared SCLK/MOSI, BL | 5 | 5 |
| | — or — I²C OLED | 2 | 2 |
| **Buttons** | 4 discrete GPIOs (internal pull-up) | 4 | 4 |
| | — or — ADC keypad (1 analog pin) | 1 | 1 |
| **Rotary encoders** | ENC1 (A, B, push), ENC2 (A, B, push) | 6 | 6 |
| **HMI total (SPI display)** | | | **10–15** |
| **HMI total (I²C display)** | | | **7–12** |

---

## 3. Consolidated GPIO Budgets

| Architecture | ADC | DAC | HV En | Sys | HMI | **Total** |
|-------------|-----|-----|-------|-----|-----|-----------|
| All SPI + SPI HMI | 20 | 16 | 16 | 12 | 15 | **79** |
| All I²C + I²C HMI | 5 | 2 | 16 | 10 | 12 | **45** |
| I²C ADC + SPI DAC + SPI HMI | 5 | 18 | 16 | 11 | 15 | **65** |
| I²C ADC + SPI DAC + I²C HMI | 5 | 18 | 16 | 11 | 12 | **62** |

---

## 4. Recommended MCU Specification

| Parameter | Minimum | Recommended | Notes |
|-----------|---------|-------------|-------|
| **Core** | Cortex-M3 | Cortex-M4 | No FPU needed; M4 gives DSP for optional filtering |
| **Max frequency** | 48 MHz | 72–100 MHz | 48 MHz is ample for the workload |
| **Flash** | 128 KB | 256 KB | Room for OTA, factory data, future features |
| **SRAM** | 48 KB | 64 KB | 32 KB used + Zephyr overhead + future headroom |
| **GPIOs (all-SPI)** | ≥ 79 | ≥ 90 | LQFP100+ required |
| **GPIOs (all-I²C)** | ≥ 45 | ≥ 55 | LQFP64 viable |
| **UART** | ≥ 2 | 3 | Modbus + debug + optional expansion |
| **I²C** | ≥ 1 | 3 | 2 for ADCs/DACs, 1 for sensors + HMI |
| **SPI** | ≥ 1 | 2 | ADC bus + DAC bus (can share if timing allows) |
| **Timers** | ≥ 2 | 4 | SysTick + PWM for display backlight + encoder decode |
| **ADC (MCU internal)** | 1 ch | 4 ch | ADC keypad for buttons (optional) |

---

## 5. Candidate MCU Families

### All-I²C path (45–55 GPIOs)

| Part | Core | Flash | SRAM | GPIO | Pkg |
|------|------|-------|------|------|-----|
| STM32F103VC | M3 72 MHz | 256 KB | 48 KB | 80 | LQFP100 |
| STM32F105VC | M3 72 MHz | 256 KB | 64 KB | 80 | LQFP100 |
| STM32F401CC | M4F 84 MHz | 256 KB | 64 KB | 36 | UFQFPN48 — **too few GPIOs** |
| STM32F411CE | M4F 100 MHz | 512 KB | 128 KB | 81 | LQFP100 |
| ATSAM4E8C | M4F 120 MHz | 512 KB | 128 KB | 79 | LQFP100 |

### All-SPI path (64–79 GPIOs)

| Part | Core | Flash | SRAM | GPIO | Pkg |
|------|------|-------|------|------|-----|
| STM32F103VE | M3 72 MHz | 512 KB | 64 KB | 112 | LQFP144 |
| STM32F407VE | M4F 168 MHz | 512 KB | 192 KB | 112 | LQFP144 |
| STM32F405RG | M4F 168 MHz | 1 MB | 192 KB | 51 | LQFP64 — **too few GPIOs** |

---

## 6. Digital Isolation Requirements

### 6.1 Isolation topology

Each HV channel operates at its own floating high-voltage potential and must be
galvanically isolated from the MCU (safe-side) ground.  Every digital signal
crossing the isolation barrier needs a dedicated isolation channel.  The MCU
communicates with the ADC and DAC on the HV side through these isolated paths.

```
  ┌───────────┐    isolation     ┌──────────────┐
  │           │    barrier       │  HV Channel  │
  │   MCU     │─── per-channel ──│ ADC │ DAC    │
  │ (safe)    │    isolators     │ HV_EN        │
  └───────────┘                  └──────────────┘
```

### 6.2 Per-channel signal inventory crossing the isolation barrier

| Signal        | Direction     | SPI architecture                          | I²C architecture         |
|---------------|---------------|-------------------------------------------|--------------------------|
| SCLK (SPI)    | MCU → HV      | ✓ (shared with MOSI/CS group)             | —                        |
| MOSI (SPI)    | MCU → HV      | ✓                                         | —                        |
| MISO (SPI)    | HV → MCU      | ✓                                         | —                        |
| CS_ADC        | MCU → HV      | ✓ (1 per ADC chip on channel)             | —                        |
| CS_DAC        | MCU → HV      | ✓ (1 per AD5541 on channel)               | —                        |
| SCL  (I²C)    | bidirectional | —                                         | ✓ (one bus per channel)  |
| SDA  (I²C)    | bidirectional | —                                         | ✓                         |
| DRDY / ALERT  | HV → MCU      | ✓ (optional, can poll via SPI)            | ✓ (optional, can poll)   |
| HV_EN         | MCU → HV      | ✓ (slow — optocoupler viable)             | ✓ (slow — optocoupler)   |

### 6.3 SPI architecture: isolation channel count

**Assumptions:**
- One ADC chip and one AD5541 DAC per HV channel, sharing a single SPI bus
  (SCLK + MOSI common, separate CS lines, single MISO from ADC).
- DRDY polled via SPI status register (no dedicated isolation channel).
- HV_EN uses a separate low-cost optocoupler (too slow for SPI but fine for
  a static enable signal).

| SPI signal  | Dir   | Channels | Isolator type          |
|-------------|-------|----------|------------------------|
| SCLK        | →     | 1        | Unidirectional digital |
| MOSI        | →     | 1        | Unidirectional digital |
| CS_ADC      | →     | 1        | Unidirectional digital |
| CS_DAC      | →     | 1        | Unidirectional digital |
| MISO        | ←     | 1        | Unidirectional digital |
| **Subtotal digital isolator** | | **5** (4 fwd + 1 rev) | 1× Si8651 (4/1) |
| HV_EN       | →     | 1        | 1× optocoupler (e.g., TLP291) |

**Per-channel SPI isolation BOM:** 1× 5-ch digital isolator + 1× optocoupler

| Scenario                       | Digital isolator | Optocoupler | pcs/ch |
|--------------------------------|-------------------|-------------|--------|
| No DRDY pin (polled)           | Si8651 (4f/1r)   | TLP291 ×1   | 2      |
| DRDY pin needed (6 signals)    | Si8662 (4f/2r)   | TLP291 ×1   | 2      |

### 6.4 I²C architecture: isolation channel count

**Assumptions:**
- One ADC and one multi-channel I²C DAC share a single I²C bus on the HV side.
- I²C isolation uses a purpose-built bidirectional I²C isolator (handles SCL
  and SDA open-drain signaling internally with a single chip).
- DRDY polled via I²C register read (no dedicated pin).
- HV_EN uses an optocoupler.
- Each channel gets its own isolated I²C bus; MCU-side I²C mux (TCA9548A)
  fans out to the per-channel isolators.

| I²C signal   | Dir           | Channels | Isolator type             |
|-------------|---------------|----------|---------------------------|
| SCL + SDA   | bidirectional | 1 bus    | 1× I²C isolator (ADUM1250)|
| HV_EN       | →             | 1        | 1× optocoupler (TLP291)   |
| DRDY        | ←             | 1        | 1-ch digital isolator (if needed) |

**Per-channel I²C isolation BOM:** 1× I²C isolator + 1× optocoupler = **2 chips**

| Scenario                    | Isolator chips | Opto | pcs/ch |
|-----------------------------|----------------|------|--------|
| No DRDY pin (polled)        | ADUM1250 ×1    | ×1   | 2      |
| DRDY pin needed             | ADUM1250 + Si8621 (1r) | ×1 | 3    |

### 6.5 Shared infrastructure (both architectures)

| Component              | SPI arch                  | I²C arch                                 | Qty  |
|------------------------|---------------------------|------------------------------------------|------|
| I²C mux (bus fan-out)  | —                         | TCA9548A (8-ch; 2 needed for 16 ch)      | 2    |
| SPI bus isolation      | Not needed (per-ch already isolated) | —                        | 0    |
| Modbus RS-485 isolation| ADM2587E (integrated iso) | ADM2587E (integrated iso)                | 1    |

The Modbus RS-485 transceiver typically integrates isolation (ADM2587E), so
it does not consume extra isolator channels.

### 6.6 Total isolation BOM for 16 channels

| Architecture      | Per-channel chips                          | ×16 total                | Shared chips | Est. isolator cost¹ |
|-------------------|--------------------------------------------|--------------------------|---------------|---------------------|
| **SPI (no DRDY)** | 1× Si8651 + 1× optocoupler                 | 16 Si8651 + 16 opto      | —             | ~$22                 |
| **SPI (w/ DRDY)** | 1× Si8662 + 1× optocoupler                 | 16 Si8662 + 16 opto      | —             | ~$30                 |
| **I²C (no DRDY)** | 1× ADUM1250 + 1× optocoupler               | 16 ADUM1250 + 16 opto    | 2× TCA9548A   | ~$34                 |
| **I²C (w/ DRDY)** | 1× ADUM1250 + 1× Si8621 + 1× optocoupler   | 16 each                  | 2× TCA9548A   | ~$48                 |

¹ Rough 1ku pricing: Si8651 ~$1.20, Si8662 ~$1.50, ADUM1250 ~$1.80, opto ~$0.15,
TCA9548A ~$0.80.

### 6.7 Board area impact

| Architecture      | Isolation chips/ch | Footprint/ch (isolators) | ×16 total area |
|-------------------|--------------------|--------------------------|----------------|
| SPI (Si8651)      | 1× SOIC-16W + 1× SO-4 | ~90 mm² + 18 mm²     | ~1,728 mm²     |
| I²C (ADUM1250)    | 1× SOIC-8 + 1× SO-4    | ~30 mm² + 18 mm²      | ~768 mm²       |

I²C saves ~55% board area on isolation components vs SPI.  The ADUM1250
SOIC-8 footprint is substantially smaller than the Si86xx SOIC-16W.

### 6.8 Summary

| Metric                  | SPI                    | I²C                     |
|-------------------------|------------------------|-------------------------|
| Isolator chips per ch   | 1 (5-ch digital)       | 1 (I²C bidirectional)   |
| Optocouplers per ch     | 1 (HV_EN)              | 1 (HV_EN)               |
| Total chips (16 ch)     | 32                     | 34 (32 per-ch + 2 mux)  |
| Board area (isolators)  | ~1,728 mm²             | ~768 mm²                |
| Latency per conversion  | < 5 µs @ 10 MHz        | ~100 µs @ 400 kHz       |
| DRDY elimination        | Poll status via SPI    | Poll status via I²C     |

**I²C wins on board area and per-channel isolator pin count.**  SPI wins on
latency, but at 10 SPS per channel latency is irrelevant.  For 16 channels the
I²C isolation path is the simpler, smaller, and nearly cost-equivalent choice.

---

## 7. Notes

- **Pin count is the binding constraint**, not compute or memory. The firmware
  at 16 channels uses only ~5% of a 256 KB flash / 64 KB RAM MCU.
- Moving from the current bit-banged ADS1232 (7 pins/ch) to an I²C or SPI ADC
  reduces the per-channel GPIO count from 9 to 2–3, making LQFP100 packages
  viable for a 16-channel design.
- The STM32F103 (Cortex-M3) family is Zephyr-supported and meets all firmware
  resource requirements at half the cost and power of the F429.
- If the HMI display requires a framebuffer (TFT), ensure the MCU has enough
  SRAM for the buffer (e.g., 320×240×16bpp = 150 KB). An I²C OLED with internal
  controller (SSD1306) avoids this; SPI TFT with external GRAM (ILI9341) also
  avoids the framebuffer requirement.
- All estimates assume single-device-per-function. Using multi-channel
  ADC/DAC chips further reduces chip count and pin requirements. E.g.,
  a single 16-ch ADC (AD7606/ADS8688 family) reduces ADC CS from 8 to 1.

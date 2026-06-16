# AGENTS.md — HVB Firmware

Zephyr RTOS v3.7.2 firmware for Jianwei High Voltage Board (STM32F429BIT6). This repo is a west manifest/application repository in a T2-style workspace.

## Guide
- Keep low-level chip drivers hardware-shaped and policy-free.
- Put HVB product behavior above low-level drivers.
- Prefer Zephyr-standard APIs where they fit.
- Keep calibration, Modbus registers, ramping, safety policy, and NVM behavior outside chip drivers.
- Use PRDs and design specs to define observable behavior, not private implementation details.
- Use tests to verify public behavior and stable seams, not private helper functions.

## Spec-Driven Development Workflow

This project follows a spec-driven process. Do not jump from ref docs straight into implementation. Use this sequence:

1. Clarify behavior against `ref/` docs and resolve ambiguities
2. Define domain vocabulary (channel, active channel, safe state, etc.)
3. Design interfaces and compare alternatives
4. Write design spec / PRD, get approval
5. Split into vertical slices
6. Write implementation plan
7. Implement test-first (Zephyr `native_posix` or `qemu_cortex_m3` where HW isn't needed)
8. Verify before claiming completion


## Schematic Net Name vs. Actual Peripheral Mismatches

**This is the most common source of bugs.** The schematic uses functional net names that do not match STM32 alternate-function peripheral numbers. Always use the actual pin/peripheral mapping below, NOT the schematic net name:

| Schematic Net        | MCU Pin | Actual Peripheral | Notes |
|----------------------|---------|-------------------|-------|
| `USART1_TX` / `USART1_RX` | PC10 / PC11 | **USART3** | Debug console on J2 header. NOT USART1. DTS already uses `usart3`. |
| `MCU_HV2_DAC_SPI2_*` | PA4 / PA5 / PA7 | **SPI1** | HV2 DAC. NOT SPI2. PA4=SPI1_NSS, PA5=SPI1_SCK, PA7=SPI1_MOSI. |
| `MCU_HV1_ADC_SPI1_*` | PE11 / PE12 / PE13 | **SPI4** | HV1 ADS1232. NOT SPI1. Use GPIO bit-bang; ADS1232 is not a register SPI device. |
| `MCU_HV2_ADC_SPI1_*` | PF6 / PF7 / PF8 | **SPI5** | HV2 ADS1232. NOT SPI1. Use GPIO bit-bang. |
| `MCU_HV1_DAC_SPI2_*` | PB12 / PB13 / PB15 | **SPI2** | HV1 DAC. This one matches. PB12=SPI2_NSS, PB13=SPI2_SCK, PB15=SPI2_MOSI. |
| `UART_RX` / `UART_TX` | PG9 / PG14 | **USART6** | RS-485 interface. Already in DTS as `usart6`. |

Key peripherals:
- HV1 DAC: SPI2 on PB12(CS)/PB13(SCK)/PB15(MOSI)
- HV2 DAC: SPI1 on PA4(CS)/PA5(SCK)/PA7(MOSI) or GPIO bit-bang
- HV1 ADC (ADS1232): custom GPIO timing on PE11(CS)/PE12(SCLK)/PE13(MISO-DRDY)
- HV2 ADC (ADS1232): custom GPIO timing on PF6(CS)/PF7(SCLK)/PF8(MISO-DRDY)
- RS-485: USART6 on PG9(RX)/PG14(TX), direction on PG11
- Console/debug: USART3 on PC10(TX)/PC11(RX)
- SHT31: I2C1 on PB8(SCL)/PB9(SDA), addr 0x44
- HV run/stop: HV1=PD9, HV2=PC4 (safe=0 at boot)
- SYS_RUN LED: PI12
- SYS_MOD DIP: PI5(bit0), PI7(bit1); DIP ON = logic 0

## Code Conventions

- Zephyr includes use `<zephyr/...>` paths
- SPDX header: `SPDX-License-Identifier: Apache-2.0` (no extra commentary)
- Copyright: `Copyright (c) 2026 Jianwei`
- `CONFIG_*` symbols go in `prj.conf` per application
- Board-level defaults in `boards/jianwei/jw_hvb/jw_hvb_defconfig` and `Kconfig.defconfig`
- Custom devicetree bindings go in `dts/bindings/`
- `zephyr/module.yml` sets `board_root: .` and `dts_root: .` — boards and bindings are discovered from repo root

## Module Layout

```
applications/     Product firmware apps (each has CMakeLists.txt, prj.conf, src/)
demos/            Bring-up demos (same structure)
boards/jianwei/   Out-of-tree board definitions
dts/bindings/     Custom devicetree bindings
drivers/          Custom Zephyr drivers (add CMakeLists.txt + Kconfig when populated)
lib/              Shared firmware libraries (add CMakeLists.txt + Kconfig when populated)
include/          Public headers for shared code
ref/              Authoritative design documents and pin maps
tests/
docs/
tools/
```

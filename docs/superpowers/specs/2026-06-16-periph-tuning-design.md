# Peripheral Tuning Phase 1 — Design Spec

Date: 2026-06-16
Status: Approved

## Problem Statement

The MCU peripherals (SHT31 humidity sensor, AD5541 DAC, ADS1232 ADC) have no driver implementation yet. The main application (`hvb_controller`) currently feeds fake noise into the voltage-control domain. Before integrating real hardware into the product firmware, each peripheral must be brought up and verified in isolation using the debug console (USART3), without Modbus overhead.

## Solution

One combined demo app (`demos/periph_tune/`) with a Zephyr shell on USART3. Three custom out-of-tree drivers (AD5541 DAC, ADS1232 ADC sensor). SHT31 reuses the existing Zephyr `sht3xd` driver. HV modules default to disabled at boot; the technician enables them explicitly via shell before measuring actual output.

The tuning demo stays in `demos/` and is not part of any production build. Drivers live in `drivers/` and their DTS bindings in `dts/bindings/` so the main application can consume them later.

## Shell Commands

| Group | Command | Behavior |
|-------|---------|----------|
| **SHT31** | `sht31 read` | Single-shot fetch + print temp (°C) and humidity (%) |
| **HV Control** | `hv1 on` | Assert PD9=1 (HV1 run) |
| | `hv1 off` | Deassert PD9=0 (HV1 stop) |
| | `hv2 on` | Assert PC4=1 (HV2 run) |
| | `hv2 off` | Deassert PC4=0 (HV2 stop) |
| | `hv status` | Print HV state + current DAC code for both channels |
| **DAC** | `dac1 <0-65535>` | Write raw 16-bit code to HV1 AD5541 via SPI2 |
| | `dac2 <0-65535>` | Write raw 16-bit code to HV2 AD5541 via SPI1 |
| **ADC** | `adc1 read` | Trigger single ADS1232 conversion on HV1, print raw code + voltage |
| | `adc2 read` | Trigger single ADS1232 conversion on HV2, print raw code + voltage |
| | `adc1 gain <1|2|64|128>` | Set HV1 ADS1232 PGA gain via GAIN0/GAIN1 GPIOs |
| | `adc2 gain <1|2|64|128>` | Set HV2 ADS1232 PGA gain via GAIN0/GAIN1 GPIOs |
| | `adc status` | Print gain + last reading for both channels |

No shell command for SPEED (hardwired to 80 SPS via board pull-up, not MCU-controlled).

## Safety

- HV disabled at boot: PD9=0, PC4=0
- DAC initialized to code 0 (Vadj = 0V)
- No automatic HV toggle on DAC code change — technician controls HV manually
- ADC channel select defaults to A0/A1 = 00 (current sense, AINP1/AINN1) at init
- ADC gain defaults to 1 at init

## DTS Additions (Board File)

All added to `boards/jianwei/jw_hvb/jw_hvb.dts`:

```
/* I2C1 — SHT31 */
&i2c1 {
    pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
    pinctrl-names = "default";
    clock-frequency = <I2C_BITRATE_STANDARD>;
    status = "okay";
    sht3xd@44 {
        compatible = "sensirion,sht3xd";
        reg = <0x44>;
    };
};

/* SPI1 — HV2 AD5541 DAC */
&spi1 {
    pinctrl-0 = <&spi1_nss_pa4 &spi1_sck_pa5 &spi1_mosi_pa7>;
    pinctrl-names = "default";
    status = "okay";
    cs-gpios = <&gpioa 4 GPIO_ACTIVE_LOW>;
    dac_hv2: dac@0 {
        compatible = "adi,ad5541";
        reg = <0>;
        spi-max-frequency = <1000000>;
    };
};

/* SPI2 — HV1 AD5541 DAC */
&spi2 {
    pinctrl-0 = <&spi2_nss_pb12 &spi2_sck_pb13 &spi2_mosi_pb15>;
    pinctrl-names = "default";
    status = "okay";
    cs-gpios = <&gpiob 12 GPIO_ACTIVE_LOW>;
    dac_hv1: dac@0 {
        compatible = "adi,ad5541";
        reg = <0>;
        spi-max-frequency = <1000000>;
    };
};

/* HV1 ADS1232 ADC — GPIO bit-bang, no SPI peripheral */
ads1232_hv1 {
    compatible = "ti,ads1232";
    drdy-gpios = <&gpioe 13 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
    sclk-gpios = <&gpioe 12 GPIO_ACTIVE_HIGH>;
    pwdn-gpios = <&gpioe 11 GPIO_ACTIVE_LOW>;
    a0-gpios = <&gpioe 8 GPIO_ACTIVE_HIGH>;
    a1-gpios = <&gpioe 7 GPIO_ACTIVE_HIGH>;
    gain0-gpios = <&gpioh 12 GPIO_ACTIVE_HIGH>;
    gain1-gpios = <&gpioe 10 GPIO_ACTIVE_HIGH>;
};

/* HV2 ADS1232 ADC */
ads1232_hv2 {
    compatible = "ti,ads1232";
    drdy-gpios = <&gpiof 8 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
    sclk-gpios = <&gpiof 7 GPIO_ACTIVE_HIGH>;
    pwdn-gpios = <&gpiof 6 GPIO_ACTIVE_LOW>;
    a0-gpios = <&gpiof 3 GPIO_ACTIVE_HIGH>;
    a1-gpios = <&gpioi 14 GPIO_ACTIVE_HIGH>;
    gain0-gpios = <&gpiof 10 GPIO_ACTIVE_HIGH>;
    gain1-gpios = <&gpiof 4 GPIO_ACTIVE_HIGH>;
};
```

Plus pinctrl entries for I2C1 and SPI1/SPI2 pins in `&pinctrl`.

## Driver Summaries

### AD5541 DAC Driver (`drivers/dac/ad5541/`)

| Aspect | Detail |
|--------|--------|
| API | Zephyr DAC API (`<zephyr/drivers/dac.h>`): `dac_write_value()` |
| Resolution | 16-bit, 0–65535 |
| Interface | SPI write-only, 16-bit MSB-first frame, no MISO |
| Binding | `dts/bindings/dac/adi,ad5541.yaml` |
| Kconfig | `CONFIG_AD5541` |

### ADS1232 ADC Driver (`drivers/sensor/ads1232/`)

| Aspect | Detail |
|--------|--------|
| API | Zephyr Sensor API: `sensor_sample_fetch()`, `sensor_channel_get()` |
| Channels | `SENSOR_CHAN_VOLTAGE` (raw ADC code), optionally `SENSOR_CHAN_CURRENT` |
| Protocol | GPIO bit-bang: wait for DRDY low → 24 SCLK pulses → read DOUT |
| Gain control | GAIN0/GAIN1 GPIO output via sensor attribute `SENSOR_ATTR_GAIN` |
| Channel select | A0/A1 GPIO outputs |
| Binding | `dts/bindings/sensor/ti,ads1232.yaml` |
| Kconfig | `CONFIG_ADS1232` |

## Demo App Configuration (`demos/periph_tune/prj.conf`)

```
CONFIG_GPIO=y
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_DAC=y
CONFIG_AD5541=y
CONFIG_SENSOR=y
CONFIG_SHT3XD=y
CONFIG_ADS1232=y
CONFIG_SHELL=y
CONFIG_SHELL_CMDS=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_LOG=y
CONFIG_LOG_MODE_IMMEDIATE=y
```

## Logging Convention

- Demo shell handler code: `printk()` for human-readable output
- Driver code: `LOG_MODULE_REGISTER` + `LOG_DBG`/`LOG_ERR` for structured filtering

## Modbus

Not enabled. No `CONFIG_MODBUS`, no USART6 transport in this demo.

## Implementation Order

1. SHT31 DTS additions for I2C1 + `sht3xd` node
2. AD5541 DAC driver + binding + DTS additions for SPI1/SPI2 + HV GPIOs
3. ADS1232 ADC driver + binding + DTS additions for GPIO bit-bang nodes
4. Combined demo app with all shell commands
5. Board-level Kconfig for new driver symbols (`Kconfig.defconfig`)

## Out of Scope

- Modbus integration
- Voltage-control domain integration
- Calibration coefficient storage
- SPEED (hardware strap, not software-controllable)
- SHT31 alert/interrupt handling (PE0)
- SHT31 heater control

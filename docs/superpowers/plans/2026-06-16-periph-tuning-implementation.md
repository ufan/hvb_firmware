# Peripheral Tuning Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up SHT31 humidity sensor, AD5541 DAC, and ADS1232 ADC on STM32F429 with a single combined shell demo on USART3, no Modbus.

**Architecture:** Three Zephyr drivers (SHT31 uses built-in `sht3xd`, AD5541 is a custom DAC driver, ADS1232 is a custom GPIO bit-bang sensor driver). One demo app at `demos/periph_tune/` with Zephyr shell commands for each peripheral. DTS entries added to the board file. HV modules default disabled.

**Tech Stack:** Zephyr RTOS v3.7.2, STM32F429BIT6, GCC ARM Embedded

---

## File Structure

| File | Responsibility |
|------|---------------|
| `dts/bindings/dac/adi,ad5541.yaml` | AD5541 16-bit SPI DAC devicetree binding |
| `dts/bindings/sensor/ti,ads1232.yaml` | ADS1232 24-bit delta-sigma ADC devicetree binding |
| `drivers/Kconfig` | Sources sub-driver Kconfig files |
| `drivers/CMakeLists.txt` | Top-level CMake for all out-of-tree drivers |
| `drivers/dac/ad5541/Kconfig` | CONFIG_AD5541 symbol definition |
| `drivers/dac/ad5541/CMakeLists.txt` | AD5541 driver CMake |
| `drivers/dac/ad5541/ad5541.c` | AD5541 Zephyr DAC driver implementation |
| `drivers/sensor/ads1232/Kconfig` | CONFIG_ADS1232 symbol definition |
| `drivers/sensor/ads1232/CMakeLists.txt` | ADS1232 driver CMake |
| `drivers/sensor/ads1232/ads1232.c` | ADS1232 Zephyr sensor driver (GPIO bit-bang) |
| `demos/periph_tune/CMakeLists.txt` | Demo app CMake |
| `demos/periph_tune/prj.conf` | Demo app Kconfig |
| `demos/periph_tune/src/main.c` | Demo app: Zephyr shell init + all peripheral commands |
| `boards/jianwei/jw_hvb/jw_hvb.dts` | Board DTS: add I2C1, SPI1, SPI2, ADS1232 nodes + pinctrl |

---

### Task 1: SHT31 Temperature/Humidity — Board DTS + Demo Skeleton

**Files:**
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts` — add I2C1 + SHT31 pinctrl and node
- Create: `demos/periph_tune/CMakeLists.txt`
- Create: `demos/periph_tune/prj.conf`
- Create: `demos/periph_tune/src/main.c` — shell skeleton + SHT31 command

- [ ] **Step 1: Add I2C1 pinctrl and enable I2C1 node**

Append to `boards/jianwei/jw_hvb/jw_hvb.dts` inside the `&pinctrl` block:

```dts
	i2c1_scl_pb8: i2c1_scl_pb8 {
		pinmux = <STM32_PINMUX('B', 8, AF4)>;
	};

	i2c1_sda_pb9: i2c1_sda_pb9 {
		pinmux = <STM32_PINMUX('B', 9, AF4)>;
	};
```

- [ ] **Step 2: Add I2C1 node with SHT31 child**

Append after the `&usart3` block (before `&usart6`):

```dts
&i2c1 {
	pinctrl-0 = <&i2c1_scl_pb8 &i2c1_sda_pb9>;
	pinctrl-names = "default";
	clock-frequency = <I2C_BITRATE_STANDARD>;
	status = "okay";

	sht3xd_44: sht3xd@44 {
		compatible = "sensirion,sht3xd";
		reg = <0x44>;
	};
};
```

- [ ] **Step 3: Create demo app CMakeLists.txt**

Create `demos/periph_tune/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(periph_tune)

target_sources(app PRIVATE src/main.c)
```

- [ ] **Step 4: Create demo app prj.conf**

Create `demos/periph_tune/prj.conf`:

```
CONFIG_GPIO=y
CONFIG_SERIAL=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_LOG=y
CONFIG_LOG_MODE_IMMEDIATE=y
CONFIG_I2C=y
CONFIG_SENSOR=y
CONFIG_SHT3XD=y
CONFIG_SHELL=y
CONFIG_SHELL_CMDS=y
CONFIG_SHELL_LOG_BACKEND=n
```

- [ ] **Step 5: Create main.c with shell framework + SHT31 command**

Create `demos/periph_tune/src/main.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

/* SHT31 via existing Zephyr sht3xd driver on I2C1 */
#define SHT31_NODE DT_CHILD(DT_NODELABEL(i2c1), sht3xd_44)
static const struct device *sht31_dev = DEVICE_DT_GET_OR_NULL(SHT31_NODE);

static int cmd_sht31_read(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct sensor_value temp, hum;

	if (!device_is_ready(sht31_dev)) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 not ready\n");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(sht31_dev);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 fetch failed: %d\n", ret);
		return ret;
	}

	sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);

	shell_fprintf(sh, SHELL_NORMAL,
		"T=%.2f C  H=%.2f %%\n",
		sensor_value_to_double(&temp),
		sensor_value_to_double(&hum));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sht31,
	SHELL_CMD(read, NULL, "Read temperature and humidity", cmd_sht31_read),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sht31, &sub_sht31, "SHT31 temperature/humidity sensor", NULL);

int main(void)
{
	printk("=== periph_tune: jw_hvb peripheral tuning shell ===\n");

	if (!device_is_ready(sht31_dev)) {
		printk("SHT31 device not ready\n");
	} else {
		printk("SHT31 ready on I2C1 addr 0x44\n");
	}

	printk("Type 'help' for commands\n");
	printk("HV modules DISABLED at boot — use 'hv1 on' / 'hv2 on' to enable\n");
	return 0;
}
```

- [ ] **Step 6: Build and verify**

```bash
west build -b jianwei/jw_hvb demos/periph_tune -d build/periph_tune
```

Expected: Build succeeds with `sht31 read` shell command available.

- [ ] **Step 7: Commit**

```bash
git add boards/jianwei/jw_hvb/jw_hvb.dts \
        demos/periph_tune/CMakeLists.txt \
        demos/periph_tune/prj.conf \
        demos/periph_tune/src/main.c
git commit -m "feat: add SHT31 I2C1 DTS entry and periph_tune demo skeleton"
```

---

### Task 2: AD5541 DAC Driver + DTS + Shell Commands

**Files:**
- Create: `dts/bindings/dac/adi,ad5541.yaml`
- Create: `drivers/Kconfig`
- Create: `drivers/CMakeLists.txt`
- Create: `drivers/dac/ad5541/Kconfig`
- Create: `drivers/dac/ad5541/CMakeLists.txt`
- Create: `drivers/dac/ad5541/ad5541.c`
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts` — add SPI1/SPI2 pinctrl and nodes
- Modify: `demos/periph_tune/prj.conf` — add SPI, DAC, AD5541
- Modify: `demos/periph_tune/CMakeLists.txt` — add driver subdirectory
- Modify: `demos/periph_tune/src/main.c` — add HV GPIO + DAC shell commands

- [ ] **Step 1: Create AD5541 DTS binding**

Create `dts/bindings/dac/adi,ad5541.yaml`:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: AD5541 16-bit SPI DAC (write-only)

compatible: "adi,ad5541"

include: [base.yaml, spi-device.yaml]

properties:
  vref-mv:
    type: int
    default: 5000
    description: Reference voltage in mV
```

- [ ] **Step 2: Create drivers/Kconfig**

```kconfig
# SPDX-License-Identifier: Apache-2.0

rsource "dac/ad5541/Kconfig"
rsource "sensor/ads1232/Kconfig"
```

- [ ] **Step 3: Create AD5541 Kconfig**

Create `drivers/dac/ad5541/Kconfig`:

```kconfig
# SPDX-License-Identifier: Apache-2.0

config AD5541
	bool "AD5541 16-bit SPI DAC driver"
	default y
	depends on DT_HAS_ADI_AD5541_ENABLED
	select SPI
	help
	  Enable the AD5541 16-bit write-only SPI DAC driver.
```

- [ ] **Step 4: Create AD5541 driver CMakeLists.txt**

Create `drivers/dac/ad5541/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(ad5541.c)
```

- [ ] **Step 5: Create drivers/CMakeLists.txt** (top-level)

Create `drivers/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AD5541 dac/ad5541)
add_subdirectory_ifdef(CONFIG_ADS1232 sensor/ads1232)
```

- [ ] **Step 6: Create AD5541 driver implementation**

Create `drivers/dac/ad5541/ad5541.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT adi_ad5541

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ad5541, CONFIG_DAC_LOG_LEVEL);

struct ad5541_config {
	struct spi_dt_spec bus;
};

struct ad5541_data {
	uint32_t channel_count;
	uint32_t resolution;
};

static int ad5541_channel_setup(const struct device *dev,
				const struct dac_channel_cfg *channel_cfg)
{
	struct ad5541_data *data = dev->data;

	if (channel_cfg->channel_id != 0) {
		LOG_ERR("Unsupported channel %u", channel_cfg->channel_id);
		return -ENOTSUP;
	}
	if (channel_cfg->resolution != 16) {
		LOG_ERR("Unsupported resolution %u", channel_cfg->resolution);
		return -ENOTSUP;
	}

	data->channel_count = 1;
	data->resolution = channel_cfg->resolution;
	return 0;
}

static int ad5541_write_value(const struct device *dev, uint8_t channel,
			      uint32_t value)
{
	const struct ad5541_config *config = dev->config;
	uint16_t tx_data;

	if (channel != 0) {
		LOG_ERR("Unsupported channel %u", channel);
		return -ENOTSUP;
	}

	tx_data = sys_cpu_to_be16((uint16_t)(value & 0xFFFF));

	const struct spi_buf tx_buf = {
		.buf = &tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};

	return spi_write_dt(&config->bus, &tx);
}

static int ad5541_init(const struct device *dev)
{
	const struct ad5541_config *config = dev->config;
	struct ad5541_data *data = dev->data;

	if (!spi_is_ready_dt(&config->bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	data->channel_count = 1;
	data->resolution = 16;

	LOG_DBG("AD5541 initialized");
	return 0;
}

static const struct dac_driver_api ad5541_api = {
	.channel_setup = ad5541_channel_setup,
	.write_value = ad5541_write_value,
};

#define AD5541_INIT(n)							\
	static const struct ad5541_config ad5541_config_##n = {	\
		.bus = SPI_DT_SPEC_INST_GET(n,			\
			SPI_OP_MODE_MASTER |				\
			SPI_WORD_SET(16) |				\
			SPI_TRANSFER_MSB |				\
			SPI_LINES_SINGLE,				\
			0),						\
	};								\
									\
	static struct ad5541_data ad5541_data_##n;			\
									\
	DEVICE_DT_INST_DEFINE(n, ad5541_init, NULL,			\
		&ad5541_data_##n, &ad5541_config_##n,			\
		POST_KERNEL, CONFIG_DAC_INIT_PRIORITY,			\
		&ad5541_api);

DT_INST_FOREACH_STATUS_OKAY(AD5541_INIT)
```

- [ ] **Step 7: Add SPI1 and SPI2 pinctrl to board DTS**

Append to the `&pinctrl` block in `boards/jianwei/jw_hvb/jw_hvb.dts`:

```dts
	spi1_nss_pa4: spi1_nss_pa4 {
		pinmux = <STM32_PINMUX('A', 4, AF5)>;
	};

	spi1_sck_pa5: spi1_sck_pa5 {
		pinmux = <STM32_PINMUX('A', 5, AF5)>;
	};

	spi1_mosi_pa7: spi1_mosi_pa7 {
		pinmux = <STM32_PINMUX('A', 7, AF5)>;
	};

	spi2_nss_pb12: spi2_nss_pb12 {
		pinmux = <STM32_PINMUX('B', 12, AF5)>;
	};

	spi2_sck_pb13: spi2_sck_pb13 {
		pinmux = <STM32_PINMUX('B', 13, AF5)>;
	};

	spi2_mosi_pb15: spi2_mosi_pb15 {
		pinmux = <STM32_PINMUX('B', 15, AF5)>;
	};
```

- [ ] **Step 8: Add SPI1, SPI2 + AD5541 nodes to board DTS**

Append after the `&i2c1` block:

```dts
/* SPI1 — HV2 AD5541 DAC */
&spi1 {
	pinctrl-0 = <&spi1_nss_pa4 &spi1_sck_pa5 &spi1_mosi_pa7>;
	pinctrl-names = "default";
	cs-gpios = <&gpioa 4 GPIO_ACTIVE_LOW>;
	status = "okay";

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
	cs-gpios = <&gpiob 12 GPIO_ACTIVE_LOW>;
	status = "okay";

	dac_hv1: dac@0 {
		compatible = "adi,ad5541";
		reg = <0>;
		spi-max-frequency = <1000000>;
	};
};
```

Also enable GPIO ports C and D (needed for HV control). Add after `&gpioi`:

```dts
&gpioc {
	status = "okay";
};

&gpiod {
	status = "okay";
};
```

Also add HV enable GPIO nodes. Append inside the root `/ { ... };` block (after the `leds` node):

```dts
	hv1_en: hv1_en {
		gpios = <&gpiod 9 GPIO_ACTIVE_HIGH>;
	};

	hv2_en: hv2_en {
		gpios = <&gpioc 4 GPIO_ACTIVE_HIGH>;
	};
```

Already present: `&gpioa` and `&gpiob` are typically enabled by default for debug/SPI. If not, add `status = "okay"` for them too.

- [ ] **Step 9: Update demo app Kconfig for DAC**

Add to `demos/periph_tune/prj.conf`:

```
CONFIG_SPI=y
CONFIG_DAC=y
CONFIG_AD5541=y
```

- [ ] **Step 10: Update demo app CMakeLists.txt to include driver**

Replace `demos/periph_tune/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(periph_tune)

target_sources(app PRIVATE src/main.c)
add_subdirectory(../../drivers drivers)
```

- [ ] **Step 11: Add HV GPIO + DAC shell commands to demo main.c**

Replace `demos/periph_tune/src/main.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

/* SHT31 via existing Zephyr sht3xd driver on I2C1 */
#define SHT31_NODE DT_CHILD(DT_NODELABEL(i2c1), sht3xd_44)
static const struct device *sht31_dev = DEVICE_DT_GET_OR_NULL(SHT31_NODE);

/* DAC nodes for AD5541 */
#define DAC_HV1_NODE DT_NODELABEL(dac_hv1)
#define DAC_HV2_NODE DT_NODELABEL(dac_hv2)
static const struct device *dac_hv1 = DEVICE_DT_GET_OR_NULL(DAC_HV1_NODE);
static const struct device *dac_hv2 = DEVICE_DT_GET_OR_NULL(DAC_HV2_NODE);

/* HV run/stop GPIOs */
#define HV1_EN_NODE DT_NODELABEL(hv1_en)
#define HV2_EN_NODE DT_NODELABEL(hv2_en)
static const struct gpio_dt_spec hv1_en = GPIO_DT_SPEC_GET_OR(HV1_EN_NODE, gpios, {0});
static const struct gpio_dt_spec hv2_en = GPIO_DT_SPEC_GET_OR(HV2_EN_NODE, gpios, {0});

/* Saved DAC codes for status display */
static uint32_t dac1_code;
static uint32_t dac2_code;

/* ============== SHT31 ============== */

static int cmd_sht31_read(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	struct sensor_value temp, hum;

	if (!device_is_ready(sht31_dev)) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 not ready\n");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(sht31_dev);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 fetch failed: %d\n", ret);
		return ret;
	}

	sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);

	shell_fprintf(sh, SHELL_NORMAL,
		"T=%.2f C  H=%.2f %%\n",
		sensor_value_to_double(&temp),
		sensor_value_to_double(&hum));
	return 0;
}

/* ============== HV Control ============== */

static int cmd_hv1_on(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv1_en, 1);
	shell_fprintf(sh, SHELL_NORMAL, "HV1 ON (PD9=1)\n");
	return 0;
}

static int cmd_hv1_off(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv1_en, 0);
	shell_fprintf(sh, SHELL_NORMAL, "HV1 OFF (PD9=0)\n");
	return 0;
}

static int cmd_hv2_on(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv2_en, 1);
	shell_fprintf(sh, SHELL_NORMAL, "HV2 ON (PC4=1)\n");
	return 0;
}

static int cmd_hv2_off(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv2_en, 0);
	shell_fprintf(sh, SHELL_NORMAL, "HV2 OFF (PC4=0)\n");
	return 0;
}

static int cmd_hv_status(const struct shell *sh, size_t argc, char **argv)
{
	int hv1_state = gpio_pin_get_dt(&hv1_en);
	int hv2_state = gpio_pin_get_dt(&hv2_en);

	shell_fprintf(sh, SHELL_NORMAL,
		"HV1: %s (PD9=%d)  DAC1: %u\n"
		"HV2: %s (PC4=%d)  DAC2: %u\n",
		hv1_state ? "ON" : "OFF", hv1_state, dac1_code,
		hv2_state ? "ON" : "OFF", hv2_state, dac2_code);
	return 0;
}

/* ============== DAC ============== */

static int cmd_dac1(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t code;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: dac1 <0-65535>\n");
		return -EINVAL;
	}

	code = strtoul(argv[1], NULL, 0);
	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "Code out of range (0-65535)\n");
		return -EINVAL;
	}

	if (!device_is_ready(dac_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC1 not ready\n");
		return -ENODEV;
	}

	/* 0V initial state: ensure dac_channel_cfg done once */
	dac_write_value(dac_hv1, DAC_CHANNEL_0, code);
	dac1_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC1: %u (%.3f V)\n", code,
		code * 5.0 / 65535.0);
	return 0;
}

static int cmd_dac2(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t code;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: dac2 <0-65535>\n");
		return -EINVAL;
	}

	code = strtoul(argv[1], NULL, 0);
	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "Code out of range (0-65535)\n");
		return -EINVAL;
	}

	if (!device_is_ready(dac_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC2 not ready\n");
		return -ENODEV;
	}

	dac_write_value(dac_hv2, DAC_CHANNEL_0, code);
	dac2_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC2: %u (%.3f V)\n", code,
		code * 5.0 / 65535.0);
	return 0;
}

/* ============== Shell Registration ============== */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sht31,
	SHELL_CMD(read, NULL, "Read temperature and humidity", cmd_sht31_read),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hv1,
	SHELL_CMD(on, NULL, "Enable HV1 output", cmd_hv1_on),
	SHELL_CMD(off, NULL, "Disable HV1 output", cmd_hv1_off),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hv2,
	SHELL_CMD(on, NULL, "Enable HV2 output", cmd_hv2_on),
	SHELL_CMD(off, NULL, "Disable HV2 output", cmd_hv2_off),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sht31, &sub_sht31, "SHT31 temperature/humidity sensor", NULL);
SHELL_CMD_REGISTER(hv1, &sub_hv1, "HV1 run/stop control", NULL);
SHELL_CMD_REGISTER(hv2, &sub_hv2, "HV2 run/stop control", NULL);
SHELL_CMD_REGISTER(hv_status, NULL, "Show HV state and DAC codes", cmd_hv_status);
SHELL_CMD_REGISTER(dac1, NULL, "Set HV1 DAC code <0-65535>", cmd_dac1);
SHELL_CMD_REGISTER(dac2, NULL, "Set HV2 DAC code <0-65535>", cmd_dac2);

int main(void)
{
	int ret;

	printk("=== periph_tune: jw_hvb peripheral tuning shell ===\n");

	if (!device_is_ready(sht31_dev)) {
		printk("SHT31: NOT READY\n");
	} else {
		printk("SHT31: ready on I2C1 addr 0x44\n");
	}

	if (!device_is_ready(dac_hv1)) {
		printk("DAC1: NOT READY\n");
	} else {
		printk("DAC1: ready on SPI2 PB12/PB13/PB15\n");
	}

	if (!device_is_ready(dac_hv2)) {
		printk("DAC2: NOT READY\n");
	} else {
		printk("DAC2: ready on SPI1 PA4/PA5/PA7\n");
	}

	/* Init HV control GPIOs — safe = off */
	ret = gpio_pin_configure_dt(&hv1_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("HV1 GPIO config failed: %d\n", ret);
	}

	ret = gpio_pin_configure_dt(&hv2_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("HV2 GPIO config failed: %d\n", ret);
	}

	/* Ensure DAC output starts at 0V */
	struct dac_channel_cfg dac_cfg = {
		.channel_id = DAC_CHANNEL_0,
		.resolution = 16,
	};

	if (device_is_ready(dac_hv1)) {
		dac_channel_setup(dac_hv1, &dac_cfg);
		dac_write_value(dac_hv1, DAC_CHANNEL_0, 0);
		dac1_code = 0;
	}

	if (device_is_ready(dac_hv2)) {
		dac_channel_setup(dac_hv2, &dac_cfg);
		dac_write_value(dac_hv2, DAC_CHANNEL_0, 0);
		dac2_code = 0;
	}

	printk("HV modules: DISABLED at boot (use 'hv1 on'/'hv2 on' to enable)\n");
	printk("Type 'help' for commands\n");
	return 0;
}
```

- [ ] **Step 12: Build and verify**

```bash
west build -b jianwei/jw_hvb demos/periph_tune -d build/periph_tune
```

Expected: Build succeeds. Shell has `sht31 read`, `hv1 on|off`, `hv2 on|off`, `hv status`, `dac1`, `dac2`.

- [ ] **Step 13: Commit**

```bash
git add dts/bindings/dac/adi,ad5541.yaml \
        drivers/ \
        demos/periph_tune/CMakeLists.txt \
        demos/periph_tune/prj.conf \
        demos/periph_tune/src/main.c \
        boards/jianwei/jw_hvb/jw_hvb.dts
git commit -m "feat: add AD5541 DAC driver, SPI1/SPI2 DTS, HV control and DAC shell"
```

---

### Task 3: ADS1232 ADC Driver + DTS + Shell Commands

**Files:**
- Create: `dts/bindings/sensor/ti,ads1232.yaml`
- Create: `drivers/sensor/ads1232/Kconfig`
- Create: `drivers/sensor/ads1232/CMakeLists.txt`
- Create: `drivers/sensor/ads1232/ads1232.c`
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts` — add ADS1232 nodes
- Modify: `demos/periph_tune/prj.conf` — add ADS1232
- Modify: `demos/periph_tune/src/main.c` — add ADC shell commands

- [ ] **Step 1: Create ADS1232 DTS binding**

Create `dts/bindings/sensor/ti,ads1232.yaml`:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: TI ADS1232 24-bit Delta-Sigma ADC

compatible: "ti,ads1232"

include: [base.yaml]

properties:
  drdy-gpios:
    type: phandle-array
    required: true
    description: Data ready / data out pin (active low DRDY, then DOUT)

  sclk-gpios:
    type: phandle-array
    required: true
    description: Serial clock output

  pwdn-gpios:
    type: phandle-array
    required: true
    description: Power down / chip select (active low)

  a0-gpios:
    type: phandle-array
    required: false
    description: Channel select A0

  a1-gpios:
    type: phandle-array
    required: false
    description: Channel select A1

  gain0-gpios:
    type: phandle-array
    required: false
    description: PGA gain select GAIN0

  gain1-gpios:
    type: phandle-array
    required: false
    description: PGA gain select GAIN1
```

- [ ] **Step 2: Create ADS1232 Kconfig**

Create `drivers/sensor/ads1232/Kconfig`:

```kconfig
# SPDX-License-Identifier: Apache-2.0

config ADS1232
	bool "ADS1232 24-bit delta-sigma ADC driver"
	default y
	depends on DT_HAS_TI_ADS1232_ENABLED
	help
	  Enable the TI ADS1232 24-bit delta-sigma ADC driver.
	  Uses GPIO bit-bang protocol (not SPI).
```

- [ ] **Step 3: Create ADS1232 driver CMakeLists.txt**

Create `drivers/sensor/ads1232/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(ads1232.c)
```

- [ ] **Step 4: Create ADS1232 driver implementation**

Create `drivers/sensor/ads1232/ads1232.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_ads1232

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ads1232, CONFIG_SENSOR_LOG_LEVEL);

/* PGA gain lookup: GAIN1|GAIN0 -> gain value */
/* 00=1, 01=2, 10=64, 11=128 */
static const uint8_t gain_table[4] = {1, 2, 64, 128};

struct ads1232_config {
	struct gpio_dt_spec drdy;
	struct gpio_dt_spec sclk;
	struct gpio_dt_spec pwdn;
	struct gpio_dt_spec a0;
	struct gpio_dt_spec a1;
	struct gpio_dt_spec gain0;
	struct gpio_dt_spec gain1;
};

struct ads1232_data {
	int32_t raw_sample;
	uint8_t pga_gain;   /* 1, 2, 64, or 128 */
};

static int ads1232_set_gain(const struct device *dev, uint8_t gain)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	uint8_t ga0, ga1;

	switch (gain) {
	case 1:   ga0 = 0; ga1 = 0; break;
	case 2:   ga0 = 1; ga1 = 0; break;
	case 64:  ga0 = 0; ga1 = 1; break;
	case 128: ga0 = 1; ga1 = 1; break;
	default:
		LOG_ERR("Unsupported PGA gain %u", gain);
		return -EINVAL;
	}

	if (!cfg->gain0.port || !cfg->gain1.port) {
		LOG_WRN("Gain GPIOs not configured, gain fixed");
		return 0;
	}

	gpio_pin_set_dt(&cfg->gain0, ga0);
	gpio_pin_set_dt(&cfg->gain1, ga1);
	data->pga_gain = gain;

	LOG_DBG("PGA gain set to %u", gain);
	return 0;
}

static int ads1232_read_raw(const struct device *dev, int32_t *result)
{
	const struct ads1232_config *cfg = dev->config;
	int32_t val = 0;
	int timeout;

	/* Exit power-down */
	gpio_pin_set_dt(&cfg->pwdn, 0);
	k_sleep(K_MSEC(1));

	/* Wait for DRDY low (conversion complete) */
	timeout = 100;
	while (gpio_pin_get_dt(&cfg->drdy) && timeout > 0) {
		k_sleep(K_USEC(1000));
		timeout--;
	}
	if (timeout == 0) {
		LOG_ERR("DRDY timeout");
		gpio_pin_set_dt(&cfg->pwdn, 1);
		return -ETIMEDOUT;
	}

	/* Clock out 24 bits on SCLK falling edge, read DOUT */
	for (int i = 0; i < 24; i++) {
		gpio_pin_set_dt(&cfg->sclk, 1);
		k_busy_wait(1);
		gpio_pin_set_dt(&cfg->sclk, 0);
		k_busy_wait(1);
		val = (val << 1) | gpio_pin_get_dt(&cfg->drdy);
	}

	/* Enter power-down */
	gpio_pin_set_dt(&cfg->pwdn, 1);

	/* Sign-extend 24-bit to 32-bit */
	if (val & 0x800000) {
		val |= 0xFF000000;
	}

	*result = val;
	return 0;
}

static int ads1232_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct ads1232_data *data = dev->data;
	return ads1232_read_raw(dev, &data->raw_sample);
}

static int ads1232_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct ads1232_data *data = dev->data;
	int32_t raw = data->raw_sample;
	int64_t uv;

	if (chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	/* Raw ADC code to microvolts: (raw * Vref * 1e6) / (2^23 * gain)
	 * Vref = 5.0V, so: uv = raw * 5000000 / (8388608 * gain)
	 * Simplified: uv = raw * 5000 / (8388608UL * gain) * 1000
	 */
	uv = (int64_t)raw * 5000000LL;
	uv /= (int64_t)(8388608ULL * data->pga_gain);

	val->val1 = (int32_t)(uv / 1000000LL);
	val->val2 = (int32_t)(uv % 1000000LL);
	return 0;
}

static int ads1232_attr_set(const struct device *dev, enum sensor_channel chan,
			    enum sensor_attribute attr, const struct sensor_value *val)
{
	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	if (attr == SENSOR_ATTR_GAIN) {
		return ads1232_set_gain(dev, (uint8_t)val->val1);
	}

	return -ENOTSUP;
}

static int ads1232_init(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->drdy.port)) {
		LOG_ERR("DRDY GPIO not ready");
		return -ENODEV;
	}

	/* Configure GPIOs */
	gpio_pin_configure_dt(&cfg->sclk, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cfg->pwdn, GPIO_OUTPUT_INACTIVE);

	/* DRDY is open-drain with pull-up — input, active low */
	gpio_pin_configure_dt(&cfg->drdy, GPIO_INPUT);

	/* Power-down on init */
	gpio_pin_set_dt(&cfg->pwdn, 1);

	/* Configure gain GPIOs if present */
	if (cfg->gain0.port) {
		gpio_pin_configure_dt(&cfg->gain0, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->gain1.port) {
		gpio_pin_configure_dt(&cfg->gain1, GPIO_OUTPUT_INACTIVE);
	}

	/* Configure channel select GPIOs if present */
	if (cfg->a0.port) {
		gpio_pin_configure_dt(&cfg->a0, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->a1.port) {
		gpio_pin_configure_dt(&cfg->a1, GPIO_OUTPUT_INACTIVE);
	}

	/* Default PGA gain = 1 */
	data->pga_gain = 1;
	if (cfg->gain0.port && cfg->gain1.port) {
		gpio_pin_set_dt(&cfg->gain0, 0);
		gpio_pin_set_dt(&cfg->gain1, 0);
	}

	LOG_DBG("ADS1232 initialized, gain=%u", data->pga_gain);
	return 0;
}

static const struct sensor_driver_api ads1232_api = {
	.sample_fetch = ads1232_sample_fetch,
	.channel_get = ads1232_channel_get,
	.attr_set = ads1232_attr_set,
};

#define ADS1232_INIT(n)							\
	static const struct ads1232_config ads1232_config_##n = {	\
		.drdy = GPIO_DT_SPEC_INST_GET(n, drdy_gpios),		\
		.sclk = GPIO_DT_SPEC_INST_GET(n, sclk_gpios),		\
		.pwdn = GPIO_DT_SPEC_INST_GET(n, pwdn_gpios),		\
		.a0 = GPIO_DT_SPEC_INST_GET_OR(n, a0_gpios, {0}),	\
		.a1 = GPIO_DT_SPEC_INST_GET_OR(n, a1_gpios, {0}),	\
		.gain0 = GPIO_DT_SPEC_INST_GET_OR(n, gain0_gpios, {0}),\
		.gain1 = GPIO_DT_SPEC_INST_GET_OR(n, gain1_gpios, {0}),\
	};									\
										\
	static struct ads1232_data ads1232_data_##n;				\
										\
	DEVICE_DT_INST_DEFINE(n, ads1232_init, NULL,				\
		&ads1232_data_##n, &ads1232_config_##n,				\
		POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,			\
		&ads1232_api);

DT_INST_FOREACH_STATUS_OKAY(ADS1232_INIT)
```

- [ ] **Step 5: Add ADS1232 nodes to board DTS**

Append after the `&spi2` block in `boards/jianwei/jw_hvb/jw_hvb.dts`:

```dts
/* HV1 ADS1232 ADC — GPIO bit-bang, not SPI */
ads1232_hv1: ads1232_hv1 {
	compatible = "ti,ads1232";
	status = "okay";
	drdy-gpios = <&gpioe 13 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
	sclk-gpios = <&gpioe 12 GPIO_ACTIVE_HIGH>;
	pwdn-gpios = <&gpioe 11 GPIO_ACTIVE_LOW>;
	a0-gpios = <&gpioe 8 GPIO_ACTIVE_HIGH>;
	a1-gpios = <&gpioe 7 GPIO_ACTIVE_HIGH>;
	gain0-gpios = <&gpioh 12 GPIO_ACTIVE_HIGH>;
	gain1-gpios = <&gpioe 10 GPIO_ACTIVE_HIGH>;
};

/* HV2 ADS1232 ADC */
ads1232_hv2: ads1232_hv2 {
	compatible = "ti,ads1232";
	status = "okay";
	drdy-gpios = <&gpiof 8 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
	sclk-gpios = <&gpiof 7 GPIO_ACTIVE_HIGH>;
	pwdn-gpios = <&gpiof 6 GPIO_ACTIVE_LOW>;
	a0-gpios = <&gpiof 3 GPIO_ACTIVE_HIGH>;
	a1-gpios = <&gpioi 14 GPIO_ACTIVE_HIGH>;
	gain0-gpios = <&gpiof 10 GPIO_ACTIVE_HIGH>;
	gain1-gpios = <&gpiof 4 GPIO_ACTIVE_HIGH>;
};
```

Enable GPIO ports E, F, H in the DTS (add after existing GPIO enable blocks):

```dts
&gpioe {
	status = "okay";
};

&gpiof {
	status = "okay";
};

&gpioh {
	status = "okay";
};
```

- [ ] **Step 6: Update demo Kconfig for ADC**

Add to `demos/periph_tune/prj.conf`:

```
CONFIG_ADS1232=y
```

- [ ] **Step 7: Add ADC shell commands to demo main.c**

Insert in `demos/periph_tune/src/main.c` after the DAC section, before `/* ============== Shell Registration ==============`:

```c
/* ============== ADC ============== */

/* ADC nodes */
#define ADC_HV1_NODE DT_NODELABEL(ads1232_hv1)
#define ADC_HV2_NODE DT_NODELABEL(ads1232_hv2)
static const struct device *adc_hv1 = DEVICE_DT_GET_OR_NULL(ADC_HV1_NODE);
static const struct device *adc_hv2 = DEVICE_DT_GET_OR_NULL(ADC_HV2_NODE);

static uint8_t adc1_gain_val = 1;
static int32_t adc1_last_raw;
static uint8_t adc2_gain_val = 1;
static int32_t adc2_last_raw;

static int cmd_adc1_read(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value val;
	int ret;

	if (!device_is_ready(adc_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "ADC1 not ready\n");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(adc_hv1);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "ADC1 fetch failed: %d\n", ret);
		return ret;
	}

	sensor_channel_get(adc_hv1, SENSOR_CHAN_VOLTAGE, &val);
	adc1_last_raw = val.val1;

	shell_fprintf(sh, SHELL_NORMAL,
		"ADC1: raw=%d  %.3f V  (gain=%u)\n",
		val.val1, sensor_value_to_double(&val), adc1_gain_val);
	return 0;
}

static int cmd_adc2_read(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value val;
	int ret;

	if (!device_is_ready(adc_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "ADC2 not ready\n");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(adc_hv2);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "ADC2 fetch failed: %d\n", ret);
		return ret;
	}

	sensor_channel_get(adc_hv2, SENSOR_CHAN_VOLTAGE, &val);
	adc2_last_raw = val.val1;

	shell_fprintf(sh, SHELL_NORMAL,
		"ADC2: raw=%d  %.3f V  (gain=%u)\n",
		val.val1, sensor_value_to_double(&val), adc2_gain_val);
	return 0;
}

static int cmd_adc1_gain(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t gain;
	struct sensor_value val;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: adc1 gain <1|2|64|128>\n");
		return -EINVAL;
	}

	gain = strtoul(argv[1], NULL, 0);
	if (gain != 1 && gain != 2 && gain != 64 && gain != 128) {
		shell_fprintf(sh, SHELL_ERROR,
			"Invalid gain %u (valid: 1, 2, 64, 128)\n", gain);
		return -EINVAL;
	}

	if (!device_is_ready(adc_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "ADC1 not ready\n");
		return -ENODEV;
	}

	val.val1 = (int32_t)gain;
	val.val2 = 0;
	sensor_attr_set(adc_hv1, SENSOR_CHAN_ALL, SENSOR_ATTR_GAIN, &val);
	adc1_gain_val = (uint8_t)gain;

	shell_fprintf(sh, SHELL_NORMAL, "ADC1 gain set to %u\n", gain);
	return 0;
}

static int cmd_adc2_gain(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t gain;
	struct sensor_value val;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: adc2 gain <1|2|64|128>\n");
		return -EINVAL;
	}

	gain = strtoul(argv[1], NULL, 0);
	if (gain != 1 && gain != 2 && gain != 64 && gain != 128) {
		shell_fprintf(sh, SHELL_ERROR,
			"Invalid gain %u (valid: 1, 2, 64, 128)\n", gain);
		return -EINVAL;
	}

	if (!device_is_ready(adc_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "ADC2 not ready\n");
		return -ENODEV;
	}

	val.val1 = (int32_t)gain;
	val.val2 = 0;
	sensor_attr_set(adc_hv2, SENSOR_CHAN_ALL, SENSOR_ATTR_GAIN, &val);
	adc2_gain_val = (uint8_t)gain;

	shell_fprintf(sh, SHELL_NORMAL, "ADC2 gain set to %u\n", gain);
	return 0;
}

static int cmd_adc_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_fprintf(sh, SHELL_NORMAL,
		"ADC1: gain=%u  last_raw=%d\n"
		"ADC2: gain=%u  last_raw=%d\n",
		adc1_gain_val, adc1_last_raw,
		adc2_gain_val, adc2_last_raw);
	return 0;
}
```

- [ ] **Step 8: Register ADC shell subcommands (append before main())**

Add these registration lines before `int main(void)`:

```c
SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc1,
	SHELL_CMD(read, NULL, "Read ADC1 raw value", cmd_adc1_read),
	SHELL_CMD(gain, NULL, "Set ADC1 PGA gain <1|2|64|128>", cmd_adc1_gain),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc2,
	SHELL_CMD(read, NULL, "Read ADC2 raw value", cmd_adc2_read),
	SHELL_CMD(gain, NULL, "Set ADC2 PGA gain <1|2|64|128>", cmd_adc2_gain),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(adc1, &sub_adc1, "ADS1232 ADC channel 1", NULL);
SHELL_CMD_REGISTER(adc2, &sub_adc2, "ADS1232 ADC channel 2", NULL);
SHELL_CMD_REGISTER(adc_status, NULL, "Show ADC configuration", cmd_adc_status);
```

- [ ] **Step 9: Add ADC init and ready check to main()**

Insert into `main()` before `printk("HV modules: DISABLED...\n")`:

```c
	if (!device_is_ready(adc_hv1)) {
		printk("ADC1: NOT READY\n");
	} else {
		printk("ADC1: ready on PE11/PE12/PE13 (GPIO bit-bang)\n");
	}

	if (!device_is_ready(adc_hv2)) {
		printk("ADC2: NOT READY\n");
	} else {
		printk("ADC2: ready on PF6/PF7/PF8 (GPIO bit-bang)\n");
	}
```

- [ ] **Step 10: Add include for sensor_attr header**

The `sensor_attr_set` function needs `<zephyr/drivers/sensor.h>` — already included.

- [ ] **Step 11: Build and verify**

```bash
west build -b jianwei/jw_hvb demos/periph_tune -d build/periph_tune
```

Expected: Build succeeds. Full shell with all commands: `sht31 read`, `hv1 on|off`, `hv2 on|off`, `hv status`, `dac1`, `dac2`, `adc1 read|gain`, `adc2 read|gain`, `adc status`.

- [ ] **Step 12: Commit**

```bash
git add dts/bindings/sensor/ti,ads1232.yaml \
        drivers/sensor/ads1232/ \
        boards/jianwei/jw_hvb/jw_hvb.dts \
        demos/periph_tune/prj.conf \
        demos/periph_tune/src/main.c
git commit -m "feat: add ADS1232 ADC driver, GPIO bit-bang DTS nodes, ADC shell commands"
```

---

### Task 4: Final Integration — Verify & Document

**Files:**
- Modify: none (verification only)

- [ ] **Step 1: Clean build**

```bash
rm -rf build/periph_tune
west build -b jianwei/jw_hvb demos/periph_tune -d build/periph_tune
```

Expected: Build succeeds, zero warnings.

- [ ] **Step 2: Check binary size**

```bash
ls -la build/periph_tune/zephyr/zephyr.elf
```

Expected: Fits comfortably in STM32F429 (2MB Flash).

- [ ] **Step 3: Flash and verify shell commands**

```bash
west flash -d build/periph_tune
```

Connect to USART3 at 115200 baud. Verify shell prompt appears and commands respond:

```
uart:~$ help
uart:~$ sht31 read
uart:~$ hv status
uart:~$ dac1 32768
uart:~$ dac2 32768
uart:~$ adc1 read
uart:~$ adc2 read
uart:~$ adc1 gain 64
uart:~$ adc1 read
```

- [ ] **Step 4: Verify HV safety defaults**

On boot, confirm output:
```
HV modules: DISABLED at boot (use 'hv1 on'/'hv2 on' to enable)
```

- [ ] **Step 5: Commit (final)**

```bash
git commit --allow-empty -m "feat: periph_tune demo complete — SHT31, AD5541 DAC, ADS1232 ADC all operational via shell"
```

---

## Self-Review Summary

| Check | Status |
|-------|--------|
| Spec coverage: SHT31, AD5541 DAC, ADS1232 ADC drivers + DTS + shell demo | All covered |
| Placeholder scan | None — every code block is complete |
| Type consistency: `dac1_code`/`dac2_code` used in both hv_status and DAC cmds | Consistent |
| SHT31 uses Zephyr built-in driver (no custom code) | Matches spec |
| AD5541: SPI1 (HV2) and SPI2 (HV1), CS via GPIO | Matches pin map |
| ADS1232: GPIO bit-bang, DRDY/DOUT shared pin, gain via GAIN0/1, SPEED hardwired | Matches board design |
| HV default disabled at boot | Matches spec |
| Shell commands match approved design | Matches spec |
| Implementation order: SHT31 → DAC → ADC | Matches spec |
| Modbus not included | Matches spec |

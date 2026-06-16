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

/* DAC nodes */
#define DAC_HV1_NODE DT_NODELABEL(dac_hv1)
#define DAC_HV2_NODE DT_NODELABEL(dac_hv2)
static const struct device *dac_hv1 = DEVICE_DT_GET_OR_NULL(DAC_HV1_NODE);
static const struct device *dac_hv2 = DEVICE_DT_GET_OR_NULL(DAC_HV2_NODE);

/* HV run/stop GPIOs */
#define HV1_EN_NODE DT_NODELABEL(hv1_en)
#define HV2_EN_NODE DT_NODELABEL(hv2_en)
static const struct gpio_dt_spec hv1_en = GPIO_DT_SPEC_GET_OR(HV1_EN_NODE, gpios, {0});
static const struct gpio_dt_spec hv2_en = GPIO_DT_SPEC_GET_OR(HV2_EN_NODE, gpios, {0});

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
		shell_fprintf(sh, SHELL_ERROR, "fetch failed: %d\n", ret);
		return ret;
	}

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "temp read failed: %d\n", ret);
		return ret;
	}

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "hum read failed: %d\n", ret);
		return ret;
	}

	shell_fprintf(sh, SHELL_NORMAL, "T=%.2f C  H=%.2f %%\n",
		sensor_value_to_double(&temp), sensor_value_to_double(&hum));
	return 0;
}

/* ============== HV Control ============== */

static int cmd_hv1_on(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv1_en, 1);
	shell_fprintf(sh, SHELL_NORMAL, "HV1 ON\n");
	return 0;
}

static int cmd_hv1_off(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv1_en, 0);
	shell_fprintf(sh, SHELL_NORMAL, "HV1 OFF\n");
	return 0;
}

static int cmd_hv2_on(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv2_en, 1);
	shell_fprintf(sh, SHELL_NORMAL, "HV2 ON\n");
	return 0;
}

static int cmd_hv2_off(const struct shell *sh, size_t argc, char **argv)
{
	gpio_pin_set_dt(&hv2_en, 0);
	shell_fprintf(sh, SHELL_NORMAL, "HV2 OFF\n");
	return 0;
}

static int cmd_hv_status(const struct shell *sh, size_t argc, char **argv)
{
	int hv1_state = gpio_pin_get_dt(&hv1_en);
	int hv2_state = gpio_pin_get_dt(&hv2_en);

	shell_fprintf(sh, SHELL_NORMAL,
		"HV1: %s  DAC1: %u\n"
		"HV2: %s  DAC2: %u\n",
		hv1_state > 0 ? "ON" : "OFF", dac1_code,
		hv2_state > 0 ? "ON" : "OFF", dac2_code);
	return 0;
}

/* ============== DAC ============== */

static int cmd_dac1(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	uint32_t code;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: dac1 <0-65535>\n");
		return -EINVAL;
	}
	code = strtoul(argv[1], NULL, 0);
	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "out of range\n");
		return -EINVAL;
	}
	if (!device_is_ready(dac_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC1 not ready\n");
		return -ENODEV;
	}
	ret = dac_write_value(dac_hv1, 0, code);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac1_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC1: %u (%.3f V)\n", code, code * 5.0 / 65535.0);
	return 0;
}

static int cmd_dac2(const struct shell *sh, size_t argc, char **argv)
{
	int ret;
	uint32_t code;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR, "Usage: dac2 <0-65535>\n");
		return -EINVAL;
	}
	code = strtoul(argv[1], NULL, 0);
	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "out of range\n");
		return -EINVAL;
	}
	if (!device_is_ready(dac_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC2 not ready\n");
		return -ENODEV;
	}
	ret = dac_write_value(dac_hv2, 0, code);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac2_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC2: %u (%.3f V)\n", code, code * 5.0 / 65535.0);
	return 0;
}

/* ============== Shell Registration ============== */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sht31,
	SHELL_CMD(read, NULL, "Read temperature and humidity", cmd_sht31_read),
	SHELL_SUBCMD_SET_END
);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_hv1,
	SHELL_CMD(on, NULL, "Enable HV1", cmd_hv1_on),
	SHELL_CMD(off, NULL, "Disable HV1", cmd_hv1_off),
	SHELL_SUBCMD_SET_END
);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_hv2,
	SHELL_CMD(on, NULL, "Enable HV2", cmd_hv2_on),
	SHELL_CMD(off, NULL, "Disable HV2", cmd_hv2_off),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sht31, &sub_sht31, "SHT31 sensor", NULL);
SHELL_CMD_REGISTER(hv1, &sub_hv1, "HV1 run/stop", NULL);
SHELL_CMD_REGISTER(hv2, &sub_hv2, "HV2 run/stop", NULL);
SHELL_CMD_REGISTER(hv_status, NULL, "HV status", cmd_hv_status);
SHELL_CMD_REGISTER(dac1, NULL, "Set HV1 DAC <0-65535>", cmd_dac1);
SHELL_CMD_REGISTER(dac2, NULL, "Set HV2 DAC <0-65535>", cmd_dac2);

int main(void)
{
	int ret;

	printk("=== periph_tune ===\n");

	if (!device_is_ready(sht31_dev)) {
		printk("SHT31: NOT READY\n");
	} else {
		printk("SHT31: ready on I2C1\n");
	}

	if (!device_is_ready(dac_hv1)) {
		printk("DAC1: NOT READY\n");
	} else {
		printk("DAC1: ready on SPI2\n");
	}

	if (!device_is_ready(dac_hv2)) {
		printk("DAC2: NOT READY\n");
	} else {
		printk("DAC2: ready on SPI1\n");
	}

	gpio_pin_configure_dt(&hv1_en, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&hv2_en, GPIO_OUTPUT_INACTIVE);

	struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };

	if (device_is_ready(dac_hv1)) {
		dac_channel_setup(dac_hv1, &dac_cfg);
		dac_write_value(dac_hv1, 0, 0);
	}

	if (device_is_ready(dac_hv2)) {
		dac_channel_setup(dac_hv2, &dac_cfg);
		dac_write_value(dac_hv2, 0, 0);
	}

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

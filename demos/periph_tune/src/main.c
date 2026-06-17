/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

/* ============== Watch support ============== */

static volatile bool watch_stop;

static void watch_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	watch_stop = true;
	shell_set_bypass(sh, NULL);
}

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

static int cmd_sht31_watch(const struct shell *sh, size_t argc, char **argv)
{
	int interval_ms = 1000;

	if (argc >= 2) {
		interval_ms = atoi(argv[1]);
		if (interval_ms < 100) {
			interval_ms = 100;
		}
	}

	if (!device_is_ready(sht31_dev)) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 not ready\n");
		return -ENODEV;
	}

	watch_stop = false;
	shell_set_bypass(sh, watch_bypass_cb);
	shell_fprintf(sh, SHELL_NORMAL, "press any key to stop\n");

	while (!watch_stop) {
		struct sensor_value temp, hum;
		int ret = sensor_sample_fetch(sht31_dev);

		if (ret < 0) {
			shell_fprintf(sh, SHELL_ERROR, "fetch failed: %d\n", ret);
		} else {
			sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP,
					   &temp);
			sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY,
					   &hum);
			shell_fprintf(sh, SHELL_NORMAL,
				"T=%.2f C  H=%.2f %%\n",
				sensor_value_to_double(&temp),
				sensor_value_to_double(&hum));
		}
		k_sleep(K_MSEC(interval_ms));
	}
	return 0;
}

/* ============== HV Control ============== */

static int cmd_hv1(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		int st = gpio_pin_get_dt(&hv1_en);

		shell_fprintf(sh, SHELL_NORMAL, "HV1: %s  DAC1=%u\n",
			st > 0 ? "ON" : "OFF", dac1_code);
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_set_dt(&hv1_en, 1);
		shell_fprintf(sh, SHELL_NORMAL, "HV1: ON\n");
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_set_dt(&hv1_en, 0);
		shell_fprintf(sh, SHELL_NORMAL, "HV1: OFF\n");
	} else {
		shell_fprintf(sh, SHELL_ERROR, "usage: hv1 [on|off]\n");
		return -EINVAL;
	}
	return 0;
}

static int cmd_hv2(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		int st = gpio_pin_get_dt(&hv2_en);

		shell_fprintf(sh, SHELL_NORMAL, "HV2: %s  DAC2=%u\n",
			st > 0 ? "ON" : "OFF", dac2_code);
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_set_dt(&hv2_en, 1);
		shell_fprintf(sh, SHELL_NORMAL, "HV2: ON\n");
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_set_dt(&hv2_en, 0);
		shell_fprintf(sh, SHELL_NORMAL, "HV2: OFF\n");
	} else {
		shell_fprintf(sh, SHELL_ERROR, "usage: hv2 [on|off]\n");
		return -EINVAL;
	}
	return 0;
}

/* ============== DAC ============== */

static int cmd_dac1(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_fprintf(sh, SHELL_NORMAL, "DAC1: %u (%.3f V)\n",
			dac1_code, dac1_code * 5.0 / 65535.0);
		return 0;
	}
	uint32_t code = strtoul(argv[1], NULL, 0);

	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "range: 0-65535\n");
		return -EINVAL;
	}
	if (!device_is_ready(dac_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC1 not ready\n");
		return -ENODEV;
	}
	int ret = dac_write_value(dac_hv1, 0, code);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac1_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC1: %u (%.3f V)\n",
		code, code * 5.0 / 65535.0);
	return 0;
}

static int cmd_dac2(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_fprintf(sh, SHELL_NORMAL, "DAC2: %u (%.3f V)\n",
			dac2_code, dac2_code * 5.0 / 65535.0);
		return 0;
	}
	uint32_t code = strtoul(argv[1], NULL, 0);

	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "range: 0-65535\n");
		return -EINVAL;
	}
	if (!device_is_ready(dac_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC2 not ready\n");
		return -ENODEV;
	}
	int ret = dac_write_value(dac_hv2, 0, code);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac2_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC2: %u (%.3f V)\n",
		code, code * 5.0 / 65535.0);
	return 0;
}

/* ============== Shell Registration ============== */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sht31,
	SHELL_CMD(read, NULL, "Read temperature and humidity", cmd_sht31_read),
	SHELL_CMD_ARG(watch, NULL, "Watch T/H [interval_ms]", cmd_sht31_watch, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sht31, &sub_sht31, "SHT31 sensor", NULL);
SHELL_CMD_ARG_REGISTER(hv1, NULL, "HV1 [on|off]", cmd_hv1, 1, 1);
SHELL_CMD_ARG_REGISTER(hv2, NULL, "HV2 [on|off]", cmd_hv2, 1, 1);
SHELL_CMD_ARG_REGISTER(dac1, NULL, "DAC1 [<0-65535>]", cmd_dac1, 1, 1);
SHELL_CMD_ARG_REGISTER(dac2, NULL, "DAC2 [<0-65535>]", cmd_dac2, 1, 1);

int main(void)
{
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

	if (hv1_en.port && device_is_ready(hv1_en.port)) {
		gpio_pin_configure_dt(&hv1_en, GPIO_OUTPUT_INACTIVE);
		printk("HV1: OFF\n");
	} else {
		printk("HV1 (hv1_en): GPIO NOT READY\n");
	}

	if (hv2_en.port && device_is_ready(hv2_en.port)) {
		gpio_pin_configure_dt(&hv2_en, GPIO_OUTPUT_INACTIVE);
		printk("HV2: OFF\n");
	} else {
		printk("HV2 (hv2_en): GPIO NOT READY\n");
	}

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

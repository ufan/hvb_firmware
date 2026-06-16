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

/* ADS1232 ADC nodes */
#define ADC_HV1_NODE DT_NODELABEL(ads1232_hv1)
#define ADC_HV2_NODE DT_NODELABEL(ads1232_hv2)
static const struct device *adc_hv1 = DEVICE_DT_GET_OR_NULL(ADC_HV1_NODE);
static const struct device *adc_hv2 = DEVICE_DT_GET_OR_NULL(ADC_HV2_NODE);

static uint8_t adc1_gain_val = 1;
static uint8_t adc2_gain_val = 1;

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

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 temp read failed: %d\n", ret);
		return ret;
	}

	ret = sensor_channel_get(sht31_dev, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "SHT31 humidity read failed: %d\n", ret);
		return ret;
	}

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

	const char *hv1_str = (hv1_state > 0) ? "ON" : (hv1_state == 0) ? "OFF" : "ERR";
	const char *hv2_str = (hv2_state > 0) ? "ON" : (hv2_state == 0) ? "OFF" : "ERR";

	shell_fprintf(sh, SHELL_NORMAL,
		"HV1: %s (PD9=%d)  DAC1: %u\n"
		"HV2: %s (PC4=%d)  DAC2: %u\n",
		hv1_str, hv1_state, dac1_code,
		hv2_str, hv2_state, dac2_code);
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
		shell_fprintf(sh, SHELL_ERROR, "Code out of range (0-65535)\n");
		return -EINVAL;
	}

	if (!device_is_ready(dac_hv1)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC1 not ready\n");
		return -ENODEV;
	}

	ret = dac_write_value(dac_hv1, 0, code);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "DAC1 write failed: %d\n", ret);
		return ret;
	}
	dac1_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC1: %u (%.3f V)\n", code,
		code * 5.0 / 65535.0);
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
		shell_fprintf(sh, SHELL_ERROR, "Code out of range (0-65535)\n");
		return -EINVAL;
	}

	if (!device_is_ready(dac_hv2)) {
		shell_fprintf(sh, SHELL_ERROR, "DAC2 not ready\n");
		return -ENODEV;
	}

	ret = dac_write_value(dac_hv2, 0, code);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "DAC2 write failed: %d\n", ret);
		return ret;
	}
	dac2_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "DAC2: %u (%.3f V)\n", code,
		code * 5.0 / 65535.0);
	return 0;
}

/* ============== ADC ============== */

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

	shell_fprintf(sh, SHELL_NORMAL,
		"ADC1: V=%.3f  gain=%u\n",
		sensor_value_to_double(&val), adc1_gain_val);
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

	shell_fprintf(sh, SHELL_NORMAL,
		"ADC2: V=%.3f  gain=%u\n",
		sensor_value_to_double(&val), adc2_gain_val);
	return 0;
}

static int cmd_adc1_gain(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t gain;
	struct sensor_value val;
	int ret;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR,
			"Usage: adc1 gain <1|2|64|128>\n");
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
	ret = sensor_attr_set(adc_hv1, SENSOR_CHAN_ALL, SENSOR_ATTR_PRIV_START, &val);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "ADC1 gain set failed: %d\n", ret);
		return ret;
	}
	adc1_gain_val = (uint8_t)gain;

	shell_fprintf(sh, SHELL_NORMAL, "ADC1 gain set to %u\n", gain);
	return 0;
}

static int cmd_adc2_gain(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t gain;
	struct sensor_value val;
	int ret;

	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR,
			"Usage: adc2 gain <1|2|64|128>\n");
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
	ret = sensor_attr_set(adc_hv2, SENSOR_CHAN_ALL, SENSOR_ATTR_PRIV_START, &val);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "ADC2 gain set failed: %d\n", ret);
		return ret;
	}
	adc2_gain_val = (uint8_t)gain;

	shell_fprintf(sh, SHELL_NORMAL, "ADC2 gain set to %u\n", gain);
	return 0;
}

static int cmd_adc_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_fprintf(sh, SHELL_NORMAL,
		"ADC1: gain=%u\n"
		"ADC2: gain=%u\n",
		adc1_gain_val, adc2_gain_val);
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
		.channel_id = 0,
		.resolution = 16,
	};

	if (device_is_ready(dac_hv1)) {
		ret = dac_channel_setup(dac_hv1, &dac_cfg);
		if (ret < 0) {
			printk("DAC1 setup failed: %d\n", ret);
		}
		ret = dac_write_value(dac_hv1, 0, 0);
		if (ret < 0) {
			printk("DAC1 init write failed: %d\n", ret);
		}
		dac1_code = 0;
	}

	if (device_is_ready(dac_hv2)) {
		ret = dac_channel_setup(dac_hv2, &dac_cfg);
		if (ret < 0) {
			printk("DAC2 setup failed: %d\n", ret);
		}
		ret = dac_write_value(dac_hv2, 0, 0);
		if (ret < 0) {
			printk("DAC2 init write failed: %d\n", ret);
		}
		dac2_code = 0;
	}

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

	printk("Type 'help' for commands\n");
	return 0;
}

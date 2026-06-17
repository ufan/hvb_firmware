/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full-chain verification: DAC (Output Drive Level) -> HV enable
 * (Output Enable) -> ADC feedback (telemetry readback).
 * All commands target HV1 channel.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

static const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac_hv1));
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(ads1232_hv1));
static const struct gpio_dt_spec hv_en =
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(hv1_en), gpios, {0});

static uint32_t dac_code;
static int32_t adc_buf[2];
static struct adc_sequence adc_seq = {
	.buffer      = adc_buf,
	.buffer_size = sizeof(adc_buf),
	.resolution  = 24,
};
static int gain_val = 1;

static enum adc_gain gain_to_enum(int g)
{
	switch (g) {
	case 2:   return ADC_GAIN_2;
	default:  return ADC_GAIN_1;
	}
}

/* ---- dac <code> ---- */

static int cmd_dac(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_fprintf(sh, SHELL_NORMAL, "dac: %u (%.3f V)\n",
			dac_code, dac_code * 5.0 / 65535.0);
		return 0;
	}
	uint32_t code = strtoul(argv[1], NULL, 0);

	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "range: 0-65535\n");
		return -EINVAL;
	}
	int ret = dac_write_value(dac_dev, 0, code);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac_code = code;
	shell_fprintf(sh, SHELL_NORMAL, "dac: %u (%.3f V)\n",
		code, code * 5.0 / 65535.0);
	return 0;
}

/* ---- hv on|off ---- */

static int cmd_hv(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		int st = gpio_pin_get_dt(&hv_en);

		shell_fprintf(sh, SHELL_NORMAL, "hv: %s  dac=%u\n",
			st > 0 ? "ON" : "OFF", dac_code);
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_set_dt(&hv_en, 1);
		shell_fprintf(sh, SHELL_NORMAL, "hv: ON\n");
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_set_dt(&hv_en, 0);
		shell_fprintf(sh, SHELL_NORMAL, "hv: OFF\n");
	} else {
		shell_fprintf(sh, SHELL_ERROR, "usage: hv [on|off]\n");
		return -EINVAL;
	}
	return 0;
}

/* ---- watch support ---- */

static volatile bool watch_stop;

static void watch_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	watch_stop = true;
	shell_set_bypass(sh, NULL);
}

/* ---- adc read / adc gain <g> ---- */

static int cmd_adc_gain(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_fprintf(sh, SHELL_NORMAL, "gain=%d\n", gain_val);
		return 0;
	}
	int g = atoi(argv[1]);

	if (g != 1 && g != 2) {
		shell_fprintf(sh, SHELL_ERROR, "gain: 1|2\n");
		return -EINVAL;
	}
	gain_val = g;

	struct adc_channel_cfg ch_cfg = {
		.gain             = gain_to_enum(g),
		.reference        = ADC_REF_EXTERNAL0,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.differential     = 1,
	};
	adc_channel_setup(adc_dev, &ch_cfg);

	shell_fprintf(sh, SHELL_NORMAL, "gain=%d\n", g);
	return 0;
}

static int cmd_adc_read_both(const struct shell *sh, size_t argc, char **argv)
{
	adc_seq.channels = BIT(0) | BIT(1);
	int ret = adc_read(adc_dev, &adc_seq);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "read: %d\n", ret);
		return ret;
	}

	for (int ch = 0; ch < 2; ch++) {
		int64_t mv = (int64_t)adc_buf[ch] * 5000LL /
			     ((int64_t)gain_val * 8388607LL);
		shell_fprintf(sh, SHELL_NORMAL,
			"adc ch%d: raw=0x%08x  %" PRId64 " mV  (gain=%d)\n",
			ch, (uint32_t)adc_buf[ch], mv, gain_val);
	}
	return 0;
}

static int cmd_adc_watch(const struct shell *sh, size_t argc, char **argv)
{
	int interval_ms = 1000;

	if (argc >= 2) {
		interval_ms = atoi(argv[1]);
		if (interval_ms < 100) {
			interval_ms = 100;
		}
	}

	watch_stop = false;
	shell_set_bypass(sh, watch_bypass_cb);
	shell_fprintf(sh, SHELL_NORMAL, "press any key to stop\n");

	while (!watch_stop) {
		adc_seq.channels = BIT(0) | BIT(1);
		int ret = adc_read(adc_dev, &adc_seq);

		if (ret < 0) {
			shell_fprintf(sh, SHELL_ERROR, "read: %d\n", ret);
		} else {
			for (int ch = 0; ch < 2; ch++) {
				int64_t mv = (int64_t)adc_buf[ch] * 5000LL /
					     ((int64_t)gain_val * 8388607LL);
				shell_fprintf(sh, SHELL_NORMAL,
					"ch%d: raw=0x%08x  %" PRId64
					" mV  (gain=%d)\n",
					ch, (uint32_t)adc_buf[ch], mv,
					gain_val);
			}
		}
		k_sleep(K_MSEC(interval_ms));
	}
	return 0;
}

/* ---- shell registration ---- */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc,
	SHELL_CMD(read, NULL, "Read both ADC channels", cmd_adc_read_both),
	SHELL_CMD_ARG(gain, NULL, "gain [<1|2>]", cmd_adc_gain, 1, 1),
	SHELL_CMD_ARG(watch, NULL, "Watch ADC [interval_ms]", cmd_adc_watch, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(dac, NULL, "DAC output [<0-65535>]", cmd_dac, 1, 1);
SHELL_CMD_ARG_REGISTER(hv, NULL, "HV enable [on|off]", cmd_hv, 1, 1);
SHELL_CMD_REGISTER(adc, &sub_adc, "ADS1232 ADC", NULL);

int main(void)
{
	printk("=== dac_verify: DAC -> HV -> ADC chain ===\n");

	if (!device_is_ready(dac_dev)) {
		printk("DAC (dac_hv1): NOT READY\n");
	} else {
		struct dac_channel_cfg cfg = { .channel_id = 0, .resolution = 16 };

		dac_channel_setup(dac_dev, &cfg);
		dac_write_value(dac_dev, 0, 0);
		printk("DAC (dac_hv1): ready, set to 0\n");
	}

	if (hv_en.port && device_is_ready(hv_en.port)) {
		gpio_pin_configure_dt(&hv_en, GPIO_OUTPUT_INACTIVE);
		printk("HV (hv1_en): OFF\n");
	} else {
		printk("HV (hv1_en): GPIO NOT READY\n");
	}

	if (!device_is_ready(adc_dev)) {
		printk("ADC (ads1232_hv1): NOT READY\n");
	} else {
		printk("ADC (ads1232_hv1): ready\n");
	}

	printk("\nTest: dac <code> -> hv on -> adc read 0\n");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full-chain verification for both HV channels:
 *   DAC (Output Drive Level) -> HV enable -> ADC feedback
 *
 * Commands use 1-based HV channel index:
 *   dac <1|2> [<0-65535>]
 *   hv  <1|2> [on|off]
 *   adc read <1|2> [0|1]
 *   adc gain <1|2> <0|1> <1|2|64|128>
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

#define NUM_HV 2

static const struct device *dac_dev[NUM_HV] = {
	DEVICE_DT_GET(DT_NODELABEL(dac_hv1)),
	DEVICE_DT_GET(DT_NODELABEL(dac_hv2)),
};

static const struct device *adc_dev[NUM_HV] = {
	DEVICE_DT_GET(DT_NODELABEL(ads1232_hv1)),
	DEVICE_DT_GET(DT_NODELABEL(ads1232_hv2)),
};

static const struct gpio_dt_spec hv_en[NUM_HV] = {
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(hv1_en), gpios, {0}),
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(hv2_en), gpios, {0}),
};

static uint32_t dac_code[NUM_HV];
static int32_t adc_buf;
static struct adc_sequence adc_seq = {
	.buffer      = &adc_buf,
	.buffer_size = sizeof(adc_buf),
	.resolution  = 24,
};
static int gain_val[NUM_HV][2] = {{1, 1}, {1, 1}};

static enum adc_gain gain_to_enum(int g)
{
	switch (g) {
	case 2:   return ADC_GAIN_2;
	case 64:  return ADC_GAIN_64;
	case 128: return ADC_GAIN_128;
	default:  return ADC_GAIN_1;
	}
}

static int parse_hv(const struct shell *sh, const char *s)
{
	int hv = atoi(s);

	if (hv < 1 || hv > NUM_HV) {
		shell_fprintf(sh, SHELL_ERROR, "hv channel: 1 or 2\n");
		return -1;
	}
	return hv - 1;
}

/* ---- dac <1|2> [<code>] ---- */

static int cmd_dac(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		for (int i = 0; i < NUM_HV; i++) {
			shell_fprintf(sh, SHELL_NORMAL,
				"dac%d: %u (%.3f V)%s\n", i + 1,
				dac_code[i], dac_code[i] * 5.0 / 65535.0,
				device_is_ready(dac_dev[i]) ? "" : " [NOT READY]");
		}
		return 0;
	}

	int idx = parse_hv(sh, argv[1]);

	if (idx < 0) {
		return -EINVAL;
	}
	if (argc < 3) {
		shell_fprintf(sh, SHELL_NORMAL, "dac%d: %u (%.3f V)\n",
			idx + 1, dac_code[idx],
			dac_code[idx] * 5.0 / 65535.0);
		return 0;
	}

	uint32_t code = strtoul(argv[2], NULL, 0);

	if (code > 65535) {
		shell_fprintf(sh, SHELL_ERROR, "range: 0-65535\n");
		return -EINVAL;
	}
	int ret = dac_write_value(dac_dev[idx], 0, code);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "write failed: %d\n", ret);
		return ret;
	}
	dac_code[idx] = code;
	shell_fprintf(sh, SHELL_NORMAL, "dac%d: %u (%.3f V)\n",
		idx + 1, code, code * 5.0 / 65535.0);
	return 0;
}

/* ---- hv <1|2> [on|off] ---- */

static int cmd_hv(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		for (int i = 0; i < NUM_HV; i++) {
			int st = hv_en[i].port ?
				gpio_pin_get_dt(&hv_en[i]) : -1;
			shell_fprintf(sh, SHELL_NORMAL,
				"hv%d: %s  dac=%u\n", i + 1,
				st < 0 ? "N/A" : (st > 0 ? "ON" : "OFF"),
				dac_code[i]);
		}
		return 0;
	}

	int idx = parse_hv(sh, argv[1]);

	if (idx < 0) {
		return -EINVAL;
	}
	if (!hv_en[idx].port) {
		shell_fprintf(sh, SHELL_ERROR, "hv%d: GPIO not available\n",
			idx + 1);
		return -ENODEV;
	}
	if (argc < 3) {
		int st = gpio_pin_get_dt(&hv_en[idx]);

		shell_fprintf(sh, SHELL_NORMAL, "hv%d: %s  dac=%u\n",
			idx + 1, st > 0 ? "ON" : "OFF", dac_code[idx]);
		return 0;
	}
	if (strcmp(argv[2], "on") == 0) {
		gpio_pin_set_dt(&hv_en[idx], 1);
		shell_fprintf(sh, SHELL_NORMAL, "hv%d: ON\n", idx + 1);
	} else if (strcmp(argv[2], "off") == 0) {
		gpio_pin_set_dt(&hv_en[idx], 0);
		shell_fprintf(sh, SHELL_NORMAL, "hv%d: OFF\n", idx + 1);
	} else {
		shell_fprintf(sh, SHELL_ERROR, "usage: hv <1|2> [on|off]\n");
		return -EINVAL;
	}
	return 0;
}

/* ---- adc read <1|2> [0|1] / adc gain <1|2> <0|1> <g> ---- */

static int cmd_adc_read(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_fprintf(sh, SHELL_ERROR,
			"usage: adc read <1|2> [0|1]\n");
		return -EINVAL;
	}

	int idx = parse_hv(sh, argv[1]);

	if (idx < 0) {
		return -EINVAL;
	}

	int ch = 0;

	if (argc >= 3) {
		ch = atoi(argv[2]);
	}
	if (ch < 0 || ch > 1) {
		shell_fprintf(sh, SHELL_ERROR, "adc channel: 0 or 1\n");
		return -EINVAL;
	}

	struct adc_channel_cfg ch_cfg = {
		.channel_id       = ch,
		.gain             = gain_to_enum(gain_val[idx][ch]),
		.reference        = ADC_REF_EXTERNAL0,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.differential     = 1,
	};
	int ret = adc_channel_setup(adc_dev[idx], &ch_cfg);

	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "ch setup: %d\n", ret);
		return ret;
	}

	adc_seq.channels = BIT(ch);
	ret = adc_read(adc_dev[idx], &adc_seq);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "read: %d\n", ret);
		return ret;
	}

	int64_t mv = (int64_t)adc_buf * 5000LL /
		     ((int64_t)gain_val[idx][ch] * 8388607LL);

	shell_fprintf(sh, SHELL_NORMAL,
		"adc%d ch%d: raw=%d  %" PRId64 " mV  (gain=%d)\n",
		idx + 1, ch, adc_buf, mv, gain_val[idx][ch]);
	return 0;
}

static int cmd_adc_gain(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		for (int i = 0; i < NUM_HV; i++) {
			shell_fprintf(sh, SHELL_NORMAL,
				"adc%d: ch0 gain=%d  ch1 gain=%d\n",
				i + 1, gain_val[i][0], gain_val[i][1]);
		}
		return 0;
	}

	int idx = parse_hv(sh, argv[1]);

	if (idx < 0) {
		return -EINVAL;
	}
	if (argc < 4) {
		shell_fprintf(sh, SHELL_NORMAL,
			"adc%d: ch0 gain=%d  ch1 gain=%d\n",
			idx + 1, gain_val[idx][0], gain_val[idx][1]);
		return 0;
	}

	int ch = atoi(argv[2]);
	int g  = atoi(argv[3]);

	if (ch < 0 || ch > 1) {
		shell_fprintf(sh, SHELL_ERROR, "adc channel: 0 or 1\n");
		return -EINVAL;
	}
	if (g != 1 && g != 2 && g != 64 && g != 128) {
		shell_fprintf(sh, SHELL_ERROR, "gain: 1|2|64|128\n");
		return -EINVAL;
	}
	gain_val[idx][ch] = g;
	shell_fprintf(sh, SHELL_NORMAL, "adc%d ch%d gain=%d\n",
		idx + 1, ch, g);
	return 0;
}

/* ---- shell registration ---- */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc,
	SHELL_CMD_ARG(read, NULL, "Read: adc read <1|2> [0|1]",
		cmd_adc_read, 2, 1),
	SHELL_CMD_ARG(gain, NULL, "Gain: adc gain <1|2> <0|1> <1|2|64|128>",
		cmd_adc_gain, 1, 3),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(dac, NULL, "DAC: dac <1|2> [<0-65535>]",
	cmd_dac, 1, 2);
SHELL_CMD_ARG_REGISTER(hv, NULL, "HV enable: hv <1|2> [on|off]",
	cmd_hv, 1, 2);
SHELL_CMD_REGISTER(adc, &sub_adc, "ADS1232 ADC", NULL);

int main(void)
{
	printk("=== dac_verify: 2-channel DAC -> HV -> ADC ===\n");

	for (int i = 0; i < NUM_HV; i++) {
		if (!device_is_ready(dac_dev[i])) {
			printk("DAC%d: NOT READY\n", i + 1);
		} else {
			struct dac_channel_cfg cfg = {
				.channel_id = 0, .resolution = 16,
			};
			dac_channel_setup(dac_dev[i], &cfg);
			dac_write_value(dac_dev[i], 0, 0);
			printk("DAC%d: ready, set to 0\n", i + 1);
		}

		if (hv_en[i].port && device_is_ready(hv_en[i].port)) {
			gpio_pin_configure_dt(&hv_en[i], GPIO_OUTPUT_INACTIVE);
			printk("HV%d:  OFF\n", i + 1);
		} else {
			printk("HV%d:  GPIO NOT READY\n", i + 1);
		}

		if (!device_is_ready(adc_dev[i])) {
			printk("ADC%d: NOT READY\n", i + 1);
		} else {
			printk("ADC%d: ready\n", i + 1);
		}
	}

	printk("\nTest: dac <1|2> <code> -> hv <1|2> on -> adc read <1|2>\n");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

static const struct device *adc_dev =
	DEVICE_DT_GET(DT_NODELABEL(ads1232_hv1));

static int32_t adc_buf;
static struct adc_sequence adc_seq = {
	.buffer      = &adc_buf,
	.buffer_size = sizeof(adc_buf),
	.resolution  = 24,
};

static int gain_val[2] = {1, 1}; /* per-channel PGA gain: 1, 2, 64, or 128 */

static enum adc_gain gain_to_enum(int g)
{
	switch (g) {
	case 2:   return ADC_GAIN_2;
	case 64:  return ADC_GAIN_64;
	case 128: return ADC_GAIN_128;
	default:  return ADC_GAIN_1;
	}
}

static int cmd_adc_read(const struct shell *sh, size_t argc, char **argv)
{
	int ch = 0;
	int ret;

	if (argc >= 2) {
		ch = atoi(argv[1]);
	}
	if (ch < 0 || ch > 1) {
		shell_fprintf(sh, SHELL_ERROR, "channel must be 0 or 1\n");
		return -EINVAL;
	}
	if (!device_is_ready(adc_dev)) {
		shell_fprintf(sh, SHELL_ERROR, "ADS1232 not ready\n");
		return -ENODEV;
	}

	struct adc_channel_cfg ch_cfg = {
		.channel_id       = ch,
		.gain             = gain_to_enum(gain_val[ch]),
		.reference        = ADC_REF_EXTERNAL0,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.differential     = 1,
	};

	ret = adc_channel_setup(adc_dev, &ch_cfg);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "channel_setup failed: %d\n", ret);
		return ret;
	}

	adc_seq.channels = BIT(ch);
	ret = adc_read(adc_dev, &adc_seq);
	if (ret < 0) {
		shell_fprintf(sh, SHELL_ERROR, "adc_read failed: %d\n", ret);
		return ret;
	}

	/*
	 * Vref = 5000 mV (external REFP-REFN on jw_hvb).
	 * Positive full-scale output = 2^23 - 1 = 8388607 (two's complement).
	 * V_in_mV = raw * Vref_mV / (gain * (2^23 - 1))
	 */
	int64_t mv = (int64_t)adc_buf * 5000LL /
		     ((int64_t)gain_val[ch] * 8388607LL);

	shell_fprintf(sh, SHELL_NORMAL,
		"ch%d: raw=%d  Vin=%" PRId64 " mV  (gain=%d, Vref=5000mV)\n",
		ch, adc_buf, mv, gain_val[ch]);
	return 0;
}

static int cmd_adc_gain(const struct shell *sh, size_t argc, char **argv)
{
	/* adc gain          → show both channels
	 * adc gain <ch> <g> → set channel ch to gain g
	 */
	if (argc < 3) {
		shell_fprintf(sh, SHELL_NORMAL,
			"ch0 gain=%d  ch1 gain=%d  (valid: 1|2|64|128)\n",
			gain_val[0], gain_val[1]);
		return 0;
	}

	int ch = atoi(argv[1]);
	int g  = atoi(argv[2]);

	if (ch < 0 || ch > 1) {
		shell_fprintf(sh, SHELL_ERROR, "channel must be 0 or 1\n");
		return -EINVAL;
	}
	if (g != 1 && g != 2 && g != 64 && g != 128) {
		shell_fprintf(sh, SHELL_ERROR, "valid gains: 1, 2, 64, 128\n");
		return -EINVAL;
	}
	gain_val[ch] = g;
	shell_fprintf(sh, SHELL_NORMAL, "ch%d gain=%d\n", ch, g);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc,
	SHELL_CMD_ARG(read, NULL, "Read ADC channel [0|1]",
		      cmd_adc_read, 1, 1),
	SHELL_CMD_ARG(gain, NULL, "Show or set per-channel PGA gain: gain [<ch> <1|2|64|128>]",
		      cmd_adc_gain, 1, 2),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(adc, &sub_adc,
	"ADS1232 24-bit ADC verification", NULL);

int main(void)
{
	printk("=== adc_verify: ADS1232 driver verification ===\n");
	printk("Board: jw_hvb  MCU: STM32F429  ADC: ads1232_hv1\n");

	if (!device_is_ready(adc_dev)) {
		printk("ADS1232 HV1: NOT READY -- check overlay, GPIO wiring\n");
	} else {
		printk("ADS1232 HV1: ready\n");
		printk("  DRDY=PE13  SCLK=PE12  PWDN=PE11\n");
		printk("  A0=PE8  GAIN[1:0]=PH12/PE10\n");
		printk("Commands:\n");
		printk("  adc read [0|1]           - sample channel (default 0)\n");
		printk("  adc gain                 - show both channel gains\n");
		printk("  adc gain <ch> <1|2|64|128> - set channel gain\n");
	}

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

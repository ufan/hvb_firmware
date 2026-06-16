/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_ads1232

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ads1232, CONFIG_ADC_LOG_LEVEL);

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
	enum adc_gain pga_gain[2];
};

static void ads1232_set_gain_gpios(const struct ads1232_config *cfg, enum adc_gain gain)
{
	if (!cfg->gain0.port || !cfg->gain1.port) {
		return;
	}

	/* Table 7-3: GAIN[1:0] 00=1x, 01=2x, 10=64x, 11=128x */
	switch (gain) {
	case ADC_GAIN_1:
		gpio_pin_set_dt(&cfg->gain1, 0);
		gpio_pin_set_dt(&cfg->gain0, 0);
		break;
	case ADC_GAIN_2:
		gpio_pin_set_dt(&cfg->gain1, 0);
		gpio_pin_set_dt(&cfg->gain0, 1);
		break;
	case ADC_GAIN_64:
		gpio_pin_set_dt(&cfg->gain1, 1);
		gpio_pin_set_dt(&cfg->gain0, 0);
		break;
	case ADC_GAIN_128:
		gpio_pin_set_dt(&cfg->gain1, 1);
		gpio_pin_set_dt(&cfg->gain0, 1);
		break;
	default:
		LOG_WRN("unsupported gain %d, keeping current", gain);
		break;
	}
}

static int ads1232_channel_setup(const struct device *dev,
				 const struct adc_channel_cfg *channel_cfg)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;

	/* ADS1232: ch0 (A0=0 → AINP1/AINN1), ch1 (A0=1 → AINP2/AINN2) */
	if (channel_cfg->channel_id > 1) {
		return -EINVAL;
	}

	/* Update A0 for channel selection (Table 7-1) */
	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, channel_cfg->channel_id & 1);
	}

	ads1232_set_gain_gpios(cfg, channel_cfg->gain);
	data->pga_gain[channel_cfg->channel_id] = channel_cfg->gain;

	return 0;
}

static int ads1232_read(const struct device *dev,
			const struct adc_sequence *sequence)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int32_t val = 0;
	int timeout;
	int32_t *buf = (int32_t *)sequence->buffer;
	int ch;

	if (sequence->buffer_size < sizeof(int32_t)) {
		return -ENOMEM;
	}

	/* One channel at a time; BIT(0)=ch0 (AINP1), BIT(1)=ch1 (AINP2) */
	switch (sequence->channels) {
	case BIT(0):
		ch = 0;
		break;
	case BIT(1):
		ch = 1;
		break;
	default:
		return -EINVAL;
	}

	/* Set A0 and restore this channel's gain (Table 7-1, Table 7-3) */
	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}
	ads1232_set_gain_gpios(cfg, data->pga_gain[ch]);

	/*
	 * Wait for DRDY to assert (physical LOW = data ready).
	 * With GPIO_ACTIVE_LOW: gpio_pin_get_dt returns 1 when physical LOW.
	 * Timeout is 600 ms: covers 10 SPS (100 ms/conv) plus 4-conversion
	 * filter settling after a channel or gain change (§7.3.7, Table 7-5).
	 */
	timeout = 1200; /* 1200 × 500 µs = 600 ms */
	while (!gpio_pin_get_dt(&cfg->drdy) && timeout > 0) {
		k_sleep(K_USEC(500));
		timeout--;
	}
	if (timeout == 0) {
		LOG_ERR("DRDY timeout");
		return -ETIMEDOUT;
	}

	/*
	 * Clock out 24 bits MSB-first. Per §7.3.10 and Table 7-8:
	 *   t4 ≤ 50 ns: SCLK rising → new bit valid on DOUT
	 *   t3 ≥ 100 ns: SCLK pulse width
	 * k_busy_wait(1) = 1 µs satisfies both. Use gpio_pin_get_raw because
	 * DOUT outputs raw binary (physical HIGH = bit '1'), not DRDY active-low.
	 */
	for (int i = 0; i < 24; i++) {
		gpio_pin_set_dt(&cfg->sclk, 1);
		k_busy_wait(1);
		val = (val << 1) |
		      gpio_pin_get_raw(cfg->drdy.port, cfg->drdy.pin);
		gpio_pin_set_dt(&cfg->sclk, 0);
		k_busy_wait(1);
	}

	/* 25th SCLK to force DRDY/DOUT high (Figure 7-10) */
	gpio_pin_set_dt(&cfg->sclk, 1);
	k_busy_wait(1);
	gpio_pin_set_dt(&cfg->sclk, 0);
	k_busy_wait(1);

	/* Sign-extend 24-bit two's complement to 32-bit */
	if (val & BIT(23)) {
		val |= (int32_t)0xFF000000;
	}

	*buf = val;
	return 0;
}

static int ads1232_init(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->drdy.port)) {
		LOG_ERR("DRDY GPIO port not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->sclk, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&cfg->pwdn, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&cfg->drdy, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	if (cfg->gain0.port) {
		gpio_pin_configure_dt(&cfg->gain0, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->gain1.port) {
		gpio_pin_configure_dt(&cfg->gain1, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->a0.port) {
		gpio_pin_configure_dt(&cfg->a0, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->a1.port) {
		gpio_pin_configure_dt(&cfg->a1, GPIO_OUTPUT_INACTIVE);
	}

	/*
	 * Power-up sequence §7.4.5, Table 7-13.
	 * PWDN is GPIO_ACTIVE_LOW: set(1)=physical LOW=power-down,
	 *                           set(0)=physical HIGH=run.
	 * t15 ≥ 10 µs: PWDN must be held high after supplies stable
	 *              (MCU boot time satisfies this already)
	 * t16 ≥ 26 µs: PWDN HIGH pulse duration
	 * t17 ≥ 26 µs: PWDN LOW pulse duration → then HIGH = start converting
	 */
	gpio_pin_set_dt(&cfg->pwdn, 1);  /* physical LOW: reset circuitry */
	k_busy_wait(100);                 /* ≥ t15 (10 µs) */
	gpio_pin_set_dt(&cfg->pwdn, 0);  /* physical HIGH: begin t16 */
	k_busy_wait(30);                  /* t16 ≥ 26 µs */
	gpio_pin_set_dt(&cfg->pwdn, 1);  /* physical LOW: t17 */
	k_busy_wait(30);                  /* t17 ≥ 26 µs */
	gpio_pin_set_dt(&cfg->pwdn, 0);  /* physical HIGH: start converting */

	data->pga_gain[0] = ADC_GAIN_1;
	data->pga_gain[1] = ADC_GAIN_1;
	ads1232_set_gain_gpios(cfg, ADC_GAIN_1);

	LOG_INF("%s ready", dev->name);
	return 0;
}

static const struct adc_driver_api ads1232_api = {
	.channel_setup = ads1232_channel_setup,
	.read          = ads1232_read,
	.ref_internal  = 5000,
};

#define ADS1232_INIT(n)							\
	static const struct ads1232_config ads1232_config_##n = {	\
		.drdy  = GPIO_DT_SPEC_INST_GET(n, drdy_gpios),		\
		.sclk  = GPIO_DT_SPEC_INST_GET(n, sclk_gpios),		\
		.pwdn  = GPIO_DT_SPEC_INST_GET(n, pwdn_gpios),		\
		.a0    = GPIO_DT_SPEC_INST_GET_OR(n, a0_gpios, {0}),	\
		.a1    = GPIO_DT_SPEC_INST_GET_OR(n, a1_gpios, {0}),	\
		.gain0 = GPIO_DT_SPEC_INST_GET_OR(n, gain0_gpios, {0}),\
		.gain1 = GPIO_DT_SPEC_INST_GET_OR(n, gain1_gpios, {0}),\
	};								\
									\
	static struct ads1232_data ads1232_data_##n;			\
									\
	DEVICE_DT_INST_DEFINE(n, ads1232_init, NULL,			\
		&ads1232_data_##n, &ads1232_config_##n,			\
		POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,			\
		&ads1232_api);

DT_INST_FOREACH_STATUS_OKAY(ADS1232_INIT)

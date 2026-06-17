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
	uint8_t init_gain;
	bool runtime_gain;
};

static enum adc_gain ads1232_int_to_gain(int g)
{
	switch (g) {
	case 2:   return ADC_GAIN_2;
	default:  return ADC_GAIN_1;
	}
}

static void ads1232_set_gain_gpios(const struct ads1232_config *cfg, enum adc_gain gain)
{
	if (!cfg->gain0.port || !cfg->gain1.port) {
		return;
	}

	/* Table 7-3: GAIN[1:0] 00=1x, 01=2x */
	switch (gain) {
	case ADC_GAIN_2:
		gpio_pin_set_dt(&cfg->gain0, 1);
		break;
	default:
		gpio_pin_set_dt(&cfg->gain0, 0);
		break;
	}
	gpio_pin_set_dt(&cfg->gain1, 0);
}

static int ads1232_channel_setup(const struct device *dev,
				 const struct adc_channel_cfg *channel_cfg)
{
	const struct ads1232_config *cfg = dev->config;

	if (channel_cfg->channel_id > 1) {
		return -EINVAL;
	}

	if (cfg->runtime_gain) {
		ads1232_set_gain_gpios(cfg, channel_cfg->gain);
	}

	return 0;
}

static int ads1232_read_one(const struct ads1232_config *cfg, int ch,
			    int32_t *out)
{
	int32_t val = 0;
	int timeout;

	// A0/A1 to select channel per Table 7-2. With GPIO_ACTIVE_LOW, set(1)=physical LOW=AINx.
	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}

	/*
	 * Wait for DRDY to assert (physical LOW = data ready).
	 * With GPIO_ACTIVE_LOW: gpio_pin_get_dt returns 1 when physical LOW.
	 * Settling is ~401 ms at 10 SPS (§7.3.7, Table 7-5).
	 */
	timeout = 420;
	while (!gpio_pin_get_dt(&cfg->drdy) && timeout > 0) {
		k_sleep(K_MSEC(1));
		timeout--;
	}
	if (timeout == 0) {
		LOG_ERR("ch%d DRDY timeout", ch);
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

	*out = val;
	return 0;
}

static int ads1232_read(const struct device *dev,
			const struct adc_sequence *sequence)
{
	const struct ads1232_config *cfg = dev->config;
	int32_t *buf = (int32_t *)sequence->buffer;
	int num_ch;
	int ret;

	if (sequence->channels == 0 || (sequence->channels & ~(BIT(0) | BIT(1)))) {
		return -EINVAL;
	}

	num_ch = !!(sequence->channels & BIT(0)) + !!(sequence->channels & BIT(1));
	if (sequence->buffer_size < num_ch * sizeof(int32_t)) {
		return -ENOMEM;
	}

	if (sequence->channels & BIT(0)) {
		ret = ads1232_read_one(cfg, 0, buf++);
		if (ret < 0) {
			return ret;
		}
	}

	if (sequence->channels & BIT(1)) {
		ret = ads1232_read_one(cfg, 1, buf++);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int ads1232_init(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;
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

	/* Set initial gain if specified. */
	ads1232_set_gain_gpios(cfg, ads1232_int_to_gain(cfg->init_gain));

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
		.init_gain = DT_INST_PROP(n, gain),			\
		.runtime_gain = DT_INST_PROP(n, runtime_gain),		\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, ads1232_init, NULL,			\
		NULL, &ads1232_config_##n,				\
		POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,			\
		&ads1232_api);

DT_INST_FOREACH_STATUS_OKAY(ADS1232_INIT)

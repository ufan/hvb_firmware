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

/* SENSOR_ATTR_GAIN not in Zephyr 3.7.2 — use vendor-private attribute */
#define ADS1232_ATTR_GAIN SENSOR_ATTR_PRIV_START

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
	uint8_t pga_gain;
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

	gpio_pin_set_dt(&cfg->pwdn, 0);
	k_sleep(K_MSEC(1));

	timeout = 200;
	while (gpio_pin_get_dt(&cfg->drdy) && timeout > 0) {
		k_sleep(K_USEC(500));
		timeout--;
	}
	if (timeout == 0) {
		LOG_ERR("DRDY timeout");
		gpio_pin_set_dt(&cfg->pwdn, 1);
		return -ETIMEDOUT;
	}

	for (int i = 0; i < 24; i++) {
		gpio_pin_set_dt(&cfg->sclk, 1);
		k_busy_wait(1);
		gpio_pin_set_dt(&cfg->sclk, 0);
		k_busy_wait(1);
		val = (val << 1) | gpio_pin_get_dt(&cfg->drdy);
	}

	gpio_pin_set_dt(&cfg->pwdn, 1);

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

	if (attr == ADS1232_ATTR_GAIN) {
		return ads1232_set_gain(dev, (uint8_t)val->val1);
	}

	return -ENOTSUP;
}

static int ads1232_init(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;

	if (!device_is_ready(cfg->drdy.port)) {
		LOG_ERR("DRDY GPIO not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&cfg->sclk, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cfg->pwdn, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cfg->drdy, GPIO_INPUT);

	gpio_pin_set_dt(&cfg->pwdn, 1);

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

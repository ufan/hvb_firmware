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

struct ads1232_data {
	struct k_mutex lock;
	const struct device *dev;
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	struct gpio_callback drdy_cb;
	struct k_work async_work;
	struct k_poll_signal *async_signal;
	const struct adc_sequence *async_seq;
	uint8_t async_ch;
#endif
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

static int32_t ads1232_bitbang_read(const struct ads1232_config *cfg)
{
	int32_t val = 0;

	for (int i = 0; i < 24; i++) {
		gpio_pin_set_dt(&cfg->sclk, 1);
		k_busy_wait(1); // todo: shorten the busy wait
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

	return val;
}

static int ads1232_read_one(const struct ads1232_config *cfg, int ch,
			    int32_t *out)
{
	int timeout;

	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}

	timeout = 420;
	while (!gpio_pin_get_dt(&cfg->drdy) && timeout > 0) {
		k_sleep(K_MSEC(1));
		timeout--;
	}
	if (timeout == 0) {
		LOG_ERR("ch%d DRDY timeout", ch);
		return -ETIMEDOUT;
	}

	*out = ads1232_bitbang_read(cfg);
	return 0;
}

static int ads1232_read(const struct device *dev,
			const struct adc_sequence *sequence)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
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

	k_mutex_lock(&data->lock, K_FOREVER);

	if (sequence->channels & BIT(0)) {
		ret = ads1232_read_one(cfg, 0, buf++);
		if (ret < 0) {
			goto unlock;
		}
	}

	if (sequence->channels & BIT(1)) {
		ret = ads1232_read_one(cfg, 1, buf++);
		if (ret < 0) {
			goto unlock;
		}
	}

	ret = 0;
unlock:
	k_mutex_unlock(&data->lock);
	return ret;
}

/* ---- Interrupt-driven mode (adc_read_async) ---- */

#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN

static void ads1232_async_work_handler(struct k_work *work)
{
	struct ads1232_data *data = CONTAINER_OF(work, struct ads1232_data, async_work);
	const struct ads1232_config *cfg = data->dev->config;
	struct k_poll_signal *signal;
	const struct adc_sequence *sequence;
	int32_t *buf;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	sequence = data->async_seq;
	signal = data->async_signal;

	if (sequence != NULL) {
		buf = (int32_t *)sequence->buffer;
		*buf = ads1232_bitbang_read(cfg);
		data->async_seq = NULL;
		data->async_signal = NULL;
	}

	k_mutex_unlock(&data->lock);

	if (signal != NULL) {
		k_poll_signal_raise(signal, ret);
	}
}

static void ads1232_drdy_isr(const struct device *port,
			     struct gpio_callback *cb,
			     gpio_port_pins_t pins)
{
	struct ads1232_data *data = CONTAINER_OF(cb, struct ads1232_data, drdy_cb);
	const struct ads1232_config *cfg = data->dev->config;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->drdy, GPIO_INT_DISABLE);
	k_work_submit(&data->async_work);
}

static int ads1232_read_async(const struct device *dev,
			      const struct adc_sequence *sequence,
			      struct k_poll_signal *async)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int ch;

	if (sequence->channels == 0 || (sequence->channels & ~(BIT(0) | BIT(1)))) {
		return -EINVAL;
	}

	if ((sequence->channels & (BIT(0) | BIT(1))) == (BIT(0) | BIT(1))) {
		return -ENOTSUP;
	}

	if (sequence->buffer_size < sizeof(int32_t)) {
		return -ENOMEM;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->async_seq != NULL) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}

	if (sequence->channels & BIT(0)) {
		ch = 0;
	} else {
		ch = 1;
	}

	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}

	data->async_ch = ch;
	data->async_signal = async;
	data->async_seq = sequence;

	if (async != NULL) {
		k_poll_signal_reset(async);
	}

	gpio_pin_interrupt_configure_dt(&cfg->drdy, GPIO_INT_EDGE_TO_ACTIVE);
	k_mutex_unlock(&data->lock);

	return 0;
}

#endif /* CONFIG_ADS1232_INTERRUPT_DRIVEN */

/* ---- Device init ---- */

static int ads1232_init(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);
	data->dev = dev;
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	k_work_init(&data->async_work, ads1232_async_work_handler);
	data->async_signal = NULL;
	data->async_seq = NULL;
	data->async_ch = 0;
#endif

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
	 */
	gpio_pin_set_dt(&cfg->pwdn, 1);
	k_busy_wait(100);
	gpio_pin_set_dt(&cfg->pwdn, 0);
	k_busy_wait(30);
	gpio_pin_set_dt(&cfg->pwdn, 1);
	k_busy_wait(30);
	gpio_pin_set_dt(&cfg->pwdn, 0);

	ads1232_set_gain_gpios(cfg, ads1232_int_to_gain(cfg->init_gain));

#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	gpio_init_callback(&data->drdy_cb, ads1232_drdy_isr, BIT(cfg->drdy.pin));
	gpio_add_callback(cfg->drdy.port, &data->drdy_cb);
#endif

	LOG_DBG("%s ready", dev->name);
	return 0;
}

static const struct adc_driver_api ads1232_api = {
	.channel_setup = ads1232_channel_setup,
	.read          = ads1232_read,
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	.read_async    = ads1232_read_async,
#endif
	.ref_internal  = 5000,
};

#define ADS1232_INIT(n)							\
	static struct ads1232_data ads1232_data_##n;			\
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
		&ads1232_data_##n, &ads1232_config_##n,			\
		POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,			\
		&ads1232_api);

DT_INST_FOREACH_STATUS_OKAY(ADS1232_INIT)

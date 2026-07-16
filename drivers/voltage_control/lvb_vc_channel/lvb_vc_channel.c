/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel_api.h"
#include "voltage_control/vc_types.h"

LOG_MODULE_REGISTER(lvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_lvb_vc_channel

#define LVB_VC_WORKQ_STACK_SIZE 2048
#define LVB_VC_WORKQ_PRIORITY   CONFIG_SYSTEM_WORKQUEUE_PRIORITY

static K_THREAD_STACK_DEFINE(lvb_vc_workq_stack, LVB_VC_WORKQ_STACK_SIZE);
static struct k_work_q lvb_vc_workq;
static bool lvb_vc_workq_started;

static void lvb_vc_ensure_workq(void)
{
	if (!lvb_vc_workq_started) {
		k_work_queue_init(&lvb_vc_workq);
		k_work_queue_start(&lvb_vc_workq, lvb_vc_workq_stack,
				   K_THREAD_STACK_SIZEOF(lvb_vc_workq_stack),
				   LVB_VC_WORKQ_PRIORITY, NULL);
		k_thread_name_set(&lvb_vc_workq.thread, "lvb_vc_workq");
		lvb_vc_workq_started = true;
	}
}

struct lvb_vc_config {
	struct gpio_dt_spec enable;
	struct adc_dt_spec voltage_ch;
	struct adc_dt_spec current_ch;
	int16_t calib_current_b;
	uint16_t capabilities;
	uint8_t channel_index;
	uint16_t sample_rate_ms;
	uint8_t oversample_count;
	bool default_output_disabled;
};

struct lvb_vc_data {
	const struct device *dev;
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	struct vc_channel_buffer *meas;
	struct k_work sample_work;
	struct k_timer sample_timer;
	bool sampling_active;
	int64_t v_accum;
	int64_t i_accum;
	uint8_t sample_count;
};

/* ---- Hardware API ---- */

static int lvb_vc_set_output(const struct device *dev, uint16_t code)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(code);
	return -ENOTSUP;
}

static int lvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct lvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

/* ---- Oversampled ADC sampling ---- */

static void lvb_vc_sample_work_fn(struct k_work *work)
{
	struct lvb_vc_data *data =
		CONTAINER_OF(work, struct lvb_vc_data, sample_work);
	const struct lvb_vc_config *cfg = data->dev->config;

	if (!data->sampling_active) {
		return;
	}

	int16_t v_buf = 0, i_buf = 0;
	struct adc_sequence v_seq = {
		.buffer      = &v_buf,
		.buffer_size = sizeof(v_buf),
	};
	struct adc_sequence i_seq = {
		.buffer      = &i_buf,
		.buffer_size = sizeof(i_buf),
	};

	adc_sequence_init_dt(&cfg->voltage_ch, &v_seq);
	adc_sequence_init_dt(&cfg->current_ch, &i_seq);

	if (adc_read_dt(&cfg->voltage_ch, &v_seq) == 0) {
		data->v_accum += v_buf;
	}
	if (adc_read_dt(&cfg->current_ch, &i_seq) == 0) {
		data->i_accum += i_buf;
	}

	if (++data->sample_count >= cfg->oversample_count) {
		vc_channel_buffer_publish_voltage(data->meas,
			(int32_t)(data->v_accum / cfg->oversample_count),
			k_uptime_get_32());
		vc_channel_buffer_publish_current(data->meas,
			(int32_t)(data->i_accum / cfg->oversample_count),
			k_uptime_get_32());

		if (data->meas_cb) {
			data->meas_cb(cfg->channel_index,
				      data->meas_cb_user_data);
		}

		data->v_accum = data->i_accum = 0;
		data->sample_count = 0;
	}
}

static void lvb_vc_timer_cb(struct k_timer *timer)
{
	struct lvb_vc_data *data =
		CONTAINER_OF(timer, struct lvb_vc_data, sample_timer);

	k_work_submit_to_queue(&lvb_vc_workq, &data->sample_work);
}

/* ---- Start/stop sampling ---- */

static int lvb_vc_start_sampling(const struct device *dev)
{
	struct lvb_vc_data *data = dev->data;
	const struct lvb_vc_config *cfg = dev->config;

	data->sampling_active = true;
	data->v_accum = data->i_accum = 0;
	data->sample_count = 0;
	k_timer_start(&data->sample_timer,
		      K_MSEC(cfg->sample_rate_ms),
		      K_MSEC(cfg->sample_rate_ms));
	return 0;
}

static int lvb_vc_stop_sampling(const struct device *dev)
{
	struct lvb_vc_data *data = dev->data;

	data->sampling_active = false;
	k_timer_stop(&data->sample_timer);
	return 0;
}

/* ---- Capability + callback ops ---- */

static uint16_t lvb_vc_get_capabilities(const struct device *dev)
{
	const struct lvb_vc_config *cfg = dev->config;

	return cfg->capabilities;
}

static int lvb_vc_set_meas_callback(const struct device *dev,
				    vc_meas_ready_cb_t cb, void *user_data)
{
	struct lvb_vc_data *data = dev->data;

	data->meas_cb = cb;
	data->meas_cb_user_data = user_data;
	return 0;
}

static int lvb_get_dt_defaults(const struct device *dev,
				struct vc_channel_config *cfg_out,
				struct vc_channel_cal_config *cal)
{
	const struct lvb_vc_config *cfg = dev->config;

	cal->measured_current_calib_b = cfg->calib_current_b;
	if (cfg->default_output_disabled) {
		cfg_out->configured_output_enabled = false;
	}
	return 0;
}

/* ---- API vtable ---- */

static const struct vc_channel_api lvb_vc_hw_api = {
	.set_output         = lvb_vc_set_output,
	.set_enable         = lvb_vc_set_enable,
	.start_sampling     = lvb_vc_start_sampling,
	.stop_sampling      = lvb_vc_stop_sampling,
	.get_capabilities   = lvb_vc_get_capabilities,
	.set_meas_callback  = lvb_vc_set_meas_callback,
	.get_dt_defaults    = lvb_get_dt_defaults,
};

/* ---- Device init ---- */

static int lvb_vc_init(const struct device *dev)
{
	const struct lvb_vc_config *cfg = dev->config;
	struct lvb_vc_data *data = dev->data;

	lvb_vc_ensure_workq();

	if (!gpio_is_ready_dt(&cfg->enable)) {
		LOG_ERR("ch%d enable GPIO not ready", cfg->channel_index);
		return -ENODEV;
	}
	gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);

	if (!adc_is_ready_dt(&cfg->voltage_ch)) {
		LOG_ERR("ch%d voltage ADC not ready", cfg->channel_index);
		return -ENODEV;
	}
	if (!adc_is_ready_dt(&cfg->current_ch)) {
		LOG_ERR("ch%d current ADC not ready", cfg->channel_index);
		return -ENODEV;
	}
	adc_channel_setup_dt(&cfg->voltage_ch);
	adc_channel_setup_dt(&cfg->current_ch);

	data->dev = dev;
	k_timer_init(&data->sample_timer, lvb_vc_timer_cb, NULL);
	k_work_init(&data->sample_work, lvb_vc_sample_work_fn);

	LOG_INF("ch%d ready caps=0x%04x sample=%dms x%d calib_i_b=%d out_disabled_override=%d",
		cfg->channel_index, cfg->capabilities,
		cfg->sample_rate_ms, cfg->oversample_count,
		cfg->calib_current_b, cfg->default_output_disabled);
	return 0;
}

/* ---- DTS instantiation ---- */

#define LVB_VC_INIT(n) \
	VC_CHANNEL_BUFFER_EXTERN(DT_DRV_INST(n)); \
	static const struct lvb_vc_config lvb_vc_config_##n = { \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
		.voltage_ch = ADC_DT_SPEC_INST_GET_BY_NAME(n, voltage), \
		.current_ch = ADC_DT_SPEC_INST_GET_BY_NAME(n, current), \
		.calib_current_b = (int16_t)DT_INST_PROP(n, calib_current_b), \
		.capabilities = DT_INST_PROP(n, capabilities), \
		.channel_index = DT_REG_ADDR(DT_INST(n, jianwei_lvb_vc_channel)), \
		.sample_rate_ms = DT_INST_PROP_OR(n, sample_rate_ms, \
				      CONFIG_LVB_VC_DEFAULT_SAMPLE_RATE_MS), \
		.oversample_count = (uint8_t)DT_INST_PROP_OR(n, oversample_count, \
				        CONFIG_LVB_VC_OVERSAMPLE_COUNT), \
		.default_output_disabled = DT_INST_PROP(n, default_output_disabled), \
	}; \
	static struct lvb_vc_data lvb_vc_data_##n = { \
		.meas = VC_CHANNEL_BUFFER_PTR(DT_DRV_INST(n)), \
	}; \
	DEVICE_DT_INST_DEFINE(n, lvb_vc_init, NULL, \
		&lvb_vc_data_##n, &lvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&lvb_vc_hw_api);

DT_INST_FOREACH_STATUS_OKAY(LVB_VC_INIT)

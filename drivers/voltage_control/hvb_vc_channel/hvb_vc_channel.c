/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel_api.h"

LOG_MODULE_REGISTER(hvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_hvb_vc_channel

/* ADS1232: BIT(0) = current (MUX ch0), BIT(1) = voltage (MUX ch1) */
#define ADC_CH_CURRENT BIT(0)
#define ADC_CH_VOLTAGE BIT(1)

#define HVB_VC_WORKQ_STACK_SIZE 2048
#define HVB_VC_WORKQ_PRIORITY   CONFIG_SYSTEM_WORKQUEUE_PRIORITY
#define HVB_VC_DRDY_TIMEOUT_MS  600

static K_THREAD_STACK_DEFINE(hvb_vc_workq_stack, HVB_VC_WORKQ_STACK_SIZE);
static struct k_work_q hvb_vc_workq;
static bool hvb_vc_workq_started;

static void hvb_vc_ensure_workq(void)
{
	if (!hvb_vc_workq_started) {
		k_work_queue_init(&hvb_vc_workq);
		k_work_queue_start(&hvb_vc_workq, hvb_vc_workq_stack,
				   K_THREAD_STACK_SIZEOF(hvb_vc_workq_stack),
				   HVB_VC_WORKQ_PRIORITY, NULL);
		k_thread_name_set(&hvb_vc_workq.thread, "hvb_vc_workq");
		hvb_vc_workq_started = true;
	}
}

struct hvb_vc_config {
	const struct device *dac;
	const struct device *adc;
	struct gpio_dt_spec enable;
	uint16_t max_raw_dac;
	uint16_t capabilities;
	uint8_t channel_index;
};

enum hvb_adc_phase {
	ADC_PHASE_VOLTAGE,
	ADC_PHASE_CURRENT,
};

struct hvb_vc_data {
	const struct device *dev;
	uint8_t channel;
	struct vc_channel_buffer *meas;
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;

	struct k_work_poll poll_work;
	struct k_poll_signal adc_signal;
	struct k_poll_event adc_event;
	struct adc_sequence adc_seq;
	int32_t adc_buf;

	enum hvb_adc_phase adc_phase;
	bool sampling_active;
};

/* ---- Hardware API ---- */

static int hvb_vc_set_output(const struct device *dev, uint16_t code)
{
	const struct hvb_vc_config *cfg = dev->config;

	if (code > cfg->max_raw_dac) {
		return -EINVAL;
	}
	return dac_write_value(cfg->dac, 0, code);
}

static int hvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct hvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

/* ---- Async ADC sampling loop ---- */

static void hvb_vc_start_next_cycle(struct hvb_vc_data *data);

static void hvb_vc_notify_meas(struct hvb_vc_data *data)
{
	if (data->meas_cb) {
		data->meas_cb(data->channel, data->meas_cb_user_data);
	}
}

static void hvb_vc_submit_read(struct hvb_vc_data *data, uint8_t adc_ch)
{
	const struct hvb_vc_config *cfg = data->dev->config;

	data->adc_seq.channels = adc_ch;
	k_poll_signal_reset(&data->adc_signal);
	adc_read_async(cfg->adc, &data->adc_seq, &data->adc_signal);
	k_work_poll_submit_to_queue(&hvb_vc_workq,
				    &data->poll_work,
				    &data->adc_event, 1,
				    K_MSEC(HVB_VC_DRDY_TIMEOUT_MS));
}

static void hvb_vc_poll_handler(struct k_work *work)
{
	struct k_work_poll *pw = CONTAINER_OF(work, struct k_work_poll, work);
	struct hvb_vc_data *data = CONTAINER_OF(pw, struct hvb_vc_data, poll_work);
	const struct hvb_vc_config *cfg = data->dev->config;
	int32_t raw = data->adc_buf;

	if (data->poll_work.poll_result != 0) {
		LOG_WRN("ch%d DRDY timeout phase=%d", data->channel, data->adc_phase);
		if (data->sampling_active) {
			hvb_vc_start_next_cycle(data);
		}
		return;
	}

	if (data->adc_phase == ADC_PHASE_VOLTAGE) {
		vc_channel_buffer_publish_voltage(data->meas, raw,
					  k_uptime_get_32());

		/* Wake any cal_sample_fresh waiter even if current phase follows.
		 * The waiter re-checks timestamps so it won't accept a half-cycle
		 * as "fresh" when both V and I are required. */
		hvb_vc_notify_meas(data);

		if (cfg->capabilities & CH_CAP_CURRENT_MEASUREMENT) {
			data->adc_phase = ADC_PHASE_CURRENT;
			hvb_vc_submit_read(data, ADC_CH_CURRENT);
			return;
		}
	} else {
		vc_channel_buffer_publish_current(data->meas, raw,
					  k_uptime_get_32());

		hvb_vc_notify_meas(data);
	}

	if (data->sampling_active) {
		hvb_vc_start_next_cycle(data);
	}
}

static void hvb_vc_start_next_cycle(struct hvb_vc_data *data)
{
	const struct hvb_vc_config *cfg = data->dev->config;

	if (cfg->capabilities & CH_CAP_VOLTAGE_MEASUREMENT) {
		data->adc_phase = ADC_PHASE_VOLTAGE;
		hvb_vc_submit_read(data, ADC_CH_VOLTAGE);
	} else {
		data->adc_phase = ADC_PHASE_CURRENT;
		hvb_vc_submit_read(data, ADC_CH_CURRENT);
	}
}

/* ---- Start/stop sampling ---- */

static int hvb_vc_start_sampling(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;
	const struct hvb_vc_config *cfg = dev->config;

	if (!(cfg->capabilities & (CH_CAP_VOLTAGE_MEASUREMENT |
				   CH_CAP_CURRENT_MEASUREMENT))) {
		return 0;
	}

	data->sampling_active = true;
	hvb_vc_start_next_cycle(data);
	return 0;
}

static int hvb_vc_stop_sampling(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	data->sampling_active = false;
	k_work_poll_cancel(&data->poll_work);
	return 0;
}

/* ---- Capability + callback ops ---- */

static uint16_t hvb_vc_get_capabilities(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	return cfg->capabilities;
}

static int hvb_vc_set_meas_callback(const struct device *dev,
				    vc_meas_ready_cb_t cb, void *user_data)
{
	struct hvb_vc_data *data = dev->data;

	data->meas_cb = cb;
	data->meas_cb_user_data = user_data;
	return 0;
}

/* ---- API vtable ---- */

static const struct vc_channel_api hvb_vc_hw_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.start_sampling = hvb_vc_start_sampling,
	.stop_sampling = hvb_vc_stop_sampling,
	.get_capabilities = hvb_vc_get_capabilities,
	.set_meas_callback = hvb_vc_set_meas_callback,
};

/* ---- Device init ---- */

static int hvb_vc_init(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct hvb_vc_data *data = dev->data;

	hvb_vc_ensure_workq();

	if (!device_is_ready(cfg->dac)) {
		LOG_ERR("DAC not ready");
		return -ENODEV;
	}
	{
		struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };
		int ret = dac_channel_setup(cfg->dac, &dac_cfg);

		if (ret < 0) {
			LOG_ERR("DAC channel setup failed: %d", ret);
			return ret;
		}
	}
	if (!device_is_ready(cfg->adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->enable)) {
		LOG_ERR("Enable GPIO not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);

	data->dev = dev;
	data->channel = cfg->channel_index;

	data->adc_seq.buffer = &data->adc_buf;
	data->adc_seq.buffer_size = sizeof(data->adc_buf);
	data->adc_seq.resolution = 24;

	k_poll_signal_init(&data->adc_signal);
	k_poll_event_init(&data->adc_event, K_POLL_TYPE_SIGNAL,
			  K_POLL_MODE_NOTIFY_ONLY, &data->adc_signal);
	k_work_poll_init(&data->poll_work, hvb_vc_poll_handler);

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x",
		cfg->channel_index,
		cfg->dac->name, cfg->adc->name, cfg->capabilities);
	return 0;
}

/* ---- DTS instantiation ---- */

#define HVB_VC_INIT(n) \
	VC_CHANNEL_BUFFER_EXTERN(DT_DRV_INST(n)); \
	static const struct hvb_vc_config hvb_vc_config_##n = { \
		.dac = DEVICE_DT_GET(DT_INST_PHANDLE(n, dac)), \
		.adc = DEVICE_DT_GET(DT_INST_PHANDLE(n, adc)), \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
		.max_raw_dac = DT_INST_PROP(n, max_raw_dac), \
		.capabilities = DT_INST_PROP(n, capabilities), \
		.channel_index = DT_REG_ADDR(DT_INST(n, jianwei_hvb_vc_channel)), \
	}; \
	static struct hvb_vc_data hvb_vc_data_##n = { \
		.meas = VC_CHANNEL_BUFFER_PTR(DT_DRV_INST(n)), \
	}; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		&hvb_vc_data_##n, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_hw_api);

DT_INST_FOREACH_STATUS_OKAY(HVB_VC_INIT)

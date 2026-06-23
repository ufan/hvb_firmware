/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel.h"
#include "voltage_control/provider_bus.h"

LOG_MODULE_REGISTER(hvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_hvb_vc_channel

#define HVB_VC_WORKQ_STACK_SIZE 1024
#define HVB_VC_WORKQ_PRIORITY   CONFIG_SYSTEM_WORKQUEUE_PRIORITY

static K_THREAD_STACK_DEFINE(hvb_vc_workq_stack, HVB_VC_WORKQ_STACK_SIZE);
static struct k_work_q hvb_vc_workq;
static bool hvb_vc_workq_started;

/* Lazily start the shared work queue for all HVB VC channel instances. */
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
	uint32_t sample_rate_ms;
	uint16_t capabilities;
	uint8_t channel_index;
};

/* Write raw DAC code to the channel's DAC device. */
static int hvb_vc_set_output(const struct device *dev, uint16_t code)
{
	const struct hvb_vc_config *cfg = dev->config;

	if (code > cfg->max_raw_dac) {
		return -EINVAL;
	}
	return dac_write_value(cfg->dac, 0, code);
}

/* Set the HV output enable GPIO. */
static int hvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct hvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

/* Read raw 24-bit voltage ADC value (channel 0). */
static int hvb_vc_measure_voltage(const struct device *dev, int32_t *value)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct adc_sequence seq = {
		.channels = BIT(0),
		.buffer = value,
		.buffer_size = sizeof(*value),
		.resolution = 24,
	};

	return adc_read(cfg->adc, &seq);
}

/* Read raw 24-bit current ADC value (channel 1). */
static int hvb_vc_measure_current(const struct device *dev, int32_t *value)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct adc_sequence seq = {
		.channels = BIT(1),
		.buffer = value,
		.buffer_size = sizeof(*value),
		.resolution = 24,
	};

	return adc_read(cfg->adc, &seq);
}

/* Return the DTS-declared capability bitmask for this channel. */
static uint16_t hvb_vc_get_capabilities(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	return cfg->capabilities;
}

struct hvb_vc_data {
	struct k_work_delayable work;
	const struct device *dev;
	uint8_t channel;
	uint32_t applied_config_version;
	uint32_t generation;
	uint16_t provider_status;
};

/* Sample voltage + current ADCs and publish a measurement snapshot to the provider bus. */
static void hvb_vc_publish_measurement(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct hvb_vc_data *data = dev->data;
	struct vc_measurement_snapshot meas = {
		.channel = data->channel,
		.generation = ++data->generation,
		.timestamp_ms = (uint32_t)k_uptime_get_32(),
		.provider_status = data->provider_status | VC_PROVIDER_STATUS_READY,
	};
	int32_t value;

	data->provider_status &= ~VC_PROVIDER_STATUS_SAMPLE_ERROR;

	if ((cfg->capabilities & CH_CAP_VOLTAGE_MEASUREMENT) != 0) {
		if (hvb_vc_measure_voltage(dev, &value) == 0) {
			meas.present_mask |= VC_MEAS_PRESENT_VOLTAGE;
			meas.raw_voltage = value;
		} else {
			data->provider_status |= VC_PROVIDER_STATUS_SAMPLE_ERROR;
		}
	}

	if ((cfg->capabilities & CH_CAP_CURRENT_MEASUREMENT) != 0) {
		if (hvb_vc_measure_current(dev, &value) == 0) {
			meas.present_mask |= VC_MEAS_PRESENT_CURRENT;
			meas.raw_current = value;
		} else {
			data->provider_status |= VC_PROVIDER_STATUS_SAMPLE_ERROR;
		}
	}

	meas.present_mask |= VC_MEAS_PRESENT_PROVIDER_STATUS;
	if ((data->provider_status & VC_PROVIDER_STATUS_SAMPLE_ERROR) != 0) {
		meas.provider_fault_cause = VC_FAULT_MEASUREMENT;
	}
	(void)vc_provider_bus_publish_measurement(&meas);
}

/* Apply a runtime config to hardware: set enable GPIO + DAC output based on mode/state. */
static int hvb_vc_apply_config(const struct device *dev,
			       const struct vc_runtime_config_snapshot *cfg)
{
	struct hvb_vc_data *data = dev->data;
	int ret;

	if (data == NULL || cfg == NULL) {
		return -EINVAL;
	}

	data->applied_config_version = cfg->version;
	data->generation++;

	if (cfg->force_safe_state) {
		ret = hvb_vc_set_enable(dev, false);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		ret = hvb_vc_set_output(dev, 0);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		data->provider_status &= ~VC_PROVIDER_STATUS_APPLY_FAILED;
		return 0;
	}

	if (cfg->calibration_mode) {
		ret = hvb_vc_set_enable(dev, cfg->calibration_output_enable);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		ret = hvb_vc_set_output(dev, cfg->calibration_raw_output_drive);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		data->provider_status &= ~VC_PROVIDER_STATUS_APPLY_FAILED;
		return 0;
	}

	ret = hvb_vc_set_enable(dev, cfg->output_enable);
	if (ret < 0) {
		data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
		return ret;
	}
	ret = hvb_vc_set_output(dev, cfg->raw_output_drive);
	if (ret < 0) {
		data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
		return ret;
	}
	data->provider_status &= ~VC_PROVIDER_STATUS_APPLY_FAILED;
	return 0;
}

/* Periodic work handler: apply pending config, sample ADCs, reschedule at sample_rate_ms. */
static void hvb_vc_work_handler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	struct hvb_vc_data *data = CONTAINER_OF(delayable, struct hvb_vc_data, work);
	const struct device *dev = data->dev;
	const struct hvb_vc_config *cfg = dev->config;
	const struct vc_runtime_config_snapshot *runtime_cfg;

	runtime_cfg = vc_provider_bus_acquire_config(data->channel);
	if (runtime_cfg != NULL) {
		if (runtime_cfg->version != data->applied_config_version) {
			if (hvb_vc_apply_config(dev, runtime_cfg) == 0) {
				data->applied_config_version = runtime_cfg->version;
			}
		}
		vc_provider_bus_release_config(data->channel);
	}

	hvb_vc_publish_measurement(dev);
	k_work_schedule_for_queue(&hvb_vc_workq, &data->work,
				  K_MSEC(cfg->sample_rate_ms));
}

/* Start the periodic work loop immediately. */
static int hvb_vc_start(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	return k_work_schedule_for_queue(&hvb_vc_workq, &data->work,
				       K_NO_WAIT) < 0 ? -EIO : 0;
}

/* Cancel the periodic work loop. */
static int hvb_vc_stop(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	return k_work_cancel_delayable(&data->work);
}

/* Wake the work handler immediately so it picks up the new config version. */
static int hvb_vc_notify_config_changed(const struct device *dev, uint32_t version)
{
	struct hvb_vc_data *data = dev->data;

	ARG_UNUSED(version);
	return k_work_schedule_for_queue(&hvb_vc_workq, &data->work,
					K_NO_WAIT) < 0 ? -EIO : 0;
}

static const struct vc_channel_api hvb_vc_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.apply_config = hvb_vc_apply_config,
	.start = hvb_vc_start,
	.stop = hvb_vc_stop,
	.notify_config_changed = hvb_vc_notify_config_changed,
	.measure_voltage = hvb_vc_measure_voltage,
	.measure_current = hvb_vc_measure_current,
	.get_capabilities = hvb_vc_get_capabilities,
};

/* Device init: start workq, verify DAC/ADC/GPIO readiness, configure DAC channel, init work item. */
static int hvb_vc_init(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

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

	{
		struct hvb_vc_data *data = dev->data;

		data->dev = dev;
		data->channel = cfg->channel_index;
		k_work_init_delayable(&data->work, hvb_vc_work_handler);
	}

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x",
		cfg->channel_index,
		cfg->dac->name, cfg->adc->name, cfg->capabilities);
	return 0;
}

#define HVB_VC_INIT(n) \
	static const struct hvb_vc_config hvb_vc_config_##n = { \
		.dac = DEVICE_DT_GET(DT_INST_PHANDLE(n, dac)), \
		.adc = DEVICE_DT_GET(DT_INST_PHANDLE(n, adc)), \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
		.max_raw_dac = DT_INST_PROP(n, max_raw_dac), \
		.sample_rate_ms = DT_INST_PROP(n, sample_rate_ms), \
		.capabilities = DT_INST_PROP(n, capabilities), \
		.channel_index = DT_INST_PROP(n, channel_index), \
	}; \
	static struct hvb_vc_data hvb_vc_data_##n; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		&hvb_vc_data_##n, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_api); \
	STRUCT_SECTION_ITERABLE(vc_provider_binding, hvb_vc_binding_##n) = { \
		.channel = DT_INST_PROP(n, channel_index), \
		.dev = DEVICE_DT_INST_GET(n), \
		.config_slot = &vc_runtime_config_slots[DT_INST_PROP(n, channel_index)], \
		.route_bit = BIT(DT_INST_PROP(n, channel_index)), \
	}; \
	STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry, \
		_##n##_, hvb_meas_buf_##n) = {0};

DT_INST_FOREACH_STATUS_OKAY(HVB_VC_INIT)

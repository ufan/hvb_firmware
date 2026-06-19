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

#include "regmap/hvb_regs.h"
#include "voltage_control/vc_channel.h"
#include "voltage_control/runtime.h"

LOG_MODULE_REGISTER(hvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_hvb_vc_channel

struct hvb_vc_config {
	const struct device *dac;
	const struct device *adc;
	struct gpio_dt_spec enable;
	uint16_t max_raw_dac;
	uint32_t sample_rate_ms;
	uint16_t capabilities;
};

static int hvb_vc_set_output(const struct device *dev, uint16_t code)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };

	if (code > cfg->max_raw_dac) {
		return -EINVAL;
	}
	dac_channel_setup(cfg->dac, &dac_cfg);
	return dac_write_value(cfg->dac, 0, code);
}

static int hvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct hvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

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

static uint16_t hvb_vc_get_capabilities(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	return cfg->capabilities;
}

struct hvb_vc_data {
	uint32_t applied_config_version;
	uint32_t generation;
	uint16_t provider_status;
};

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

static const struct vc_channel_api hvb_vc_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.apply_config = hvb_vc_apply_config,
	.measure_voltage = hvb_vc_measure_voltage,
	.measure_current = hvb_vc_measure_current,
	.get_capabilities = hvb_vc_get_capabilities,
};

static int hvb_vc_init(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	if (!device_is_ready(cfg->dac)) {
		LOG_ERR("DAC not ready");
		return -ENODEV;
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

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x",
		DT_INST_PROP(0, channel_index),
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
	}; \
	static struct hvb_vc_data hvb_vc_data_##n; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		&hvb_vc_data_##n, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_api);

DT_INST_FOREACH_STATUS_OKAY(HVB_VC_INIT)

/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel_hw.h"

#define DT_DRV_COMPAT jianwei_vc_channel_stub

struct vc_stub_config {
	uint16_t capabilities;
};

struct vc_stub_data {
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	uint16_t last_output_code;
	bool last_enable;
};

static int stub_set_output(const struct device *dev, uint16_t code)
{
	struct vc_stub_data *data = dev->data;

	data->last_output_code = code;
	return 0;
}

static int stub_set_enable(const struct device *dev, bool enable)
{
	struct vc_stub_data *data = dev->data;

	data->last_enable = enable;
	return 0;
}

static int stub_start_sampling(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int stub_stop_sampling(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static uint16_t stub_get_capabilities(const struct device *dev)
{
	const struct vc_stub_config *cfg = dev->config;

	return cfg->capabilities;
}

static int stub_set_meas_callback(const struct device *dev,
				  vc_meas_ready_cb_t cb, void *user_data)
{
	struct vc_stub_data *data = dev->data;

	data->meas_cb = cb;
	data->meas_cb_user_data = user_data;
	return 0;
}

static const struct vc_channel_hw_api stub_hw_api = {
	.set_output = stub_set_output,
	.set_enable = stub_set_enable,
	.start_sampling = stub_start_sampling,
	.stop_sampling = stub_stop_sampling,
	.get_capabilities = stub_get_capabilities,
	.set_meas_callback = stub_set_meas_callback,
};

static int vc_channel_stub_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define VC_CHANNEL_STUB_DEFINE(inst)                                     \
	static const struct vc_stub_config vc_stub_config_##inst = {     \
		.capabilities = DT_INST_PROP(inst, capabilities),        \
	};                                                               \
	static struct vc_stub_data vc_stub_data_##inst;                  \
	DEVICE_DT_INST_DEFINE(inst, vc_channel_stub_init, NULL,          \
			      &vc_stub_data_##inst,                      \
			      &vc_stub_config_##inst,                    \
			      POST_KERNEL,                               \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,        \
			      &stub_hw_api);

DT_INST_FOREACH_STATUS_OKAY(VC_CHANNEL_STUB_DEFINE)

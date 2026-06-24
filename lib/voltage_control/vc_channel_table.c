/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel_table.h"
#include "voltage_control/vc_channel_hw.h"
#include "voltage_control/vc_controller.h"
#include <zephyr/devicetree.h>
#include <errno.h>

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

static struct vc_controller *g_ctrl;

static struct vc_measurement_buffer_entry
	meas_buffers[DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE)];

#define CH_TABLE_ENTRY(node_id) \
	{ \
		.dev = DEVICE_DT_GET(node_id), \
		.index = DT_REG_ADDR(node_id), \
		.capabilities = DT_PROP(node_id, capabilities), \
		.meas = &meas_buffers[DT_REG_ADDR(node_id)], \
	},

struct vc_channel_table_entry vc_channel_table[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_TABLE_ENTRY)
};

static const size_t vc_channel_table_size = ARRAY_SIZE(vc_channel_table);

void vc_channel_table_init(struct vc_controller *ctrl)
{
	g_ctrl = ctrl;
}

struct vc_controller *vc_channel_table_get_controller(void)
{
	return g_ctrl;
}

static const struct vc_channel_hw_api *get_hw_api(uint8_t ch)
{
	if (ch >= vc_channel_table_size || vc_channel_table[ch].dev == NULL) {
		return NULL;
	}
	return vc_channel_table[ch].dev->api;
}

int vc_channel_table_set_output(uint8_t ch, uint16_t code)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->set_output == NULL) {
		return -ENOTSUP;
	}
	return api->set_output(vc_channel_table[ch].dev, code);
}

int vc_channel_table_set_enable(uint8_t ch, bool enable)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->set_enable == NULL) {
		return -ENOTSUP;
	}
	return api->set_enable(vc_channel_table[ch].dev, enable);
}

int vc_channel_table_start_sampling(uint8_t ch)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->start_sampling == NULL) {
		return -ENOTSUP;
	}
	return api->start_sampling(vc_channel_table[ch].dev);
}

int vc_channel_table_stop_sampling(uint8_t ch)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->stop_sampling == NULL) {
		return -ENOTSUP;
	}
	return api->stop_sampling(vc_channel_table[ch].dev);
}

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch)
{
	if (ch >= vc_channel_table_size) {
		return NULL;
	}
	return vc_channel_table[ch].meas;
}

size_t vc_channel_table_count(void)
{
	return vc_channel_table_size;
}

uint16_t vc_channel_table_capabilities(uint8_t ch)
{
	if (ch >= vc_channel_table_size) {
		return 0;
	}
	return vc_channel_table[ch].capabilities;
}

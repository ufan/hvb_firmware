/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include "voltage_control/domain.h"

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define CH_ENTRY(node_id) \
	{ \
		.dev = DEVICE_DT_GET(node_id), \
		.index = DT_REG_ADDR(node_id), \
		.capabilities = DT_PROP(node_id, capabilities), \
	},

const struct vc_channel_entry vc_domain_channels[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_ENTRY)
};

const size_t vc_domain_channel_count = ARRAY_SIZE(vc_domain_channels);

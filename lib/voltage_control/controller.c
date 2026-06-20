/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compiles only when DTS vc_controller node exists (Kconfig guard).
 */

#include <zephyr/devicetree.h>
#include "voltage_control/domain.h"
#include "voltage_control/vc_channel.h"

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define CH_ENTRY(node_id, prop, idx) \
	[idx] = { \
		.dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)), \
		.index = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
				 channel_index), \
		.capabilities = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
					 capabilities), \
	},

const struct vc_channel_entry vc_domain_channels[] = {
	DT_FOREACH_PROP_ELEM(VC_CONTROLLER_NODE, channels, CH_ENTRY)
};

const size_t vc_domain_channel_count = ARRAY_SIZE(vc_domain_channels);

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#define DT_DRV_COMPAT jianwei_vc_channel_stub

static int vc_channel_stub_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define VC_CHANNEL_STUB_DEFINE(inst) \
	DEVICE_DT_INST_DEFINE(inst, vc_channel_stub_init, NULL, \
			      NULL, NULL, POST_KERNEL, \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(VC_CHANNEL_STUB_DEFINE)

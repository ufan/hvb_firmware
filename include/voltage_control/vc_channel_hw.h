/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_HW_H
#define VOLTAGE_CONTROL_VC_CHANNEL_HW_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct vc_channel_hw_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*start_sampling)(const struct device *dev);
	int (*stop_sampling)(const struct device *dev);
};

#endif

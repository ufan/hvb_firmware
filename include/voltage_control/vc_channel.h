/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_H
#define VOLTAGE_CONTROL_VC_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct vc_runtime_config_snapshot;

struct vc_channel_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*apply_config)(const struct device *dev,
			    const struct vc_runtime_config_snapshot *cfg);
	int (*measure_voltage)(const struct device *dev, int32_t *value);
	int (*measure_current)(const struct device *dev, int32_t *value);
	uint16_t (*get_capabilities)(const struct device *dev);
};

#endif

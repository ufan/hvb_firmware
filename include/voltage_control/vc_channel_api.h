/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_API_H
#define VOLTAGE_CONTROL_VC_CHANNEL_API_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

typedef void (*vc_meas_ready_cb_t)(uint8_t channel, void *user_data);

struct vc_channel_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*start_sampling)(const struct device *dev);
	int (*stop_sampling)(const struct device *dev);
	uint16_t (*get_capabilities)(const struct device *dev);
	int (*set_meas_callback)(const struct device *dev,
				 vc_meas_ready_cb_t cb, void *user_data);
};

struct vc_meas_buffer {
	uint8_t channel_id;
	int32_t raw_voltage;
	uint32_t voltage_timestamp_ms;
	int32_t raw_current;
	uint32_t current_timestamp_ms;
};

#define VC_MEAS_BUFFER_NAME(node_id) \
	UTIL_CAT(_vc_meas_, DT_REG_ADDR(node_id))

#define VC_MEAS_BUFFER_EXTERN(node_id) \
	extern struct vc_meas_buffer VC_MEAS_BUFFER_NAME(node_id)

#define VC_MEAS_BUFFER_PTR(node_id) \
	(&VC_MEAS_BUFFER_NAME(node_id))

#endif

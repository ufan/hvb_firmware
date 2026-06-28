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
#include <zephyr/spinlock.h>

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

struct vc_channel_buffer {
	struct k_spinlock lock;
	uint8_t channel_id;
	int32_t raw_voltage;
	uint32_t voltage_timestamp_ms;
	int32_t raw_current;
	uint32_t current_timestamp_ms;
};

static inline void vc_channel_buffer_publish_voltage(
	struct vc_channel_buffer *buffer, int32_t raw, uint32_t timestamp_ms)
{
	k_spinlock_key_t key = k_spin_lock(&buffer->lock);

	buffer->raw_voltage = raw;
	buffer->voltage_timestamp_ms = timestamp_ms;
	k_spin_unlock(&buffer->lock, key);
}

static inline void vc_channel_buffer_publish_current(
	struct vc_channel_buffer *buffer, int32_t raw, uint32_t timestamp_ms)
{
	k_spinlock_key_t key = k_spin_lock(&buffer->lock);

	buffer->raw_current = raw;
	buffer->current_timestamp_ms = timestamp_ms;
	k_spin_unlock(&buffer->lock, key);
}

static inline void vc_channel_buffer_read(
	struct vc_channel_buffer *buffer,
	int32_t *raw_voltage, uint32_t *voltage_timestamp_ms,
	int32_t *raw_current, uint32_t *current_timestamp_ms)
{
	k_spinlock_key_t key = k_spin_lock(&buffer->lock);

	*raw_voltage = buffer->raw_voltage;
	*voltage_timestamp_ms = buffer->voltage_timestamp_ms;
	*raw_current = buffer->raw_current;
	*current_timestamp_ms = buffer->current_timestamp_ms;
	k_spin_unlock(&buffer->lock, key);
}

#define VC_CHANNEL_BUFFER_NAME(node_id) \
	UTIL_CAT(_vc_ch_buf_, DT_REG_ADDR(node_id))

#define VC_CHANNEL_BUFFER_EXTERN(node_id) \
	extern struct vc_channel_buffer VC_CHANNEL_BUFFER_NAME(node_id)

#define VC_CHANNEL_BUFFER_PTR(node_id) \
	(&VC_CHANNEL_BUFFER_NAME(node_id))

#endif

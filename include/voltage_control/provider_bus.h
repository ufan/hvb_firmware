/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_PROVIDER_BUS_H
#define VOLTAGE_CONTROL_PROVIDER_BUS_H

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>

#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"

enum vc_provider_msg_type {
	VC_PROVIDER_MSG_CONFIG_CHANGED = 0,
	VC_PROVIDER_MSG_SAMPLE_NOW,
	VC_PROVIDER_MSG_STOP,
};

struct vc_provider_msg {
	enum vc_provider_msg_type type;
	uint8_t channel;
	uint32_t config_version;
};

struct vc_runtime_config_slot {
	struct k_mutex lock;
	struct vc_runtime_config_snapshot snapshot;
};

struct vc_provider_binding {
	uint8_t channel;
	const struct device *dev;
	struct vc_runtime_config_slot *config_slot;
	uint32_t route_bit;
};

struct vc_measurement_buffer_entry {
	struct k_mutex lock;
	struct vc_measurement_snapshot snapshot;
};

extern struct vc_runtime_config_slot vc_runtime_config_slots[VC_MAX_CHANNELS];

/* Initialize config slots, purge message queues, and init measurement buffers. */
void vc_provider_bus_init(void);
/* Write a runtime config snapshot into the slot and enqueue a CONFIG_CHANGED message. */
enum vc_status vc_provider_bus_publish_config(uint8_t channel,
					      const struct vc_runtime_config_snapshot *cfg);
/* Lock and return a pointer to the config slot. Caller must call release_config after use. */
const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel);
/* Unlock the config slot acquired by acquire_config. */
void vc_provider_bus_release_config(uint8_t channel);
/* Enqueue a measurement snapshot for the runtime worker to consume. */
enum vc_status vc_provider_bus_publish_measurement(
	const struct vc_measurement_snapshot *meas);
/* Dequeue a measurement snapshot (non-blocking). */
enum vc_status vc_provider_bus_take_measurement(struct vc_measurement_snapshot *meas);
/* Dequeue a provider message (CONFIG_CHANGED / SAMPLE_NOW / STOP). */
enum vc_status vc_provider_bus_take_message(struct vc_provider_msg *msg,
					    k_timeout_t timeout);

/* Count the number of registered vc_provider_binding entries (iterable section). */
size_t vc_provider_bus_binding_count(void);
/* Call start() on all registered provider bindings. */
enum vc_status vc_provider_bus_start_all(void);
/* Notify the channel's driver that its config version changed. */
enum vc_status vc_provider_bus_notify_channel(uint8_t channel, uint32_t version);
/* Dequeue and dispatch one provider message (notify the appropriate channel). */
enum vc_status vc_provider_bus_dispatch_one(k_timeout_t timeout);

/* Initialize per-channel measurement buffer mutexes (iterable section). */
void vc_measurement_buffer_init(void);
/* Store a measurement snapshot into the per-channel buffer (latest wins). */
enum vc_status vc_measurement_buffer_store(uint8_t channel,
					   const struct vc_measurement_snapshot *meas);
/* Read the latest measurement snapshot from the per-channel buffer. */
enum vc_status vc_measurement_buffer_read(uint8_t channel,
					  struct vc_measurement_snapshot *meas);
/* Return the number of measurement buffer entries (iterable section count). */
size_t vc_measurement_buffer_count(void);

#endif

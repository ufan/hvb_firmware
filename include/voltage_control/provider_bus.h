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

extern struct vc_runtime_config_slot vc_runtime_config_slots[VC_MAX_CHANNELS];

void vc_provider_bus_init(void);
enum vc_status vc_provider_bus_publish_config(uint8_t channel,
					      const struct vc_runtime_config_snapshot *cfg);
const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel);
void vc_provider_bus_release_config(uint8_t channel);
enum vc_status vc_provider_bus_publish_measurement(
	const struct vc_measurement_snapshot *meas);
enum vc_status vc_provider_bus_take_measurement(struct vc_measurement_snapshot *meas);
enum vc_status vc_provider_bus_take_message(struct vc_provider_msg *msg,
					    k_timeout_t timeout);

size_t vc_provider_bus_binding_count(void);
enum vc_status vc_provider_bus_start_all(void);
enum vc_status vc_provider_bus_notify_channel(uint8_t channel, uint32_t version);
enum vc_status vc_provider_bus_dispatch_one(k_timeout_t timeout);

#endif

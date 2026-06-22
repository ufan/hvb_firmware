/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "voltage_control/provider_bus.h"
#include "voltage_control/vc_channel.h"

struct vc_runtime_config_slot vc_runtime_config_slots[VC_MAX_CHANNELS];

K_MSGQ_DEFINE(vc_provider_msgq,
	      sizeof(struct vc_provider_msg),
	      CONFIG_VC_PROVIDER_MSGQ_DEPTH,
	      4);

K_MSGQ_DEFINE(vc_runtime_evidence_msgq,
	      sizeof(struct vc_measurement_snapshot),
	      CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH,
	      4);

void vc_provider_bus_init(void)
{
	for (size_t i = 0; i < VC_MAX_CHANNELS; i++) {
		k_mutex_init(&vc_runtime_config_slots[i].lock);
		memset(&vc_runtime_config_slots[i].snapshot, 0,
		       sizeof(vc_runtime_config_slots[i].snapshot));
	}
	k_msgq_purge(&vc_provider_msgq);
	k_msgq_purge(&vc_runtime_evidence_msgq);
	vc_measurement_buffer_init();
}

enum vc_status vc_provider_bus_publish_config(uint8_t channel,
					       const struct vc_runtime_config_snapshot *cfg)
{
	struct vc_provider_msg msg;

	if (cfg == NULL || channel >= VC_MAX_CHANNELS) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&vc_runtime_config_slots[channel].lock, K_FOREVER);
	vc_runtime_config_slots[channel].snapshot = *cfg;
	k_mutex_unlock(&vc_runtime_config_slots[channel].lock);

	msg = (struct vc_provider_msg){
		.type = VC_PROVIDER_MSG_CONFIG_CHANGED,
		.channel = channel,
		.config_version = cfg->version,
	};

	if (k_msgq_put(&vc_provider_msgq, &msg, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel)
{
	if (channel >= VC_MAX_CHANNELS) {
		return NULL;
	}

	k_mutex_lock(&vc_runtime_config_slots[channel].lock, K_FOREVER);
	return &vc_runtime_config_slots[channel].snapshot;
}

void vc_provider_bus_release_config(uint8_t channel)
{
	if (channel < VC_MAX_CHANNELS) {
		k_mutex_unlock(&vc_runtime_config_slots[channel].lock);
	}
}

enum vc_status vc_provider_bus_take_message(struct vc_provider_msg *msg,
					     k_timeout_t timeout)
{
	if (msg == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_get(&vc_provider_msgq, msg, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

enum vc_status vc_provider_bus_publish_measurement(
	const struct vc_measurement_snapshot *meas)
{
	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_put(&vc_runtime_evidence_msgq, meas, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

enum vc_status vc_provider_bus_take_measurement(struct vc_measurement_snapshot *meas)
{
	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_get(&vc_runtime_evidence_msgq, meas, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

size_t vc_provider_bus_binding_count(void)
{
	size_t count = 0;
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		ARG_UNUSED(binding);
		count++;
	}
	return count;
}

enum vc_status vc_provider_bus_start_all(void)
{
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		const struct vc_channel_api *api = binding->dev->api;

		if (api != NULL && api->start != NULL) {
			int ret = api->start(binding->dev);
			if (ret < 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	return VC_OK;
}

enum vc_status vc_provider_bus_notify_channel(uint8_t channel, uint32_t version)
{
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		if (binding->channel == channel) {
			const struct vc_channel_api *api = binding->dev->api;

			if (api != NULL && api->notify_config_changed != NULL) {
				int ret = api->notify_config_changed(binding->dev, version);
				return ret < 0 ? VC_ERR_UNSAFE_STATE : VC_OK;
			}
			return VC_OK;
		}
	}
	return VC_ERR_UNSUPPORTED_CHANNEL;
}

enum vc_status vc_provider_bus_dispatch_one(k_timeout_t timeout)
{
	struct vc_provider_msg msg;
	enum vc_status status = vc_provider_bus_take_message(&msg, timeout);

	if (status != VC_OK) {
		return status;
	}

	switch (msg.type) {
	case VC_PROVIDER_MSG_CONFIG_CHANGED:
	case VC_PROVIDER_MSG_SAMPLE_NOW:
		return vc_provider_bus_notify_channel(msg.channel, msg.config_version);
	case VC_PROVIDER_MSG_STOP:
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

void vc_measurement_buffer_init(void)
{
	STRUCT_SECTION_FOREACH(vc_measurement_buffer_entry, entry) {
		k_mutex_init(&entry->lock);
		memset(&entry->snapshot, 0, sizeof(entry->snapshot));
	}
}

enum vc_status vc_measurement_buffer_store(uint8_t channel,
					   const struct vc_measurement_snapshot *meas)
{
	struct vc_measurement_buffer_entry *entry;
	size_t count;

	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	if (channel >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	STRUCT_SECTION_GET(vc_measurement_buffer_entry, channel, &entry);
	k_mutex_lock(&entry->lock, K_FOREVER);
	entry->snapshot = *meas;
	k_mutex_unlock(&entry->lock);

	return VC_OK;
}

enum vc_status vc_measurement_buffer_read(uint8_t channel,
					  struct vc_measurement_snapshot *meas)
{
	struct vc_measurement_buffer_entry *entry;
	size_t count;

	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	if (channel >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	STRUCT_SECTION_GET(vc_measurement_buffer_entry, channel, &entry);
	k_mutex_lock(&entry->lock, K_FOREVER);
	*meas = entry->snapshot;
	k_mutex_unlock(&entry->lock);

	return VC_OK;
}

size_t vc_measurement_buffer_count(void)
{
	size_t count;

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	return count;
}

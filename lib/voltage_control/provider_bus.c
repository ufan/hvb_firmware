/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "voltage_control/provider_bus.h"

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

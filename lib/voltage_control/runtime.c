/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "voltage_control/runtime.h"

struct vc_runtime {
	struct domain *domain;
	struct k_mutex lock;
};

struct vc_runtime *vc_runtime_create(struct domain *domain)
{
	struct vc_runtime *runtime;

	if (domain == NULL) {
		return NULL;
	}

	runtime = malloc(sizeof(*runtime));
	if (runtime == NULL) {
		return NULL;
	}

	runtime->domain = domain;
	k_mutex_init(&runtime->lock);

	return runtime;
}

void vc_runtime_destroy(struct vc_runtime *runtime)
{
	free(runtime);
}

enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas)
{
	enum vc_status status;

	if (runtime == NULL || meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&runtime->lock, K_FOREVER);
	status = domain_consume_measurement(runtime->domain, meas);
	k_mutex_unlock(&runtime->lock);

	return status;
}

enum vc_status vc_runtime_get_channel_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_runtime_config_snapshot *cfg)
{
	enum vc_status status;

	if (runtime == NULL || cfg == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&runtime->lock, K_FOREVER);
	status = domain_get_runtime_config(runtime->domain, channel, cfg);
	k_mutex_unlock(&runtime->lock);

	return status;
}

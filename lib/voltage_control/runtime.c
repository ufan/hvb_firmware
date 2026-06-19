/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "voltage_control/runtime.h"

K_KERNEL_STACK_DEFINE(vc_runtime_stack, CONFIG_VC_RUNTIME_THREAD_STACK_SIZE);

struct vc_runtime_work_item {
	struct vc_runtime_command command;
};

struct vc_runtime_evidence_item {
	struct vc_measurement_snapshot measurement;
};

struct vc_runtime {
	struct domain *domain;
	struct k_mutex lock;
	struct k_msgq command_queue;
	struct k_msgq evidence_queue;
	struct k_sem wake;
	struct k_thread thread;
	k_tid_t tid;
	bool stop_requested;
	char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH * sizeof(struct vc_runtime_work_item)];
	char evidence_buffer[CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH * sizeof(struct vc_runtime_evidence_item)];
};

static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
						  const struct vc_runtime_command *cmd)
{
	ARG_UNUSED(runtime);
	ARG_UNUSED(cmd);
	return VC_ERR_INVALID_COMMAND;
}

static void vc_runtime_worker(void *p1, void *p2, void *p3)
{
	struct vc_runtime *runtime = p1;
	struct vc_runtime_work_item work;
	struct vc_runtime_evidence_item evidence;
	int wake_ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!runtime->stop_requested) {
		wake_ret = k_sem_take(&runtime->wake, K_MSEC(CONFIG_VC_RUNTIME_TICK_INTERVAL_MS));

		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;

			k_mutex_lock(&runtime->lock, K_FOREVER);
			result = vc_runtime_dispatch_command(runtime, &work.command);
			k_mutex_unlock(&runtime->lock);

			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		while (k_msgq_get(&runtime->evidence_queue, &evidence, K_NO_WAIT) == 0) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			k_mutex_unlock(&runtime->lock);
		}

		if (wake_ret == -EAGAIN) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			domain_tick(runtime->domain, CONFIG_VC_RUNTIME_TICK_INTERVAL_MS, NULL, NULL);
			k_mutex_unlock(&runtime->lock);
		}
	}
}

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
	runtime->stop_requested = false;
	k_mutex_init(&runtime->lock);
	k_sem_init(&runtime->wake, 0, 1);
	k_msgq_init(&runtime->command_queue, runtime->command_buffer,
		    sizeof(struct vc_runtime_work_item), CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH);
	k_msgq_init(&runtime->evidence_queue, runtime->evidence_buffer,
		    sizeof(struct vc_runtime_evidence_item), CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH);
	runtime->tid = k_thread_create(&runtime->thread, vc_runtime_stack,
				       K_KERNEL_STACK_SIZEOF(vc_runtime_stack),
				       vc_runtime_worker, runtime, NULL, NULL,
				       CONFIG_VC_RUNTIME_THREAD_PRIORITY, 0, K_NO_WAIT);

	return runtime;
}

void vc_runtime_destroy(struct vc_runtime *runtime)
{
	if (runtime == NULL) {
		return;
	}

	runtime->stop_requested = true;
	k_sem_give(&runtime->wake);
	(void)k_thread_join(&runtime->thread, K_FOREVER);
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

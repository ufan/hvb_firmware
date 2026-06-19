/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "voltage_control/runtime.h"
#include "voltage_control/provider_bus.h"

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
	bool stop_requested;
	char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH * sizeof(struct vc_runtime_work_item)];
	char evidence_buffer[CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH * sizeof(struct vc_runtime_evidence_item)];
};

static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
						  const struct vc_runtime_command *cmd)
{
	switch (cmd->type) {
	case VC_RUNTIME_CMD_SET_OPERATING_MODE:
		return domain_set_operating_mode(runtime->domain, cmd->payload.operating_mode);
	case VC_RUNTIME_CMD_SET_SYSTEM_CONFIG:
		return domain_set_system_config(runtime->domain, &cmd->payload.system_config);
	case VC_RUNTIME_CMD_SET_CHANNEL_CONFIG:
		return domain_set_channel_config(runtime->domain, cmd->channel,
						 &cmd->payload.channel_config);
	case VC_RUNTIME_CMD_OUTPUT_ACTION:
		return domain_channel_output_action(runtime->domain, cmd->channel,
						    cmd->payload.output_action);
	case VC_RUNTIME_CMD_FAULT_COMMAND:
		return domain_channel_fault_command(runtime->domain, cmd->channel,
						    cmd->payload.fault_command);
	case VC_RUNTIME_CMD_CALIBRATION_UNLOCK:
		return domain_calibration_unlock(runtime->domain,
						 cmd->payload.calibration_unlock_value);
	case VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE:
		return domain_calibration_set_output_enable(runtime->domain, cmd->channel,
							    cmd->payload.calibration_output_enable);
	case VC_RUNTIME_CMD_CALIBRATION_RAW_DAC:
		return domain_calibration_set_raw_dac(runtime->domain, cmd->channel,
						      cmd->payload.calibration_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_SAMPLE:
		return domain_calibration_sample(runtime->domain, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_COMMIT:
		return domain_calibration_commit(runtime->domain, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC:
		return domain_calibration_set_max_raw_dac(runtime->domain, cmd->channel,
							 cmd->payload.calibration_max_raw_dac);
	case VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION:
		return domain_system_param_action(runtime->domain, cmd->payload.param_action);
	case VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION:
		return domain_channel_param_action(runtime->domain, cmd->channel,
						   cmd->payload.param_action);
	case VC_RUNTIME_CMD_SET_UPTIME:
		domain_set_uptime(runtime->domain, cmd->payload.uptime_seconds);
		return VC_OK;
	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

static void vc_runtime_publish_all_configs(struct vc_runtime *runtime)
{
	uint16_t count = domain_get_supported_channel_count(runtime->domain);

	for (uint8_t ch = 0; ch < count; ch++) {
		struct vc_runtime_config_snapshot cfg;

		if (domain_get_runtime_config(runtime->domain, ch, &cfg) == VC_OK) {
			(void)vc_provider_bus_publish_config(ch, &cfg);
			(void)vc_provider_bus_dispatch_one(K_NO_WAIT);
		}
	}
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
			vc_runtime_publish_all_configs(runtime);
			k_mutex_unlock(&runtime->lock);

			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		while (vc_provider_bus_take_measurement(&evidence.measurement) == VC_OK) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			vc_runtime_publish_all_configs(runtime);
			k_mutex_unlock(&runtime->lock);
		}

		if (wake_ret == -EAGAIN) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			domain_tick(runtime->domain, CONFIG_VC_RUNTIME_TICK_INTERVAL_MS, NULL, NULL);
			vc_runtime_publish_all_configs(runtime);
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
	vc_provider_bus_init();
	vc_runtime_publish_all_configs(runtime);
	k_msgq_init(&runtime->command_queue, runtime->command_buffer,
		    sizeof(struct vc_runtime_work_item), CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH);
	k_msgq_init(&runtime->evidence_queue, runtime->evidence_buffer,
		    sizeof(struct vc_runtime_evidence_item), CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH);
	(void)k_thread_create(&runtime->thread, vc_runtime_stack,
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

enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
					 const struct vc_runtime_command *cmd,
					 k_timeout_t timeout)
{
	struct vc_runtime_work_item work;
	struct k_sem result_sem;
	enum vc_status result = VC_ERR_UNSAFE_STATE;

	if (runtime == NULL || cmd == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	work.command = *cmd;
	work.command.result = &result;
	work.command.result_sem = &result_sem;
	k_sem_init(&result_sem, 0, 1);

	if (k_msgq_put(&runtime->command_queue, &work, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}
	k_sem_give(&runtime->wake);

	k_sem_take(&result_sem, K_FOREVER);

	return result;
}

enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
					     enum vc_operating_mode mode,
					     k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_OPERATING_MODE,
		.payload.operating_mode = mode,
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_set_uptime(struct vc_runtime *runtime,
				     uint32_t seconds,
				     k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_UPTIME,
		.payload.uptime_seconds = seconds,
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

	enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas)
{
	if (runtime == NULL || meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (vc_provider_bus_publish_measurement(meas) != VC_OK) {
		return VC_ERR_UNSAFE_STATE;
	}
	k_sem_give(&runtime->wake);
	return VC_OK;
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

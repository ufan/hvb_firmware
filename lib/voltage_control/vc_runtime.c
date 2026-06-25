/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "voltage_control/vc_runtime.h"
#include "voltage_control/vc_controller.h"
#include "voltage_control/vc_storage.h"

#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
#include <zephyr/settings/settings.h>
#endif

K_KERNEL_STACK_DEFINE(vc_runtime_stack, CONFIG_VC_RUNTIME_THREAD_STACK_SIZE);

struct vc_runtime_work_item {
	struct vc_runtime_command command;
};

struct vc_published_snapshot {
	struct vc_system_snapshot system;
	struct vc_channel_snapshot channels[VC_MAX_CHANNELS];
	struct vc_channel_config configs[VC_MAX_CHANNELS];
	struct vc_system_config sys_config;
};

struct vc_runtime {
	struct vc_controller *ctrl;
	struct k_mutex lock;
	struct k_msgq command_queue;
	struct k_sem wake;
	struct k_thread thread;
	bool stop_requested;
	struct vc_published_snapshot published;
	struct k_mutex snapshot_lock;
	char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH *
			    sizeof(struct vc_runtime_work_item)];
};

static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
						  const struct vc_runtime_command *cmd)
{
	struct vc_controller *ctrl = runtime->ctrl;

	switch (cmd->type) {
	case VC_RUNTIME_CMD_SET_OPERATING_MODE:
		return vc_controller_set_operating_mode(ctrl,
							cmd->payload.operating_mode);
	case VC_RUNTIME_CMD_OUTPUT_ACTION:
		return vc_controller_channel_output_action(ctrl, cmd->channel,
							   cmd->payload.output_action);
	case VC_RUNTIME_CMD_FAULT_COMMAND:
		return vc_controller_channel_fault_command(ctrl, cmd->channel,
							   cmd->payload.fault_command);
	case VC_RUNTIME_CMD_CALIBRATION_UNLOCK:
		return vc_controller_calibration_unlock(ctrl,
						       cmd->payload.calibration_unlock_value);
	case VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE:
		return vc_controller_channel_cal_output_enable(ctrl, cmd->channel,
							      cmd->payload.calibration_output_enable);
	case VC_RUNTIME_CMD_CALIBRATION_RAW_DAC:
		return vc_controller_channel_cal_raw_dac(ctrl, cmd->channel,
							 cmd->payload.calibration_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_SAMPLE:
		return vc_controller_channel_cal_sample(ctrl, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_COMMIT:
		return vc_controller_channel_cal_commit(ctrl, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC:
		return vc_controller_channel_cal_max_raw_dac(ctrl, cmd->channel,
							    cmd->payload.calibration_max_raw_dac);
	case VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION:
		return vc_controller_system_param_action(ctrl,
							 cmd->payload.param_action);
	case VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION:
		return vc_controller_channel_param_action(ctrl, cmd->channel,
							  cmd->payload.param_action);
	case VC_RUNTIME_CMD_SET_SYSTEM_FIELD:
		return vc_controller_set_system_field(ctrl,
						     cmd->payload.field_write.field,
						     cmd->payload.field_write.value);
	case VC_RUNTIME_CMD_SET_CHANNEL_FIELD:
		return vc_controller_channel_set_field(ctrl, cmd->channel,
						      cmd->payload.field_write.field,
						      cmd->payload.field_write.value);
	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

static void vc_runtime_publish_snapshot(struct vc_runtime *runtime)
{
	struct vc_controller *ctrl = runtime->ctrl;
	size_t count = vc_controller_channel_count(ctrl);

	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	vc_controller_get_system_snapshot(ctrl, &runtime->published.system);
	vc_controller_get_system_config(ctrl, &runtime->published.sys_config);
	for (uint8_t ch = 0; ch < count; ch++) {
		vc_controller_get_channel_snapshot(ctrl, ch,
						   &runtime->published.channels[ch]);
		vc_controller_get_channel_config(ctrl, ch,
						 &runtime->published.configs[ch]);
	}
	k_mutex_unlock(&runtime->snapshot_lock);
}

static void runtime_wake(void *user_data)
{
	struct vc_runtime *runtime = user_data;

	k_sem_give(&runtime->wake);
}

static void vc_runtime_worker(void *p1, void *p2, void *p3)
{
	struct vc_runtime *runtime = p1;
	struct vc_runtime_work_item work;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!runtime->stop_requested) {
		k_sem_take(&runtime->wake,
			   K_MSEC(CONFIG_VC_RUNTIME_TICK_INTERVAL_MS));

		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;

			result = vc_runtime_dispatch_command(runtime, &work.command);

			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		vc_controller_tick(runtime->ctrl,
				   CONFIG_VC_RUNTIME_TICK_INTERVAL_MS);

		vc_runtime_publish_snapshot(runtime);
	}
}

static void vc_runtime_auto_load(struct vc_runtime *runtime)
{
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
	struct vc_controller *ctrl = runtime->ctrl;

	vc_controller_set_storage_backend(ctrl, &vc_settings_storage);
	settings_subsys_init();

	(void)vc_controller_system_param_action(ctrl, VC_PARAM_ACTION_LOAD);

	size_t count = vc_controller_channel_count(ctrl);

	for (uint8_t ch = 0; ch < count; ch++) {
		(void)vc_controller_channel_param_action(ctrl, ch,
							 VC_PARAM_ACTION_LOAD);
	}
#else
	ARG_UNUSED(runtime);
#endif
}

struct vc_runtime *vc_runtime_create_static(void)
{
	static struct vc_runtime runtime;
	struct vc_controller *ctrl;

	memset(&runtime, 0, sizeof(runtime));
	runtime.stop_requested = false;
	k_mutex_init(&runtime.lock);
	k_mutex_init(&runtime.snapshot_lock);
	k_sem_init(&runtime.wake, 0, 1);

	ctrl = vc_controller_init(runtime_wake, &runtime);
	if (ctrl == NULL) {
		return NULL;
	}

	runtime.ctrl = ctrl;

	vc_runtime_auto_load(&runtime);
	vc_runtime_publish_snapshot(&runtime);

	k_msgq_init(&runtime.command_queue, runtime.command_buffer,
		     sizeof(struct vc_runtime_work_item),
		     CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH);

	(void)k_thread_create(&runtime.thread, vc_runtime_stack,
			       K_KERNEL_STACK_SIZEOF(vc_runtime_stack),
			       vc_runtime_worker, &runtime, NULL, NULL,
			       CONFIG_VC_RUNTIME_THREAD_PRIORITY, 0, K_NO_WAIT);

	return &runtime;
}

void vc_runtime_destroy(struct vc_runtime *runtime)
{
	if (runtime == NULL) {
		return;
	}

	runtime->stop_requested = true;
	k_sem_give(&runtime->wake);
	(void)k_thread_join(&runtime->thread, K_FOREVER);
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

enum vc_status vc_runtime_set_system_field(struct vc_runtime *runtime,
					   enum vc_config_field field,
					   uint16_t value,
					   k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
		.payload.field_write = { .field = field, .value = value },
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_set_channel_field(struct vc_runtime *runtime,
					    uint8_t channel,
					    enum vc_config_field field,
					    uint16_t value,
					    k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
		.channel = channel,
		.payload.field_write = { .field = field, .value = value },
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas)
{
	struct vc_controller *ctrl;
	uint8_t ch;

	if (runtime == NULL || meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	ctrl = runtime->ctrl;
	ch = meas->channel;

	if (ch >= vc_controller_channel_count(ctrl)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	if (meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) {
		uint16_t fault_cause = 0;

		if (meas->provider_status & VC_PROVIDER_STATUS_APPLY_FAILED) {
			fault_cause |= VC_FAULT_HARDWARE;
		}
		if (meas->provider_status & VC_PROVIDER_STATUS_SAMPLE_ERROR) {
			fault_cause |= VC_FAULT_MEASUREMENT;
		}
		if (meas->provider_status & VC_PROVIDER_STATUS_INTERLOCK) {
			fault_cause |= VC_FAULT_INTERLOCK;
		}
		if (fault_cause) {
			vc_channel_consume_fault(&ctrl->channels[ch], fault_cause);
		}
	}
	if (meas->present_mask & VC_MEAS_PRESENT_VOLTAGE) {
		vc_channel_consume_voltage(&ctrl->channels[ch], meas->raw_voltage);
	}
	if (meas->present_mask & VC_MEAS_PRESENT_CURRENT) {
		vc_channel_consume_current(&ctrl->channels[ch], meas->raw_current);
	}

	return VC_OK;
}

enum vc_status vc_runtime_get_published_system_snapshot(
	struct vc_runtime *runtime,
	struct vc_system_snapshot *snap)
{
	if (runtime == NULL || snap == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	*snap = runtime->published.system;
	k_mutex_unlock(&runtime->snapshot_lock);
	return VC_OK;
}

enum vc_status vc_runtime_get_published_channel_snapshot(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_snapshot *snap)
{
	if (runtime == NULL || snap == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	if (channel >= vc_controller_channel_count(runtime->ctrl)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	*snap = runtime->published.channels[channel];
	k_mutex_unlock(&runtime->snapshot_lock);
	return VC_OK;
}

enum vc_status vc_runtime_get_published_system_config(
	struct vc_runtime *runtime,
	struct vc_system_config *cfg)
{
	if (runtime == NULL || cfg == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	*cfg = runtime->published.sys_config;
	k_mutex_unlock(&runtime->snapshot_lock);
	return VC_OK;
}

enum vc_status vc_runtime_get_published_channel_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_config *cfg)
{
	if (runtime == NULL || cfg == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	if (channel >= vc_controller_channel_count(runtime->ctrl)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	*cfg = runtime->published.configs[channel];
	k_mutex_unlock(&runtime->snapshot_lock);
	return VC_OK;
}

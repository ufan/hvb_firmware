/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_RUNTIME_H
#define VOLTAGE_CONTROL_VC_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "voltage_control/vc_types.h"

enum vc_runtime_command_type {
	VC_RUNTIME_CMD_SET_OPERATING_MODE = 0,
	VC_RUNTIME_CMD_OUTPUT_ACTION,
	VC_RUNTIME_CMD_FAULT_COMMAND,
	VC_RUNTIME_CMD_CALIBRATION_UNLOCK,
	VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE,
	VC_RUNTIME_CMD_CALIBRATION_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_SAMPLE,
	VC_RUNTIME_CMD_CALIBRATION_COMMIT,
	VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_EXIT,
	VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
	VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
	VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
	VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
	VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
};

struct vc_runtime_command {
	enum vc_runtime_command_type type;
	uint8_t channel;
	union {
		enum vc_operating_mode operating_mode;
		enum vc_output_action output_action;
		enum vc_channel_fault_command fault_command;
		uint16_t calibration_unlock_value;
		bool calibration_output_enable;
		uint16_t calibration_raw_dac;
		uint16_t calibration_max_raw_dac;
		enum vc_param_action param_action;
		struct vc_field_write field_write;
		struct { enum vc_cal_field field; uint16_t value; } cal_field_write;
	} payload;
	struct k_sem *result_sem;
	enum vc_status *result;
};

struct vc_runtime;

/* Start hardware sampling on all channels. */
enum vc_status vc_runtime_start_sampling(struct vc_runtime *runtime);
/* Stop the runtime worker thread and free if heap-allocated. */
void vc_runtime_destroy(struct vc_runtime *runtime);

/* Create a runtime + controller in static storage (single-instance). */
struct vc_runtime *vc_runtime_create_static(void);

/* Enqueue a command and block until the worker thread processes it. */
enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
					 const struct vc_runtime_command *cmd,
					 k_timeout_t timeout);
/* Convenience: submit a set-operating-mode command. */
enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
					     enum vc_operating_mode mode,
					     k_timeout_t timeout);
/* Convenience: submit a set-system-field command. */
enum vc_status vc_runtime_set_system_field(struct vc_runtime *runtime,
					   enum vc_config_field field,
					   uint16_t value,
					   k_timeout_t timeout);
/* Convenience: submit a set-channel-field command. */
enum vc_status vc_runtime_set_channel_field(struct vc_runtime *runtime,
					    uint8_t channel,
					    enum vc_config_field field,
					    uint16_t value,
					    k_timeout_t timeout);

/* Convenience: submit a set-channel-cal-field command. */
enum vc_status vc_runtime_set_channel_cal_field(struct vc_runtime *runtime,
						uint8_t channel,
						enum vc_cal_field field,
						uint16_t value,
						k_timeout_t timeout);

/* Peek at remaining calibration watchdog time without resetting it.
 * Uses the catalog singleton. Returns 0 when not in calibration mode. */
uint16_t vc_runtime_peek_cal_watchdog_s(void);

#endif

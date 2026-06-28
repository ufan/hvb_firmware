/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified voltage-control API.
 *
 * vc_init() constructs the singleton from DTS-composed channel data.
 * Frontends access state and commands through the Register Catalog.
 */

#ifndef VOLTAGE_CONTROL_VC_H
#define VOLTAGE_CONTROL_VC_H

#include "voltage_control/vc_runtime.h"

/* ------------------------------------------------------------------ */
/* Opaque context                                                      */
/* ------------------------------------------------------------------ */

struct vc_ctx;

/* Create the singleton vc_ctx from DTS channel data; returns NULL on failure. */
struct vc_ctx *vc_init(void);
/* Stop the runtime worker and release the singleton context. */
void vc_destroy(struct vc_ctx *ctx);
/* Start channel hw sampling; call once after vc_init. */
enum vc_status vc_ctx_start(struct vc_ctx *ctx);

/* ------------------------------------------------------------------ */
/* Calibration command (6 functions -> 1 struct)                       */
/* ------------------------------------------------------------------ */

enum vc_cal_action {
	VC_CAL_UNLOCK,
	VC_CAL_SET_OUTPUT_ENABLE,
	VC_CAL_SET_RAW_DAC,
	VC_CAL_SAMPLE,
	VC_CAL_COMMIT,
	VC_CAL_SET_MAX_RAW_DAC,
	VC_CAL_EXIT,
};

struct vc_cal_command {
	enum vc_cal_action action;
	uint8_t channel;
	uint16_t value;
	bool enable;
};

/* ------------------------------------------------------------------ */
/* Command types                                                       */
/* ------------------------------------------------------------------ */

enum vc_cmd_type {
	VC_CMD_SET_OPERATING_MODE,
	VC_CMD_OUTPUT_ACTION,
	VC_CMD_FAULT_COMMAND,
	VC_CMD_SET_SYSTEM_FIELD,
	VC_CMD_SET_CHANNEL_FIELD,
	VC_CMD_SET_CHANNEL_CAL_FIELD,
	VC_CMD_CALIBRATION,
	VC_CMD_SYSTEM_PARAM_ACTION,
	VC_CMD_CHANNEL_PARAM_ACTION,
};

struct vc_cmd {
	enum vc_cmd_type type;
	uint8_t channel;
	union {
		enum vc_operating_mode operating_mode;
		enum vc_output_action output_action;
		enum vc_channel_fault_command fault_command;
		struct vc_field_write field_write;
		struct { enum vc_cal_field field; uint16_t value; } cal_field_write;
		struct vc_cal_command cal;
		enum vc_param_action param_action;
	};
};

/* ------------------------------------------------------------------ */
/* Command builders                                                    */
/* ------------------------------------------------------------------ */

/* Build a calibration exit command — restores the pre-calibration operating mode. */
static inline struct vc_cmd vc_cmd_cal_exit(void)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.cal = { .action = VC_CAL_EXIT },
	};
}

/* Build a command to switch operating mode (normal/automatic/calibration). */
static inline struct vc_cmd vc_cmd_set_mode(enum vc_operating_mode mode)
{
	return (struct vc_cmd){
		.type = VC_CMD_SET_OPERATING_MODE,
		.operating_mode = mode,
	};
}

/* Build a command to enable/disable channel output. */
static inline struct vc_cmd vc_cmd_output(uint8_t ch,
					  enum vc_output_action action)
{
	return (struct vc_cmd){
		.type = VC_CMD_OUTPUT_ACTION,
		.channel = ch,
		.output_action = action,
	};
}

/* Build a command to clear active or history faults on a channel. */
static inline struct vc_cmd vc_cmd_fault(uint8_t ch,
					 enum vc_channel_fault_command cmd)
{
	return (struct vc_cmd){
		.type = VC_CMD_FAULT_COMMAND,
		.channel = ch,
		.fault_command = cmd,
	};
}

/* Build a command to write a single system config field. */
static inline struct vc_cmd vc_cmd_sys_field(enum vc_config_field field,
					     uint16_t value)
{
	return (struct vc_cmd){
		.type = VC_CMD_SET_SYSTEM_FIELD,
		.field_write = { .field = field, .value = value },
	};
}

/* Build a command to write a single channel config field. */
static inline struct vc_cmd vc_cmd_ch_field(uint8_t ch,
					    enum vc_config_field field,
					    uint16_t value)
{
	return (struct vc_cmd){
		.type = VC_CMD_SET_CHANNEL_FIELD,
		.channel = ch,
		.field_write = { .field = field, .value = value },
	};
}

/* Build a command to write a single channel calibration coefficient field. */
static inline struct vc_cmd vc_cmd_cal_set_field(uint8_t ch,
						 enum vc_cal_field field,
						 uint16_t value)
{
	return (struct vc_cmd){
		.type = VC_CMD_SET_CHANNEL_CAL_FIELD,
		.channel = ch,
		.cal_field_write = { .field = field, .value = value },
	};
}

/* Build a calibration unlock command (two-step: STEP1 then STEP2). */
static inline struct vc_cmd vc_cmd_cal_unlock(uint16_t value)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.cal = { .action = VC_CAL_UNLOCK, .value = value },
	};
}

/* Build a calibration output enable/disable command. */
static inline struct vc_cmd vc_cmd_cal_output(uint8_t ch, bool enable)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.channel = ch,
		.cal = { .action = VC_CAL_SET_OUTPUT_ENABLE,
			 .channel = ch, .enable = enable },
	};
}

/* Build a calibration raw DAC code write command. */
static inline struct vc_cmd vc_cmd_cal_dac(uint8_t ch, uint16_t code)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.channel = ch,
		.cal = { .action = VC_CAL_SET_RAW_DAC,
			 .channel = ch, .value = code },
	};
}

/* Build a calibration ADC sample trigger command. */
static inline struct vc_cmd vc_cmd_cal_sample(uint8_t ch)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.channel = ch,
		.cal = { .action = VC_CAL_SAMPLE, .channel = ch },
	};
}

/* Build a calibration commit command (finalize calibration data). */
static inline struct vc_cmd vc_cmd_cal_commit(uint8_t ch)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.channel = ch,
		.cal = { .action = VC_CAL_COMMIT, .channel = ch },
	};
}

/* Build a command to set the max raw DAC limit for calibration. */
static inline struct vc_cmd vc_cmd_cal_max_dac(uint8_t ch, uint16_t limit)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.channel = ch,
		.cal = { .action = VC_CAL_SET_MAX_RAW_DAC,
			 .channel = ch, .value = limit },
	};
}

/* Build a system-level param action command (save/load/factory-reset/reboot). */
static inline struct vc_cmd vc_cmd_sys_param(enum vc_param_action action)
{
	return (struct vc_cmd){
		.type = VC_CMD_SYSTEM_PARAM_ACTION,
		.param_action = action,
	};
}

/* Build a channel-level param action command (save/load/factory-reset/reboot). */
static inline struct vc_cmd vc_cmd_ch_param(uint8_t ch,
					    enum vc_param_action action)
{
	return (struct vc_cmd){
		.type = VC_CMD_CHANNEL_PARAM_ACTION,
		.channel = ch,
		.param_action = action,
	};
}

#endif /* VOLTAGE_CONTROL_VC_H */

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified voltage-control API.
 *
 * vc_init() constructs the singleton from DTS-composed channel data.
 * vc_dispatch() handles all state mutations.
 * vc_query() handles all state reads.
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
	VC_CMD_CALIBRATION,
	VC_CMD_SYSTEM_PARAM_ACTION,
	VC_CMD_CHANNEL_PARAM_ACTION,
	VC_CMD_SUBMIT_MEASUREMENT,
};

struct vc_cmd {
	enum vc_cmd_type type;
	uint8_t channel;
	union {
		enum vc_operating_mode operating_mode;
		enum vc_output_action output_action;
		enum vc_channel_fault_command fault_command;
		struct vc_field_write field_write;
		struct vc_cal_command cal;
		enum vc_param_action param_action;
		struct vc_measurement_snapshot measurement;
	};
};

/* Route a command to the runtime worker thread; blocks until processed or timeout. */
enum vc_status vc_dispatch(struct vc_ctx *ctx, struct vc_cmd cmd,
			   k_timeout_t timeout);

/* ------------------------------------------------------------------ */
/* Query types                                                         */
/* ------------------------------------------------------------------ */

enum vc_query_type {
	VC_QUERY_SYSTEM_SNAPSHOT,
	VC_QUERY_CHANNEL_SNAPSHOT,
	VC_QUERY_SYSTEM_CONFIG,
	VC_QUERY_CHANNEL_CONFIG,
	VC_QUERY_RUNTIME_CONFIG,
};

struct vc_query_msg {
	enum vc_query_type type;
	uint8_t channel;
	union {
		struct vc_system_snapshot *system_snapshot;
		struct vc_channel_snapshot *channel_snapshot;
		struct vc_system_config *system_config;
		struct vc_channel_config *channel_config;
		struct vc_runtime_config_snapshot *runtime_config;
	} out;
};

/* Read published state (snapshot or config) from the runtime; non-blocking. */
enum vc_status vc_query(struct vc_ctx *ctx, struct vc_query_msg q);

/* ------------------------------------------------------------------ */
/* Command builders                                                    */
/* ------------------------------------------------------------------ */

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

/* Build a command to submit a measurement snapshot from a provider. */
static inline struct vc_cmd vc_cmd_measurement(
	const struct vc_measurement_snapshot *meas)
{
	return (struct vc_cmd){
		.type = VC_CMD_SUBMIT_MEASUREMENT,
		.channel = meas->channel,
		.measurement = *meas,
	};
}

/* ------------------------------------------------------------------ */
/* Query builders                                                      */
/* ------------------------------------------------------------------ */

/* Build a query for the system snapshot (protocol, uptime, channel info). */
static inline struct vc_query_msg vc_q_system_snapshot(
	struct vc_system_snapshot *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_SYSTEM_SNAPSHOT,
		.out.system_snapshot = out,
	};
}

/* Build a query for a channel snapshot (measurements, status, faults). */
static inline struct vc_query_msg vc_q_channel_snapshot(
	uint8_t ch, struct vc_channel_snapshot *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_CHANNEL_SNAPSHOT,
		.channel = ch,
		.out.channel_snapshot = out,
	};
}

/* Build a query for the system config (address, baud, recovery, etc.). */
static inline struct vc_query_msg vc_q_system_config(
	struct vc_system_config *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_SYSTEM_CONFIG,
		.out.system_config = out,
	};
}

/* Build a query for a channel config (target voltage, ramp, protection, cal). */
static inline struct vc_query_msg vc_q_channel_config(
	uint8_t ch, struct vc_channel_config *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_CHANNEL_CONFIG,
		.channel = ch,
		.out.channel_config = out,
	};
}

/* Build a query for a channel's runtime config (output drive, cal state). */
static inline struct vc_query_msg vc_q_runtime_config(
	uint8_t ch, struct vc_runtime_config_snapshot *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_RUNTIME_CONFIG,
		.channel = ch,
		.out.runtime_config = out,
	};
}

#endif /* VOLTAGE_CONTROL_VC_H */

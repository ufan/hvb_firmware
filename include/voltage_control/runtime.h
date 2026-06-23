/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_RUNTIME_H
#define VOLTAGE_CONTROL_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define VC_MEAS_PRESENT_VOLTAGE         0x0001
#define VC_MEAS_PRESENT_CURRENT         0x0002
#define VC_MEAS_PRESENT_PROVIDER_STATUS 0x0004

#define VC_PROVIDER_STATUS_READY        0x0001
#define VC_PROVIDER_STATUS_APPLY_FAILED 0x0002
#define VC_PROVIDER_STATUS_SAMPLE_ERROR 0x0004
#define VC_PROVIDER_STATUS_INTERLOCK    0x0008

struct vc_runtime_config_snapshot {
	uint8_t channel;
	uint32_t version;
	uint16_t capability_flags;

	bool output_enable;
	uint16_t raw_output_drive;

	bool calibration_mode;
	bool calibration_output_enable;
	uint16_t calibration_raw_output_drive;

	bool force_safe_state;
};

struct vc_measurement_snapshot {
	uint8_t channel;
	uint32_t generation;
	uint32_t timestamp_ms;

	uint16_t present_mask;
	int32_t raw_voltage;
	int32_t raw_current;
	uint16_t provider_status;
	uint16_t provider_fault_cause;
};

#include "voltage_control/domain.h"

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
	VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
	VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
	VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
	VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
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
	} payload;
	struct k_sem *result_sem;
	enum vc_status *result;
};

struct vc_runtime;
struct domain;

/* Stop the runtime worker thread and free if heap-allocated. */
void vc_runtime_destroy(struct vc_runtime *runtime);

/* Create a runtime + domain on the heap from DTS channel entries. */
struct vc_runtime *vc_domain_runtime_create(
	const struct vc_channel_entry *channels, size_t count);
/* Create a runtime + domain in static storage (single-instance). */
struct vc_runtime *vc_domain_runtime_create_static(
	const struct vc_channel_entry *channels, size_t count);
/* Publish a measurement to the provider bus and wake the runtime worker. */
enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas);
/* Read the current runtime config for a channel (under domain lock). */
enum vc_status vc_runtime_get_channel_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_runtime_config_snapshot *cfg);

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

/* Read the last published system snapshot (lock-free copy from snapshot_lock). */
enum vc_status vc_runtime_get_published_system_snapshot(
	struct vc_runtime *runtime,
	struct vc_system_snapshot *snap);
/* Read the last published channel snapshot for the given channel. */
enum vc_status vc_runtime_get_published_channel_snapshot(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_snapshot *snap);
/* Read the last published system config. */
enum vc_status vc_runtime_get_published_system_config(
	struct vc_runtime *runtime,
	struct vc_system_config *cfg);
/* Read the last published channel config for the given channel. */
enum vc_status vc_runtime_get_published_channel_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_config *cfg);

#endif

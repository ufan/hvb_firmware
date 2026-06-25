/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_TYPES_H
#define VOLTAGE_CONTROL_VC_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#define VC_MAX_CHANNELS DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))

struct vc_channel_entry {
	const struct device *dev;
	uint8_t            index;
	uint16_t           capabilities;
};

#define VC_FAULT_CURRENT       0x0002
#define VC_FAULT_MEASUREMENT   0x0004
#define VC_FAULT_HARDWARE      0x0008
#define VC_FAULT_INTERLOCK     0x0010
#define VC_FAULT_RETRY_EXHAUST 0x0020
#define VC_FAULT_CFG_INVALID   0x0040
#define VC_FAULT_STALE         0x0080

enum vc_status {
	VC_OK = 0,
	VC_ERR_UNSUPPORTED_CHANNEL = -1,
	VC_ERR_INVALID_VALUE = -2,
	VC_ERR_INVALID_COMMAND = -3,
	VC_ERR_UNSAFE_STATE = -4,
	VC_ERR_STORAGE = -5,
	VC_ERR_UNSUPPORTED_CAPABILITY = -6,
};

enum vc_operating_mode {
	VC_OPERATING_MODE_NORMAL = 0,
	VC_OPERATING_MODE_AUTOMATIC = 1,
	VC_OPERATING_MODE_CALIBRATION = 2,
};

enum vc_cal_sample_status {
	VC_CAL_SAMPLE_NONE = 0,
	VC_CAL_SAMPLE_VALID = 1,
	VC_CAL_SAMPLE_BUSY = 2,
	VC_CAL_SAMPLE_ERROR = 3,
};

enum vc_output_action {
	VC_OUTPUT_ACTION_NONE = 0,
	VC_OUTPUT_ACTION_ENABLE = 1,
	VC_OUTPUT_ACTION_DISABLE_GRACEFUL = 2,
	VC_OUTPUT_ACTION_DISABLE_IMMEDIATE = 3,
	VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO = 4,
};

enum vc_channel_fault_command {
	VC_CHANNEL_FAULT_COMMAND_NONE = 0,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE = 1,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY = 2,
};

enum vc_protection_mode {
	VC_PROTECTION_MODE_DISABLED = 0,
	VC_PROTECTION_MODE_FLAG_ONLY = 1,
	VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION = 2,
};

enum vc_recovery_policy_mode {
	VC_RECOVERY_MANUAL_LATCH = 0,
	VC_RECOVERY_AUTO_RETRY = 1,
	VC_RECOVERY_AUTO_DERATE_RETRY = 2,
	VC_RECOVERY_NEVER_RETRY = 3,
};

enum vc_param_action {
	VC_PARAM_ACTION_NONE = 0,
	VC_PARAM_ACTION_SAVE = 1,
	VC_PARAM_ACTION_LOAD = 2,
	VC_PARAM_ACTION_FACTORY_RESET = 3,
	VC_PARAM_ACTION_SOFTWARE_RESET = 255,
};

struct vc_channel_config {
	int16_t configured_target_voltage;
	uint16_t ramp_up_step;
	uint16_t ramp_up_interval;
	uint16_t ramp_down_step;
	uint16_t ramp_down_interval;
	enum vc_protection_mode current_protection_mode;
	enum vc_output_action current_protection_output_action;
	int16_t current_limit_threshold;
	uint16_t auto_derate_step;
	uint16_t save_target_policy;
	uint16_t output_calib_k;
	int16_t output_calib_b;
	uint16_t measured_voltage_calib_k;
	int16_t measured_voltage_calib_b;
	uint16_t measured_current_calib_k;
	int16_t measured_current_calib_b;
};

struct vc_system_config {
	enum vc_operating_mode operating_mode;
	enum vc_recovery_policy_mode recovery_policy_mode;
	uint16_t auto_retry_delay;
	uint16_t auto_retry_max_count;
	uint16_t auto_retry_window;
	uint16_t current_safe_band_pct;
};

struct vc_channel_snapshot {
	int16_t measured_voltage;
	int16_t measured_current;
	int16_t operational_target_voltage;
	uint16_t status_bits;
	uint16_t active_fault_cause;
	uint16_t fault_history_cause;
	enum vc_output_action last_protection_output_action;
	uint16_t auto_retry_count;
	uint16_t auto_cooldown_remaining;
	uint32_t last_fault_timestamp;
	uint16_t channel_capability_flags;
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;
	enum vc_cal_sample_status cal_sample_status;
	uint16_t raw_dac_readback;
	uint16_t cal_output_enabled;
	uint16_t cal_max_raw_dac_limit;
};

struct vc_system_snapshot {
	uint16_t protocol_major;
	uint16_t protocol_minor;
	uint16_t variant_id;
	uint16_t system_capability_flags;
	uint16_t supported_channel_count;
	uint16_t active_channel_mask;
	enum vc_operating_mode active_operating_mode;
	uint16_t system_status;
	uint16_t system_fault_cause;
};

enum vc_config_field {
	VC_FIELD_OPERATING_MODE,
	VC_FIELD_RECOVERY_POLICY_MODE,
	VC_FIELD_AUTO_RETRY_DELAY,
	VC_FIELD_AUTO_RETRY_MAX_COUNT,
	VC_FIELD_AUTO_RETRY_WINDOW,
	VC_FIELD_CURRENT_SAFE_BAND_PCT,

	VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
	VC_FIELD_RAMP_UP_STEP,
	VC_FIELD_RAMP_UP_INTERVAL,
	VC_FIELD_RAMP_DOWN_STEP,
	VC_FIELD_RAMP_DOWN_INTERVAL,
	VC_FIELD_CURRENT_PROTECTION_MODE,
	VC_FIELD_CURRENT_PROT_OUT_ACTION,
	VC_FIELD_CURRENT_LIMIT_THRESHOLD,
	VC_FIELD_AUTO_DERATE_STEP,
	VC_FIELD_SAVE_TARGET_POLICY,
	VC_FIELD_OUTPUT_CAL_K,
	VC_FIELD_OUTPUT_CAL_B,
	VC_FIELD_MEASURED_V_CAL_K,
	VC_FIELD_MEASURED_V_CAL_B,
	VC_FIELD_MEASURED_I_CAL_K,
	VC_FIELD_MEASURED_I_CAL_B,
};

struct vc_field_write {
	enum vc_config_field field;
	uint16_t value;
};

#endif

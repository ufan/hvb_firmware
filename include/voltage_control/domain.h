/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_DOMAIN_H
#define VOLTAGE_CONTROL_DOMAIN_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#define VC_MAX_CHANNELS DT_PROP_LEN(DT_NODELABEL(vc_controller), channels)

struct vc_channel_entry {
	const struct device *dev;
	uint8_t            index;
	uint16_t           capabilities;
};

#define VC_FAULT_VOLTAGE       0x0001
#define VC_FAULT_CURRENT       0x0002
#define VC_FAULT_MEASUREMENT   0x0004
#define VC_FAULT_HARDWARE      0x0008
#define VC_FAULT_INTERLOCK     0x0010
#define VC_FAULT_RETRY_EXHAUST 0x0020
#define VC_FAULT_CFG_INVALID   0x0040

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
	VC_OUTPUT_ACTION_CLAMP = 5,
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

enum vc_baud_rate_code {
	VC_BAUD_RATE_115200 = 0,
	VC_BAUD_RATE_9600 = 1,
};

struct vc_channel_config {
	int16_t configured_target_voltage;
	uint16_t ramp_up_step;
	uint16_t ramp_up_interval;
	uint16_t ramp_down_step;
	uint16_t ramp_down_interval;
	enum vc_protection_mode voltage_protection_mode;
	enum vc_output_action voltage_protection_output_action;
	int16_t voltage_limit_threshold;
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
	uint16_t slave_address;
	enum vc_baud_rate_code baud_rate_code;
	enum vc_recovery_policy_mode recovery_policy_mode;
	uint16_t auto_retry_delay;
	uint16_t auto_retry_max_count;
	uint16_t auto_retry_window;
	uint16_t voltage_safe_band_pct;
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
	int16_t board_temperature;
	uint16_t board_humidity;
	uint32_t uptime;
	uint16_t fw_version_high;
	uint16_t fw_version_low;
	enum vc_operating_mode active_operating_mode;
	uint16_t system_status;
	uint16_t system_fault_cause;
};

enum vc_config_field {
	VC_FIELD_OPERATING_MODE,
	VC_FIELD_SLAVE_ADDRESS,
	VC_FIELD_BAUD_RATE_CODE,
	VC_FIELD_RECOVERY_POLICY_MODE,
	VC_FIELD_AUTO_RETRY_DELAY,
	VC_FIELD_AUTO_RETRY_MAX_COUNT,
	VC_FIELD_AUTO_RETRY_WINDOW,
	VC_FIELD_VOLTAGE_SAFE_BAND_PCT,
	VC_FIELD_CURRENT_SAFE_BAND_PCT,

	VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
	VC_FIELD_RAMP_UP_STEP,
	VC_FIELD_RAMP_UP_INTERVAL,
	VC_FIELD_RAMP_DOWN_STEP,
	VC_FIELD_RAMP_DOWN_INTERVAL,
	VC_FIELD_VOLTAGE_PROTECTION_MODE,
	VC_FIELD_VOLTAGE_PROT_OUT_ACTION,
	VC_FIELD_VOLTAGE_LIMIT_THRESHOLD,
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

struct domain;

struct domain *domain_create(const struct vc_channel_entry *channels,
			     size_t count);
struct domain *domain_create_static(const struct vc_channel_entry *channels,
				    size_t count);

enum vc_operating_mode domain_get_operating_mode(const struct domain *domain);
enum vc_status domain_set_operating_mode(struct domain *domain,
					    enum vc_operating_mode mode);

enum vc_status domain_get_system_config(const struct domain *domain,
					   struct vc_system_config *cfg);
enum vc_status domain_set_system_config(struct domain *domain,
					   const struct vc_system_config *cfg);

enum vc_status domain_get_channel_config(const struct domain *domain,
					    uint8_t channel,
					    struct vc_channel_config *cfg);
enum vc_status domain_set_channel_config(struct domain *domain,
					    uint8_t channel,
					    const struct vc_channel_config *cfg);

enum vc_status domain_get_system_snapshot(const struct domain *domain,
					     struct vc_system_snapshot *snap);
enum vc_status domain_get_channel_snapshot(const struct domain *domain,
					      uint8_t channel,
					      struct vc_channel_snapshot *snap);

enum vc_status domain_channel_output_action(struct domain *domain,
					       uint8_t channel,
					       enum vc_output_action action);
enum vc_status domain_channel_fault_command(struct domain *domain,
					       uint8_t channel,
					       enum vc_channel_fault_command cmd);

enum vc_status domain_calibration_unlock(struct domain *domain,
					    uint16_t value);
enum vc_status domain_calibration_set_output_enable(struct domain *domain,
						       uint8_t channel,
						       bool enabled);
enum vc_status domain_calibration_set_raw_dac(struct domain *domain,
						 uint8_t channel,
						 uint16_t code);
enum vc_status domain_calibration_sample(struct domain *domain,
					    uint8_t channel);
enum vc_status domain_calibration_commit(struct domain *domain,
					    uint8_t channel);
enum vc_status domain_calibration_set_max_raw_dac(struct domain *domain,
						     uint8_t channel,
						     uint16_t limit);

enum vc_status domain_system_param_action(struct domain *domain,
					     enum vc_param_action action);
enum vc_status domain_channel_param_action(struct domain *domain,
					      uint8_t channel,
					      enum vc_param_action action);

bool domain_is_channel_supported(const struct domain *domain, uint8_t channel);
uint16_t domain_get_supported_channel_count(const struct domain *domain);
uint16_t domain_get_active_channel_mask(const struct domain *domain);
uint16_t domain_get_variant_id(const struct domain *domain);

void domain_set_uptime(struct domain *domain, uint32_t seconds);
void domain_process_periodic(struct domain *domain, uint32_t dt_ms);

struct vc_runtime_config_snapshot;
struct vc_measurement_snapshot;

enum vc_status domain_get_runtime_config(const struct domain *domain,
					    uint8_t channel,
					    struct vc_runtime_config_snapshot *cfg);
enum vc_status domain_consume_measurement(struct domain *domain,
					     const struct vc_measurement_snapshot *meas);

enum vc_status domain_set_system_field(struct domain *domain,
				       enum vc_config_field field,
				       uint16_t value);
enum vc_status domain_set_channel_field(struct domain *domain,
					uint8_t channel,
					enum vc_config_field field,
					uint16_t value);

#endif

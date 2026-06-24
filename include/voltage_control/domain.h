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

#define VC_MAX_CHANNELS DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))

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

/* Allocate domain on heap; caller must free. Returns NULL on failure. */
struct domain *domain_create(const struct vc_channel_entry *channels,
			     size_t count);
/* Allocate domain in a static variable; single-instance only. */
struct domain *domain_create_static(const struct vc_channel_entry *channels,
				    size_t count);

/* Return current operating mode (normal/automatic/calibration). */
enum vc_operating_mode domain_get_operating_mode(const struct domain *domain);
/* Switch operating mode; calibration requires prior unlock. Resets channel states on transition. */
enum vc_status domain_set_operating_mode(struct domain *domain,
					    enum vc_operating_mode mode);

/* Copy system-level config (address, baud, recovery policy, etc.) into *cfg. */
enum vc_status domain_get_system_config(const struct domain *domain,
					   struct vc_system_config *cfg);
/* Validate and apply a full system config. Triggers mode transition if operating_mode changed. */
enum vc_status domain_set_system_config(struct domain *domain,
					   const struct vc_system_config *cfg);

/* Copy per-channel config (target voltage, ramp, protection, calibration) into *cfg. */
enum vc_status domain_get_channel_config(const struct domain *domain,
					    uint8_t channel,
					    struct vc_channel_config *cfg);
/* Validate and apply a full channel config. Enforces capability and calibration-mode guards. */
enum vc_status domain_set_channel_config(struct domain *domain,
					    uint8_t channel,
					    const struct vc_channel_config *cfg);

/* Populate a system snapshot (protocol version, uptime, channel count, fault state). */
enum vc_status domain_get_system_snapshot(const struct domain *domain,
					     struct vc_system_snapshot *snap);
/* Populate a channel snapshot (measured values, status bits, fault cause, calibration state). */
enum vc_status domain_get_channel_snapshot(const struct domain *domain,
					      uint8_t channel,
					      struct vc_channel_snapshot *snap);

/* Execute an output action (enable/disable) on a channel. Blocked in calibration mode. */
enum vc_status domain_channel_output_action(struct domain *domain,
					       uint8_t channel,
					       enum vc_output_action action);
/* Clear active or history faults on a channel. Active clear requires safe-band check. */
enum vc_status domain_channel_fault_command(struct domain *domain,
					       uint8_t channel,
					       enum vc_channel_fault_command cmd);

/* Two-step unlock sequence (STEP1 then STEP2) to enable calibration mode entry. */
enum vc_status domain_calibration_unlock(struct domain *domain,
					    uint16_t value);
/* Enable/disable raw DAC output for one channel. Only one channel may be active at a time. */
enum vc_status domain_calibration_set_output_enable(struct domain *domain,
						       uint8_t channel,
						       bool enabled);
/* Set raw DAC code for calibration output. Requires output enabled and no safety faults. */
enum vc_status domain_calibration_set_raw_dac(struct domain *domain,
						 uint8_t channel,
						 uint16_t code);
/* Trigger ADC sample capture for calibration. Requires voltage or current measurement cap. */
enum vc_status domain_calibration_sample(struct domain *domain,
					    uint8_t channel);
/* Commit calibration data. Requires output disabled and no safety faults. */
enum vc_status domain_calibration_commit(struct domain *domain,
					    uint8_t channel);
/* Set upper limit for raw DAC code in calibration mode. Must be >= current DAC readback. */
enum vc_status domain_calibration_set_max_raw_dac(struct domain *domain,
						     uint8_t channel,
						     uint16_t limit);

/* Save/load/factory-reset/reboot at system level via storage backend. */
enum vc_status domain_system_param_action(struct domain *domain,
					     enum vc_param_action action);
/* Save/load/factory-reset per-channel config. Preserves calibration coefficients on load/reset. */
enum vc_status domain_channel_param_action(struct domain *domain,
					      uint8_t channel,
					      enum vc_param_action action);

/* Return true if channel index is within the configured channel count. */
bool domain_is_channel_supported(const struct domain *domain, uint8_t channel);
/* Return number of channels configured at domain creation. */
uint16_t domain_get_supported_channel_count(const struct domain *domain);
/* Return bitmask of active channels (all channels up to count are active). */
uint16_t domain_get_active_channel_mask(const struct domain *domain);
/* Return hardware variant identifier (compile-time constant). */
uint16_t domain_get_variant_id(const struct domain *domain);

/* Update the stored uptime counter (fed from kernel uptime). */
void domain_set_uptime(struct domain *domain, uint32_t seconds);
/* Run one tick of ramp, protection, recovery, and status-bit updates for all channels. */
void domain_process_periodic(struct domain *domain, uint32_t dt_ms);

struct vc_runtime_config_snapshot;
struct vc_measurement_snapshot;
struct vc_storage_backend;

/* Attach a storage backend for save/load/erase operations. NULL disables persistence. */
void domain_set_storage_backend(struct domain *domain,
				const struct vc_storage_backend *backend);

/* Build a runtime config snapshot for the provider bus (output drive, calibration state, etc.). */
enum vc_status domain_get_runtime_config(const struct domain *domain,
					    uint8_t channel,
					    struct vc_runtime_config_snapshot *cfg);
/* Ingest a measurement from the provider bus; applies calibration scaling and runs protection. */
enum vc_status domain_consume_measurement(struct domain *domain,
					     const struct vc_measurement_snapshot *meas);

/* Set a single system config field by enum key. Delegates to domain_set_system_config. */
enum vc_status domain_set_system_field(struct domain *domain,
				       enum vc_config_field field,
				       uint16_t value);
/* Set a single channel config field by enum key. Delegates to domain_set_channel_config. */
enum vc_status domain_set_channel_field(struct domain *domain,
					uint8_t channel,
					enum vc_config_field field,
					uint16_t value);

#endif

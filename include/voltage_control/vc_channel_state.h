/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_STATE_H
#define VOLTAGE_CONTROL_VC_CHANNEL_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>

#include "voltage_control/vc_types.h"

struct vc_pending_command {
	uint16_t output_code;
	bool output_enable;
	bool valid;
};

enum vc_channel_smf_state {
	VC_CHANNEL_SMF_DISABLED_SAFE,
	VC_CHANNEL_SMF_ENABLED_HOLDING,
	VC_CHANNEL_SMF_RAMPING,
	VC_CHANNEL_SMF_FAULT_LATCHED,
	VC_CHANNEL_SMF_RETRY_COOLDOWN,
	VC_CHANNEL_SMF_CALIBRATION_OUTPUT,
	VC_CHANNEL_SMF_COUNT,
};

struct vc_channel {
	struct k_spinlock lock;
	struct smf_ctx smf;
	uint8_t index;
	uint16_t capabilities;

	struct vc_channel_config config;

	bool output_enabled;
	bool ramping;
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	int16_t operational_target_voltage;

	int16_t measured_voltage;
	int16_t measured_current;

	uint16_t active_fault_cause;
	uint16_t fault_history_cause;
	uint16_t status_bits;
	uint32_t last_fault_timestamp;
	enum vc_output_action last_protection_output_action;

	uint16_t cal_max_raw_dac_limit;
	uint16_t raw_dac_readback;
	uint16_t cal_output_enabled;
	enum vc_cal_sample_status cal_sample_status;
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;

	uint32_t uptime_ref;

	struct vc_pending_command pending;
};

void vc_channel_init(struct vc_channel *ch, uint8_t index, uint16_t caps);

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode);
enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg);
enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode);
enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg);
enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value,
				    bool calibration_mode);

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage);
void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current);
void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause);

void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg);

void vc_channel_get_snapshot(const struct vc_channel *ch,
			     struct vc_channel_snapshot *snap);

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable,
						const struct vc_channel *all_channels,
						size_t channel_count);
enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code);
enum vc_status vc_channel_cal_sample(struct vc_channel *ch);
enum vc_status vc_channel_cal_commit(struct vc_channel *ch);
enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit);

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering);

bool vc_channel_has_pending_command(const struct vc_channel *ch);
struct vc_pending_command vc_channel_take_pending_command(struct vc_channel *ch);

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch);

#endif

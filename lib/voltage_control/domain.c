/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/domain.h"
#include "voltage_control/variant.h"
#include "regmap/hvb_regs.h"
#include <stdlib.h>
#include <string.h>

struct vc_channel_runtime {
	bool output_enabled;
	bool ramping;
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	uint16_t cal_max_raw_dac_limit;
};

struct domain {
	const struct vc_variant_profile *variant;
	enum vc_operating_mode operating_mode;
	uint32_t uptime_seconds;
	struct vc_system_config sys_cfg;
	struct vc_channel_config channels[VC_MAX_CHANNELS];
	struct vc_channel_snapshot snapshots[VC_MAX_CHANNELS];
	struct vc_channel_runtime runtime[VC_MAX_CHANNELS];
	uint16_t system_fault_cause;
	uint8_t cal_unlock_step;
	bool cal_unlocked;
};

static bool channel_valid(const struct domain *domain, uint8_t channel)
{
	return channel < domain->variant->num_channels;
}

static bool is_valid_operating_mode(enum vc_operating_mode mode)
{
	return mode == VC_OPERATING_MODE_NORMAL ||
	       mode == VC_OPERATING_MODE_AUTOMATIC ||
	       mode == VC_OPERATING_MODE_CALIBRATION;
}

static bool is_entering_calibration(const struct domain *domain,
					   enum vc_operating_mode mode)
{
	return domain->operating_mode != VC_OPERATING_MODE_CALIBRATION &&
	       mode == VC_OPERATING_MODE_CALIBRATION;
}

static void clear_calibration_unlock(struct domain *domain)
{
	domain->cal_unlock_step = 0;
	domain->cal_unlocked = false;
}

static void clear_normal_runtime_channel(struct domain *domain, uint8_t channel)
{
	struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	struct vc_channel_runtime *rt = &domain->runtime[channel];
	uint16_t preserved = 0;

	if (snap->active_fault_cause != 0) {
		preserved |= 0x0008;
	}
	if (snap->fault_history_cause != 0) {
		preserved |= 0x0010;
	}

	rt->output_enabled = false;
	rt->ramping = false;
	rt->ramp_accum_ms = 0;
	rt->cooldown_remaining_ms = 0;
	snap->operational_target_voltage = 0;
	snap->measured_voltage = 0;
	snap->measured_current = 0;
	snap->auto_cooldown_remaining = 0;
	snap->status_bits = preserved;
}

static void reset_calibration_channel(struct domain *domain, uint8_t channel,
					      bool entering)
{
	struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	struct vc_channel_runtime *rt = &domain->runtime[channel];

	clear_normal_runtime_channel(domain, channel);
	snap->raw_dac_readback = 0;
	snap->cal_output_enabled = 0;
	rt->cal_max_raw_dac_limit = domain->variant->max_raw_dac_code;
	snap->raw_adc_voltage = 0;
	snap->raw_adc_current = 0;
	snap->cal_sample_status = VC_CAL_SAMPLE_NONE;
}

static void reset_calibration_outputs(struct domain *domain, bool entering)
{
	for (uint8_t ch = 0; ch < domain->variant->num_channels; ch++) {
		reset_calibration_channel(domain, ch, entering);
	}
}

static bool is_valid_recovery_policy(enum vc_recovery_policy_mode mode)
{
	return mode <= VC_RECOVERY_NEVER_RETRY;
}

static bool is_valid_protection_mode(enum vc_protection_mode mode)
{
	return mode <= VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
}

static bool is_valid_protection_output_action(enum vc_output_action action,
					      bool is_voltage)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		return true;
	case VC_OUTPUT_ACTION_CLAMP:
		return is_voltage;
	default:
		return false;
	}
}

static bool is_valid_host_output_action(enum vc_output_action action)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_ENABLE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		return true;
	default:
		return false;
	}
}

static bool is_valid_channel_fault_command(enum vc_channel_fault_command cmd)
{
	return cmd >= VC_CHANNEL_FAULT_COMMAND_NONE &&
	       cmd <= VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY;
}

static bool has_hard_safety_fault(const struct domain *domain, uint8_t channel)
{
	return (domain->snapshots[channel].active_fault_cause &
		(VC_FAULT_HARDWARE | VC_FAULT_INTERLOCK)) != 0;
}

static enum vc_status enter_calibration_mode(struct domain *domain)
{
	reset_calibration_outputs(domain, true);
	if (!domain->variant->calibration_output_disable_confirmed) {
		domain->system_fault_cause |= VC_FAULT_HARDWARE;
		clear_calibration_unlock(domain);
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

static bool calibration_fields_changed(const struct vc_channel_config *old_cfg,
					      const struct vc_channel_config *new_cfg)
{
	return old_cfg->output_calib_k != new_cfg->output_calib_k ||
	       old_cfg->output_calib_b != new_cfg->output_calib_b ||
	       old_cfg->measured_voltage_calib_k != new_cfg->measured_voltage_calib_k ||
	       old_cfg->measured_voltage_calib_b != new_cfg->measured_voltage_calib_b ||
	       old_cfg->measured_current_calib_k != new_cfg->measured_current_calib_k ||
	       old_cfg->measured_current_calib_b != new_cfg->measured_current_calib_b;
}

struct domain *domain_create(const struct vc_variant_profile *variant)
{
	struct domain *domain;

	if (!variant || variant->num_channels > VC_MAX_CHANNELS) {
		return NULL;
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		return NULL;
	}

	domain->variant = variant;
	domain->operating_mode = variant->default_system_config.operating_mode;

	memcpy(&domain->sys_cfg, &variant->default_system_config,
	       sizeof(domain->sys_cfg));

	for (int i = 0; i < variant->num_channels && i < VC_MAX_CHANNELS; i++) {
		memcpy(&domain->channels[i], &variant->default_channel_config,
		       sizeof(domain->channels[i]));
		domain->runtime[i].cal_max_raw_dac_limit =
			variant->max_raw_dac_code;
	}

	return domain;
}

enum vc_operating_mode domain_get_operating_mode(const struct domain *domain)
{
	return domain->operating_mode;
}

enum vc_status domain_set_operating_mode(struct domain *domain,
					    enum vc_operating_mode mode)
{
	enum vc_operating_mode old_mode = domain->operating_mode;

	if (!is_valid_operating_mode(mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (is_entering_calibration(domain, mode) && !domain->cal_unlocked) {
		return VC_ERR_INVALID_COMMAND;
	}

	if (domain->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    mode == VC_OPERATING_MODE_NORMAL) {
		for (int i = 0; i < domain->variant->num_channels; i++) {
			domain->runtime[i].cooldown_remaining_ms = 0;
		}
	}

	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		enum vc_status status = enter_calibration_mode(domain);

		if (status != VC_OK) {
			return status;
		}
	} else if (old_mode == VC_OPERATING_MODE_CALIBRATION) {
		reset_calibration_outputs(domain, false);
	}

	domain->operating_mode = mode;
	if (mode != VC_OPERATING_MODE_CALIBRATION) {
		domain->sys_cfg.operating_mode = mode;
	}
	if (mode == VC_OPERATING_MODE_CALIBRATION ||
	    old_mode == VC_OPERATING_MODE_CALIBRATION) {
		clear_calibration_unlock(domain);
	}
	return VC_OK;
}

enum vc_status domain_get_system_config(const struct domain *domain,
					   struct vc_system_config *cfg)
{
	memcpy(cfg, &domain->sys_cfg, sizeof(*cfg));
	return VC_OK;
}

enum vc_status domain_set_system_config(struct domain *domain,
					   const struct vc_system_config *cfg)
{
	enum vc_operating_mode old_mode = domain->operating_mode;
	enum vc_operating_mode old_cfg_mode = domain->sys_cfg.operating_mode;

	if (!is_valid_operating_mode(cfg->operating_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (is_entering_calibration(domain, cfg->operating_mode) &&
	    !domain->cal_unlocked) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (cfg->slave_address > 247) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->baud_rate_code > VC_BAUD_RATE_9600) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_recovery_policy(cfg->recovery_policy_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}

	if (domain->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    cfg->operating_mode == VC_OPERATING_MODE_NORMAL) {
		for (int i = 0; i < domain->variant->num_channels; i++) {
			domain->runtime[i].cooldown_remaining_ms = 0;
		}
	}

	if (cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		enum vc_status status = enter_calibration_mode(domain);

		if (status != VC_OK) {
			return status;
		}
	} else if (old_mode == VC_OPERATING_MODE_CALIBRATION) {
		reset_calibration_outputs(domain, false);
	}

	memcpy(&domain->sys_cfg, cfg, sizeof(*cfg));
	if (cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		domain->sys_cfg.operating_mode = old_cfg_mode;
	}
	domain->operating_mode = cfg->operating_mode;
	if (domain->operating_mode == VC_OPERATING_MODE_CALIBRATION ||
	    old_mode == VC_OPERATING_MODE_CALIBRATION) {
		clear_calibration_unlock(domain);
	}
	return VC_OK;
}

enum vc_status domain_get_channel_config(const struct domain *domain,
					    uint8_t channel,
					    struct vc_channel_config *cfg)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	memcpy(cfg, &domain->channels[channel], sizeof(*cfg));
	return VC_OK;
}

enum vc_status domain_set_channel_config(struct domain *domain,
					    uint8_t channel,
					    const struct vc_channel_config *cfg)
{
	int16_t max_v, min_v;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (domain->operating_mode != VC_OPERATING_MODE_CALIBRATION &&
	    calibration_fields_changed(&domain->channels[channel], cfg)) {
		return VC_ERR_INVALID_COMMAND;
	}

	max_v = domain->variant->max_voltage_raw;
	min_v = domain->variant->min_voltage_raw;

	if (cfg->configured_target_voltage > max_v ||
	    cfg->configured_target_voltage < min_v) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_limit_threshold > max_v ||
	    cfg->voltage_limit_threshold < min_v) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_limit_threshold < 0) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->voltage_protection_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_output_action(cfg->voltage_protection_output_action,
					       true)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->current_protection_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_output_action(cfg->current_protection_output_action,
					       false)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->save_target_policy > 1) {
		return VC_ERR_INVALID_VALUE;
	}

	memcpy(&domain->channels[channel], cfg, sizeof(*cfg));
	return VC_OK;
}

enum vc_status domain_get_system_snapshot(const struct domain *domain,
					     struct vc_system_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->protocol_major = HVB_PROTOCOL_MAJOR;
	snap->protocol_minor = HVB_PROTOCOL_MINOR;
	snap->variant_id = domain->variant->variant_id;
	snap->system_capability_flags = domain->variant->system_capability_flags;
	snap->supported_channel_count = domain->variant->num_channels;
	snap->active_channel_mask = domain->variant->channel_mask;
	snap->uptime = domain->uptime_seconds;
	snap->active_operating_mode = domain->operating_mode;
	snap->system_fault_cause = domain->system_fault_cause;
	return VC_OK;
}

enum vc_status domain_get_channel_snapshot(const struct domain *domain,
					      uint8_t channel,
					      struct vc_channel_snapshot *snap)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	memcpy(snap, &domain->snapshots[channel], sizeof(*snap));
	snap->channel_capability_flags = domain->variant->channel_capability_flags;
	snap->cal_max_raw_dac_limit = domain->runtime[channel].cal_max_raw_dac_limit;
	return VC_OK;
}

enum vc_status domain_calibration_unlock(struct domain *domain,
					    uint16_t value)
{
	if (value == CAL_UNLOCK_STEP1) {
		domain->cal_unlock_step = 1;
		domain->cal_unlocked = false;
		return VC_OK;
	}

	if (value == CAL_UNLOCK_STEP2 && domain->cal_unlock_step == 1) {
		domain->cal_unlock_step = 0;
		domain->cal_unlocked = true;
		return VC_OK;
	}

	clear_calibration_unlock(domain);
	return VC_ERR_INVALID_COMMAND;
}

static enum vc_status calibration_channel_ready(const struct domain *domain,
						       uint8_t channel)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (domain->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return VC_OK;
}

enum vc_status domain_calibration_set_output_enable(struct domain *domain,
						       uint8_t channel,
						       bool enabled)
{
	enum vc_status status = calibration_channel_ready(domain, channel);

	if (status != VC_OK) {
		return status;
	}

	if (enabled) {
		if (has_hard_safety_fault(domain, channel)) {
			return VC_ERR_UNSAFE_STATE;
		}
		for (uint8_t ch = 0; ch < domain->variant->num_channels; ch++) {
			if (ch == channel) {
				continue;
			}
			if (domain->snapshots[ch].cal_output_enabled ||
			    domain->snapshots[ch].raw_dac_readback != 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
		domain->snapshots[channel].cal_output_enabled = 1;
		return VC_OK;
	}

	domain->snapshots[channel].cal_output_enabled = 0;
	domain->snapshots[channel].raw_dac_readback = 0;
	domain->snapshots[channel].raw_adc_voltage = 0;
	domain->snapshots[channel].raw_adc_current = 0;
	domain->snapshots[channel].cal_sample_status = VC_CAL_SAMPLE_NONE;
	return VC_OK;
}

enum vc_status domain_calibration_set_raw_dac(struct domain *domain,
						 uint8_t channel,
						 uint16_t code)
{
	enum vc_status status = calibration_channel_ready(domain, channel);
	uint16_t limit;

	if (status != VC_OK) {
		return status;
	}

	limit = domain->runtime[channel].cal_max_raw_dac_limit;
	if (code > limit) {
		return VC_ERR_INVALID_VALUE;
	}
	if (code != 0 && has_hard_safety_fault(domain, channel)) {
		return VC_ERR_UNSAFE_STATE;
	}
	if (code != 0 && !domain->snapshots[channel].cal_output_enabled) {
		return VC_ERR_UNSAFE_STATE;
	}

	domain->snapshots[channel].raw_dac_readback = code;
	return VC_OK;
}

enum vc_status domain_calibration_set_max_raw_dac(struct domain *domain,
						     uint8_t channel,
						     uint16_t limit)
{
	enum vc_status status = calibration_channel_ready(domain, channel);

	if (status != VC_OK) {
		return status;
	}
	if (limit > domain->variant->max_raw_dac_code) {
		return VC_ERR_INVALID_VALUE;
	}
	if (limit < domain->snapshots[channel].raw_dac_readback) {
		return VC_ERR_UNSAFE_STATE;
	}

	domain->runtime[channel].cal_max_raw_dac_limit = limit;
	return VC_OK;
}

enum vc_status domain_calibration_sample(struct domain *domain,
					    uint8_t channel)
{
	enum vc_status status = calibration_channel_ready(domain, channel);
	struct vc_channel_snapshot *snap;

	if (status != VC_OK) {
		return status;
	}

	snap = &domain->snapshots[channel];
	snap->raw_adc_voltage = snap->raw_dac_readback;
	snap->raw_adc_current = 0;
	snap->cal_sample_status = VC_CAL_SAMPLE_VALID;
	return VC_OK;
}

enum vc_status domain_calibration_commit(struct domain *domain,
					    uint8_t channel)
{
	enum vc_status status = calibration_channel_ready(domain, channel);
	const struct vc_channel_snapshot *snap;

	if (status != VC_OK) {
		return status;
	}

	snap = &domain->snapshots[channel];
	if (snap->cal_output_enabled || snap->raw_dac_readback != 0 ||
	    has_hard_safety_fault(domain, channel)) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

enum vc_status domain_channel_output_action(struct domain *domain,
					       uint8_t channel,
					       enum vc_output_action action)
{
	struct vc_channel_runtime *rt;
	struct vc_channel_snapshot *snap;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!is_valid_host_output_action(action)) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (domain->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}

	rt = &domain->runtime[channel];
	snap = &domain->snapshots[channel];

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (snap->active_fault_cause != 0) {
			return VC_ERR_UNSAFE_STATE;
		}
		rt->output_enabled = true;
		rt->ramping = true;
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		rt->output_enabled = false;
		rt->ramping = false;
		snap->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_NONE:
	default:
		break;
	}

	return VC_OK;
}

static bool vc_is_safe_to_clear_active(const struct domain *domain,
				       uint8_t channel,
				       uint16_t fault_bits)
{
	const struct vc_channel_config *cfg = &domain->channels[channel];
	const struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	const struct vc_system_config *sys = &domain->sys_cfg;
	int32_t safe_limit;

	if (fault_bits & VC_FAULT_VOLTAGE) {
		safe_limit = (int32_t)cfg->voltage_limit_threshold *
			     (100 - (int32_t)sys->voltage_safe_band_pct) / 100;
		if (snap->measured_voltage > safe_limit) {
			return false;
		}
	}
	if (fault_bits & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)sys->current_safe_band_pct) / 100;
		if (snap->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}

enum vc_status domain_channel_fault_command(struct domain *domain,
					       uint8_t channel,
					       enum vc_channel_fault_command cmd)
{
	struct vc_channel_snapshot *snap;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!is_valid_channel_fault_command(cmd)) {
		return VC_ERR_INVALID_COMMAND;
	}

	snap = &domain->snapshots[channel];

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (snap->active_fault_cause == 0) {
			break;
		}
		if (!vc_is_safe_to_clear_active(domain, channel,
						snap->active_fault_cause)) {
			return VC_ERR_UNSAFE_STATE;
		}
		snap->active_fault_cause = 0;
		break;
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY:
		snap->fault_history_cause = 0;
		snap->auto_retry_count = 0;
		break;
	default:
		break;
	}

	return VC_OK;
}

enum vc_status domain_system_param_action(struct domain *domain,
					     enum vc_param_action action)
{
	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
	case VC_PARAM_ACTION_LOAD:
	case VC_PARAM_ACTION_FACTORY_RESET:
		return VC_ERR_STORAGE;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
		return VC_ERR_STORAGE;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

enum vc_status domain_channel_param_action(struct domain *domain,
					      uint8_t channel,
					      enum vc_param_action action)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
	case VC_PARAM_ACTION_LOAD:
	case VC_PARAM_ACTION_FACTORY_RESET:
		return VC_ERR_STORAGE;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
		return VC_ERR_STORAGE;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

bool domain_is_channel_supported(const struct domain *domain,
				    uint8_t channel)
{
	return channel_valid(domain, channel);
}

uint16_t domain_get_supported_channel_count(const struct domain *domain)
{
	return domain->variant->num_channels;
}

uint16_t domain_get_active_channel_mask(const struct domain *domain)
{
	return domain->variant->channel_mask;
}

uint16_t domain_get_variant_id(const struct domain *domain)
{
	return domain->variant->variant_id;
}

void domain_set_uptime(struct domain *domain, uint32_t seconds)
{
	domain->uptime_seconds = seconds;
}

/* ---- Tick sub-functions ---- */

static void vc_tick_ramp(struct domain *domain, uint8_t ch, uint32_t dt_ms)
{
	struct vc_channel_config *cfg = &domain->channels[ch];
	struct vc_channel_snapshot *snap = &domain->snapshots[ch];
	struct vc_channel_runtime *rt = &domain->runtime[ch];
	int16_t target, current;
	uint16_t step, interval;
	uint32_t interval_ms;

	if (!rt->output_enabled) {
		return;
	}

	target = cfg->configured_target_voltage;
	current = snap->operational_target_voltage;

	if (current == target) {
		rt->ramping = false;
		return;
	}

	rt->ramping = true;

	if (current < target) {
		step = cfg->ramp_up_step;
		interval = cfg->ramp_up_interval;
	} else {
		step = cfg->ramp_down_step;
		interval = cfg->ramp_down_interval;
	}

	if (step == 0 || interval == 0) {
		snap->operational_target_voltage = target;
		rt->ramping = false;
		return;
	}

	interval_ms = (uint32_t)interval * 100;
	rt->ramp_accum_ms += dt_ms;

	while (rt->ramp_accum_ms >= interval_ms && current != target) {
		rt->ramp_accum_ms -= interval_ms;
		if (current < target) {
			current += (int16_t)step;
			if (current > target) {
				current = target;
			}
		} else {
			current -= (int16_t)step;
			if (current < target) {
				current = target;
			}
		}
	}

	snap->operational_target_voltage = current;
	if (current == target) {
		rt->ramping = false;
	}
}

static void vc_tick_measure(struct domain *domain, uint8_t ch,
			    int16_t v_noise, int16_t i_noise)
{
	struct vc_channel_snapshot *snap = &domain->snapshots[ch];
	int16_t max_v = domain->variant->max_voltage_raw;
	int16_t min_v = domain->variant->min_voltage_raw;
	int16_t max_i = domain->variant->max_current_raw;
	int32_t val;

	val = (int32_t)snap->operational_target_voltage + v_noise;
	if (val > max_v) val = max_v;
	if (val < min_v) val = min_v;
	snap->measured_voltage = (int16_t)val;

	val = (int32_t)snap->measured_voltage / 2 + i_noise;
	if (val > max_i) val = max_i;
	if (val < 0) val = 0;
	snap->measured_current = (int16_t)val;
}

static void apply_protection_action(struct vc_channel_snapshot *snap,
				    struct vc_channel_runtime *rt,
				    enum vc_output_action action,
				    int16_t clamp_limit)
{
	snap->last_protection_output_action = action;

	switch (action) {
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		snap->operational_target_voltage = 0;
		rt->output_enabled = false;
		break;
	case VC_OUTPUT_ACTION_CLAMP:
		snap->operational_target_voltage = clamp_limit;
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		rt->output_enabled = false;
		snap->operational_target_voltage = 0;
		break;
	default:
		break;
	}
}

static void start_cooldown(struct vc_channel_runtime *rt,
			   const struct vc_system_config *sys)
{
	rt->cooldown_remaining_ms =
		(uint32_t)sys->auto_retry_delay * 1000;
}

static bool should_start_cooldown(const struct domain *domain)
{
	const struct vc_system_config *sys = &domain->sys_cfg;

	return domain->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	       sys->recovery_policy_mode != VC_RECOVERY_MANUAL_LATCH &&
	       sys->recovery_policy_mode != VC_RECOVERY_NEVER_RETRY;
}

static void vc_tick_protection(struct domain *domain, uint8_t ch)
{
	struct vc_channel_config *cfg = &domain->channels[ch];
	struct vc_channel_snapshot *snap = &domain->snapshots[ch];
	struct vc_channel_runtime *rt = &domain->runtime[ch];
	bool cur_fault, vol_fault;
	bool cur_block, vol_block;
	enum vc_output_action cur_action, vol_action;

	if (snap->active_fault_cause != 0) {
		return;
	}

	cur_fault = (cfg->current_protection_mode != VC_PROTECTION_MODE_DISABLED &&
		     snap->measured_current > cfg->current_limit_threshold);
	vol_fault = (cfg->voltage_protection_mode != VC_PROTECTION_MODE_DISABLED &&
		     snap->measured_voltage > cfg->voltage_limit_threshold);

	if (!cur_fault && !vol_fault) {
		return;
	}

	cur_block = cur_fault &&
		    cfg->current_protection_mode == VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	vol_block = vol_fault &&
		    cfg->voltage_protection_mode == VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;

	cur_action = (cur_fault) ? cfg->current_protection_output_action
				 : VC_OUTPUT_ACTION_NONE;
	vol_action = (vol_fault) ? cfg->voltage_protection_output_action
				 : VC_OUTPUT_ACTION_NONE;

	if (cur_fault) {
		snap->fault_history_cause |= VC_FAULT_CURRENT;
	}
	if (vol_fault) {
		snap->fault_history_cause |= VC_FAULT_VOLTAGE;
	}

	if (!cur_block && !vol_block) {
		return;
	}

	snap->last_fault_timestamp = domain->uptime_seconds;

	if (cur_block && vol_block) {
		snap->active_fault_cause |= VC_FAULT_CURRENT | VC_FAULT_VOLTAGE;
		apply_protection_action(snap, rt, cur_action,
					cfg->current_limit_threshold);
	} else if (cur_block) {
		snap->active_fault_cause |= VC_FAULT_CURRENT;
		apply_protection_action(snap, rt, cur_action,
					cfg->current_limit_threshold);
	} else {
		snap->active_fault_cause |= VC_FAULT_VOLTAGE;
		apply_protection_action(snap, rt, vol_action,
					cfg->voltage_limit_threshold);
	}

	if (should_start_cooldown(domain)) {
		start_cooldown(rt, &domain->sys_cfg);
	}
}

static void vc_tick_recovery(struct domain *domain, uint8_t ch,
			     uint32_t dt_ms)
{
	struct vc_channel_runtime *rt = &domain->runtime[ch];
	struct vc_channel_snapshot *snap = &domain->snapshots[ch];
	struct vc_system_config *sys = &domain->sys_cfg;

	if (domain->operating_mode != VC_OPERATING_MODE_AUTOMATIC) {
		return;
	}
	if (snap->active_fault_cause == 0) {
		return;
	}
	if (sys->recovery_policy_mode == VC_RECOVERY_MANUAL_LATCH ||
	    sys->recovery_policy_mode == VC_RECOVERY_NEVER_RETRY) {
		return;
	}

	if (rt->cooldown_remaining_ms > 0) {
		if (rt->cooldown_remaining_ms <= dt_ms) {
			rt->cooldown_remaining_ms = 0;
		} else {
			rt->cooldown_remaining_ms -= dt_ms;
		}
	}
}

static void vc_tick_status_bits(struct domain *domain, uint8_t ch)
{
	struct vc_channel_runtime *rt = &domain->runtime[ch];
	struct vc_channel_snapshot *snap = &domain->snapshots[ch];
	uint16_t bits = 0;

	if (snap->operational_target_voltage != 0 ||
	    snap->measured_voltage != 0) {
		bits |= 0x0001;
	}
	if (rt->output_enabled) {
		bits |= 0x0002;
	}
	if (rt->ramping) {
		bits |= 0x0004;
	}
	if (snap->active_fault_cause != 0) {
		bits |= 0x0008;
	}
	if (snap->fault_history_cause != 0) {
		bits |= 0x0010;
	}
	if (rt->cooldown_remaining_ms > 0) {
		bits |= 0x0020;
	}

	snap->auto_cooldown_remaining =
		(uint16_t)(rt->cooldown_remaining_ms / 1000);
	snap->status_bits = bits;
}

/* ---- Public tick entry ---- */

void domain_tick(struct domain *domain, uint32_t dt_ms,
		    const int16_t voltage_noise[],
		    const int16_t current_noise[])
{
	uint8_t n = domain->variant->num_channels;

	if (domain->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}

	for (uint8_t ch = 0; ch < n; ch++) {
		vc_tick_ramp(domain, ch, dt_ms);
		vc_tick_measure(domain, ch, voltage_noise[ch],
				current_noise[ch]);
		vc_tick_protection(domain, ch);
		vc_tick_recovery(domain, ch, dt_ms);
		vc_tick_status_bits(domain, ch);
	}
}

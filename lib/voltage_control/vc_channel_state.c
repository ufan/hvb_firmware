/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel_state.h"
#include "regmap/vc_regs.h"
#include <string.h>

#define VC_DEFAULT_MAX_RAW_DAC      0xFFFF
#define VC_DEFAULT_MAX_VOLTAGE_RAW  20000
#define VC_DEFAULT_MIN_VOLTAGE_RAW  0
#define VC_DEFAULT_MAX_CURRENT_RAW  32767

static const struct smf_state vc_channel_states[VC_CHANNEL_SMF_COUNT] = {
	[VC_CHANNEL_SMF_DISABLED_SAFE]      = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_ENABLED_HOLDING]    = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RAMPING]            = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_FAULT_LATCHED]      = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RETRY_COOLDOWN]     = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_CALIBRATION_OUTPUT] = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
};

static struct vc_channel_config default_channel_config(void)
{
	return (struct vc_channel_config){
		.voltage_limit_threshold = VC_DEFAULT_MAX_VOLTAGE_RAW,
		.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
		.output_calib_k = 10000,
		.measured_voltage_calib_k = 10000,
		.measured_current_calib_k = 10000,
	};
}

static void set_smf_state(struct vc_channel *ch, enum vc_channel_smf_state state)
{
	smf_set_state(SMF_CTX(ch), &vc_channel_states[state]);
}

static bool channel_has_cap(const struct vc_channel *ch, uint16_t cap)
{
	return (ch->capabilities & cap) == cap;
}

static bool has_hard_safety_fault(const struct vc_channel *ch)
{
	return (ch->active_fault_cause & (VC_FAULT_HARDWARE | VC_FAULT_INTERLOCK)) != 0;
}

static int16_t clamp_int16(int64_t value)
{
	if (value > INT16_MAX) {
		return INT16_MAX;
	}
	if (value < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)value;
}

static uint16_t raw_drive_from_target(const struct vc_channel_config *cfg,
				      int32_t target)
{
	int64_t raw = ((int64_t)target * cfg->output_calib_k) / 10000 +
		      cfg->output_calib_b;

	if (raw <= 0) {
		return 0;
	}
	if (raw > UINT16_MAX) {
		return UINT16_MAX;
	}
	return (uint16_t)raw;
}

static void set_pending(struct vc_channel *ch)
{
	ch->pending.valid = true;
	ch->pending.output_enable = ch->output_enabled;
	ch->pending.output_code = ch->output_enabled ?
		raw_drive_from_target(&ch->config, ch->operational_target_voltage) : 0;
}

static void update_status_bits(struct vc_channel *ch)
{
	enum vc_channel_smf_state state = vc_channel_get_smf_state(ch);
	uint16_t bits = 0;

	if (state == VC_CHANNEL_SMF_ENABLED_HOLDING ||
	    state == VC_CHANNEL_SMF_RAMPING) {
		bits |= 0x0001;
	} else if (ch->operational_target_voltage != 0 ||
		   ch->measured_voltage != 0) {
		bits |= 0x0001;
	}
	if (ch->output_enabled) {
		bits |= 0x0002;
	}
	if (state == VC_CHANNEL_SMF_RAMPING) {
		bits |= 0x0004;
	}
	if (state == VC_CHANNEL_SMF_FAULT_LATCHED ||
	    state == VC_CHANNEL_SMF_RETRY_COOLDOWN) {
		bits |= 0x0008;
	}
	if (ch->fault_history_cause != 0) {
		bits |= 0x0010;
	}
	if (state == VC_CHANNEL_SMF_RETRY_COOLDOWN) {
		bits |= 0x0020;
	}
	ch->status_bits = bits;
}

static bool is_valid_host_output_action(enum vc_output_action action)
{
	return action == VC_OUTPUT_ACTION_NONE ||
	       action == VC_OUTPUT_ACTION_ENABLE ||
	       action == VC_OUTPUT_ACTION_DISABLE_GRACEFUL ||
	       action == VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
}

static bool is_valid_channel_fault_command(enum vc_channel_fault_command cmd)
{
	return cmd >= VC_CHANNEL_FAULT_COMMAND_NONE &&
	       cmd <= VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY;
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

static enum vc_status validate_capability_config(
	const struct vc_channel *ch,
	const struct vc_channel_config *old_cfg,
	const struct vc_channel_config *new_cfg)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		if (new_cfg->configured_target_voltage != 0 ||
		    new_cfg->ramp_up_step != old_cfg->ramp_up_step ||
		    new_cfg->ramp_up_interval != old_cfg->ramp_up_interval ||
		    new_cfg->ramp_down_step != old_cfg->ramp_down_step ||
		    new_cfg->ramp_down_interval != old_cfg->ramp_down_interval ||
		    new_cfg->save_target_policy != old_cfg->save_target_policy ||
		    new_cfg->output_calib_k != old_cfg->output_calib_k ||
		    new_cfg->output_calib_b != old_cfg->output_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->voltage_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->voltage_protection_output_action != old_cfg->voltage_protection_output_action ||
		    new_cfg->voltage_limit_threshold != old_cfg->voltage_limit_threshold ||
		    new_cfg->measured_voltage_calib_k != old_cfg->measured_voltage_calib_k ||
		    new_cfg->measured_voltage_calib_b != old_cfg->measured_voltage_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_CURRENT_MEASUREMENT)) {
		if (new_cfg->current_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->current_protection_output_action != old_cfg->current_protection_output_action ||
		    new_cfg->current_limit_threshold != old_cfg->current_limit_threshold ||
		    new_cfg->measured_current_calib_k != old_cfg->measured_current_calib_k ||
		    new_cfg->measured_current_calib_b != old_cfg->measured_current_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE) ||
	    !channel_has_cap(ch, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->auto_derate_step != old_cfg->auto_derate_step) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	return VC_OK;
}

static void apply_protection_action(struct vc_channel *ch,
				    enum vc_output_action action,
				    int16_t clamp_limit)
{
	ch->last_protection_output_action = action;

	switch (action) {
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		ch->operational_target_voltage = 0;
		ch->output_enabled = false;
		break;
	case VC_OUTPUT_ACTION_CLAMP:
		ch->operational_target_voltage = clamp_limit;
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->raw_dac_readback = 0;
		ch->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		ch->output_enabled = false;
		ch->operational_target_voltage = 0;
		break;
	default:
		break;
	}
	set_pending(ch);
}

static void tick_voltage_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
		return;
	}
	if (cfg->voltage_protection_mode == VC_PROTECTION_MODE_DISABLED) {
		return;
	}
	if (ch->measured_voltage <= cfg->voltage_limit_threshold) {
		return;
	}

	ch->fault_history_cause |= VC_FAULT_VOLTAGE;

	if (cfg->voltage_protection_mode != VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION) {
		return;
	}

	ch->active_fault_cause |= VC_FAULT_VOLTAGE;
	ch->last_fault_timestamp = ch->uptime_ref;
	apply_protection_action(ch, cfg->voltage_protection_output_action,
				cfg->voltage_limit_threshold);
	set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
}

static void tick_current_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
		return;
	}
	if (cfg->current_protection_mode == VC_PROTECTION_MODE_DISABLED) {
		return;
	}
	if (ch->measured_current <= cfg->current_limit_threshold) {
		return;
	}

	ch->fault_history_cause |= VC_FAULT_CURRENT;

	if (cfg->current_protection_mode != VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION) {
		return;
	}

	ch->active_fault_cause |= VC_FAULT_CURRENT;
	ch->last_fault_timestamp = ch->uptime_ref;
	apply_protection_action(ch, cfg->current_protection_output_action,
				cfg->current_limit_threshold);
	set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
}

static void force_safe_state(struct vc_channel *ch)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->cal_output_enabled = 0;
	ch->raw_dac_readback = 0;
	ch->operational_target_voltage = 0;
	set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	set_pending(ch);
}

static bool is_safe_to_clear_active(const struct vc_channel *ch,
				    const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int32_t safe_limit;

	if (ch->active_fault_cause & VC_FAULT_VOLTAGE) {
		safe_limit = (int32_t)cfg->voltage_limit_threshold *
			     (100 - (int32_t)sys_cfg->voltage_safe_band_pct) / 100;
		if (ch->measured_voltage > safe_limit) {
			return false;
		}
	}
	if (ch->active_fault_cause & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)sys_cfg->current_safe_band_pct) / 100;
		if (ch->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}

/* ---- Public API ---- */

void vc_channel_init(struct vc_channel *ch, uint8_t index, uint16_t caps)
{
	memset(ch, 0, sizeof(*ch));
	ch->index = index;
	ch->capabilities = caps;
	ch->config = default_channel_config();
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	smf_set_initial(SMF_CTX(ch), &vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);
}

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch)
{
	const struct smf_ctx *ctx = SMF_CTX((struct vc_channel *)ch);

	return (enum vc_channel_smf_state)(ctx->current - &vc_channel_states[0]);
}

enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg)
{
	k_spinlock_key_t key = k_spin_lock((struct k_spinlock *)&ch->lock);

	*cfg = ch->config;
	k_spin_unlock((struct k_spinlock *)&ch->lock, key);
	return VC_OK;
}

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st;

	st = validate_capability_config(ch, &ch->config, cfg);
	if (st != VC_OK) {
		goto out;
	}
	if (!calibration_mode && calibration_fields_changed(&ch->config, cfg)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}
	if (cfg->configured_target_voltage > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->configured_target_voltage < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->voltage_limit_threshold > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->voltage_limit_threshold < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->current_limit_threshold < 0) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (!is_valid_protection_mode(cfg->voltage_protection_mode) ||
	    !is_valid_protection_output_action(cfg->voltage_protection_output_action, true) ||
	    !is_valid_protection_mode(cfg->current_protection_mode) ||
	    !is_valid_protection_output_action(cfg->current_protection_output_action, false)) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->save_target_policy > 1) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}

	ch->config = *cfg;
	if (!calibration_mode) {
		tick_voltage_protection(ch);
		tick_current_protection(ch);
	}
	set_pending(ch);
	update_status_bits(ch);
	st = VC_OK;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value,
				    bool calibration_mode)
{
	struct vc_channel_config cfg;
	enum vc_status st;

	st = vc_channel_get_config(ch, &cfg);
	if (st != VC_OK) {
		return st;
	}

	switch (field) {
	case VC_FIELD_CONFIGURED_TARGET_VOLTAGE:
		cfg.configured_target_voltage = (int16_t)value;
		break;
	case VC_FIELD_RAMP_UP_STEP:
		cfg.ramp_up_step = value;
		break;
	case VC_FIELD_RAMP_UP_INTERVAL:
		cfg.ramp_up_interval = value;
		break;
	case VC_FIELD_RAMP_DOWN_STEP:
		cfg.ramp_down_step = value;
		break;
	case VC_FIELD_RAMP_DOWN_INTERVAL:
		cfg.ramp_down_interval = value;
		break;
	case VC_FIELD_VOLTAGE_PROTECTION_MODE:
		cfg.voltage_protection_mode = (enum vc_protection_mode)value;
		break;
	case VC_FIELD_VOLTAGE_PROT_OUT_ACTION:
		cfg.voltage_protection_output_action = (enum vc_output_action)value;
		break;
	case VC_FIELD_VOLTAGE_LIMIT_THRESHOLD:
		cfg.voltage_limit_threshold = (int16_t)value;
		break;
	case VC_FIELD_CURRENT_PROTECTION_MODE:
		cfg.current_protection_mode = (enum vc_protection_mode)value;
		break;
	case VC_FIELD_CURRENT_PROT_OUT_ACTION:
		cfg.current_protection_output_action = (enum vc_output_action)value;
		break;
	case VC_FIELD_CURRENT_LIMIT_THRESHOLD:
		cfg.current_limit_threshold = (int16_t)value;
		break;
	case VC_FIELD_AUTO_DERATE_STEP:
		cfg.auto_derate_step = value;
		break;
	case VC_FIELD_SAVE_TARGET_POLICY:
		cfg.save_target_policy = value;
		break;
	case VC_FIELD_OUTPUT_CAL_K:
		cfg.output_calib_k = value;
		break;
	case VC_FIELD_OUTPUT_CAL_B:
		cfg.output_calib_b = (int16_t)value;
		break;
	case VC_FIELD_MEASURED_V_CAL_K:
		cfg.measured_voltage_calib_k = value;
		break;
	case VC_FIELD_MEASURED_V_CAL_B:
		cfg.measured_voltage_calib_b = (int16_t)value;
		break;
	case VC_FIELD_MEASURED_I_CAL_K:
		cfg.measured_current_calib_k = value;
		break;
	case VC_FIELD_MEASURED_I_CAL_B:
		cfg.measured_current_calib_b = (int16_t)value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_channel_set_config(ch, &cfg, calibration_mode);
}

void vc_channel_get_snapshot(const struct vc_channel *ch,
			     struct vc_channel_snapshot *snap)
{
	k_spinlock_key_t key = k_spin_lock((struct k_spinlock *)&ch->lock);

	memset(snap, 0, sizeof(*snap));
	snap->measured_voltage = ch->measured_voltage;
	snap->measured_current = ch->measured_current;
	snap->operational_target_voltage = ch->operational_target_voltage;
	snap->status_bits = ch->status_bits;
	snap->active_fault_cause = ch->active_fault_cause;
	snap->fault_history_cause = ch->fault_history_cause;
	snap->last_protection_output_action = ch->last_protection_output_action;
	snap->auto_retry_count = 0;
	snap->auto_cooldown_remaining = (uint16_t)(ch->cooldown_remaining_ms / 1000);
	snap->last_fault_timestamp = ch->last_fault_timestamp;
	snap->channel_capability_flags = ch->capabilities;
	snap->raw_adc_voltage = ch->raw_adc_voltage;
	snap->raw_adc_current = ch->raw_adc_current;
	snap->cal_sample_status = ch->cal_sample_status;
	snap->raw_dac_readback = ch->raw_dac_readback;
	snap->cal_output_enabled = ch->cal_output_enabled;
	snap->cal_max_raw_dac_limit = ch->cal_max_raw_dac_limit;

	k_spin_unlock((struct k_spinlock *)&ch->lock, key);
}

enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!is_valid_host_output_action(action)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}
	if (calibration_mode) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (ch->active_fault_cause != 0) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
		}
		ch->output_enabled = true;
		ch->ramping = true;
		set_smf_state(ch, VC_CHANNEL_SMF_RAMPING);
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->operational_target_voltage = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->raw_dac_readback = 0;
		ch->operational_target_voltage = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	default:
		goto out;
	}
	set_pending(ch);
	update_status_bits(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!is_valid_channel_fault_command(cmd)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (ch->active_fault_cause == 0) {
			break;
		}
		if (!is_safe_to_clear_active(ch, sys_cfg)) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
		}
		ch->active_fault_cause = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY:
		ch->fault_history_cause = 0;
		break;
	default:
		break;
	}
	update_status_bits(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->raw_adc_voltage = raw_voltage;
	ch->measured_voltage = clamp_int16(
		((int64_t)raw_voltage * ch->config.measured_voltage_calib_k) /
		10000 + ch->config.measured_voltage_calib_b);

	tick_voltage_protection(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->raw_adc_current = raw_current;
	ch->measured_current = clamp_int16(
		((int64_t)raw_current * ch->config.measured_current_calib_k) /
		10000 + ch->config.measured_current_calib_b);

	tick_current_protection(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->active_fault_cause |= fault_cause;
	ch->fault_history_cause |= fault_cause;
	force_safe_state(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	const struct vc_channel_config *cfg = &ch->config;
	int16_t target, current;
	uint16_t step, interval;
	uint32_t interval_ms;

	ARG_UNUSED(sys_cfg);

	if (!ch->output_enabled || ch->active_fault_cause != 0) {
		goto out;
	}

	target = cfg->configured_target_voltage;
	current = ch->operational_target_voltage;

	if (current == target) {
		if (ch->ramping) {
			ch->ramping = false;
			set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		}
		goto out;
	}

	ch->ramping = true;

	if (current < target) {
		step = cfg->ramp_up_step;
		interval = cfg->ramp_up_interval;
	} else {
		step = cfg->ramp_down_step;
		interval = cfg->ramp_down_interval;
	}

	if (step == 0 || interval == 0) {
		ch->operational_target_voltage = target;
		ch->ramping = false;
		set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		set_pending(ch);
		update_status_bits(ch);
		goto out;
	}

	interval_ms = (uint32_t)interval * 100;
	ch->ramp_accum_ms += dt_ms;

	while (ch->ramp_accum_ms >= interval_ms && current != target) {
		ch->ramp_accum_ms -= interval_ms;
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

	ch->operational_target_voltage = current;
	if (current == target) {
		ch->ramping = false;
		set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
	}
	set_pending(ch);
	update_status_bits(ch);

out:
	k_spin_unlock(&ch->lock, key);
}

bool vc_channel_has_pending_command(const struct vc_channel *ch)
{
	return ch->pending.valid;
}

struct vc_pending_command vc_channel_take_pending_command(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	struct vc_pending_command cmd = ch->pending;

	ch->pending.valid = false;
	k_spin_unlock(&ch->lock, key);
	return cmd;
}

/* ---- Calibration ---- */

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->output_enabled = false;
	ch->ramping = false;
	ch->ramp_accum_ms = 0;
	ch->cooldown_remaining_ms = 0;
	ch->operational_target_voltage = 0;
	ch->measured_voltage = 0;
	ch->measured_current = 0;
	ch->raw_dac_readback = 0;
	ch->cal_output_enabled = 0;
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	ch->raw_adc_voltage = 0;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_NONE;

	if (entering) {
		set_smf_state(ch, VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
	} else {
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	}
	set_pending(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable,
						const struct vc_channel *all_channels,
						size_t channel_count)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}

	if (enable) {
		if (has_hard_safety_fault(ch)) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
		}
		if (all_channels != NULL) {
			for (size_t i = 0; i < channel_count; i++) {
				if (&all_channels[i] == ch) {
					continue;
				}
				if (all_channels[i].cal_output_enabled ||
				    all_channels[i].raw_dac_readback != 0) {
					st = VC_ERR_UNSAFE_STATE;
					goto out;
				}
			}
		}
		ch->cal_output_enabled = 1;
	} else {
		ch->cal_output_enabled = 0;
		ch->raw_dac_readback = 0;
		ch->raw_adc_voltage = 0;
		ch->raw_adc_current = 0;
		ch->cal_sample_status = VC_CAL_SAMPLE_NONE;
	}
	set_pending(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	if (code > ch->cal_max_raw_dac_limit) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (code != 0 && has_hard_safety_fault(ch)) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	if (code != 0 && !ch->cal_output_enabled) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	ch->raw_dac_readback = code;
	set_pending(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	if (limit > VC_DEFAULT_MAX_RAW_DAC) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (limit < ch->raw_dac_readback) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	ch->cal_max_raw_dac_limit = limit;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_sample(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if ((ch->capabilities &
	     (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)) == 0) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	ch->raw_adc_voltage = ch->raw_dac_readback;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_VALID;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_commit(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (ch->cal_output_enabled || ch->raw_dac_readback != 0 ||
	    has_hard_safety_fault(ch)) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

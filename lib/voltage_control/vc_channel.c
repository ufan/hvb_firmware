/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel.h"
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

static void apply_hw(struct vc_channel *ch)
{
	if (ch->dev == NULL) {
		return;
	}
	const struct vc_channel_api *api = ch->dev->api;

	if (api == NULL) {
		return;
	}

	bool enable;
	uint16_t code;

	if (ch->cal_output_enabled) {
		enable = true;
		code = ch->raw_dac_readback;
	} else if (ch->output_enabled) {
		enable = true;
		code = raw_drive_from_target(&ch->config,
					     ch->operational_target_voltage);
	} else {
		enable = false;
		code = 0;
	}

	if (api->set_output) {
		api->set_output(ch->dev, code);
	}
	if (api->set_enable) {
		api->set_enable(ch->dev, enable);
	}
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

static bool is_valid_protection_output_action(enum vc_output_action action)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		return true;
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
		if (new_cfg->measured_voltage_calib_k != old_cfg->measured_voltage_calib_k ||
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
				    enum vc_output_action action)
{
	ch->last_protection_output_action = action;

	switch (action) {
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		ch->operational_target_voltage = 0;
		ch->output_enabled = false;
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
	apply_hw(ch);
}

static void tick_current_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
		return;
	}
	if (ch->ramping) {
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
	apply_protection_action(ch, cfg->current_protection_output_action);
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
	apply_hw(ch);
}

static bool is_safe_to_clear_active(const struct vc_channel *ch,
				    const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int32_t safe_limit;

	if (ch->active_fault_cause & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)sys_cfg->current_safe_band_pct) / 100;
		if (ch->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}

/* ---- Measurement callback — registered with hw driver ---- */

static void vc_channel_meas_ready(uint8_t channel, void *user_data)
{
	struct vc_channel *ch = user_data;

	ARG_UNUSED(channel);
	if (ch->wake_fn) {
		ch->wake_fn(ch->wake_user_data);
	}
}

/* ---- Public API ---- */

void vc_channel_init(struct vc_channel *ch,
		     const struct device *dev,
		     uint8_t index, uint16_t caps,
		     struct vc_channel_buffer *meas,
		     vc_wake_fn_t wake_fn, void *wake_user_data)
{
	memset(ch, 0, sizeof(*ch));
	ch->dev = dev;
	ch->index = index;
	ch->capabilities = caps;
	ch->meas = meas;
	ch->wake_fn = wake_fn;
	ch->wake_user_data = wake_user_data;
	ch->config = default_channel_config();
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	smf_set_initial(SMF_CTX(ch), &vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);

	if (dev != NULL && dev->api != NULL) {
		const struct vc_channel_api *api = dev->api;

		if (api->set_meas_callback) {
			api->set_meas_callback(dev, vc_channel_meas_ready, ch);
		}
	}
}

void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
		    const struct vc_system_config *sys_cfg)
{
	if (ch->meas != NULL) {
		if (ch->meas->voltage_timestamp_ms != ch->last_consumed_voltage_ts) {
			vc_channel_consume_voltage(ch, ch->meas->raw_voltage);
			ch->last_consumed_voltage_ts = ch->meas->voltage_timestamp_ms;
		}
		if (ch->meas->current_timestamp_ms != ch->last_consumed_current_ts) {
			vc_channel_consume_current(ch, ch->meas->raw_current);
			ch->last_consumed_current_ts = ch->meas->current_timestamp_ms;
		}
	}

	vc_channel_tick_ramp(ch, dt_ms, sys_cfg);
}

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch)
{
	const struct smf_ctx *ctx = SMF_CTX((struct vc_channel *)ch);

	return (enum vc_channel_smf_state)(ctx->current - &vc_channel_states[0]);
}

enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg)
{
	*cfg = ch->config;
	return VC_OK;
}

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode)
{
	enum vc_status st;

	st = validate_capability_config(ch, &ch->config, cfg);
	if (st != VC_OK) {
		return st;
	}
	if (!calibration_mode && calibration_fields_changed(&ch->config, cfg)) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (cfg->configured_target_voltage > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->configured_target_voltage < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_limit_threshold < 0) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->current_protection_mode) ||
	    !is_valid_protection_output_action(cfg->current_protection_output_action)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->save_target_policy > 1) {
		return VC_ERR_INVALID_VALUE;
	}

	ch->config = *cfg;
	if (!calibration_mode) {
		tick_current_protection(ch);
	}
	apply_hw(ch);
	update_status_bits(ch);
	return VC_OK;
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
}

enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode)
{
	if (!is_valid_host_output_action(action)) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (calibration_mode) {
		return VC_ERR_INVALID_COMMAND;
	}

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (ch->active_fault_cause != 0) {
			return VC_ERR_UNSAFE_STATE;
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
		return VC_OK;
	}
	apply_hw(ch);
	update_status_bits(ch);
	return VC_OK;
}

enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg)
{
	if (!is_valid_channel_fault_command(cmd)) {
		return VC_ERR_INVALID_COMMAND;
	}

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (ch->active_fault_cause == 0) {
			break;
		}
		if (!is_safe_to_clear_active(ch, sys_cfg)) {
			return VC_ERR_UNSAFE_STATE;
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
	return VC_OK;
}

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage)
{
	ch->raw_adc_voltage = raw_voltage;
	ch->measured_voltage = clamp_int16(
		((int64_t)raw_voltage * ch->config.measured_voltage_calib_k) /
		10000 + ch->config.measured_voltage_calib_b);

	update_status_bits(ch);
}

void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current)
{
	ch->raw_adc_current = raw_current;
	ch->measured_current = clamp_int16(
		((int64_t)raw_current * ch->config.measured_current_calib_k) /
		10000 + ch->config.measured_current_calib_b);

	if (!ch->ramping) {
		tick_current_protection(ch);
	}
	update_status_bits(ch);
}

void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause)
{
	ch->active_fault_cause |= fault_cause;
	ch->fault_history_cause |= fault_cause;
	force_safe_state(ch);
	update_status_bits(ch);
}

void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int16_t target, current;
	uint16_t step, interval;
	uint32_t interval_ms;

	ARG_UNUSED(sys_cfg);

	if (!ch->output_enabled || ch->active_fault_cause != 0) {
		return;
	}

	target = cfg->configured_target_voltage;
	current = ch->operational_target_voltage;

	if (current == target) {
		if (ch->ramping) {
			ch->ramping = false;
			set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		}
		return;
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
		apply_hw(ch);
		update_status_bits(ch);
		return;
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
	apply_hw(ch);
	update_status_bits(ch);
}

/* ---- Calibration ---- */

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering)
{
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
	apply_hw(ch);
	update_status_bits(ch);
}

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	if (enable) {
		if (has_hard_safety_fault(ch)) {
			return VC_ERR_UNSAFE_STATE;
		}
		ch->cal_output_enabled = 1;
	} else {
		ch->cal_output_enabled = 0;
		ch->raw_dac_readback = 0;
		ch->raw_adc_voltage = 0;
		ch->raw_adc_current = 0;
		ch->cal_sample_status = VC_CAL_SAMPLE_NONE;
	}
	apply_hw(ch);
	return VC_OK;
}

enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if (code > ch->cal_max_raw_dac_limit) {
		return VC_ERR_INVALID_VALUE;
	}
	if (code != 0 && has_hard_safety_fault(ch)) {
		return VC_ERR_UNSAFE_STATE;
	}
	if (code != 0 && !ch->cal_output_enabled) {
		return VC_ERR_UNSAFE_STATE;
	}
	ch->raw_dac_readback = code;
	apply_hw(ch);
	return VC_OK;
}

enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if (limit > VC_DEFAULT_MAX_RAW_DAC) {
		return VC_ERR_INVALID_VALUE;
	}
	if (limit < ch->raw_dac_readback) {
		return VC_ERR_UNSAFE_STATE;
	}
	ch->cal_max_raw_dac_limit = limit;
	return VC_OK;
}

enum vc_status vc_channel_cal_sample(struct vc_channel *ch)
{
	if ((ch->capabilities &
	     (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)) == 0) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	ch->raw_adc_voltage = ch->raw_dac_readback;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_VALID;
	return VC_OK;
}

enum vc_status vc_channel_cal_commit(struct vc_channel *ch)
{
	if (ch->cal_output_enabled || ch->raw_dac_readback != 0 ||
	    has_hard_safety_fault(ch)) {
		return VC_ERR_UNSAFE_STATE;
	}
	return VC_OK;
}

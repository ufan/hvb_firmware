/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"
#include "regmap/hvb_regs.h"
#include <stdlib.h>
#include <string.h>
#include <zephyr/smf.h>

#define VC_DEFAULT_MAX_RAW_DAC      0xFFFF
#define VC_DEFAULT_MAX_VOLTAGE_RAW  20000
#define VC_DEFAULT_MIN_VOLTAGE_RAW  0
#define VC_DEFAULT_MAX_CURRENT_RAW  32767
#define VC_DEFAULT_SYSTEM_CAPS      (SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR | SYS_CAP_CALIBRATION_MODE)
#define VC_VARIANT_ID               1
#define VC_CHANNEL_MASK(c)          ((1U << (c)) - 1)

enum vc_domain_smf_state {
	VC_DOMAIN_SMF_NORMAL,
	VC_DOMAIN_SMF_AUTOMATIC,
	VC_DOMAIN_SMF_CALIBRATION,
	VC_DOMAIN_SMF_COUNT,
};

enum vc_channel_smf_state {
	VC_CHANNEL_SMF_DISABLED_SAFE,
	VC_CHANNEL_SMF_ENABLED_HOLDING,
	VC_CHANNEL_SMF_RAMPING,
	VC_CHANNEL_SMF_FAULT_LATCHED,
	VC_CHANNEL_SMF_RETRY_COOLDOWN,
	VC_CHANNEL_SMF_CALIBRATION_OUTPUT,
	VC_CHANNEL_SMF_UNAVAILABLE,
	VC_CHANNEL_SMF_COUNT,
};

struct vc_domain_smf_ctx {
	struct smf_ctx ctx;
	struct domain *domain;
};

struct vc_channel_smf_ctx {
	struct smf_ctx ctx;
	struct domain *domain;
	uint8_t channel;
};

struct vc_channel_runtime {
	bool output_enabled;
	bool ramping;
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	uint16_t cal_max_raw_dac_limit;
	uint32_t runtime_config_version;
	struct vc_channel_smf_ctx smf;
};

struct domain {
	struct vc_domain_smf_ctx smf;
	const struct vc_channel_entry *ch_entry;
	size_t channel_count;
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

struct runtime_config_visible_state {
	bool output_enabled;
	uint16_t raw_output_drive;
	bool calibration_mode;
	bool calibration_output_enabled;
	uint16_t calibration_raw_output_drive;
	bool force_safe_state;
	int16_t operational_target_voltage;
};

static bool channel_valid(const struct domain *domain, uint8_t channel)
{
	return channel < domain->channel_count;
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
	rt->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	snap->raw_adc_voltage = 0;
	snap->raw_adc_current = 0;
	snap->cal_sample_status = VC_CAL_SAMPLE_NONE;
}

static void reset_calibration_outputs(struct domain *domain, bool entering)
{
	for (size_t ch = 0; ch < domain->channel_count; ch++) {
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

static bool channel_has_cap(const struct domain *domain, uint8_t channel,
			    uint16_t cap)
{
	return (domain->ch_entry[channel].capabilities & cap) == cap;
}

static int16_t clamp_int16_from_i64(int64_t value)
{
	if (value > INT16_MAX) {
		return INT16_MAX;
	}
	if (value < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)value;
}

static uint16_t raw_drive_from_operational_target(
	const struct vc_channel_config *cfg, int32_t target)
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

static void mark_runtime_config_changed(struct domain *domain, uint8_t channel)
{
	if (channel_valid(domain, channel)) {
		domain->runtime[channel].runtime_config_version++;
		if (domain->runtime[channel].runtime_config_version == 0) {
			domain->runtime[channel].runtime_config_version = 1;
		}
	}
}

static struct runtime_config_visible_state capture_runtime_config_visible_state(
	const struct domain *domain, uint8_t channel)
{
	const struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	const struct vc_channel_config *cfg = &domain->channels[channel];
	const struct vc_channel_runtime *rt = &domain->runtime[channel];
	bool calibration_mode = domain->operating_mode == VC_OPERATING_MODE_CALIBRATION;

	return (struct runtime_config_visible_state){
		.output_enabled = rt->output_enabled,
		.raw_output_drive = calibration_mode ? 0 :
				    raw_drive_from_operational_target(cfg,
					    snap->operational_target_voltage),
		.calibration_mode = calibration_mode,
		.calibration_output_enabled = snap->cal_output_enabled != 0,
		.calibration_raw_output_drive = snap->raw_dac_readback,
		.force_safe_state = !rt->output_enabled &&
				    snap->cal_output_enabled == 0 &&
				    snap->raw_dac_readback == 0,
		.operational_target_voltage = snap->operational_target_voltage,
	};
}

static bool runtime_config_visible_state_changed(
	const struct runtime_config_visible_state *before,
	const struct runtime_config_visible_state *after)
{
	return before->output_enabled != after->output_enabled ||
	       before->raw_output_drive != after->raw_output_drive ||
	       before->calibration_mode != after->calibration_mode ||
	       before->calibration_output_enabled != after->calibration_output_enabled ||
	       before->calibration_raw_output_drive != after->calibration_raw_output_drive ||
	       before->force_safe_state != after->force_safe_state ||
	       before->operational_target_voltage != after->operational_target_voltage;
}

static void mark_runtime_config_changed_if_visible_state_changed(
	struct domain *domain, uint8_t channel,
	const struct runtime_config_visible_state *before)
{
	struct runtime_config_visible_state after =
		capture_runtime_config_visible_state(domain, channel);

	if (runtime_config_visible_state_changed(before, &after)) {
		mark_runtime_config_changed(domain, channel);
	}
}

static void force_runtime_safe_state(struct domain *domain, uint8_t channel)
{
	struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	struct vc_channel_runtime *rt = &domain->runtime[channel];

	rt->output_enabled = false;
	rt->ramping = false;
	snap->cal_output_enabled = 0;
	snap->raw_dac_readback = 0;
	snap->operational_target_voltage = 0;
}

static enum vc_status validate_channel_capability_config(
	const struct domain *domain,
	uint8_t channel,
	const struct vc_channel_config *old_cfg,
	const struct vc_channel_config *new_cfg)
{
	if (!channel_has_cap(domain, channel, CH_CAP_RAW_OUTPUT_DRIVE)) {
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
	if (!channel_has_cap(domain, channel, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->voltage_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->voltage_protection_output_action != old_cfg->voltage_protection_output_action ||
		    new_cfg->voltage_limit_threshold != old_cfg->voltage_limit_threshold ||
		    new_cfg->measured_voltage_calib_k != old_cfg->measured_voltage_calib_k ||
		    new_cfg->measured_voltage_calib_b != old_cfg->measured_voltage_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(domain, channel, CH_CAP_CURRENT_MEASUREMENT)) {
		if (new_cfg->current_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->current_protection_output_action != old_cfg->current_protection_output_action ||
		    new_cfg->current_limit_threshold != old_cfg->current_limit_threshold ||
		    new_cfg->measured_current_calib_k != old_cfg->measured_current_calib_k ||
		    new_cfg->measured_current_calib_b != old_cfg->measured_current_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(domain, channel, CH_CAP_RAW_OUTPUT_DRIVE) ||
	    !channel_has_cap(domain, channel, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->auto_derate_step != old_cfg->auto_derate_step) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}

	return VC_OK;
}

static enum vc_status enter_calibration_mode(struct domain *domain)
{
	reset_calibration_outputs(domain, true);
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

static void vc_domain_smf_noop(void *obj)
{
	ARG_UNUSED(obj);
}

static const struct smf_state vc_domain_states[VC_DOMAIN_SMF_COUNT] = {
	[VC_DOMAIN_SMF_NORMAL] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_DOMAIN_SMF_AUTOMATIC] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_DOMAIN_SMF_CALIBRATION] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
};

static const struct smf_state vc_channel_states[VC_CHANNEL_SMF_COUNT] = {
	[VC_CHANNEL_SMF_DISABLED_SAFE] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_ENABLED_HOLDING] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RAMPING] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_FAULT_LATCHED] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RETRY_COOLDOWN] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_CALIBRATION_OUTPUT] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_UNAVAILABLE] = SMF_CREATE_STATE(NULL, vc_domain_smf_noop, NULL, NULL, NULL),
};

static void domain_init_smf(struct domain *domain)
{
	domain->smf.domain = domain;
	smf_set_initial(SMF_CTX(&domain->smf), &vc_domain_states[VC_DOMAIN_SMF_NORMAL]);

	for (size_t ch = 0; ch < domain->channel_count; ch++) {
		domain->runtime[ch].smf.domain = domain;
		domain->runtime[ch].smf.channel = ch;
		smf_set_initial(SMF_CTX(&domain->runtime[ch].smf),
				&vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);
	}
}

static struct domain *domain_init(struct domain *domain,
				  const struct vc_channel_entry *channels,
				  size_t count)
{
	if (domain == NULL || channels == NULL || count > VC_MAX_CHANNELS) {
		return NULL;
	}

	memset(domain, 0, sizeof(*domain));

	domain->ch_entry = channels;
	domain->channel_count = count;

	domain_init_smf(domain);

	domain->operating_mode = VC_OPERATING_MODE_NORMAL;
	domain->sys_cfg = (struct vc_system_config){
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.slave_address = 1,
		.baud_rate_code = VC_BAUD_RATE_115200,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
		.voltage_safe_band_pct = 10,
		.current_safe_band_pct = 10,
	};

	for (size_t i = 0; i < count && i < VC_MAX_CHANNELS; i++) {
		domain->channels[i] = (struct vc_channel_config){
			.voltage_limit_threshold = VC_DEFAULT_MAX_VOLTAGE_RAW,
			.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
			.output_calib_k = 10000,
			.measured_voltage_calib_k = 10000,
			.measured_current_calib_k = 10000,
		};
		domain->runtime[i].cal_max_raw_dac_limit = 0xFFFF;
		domain->runtime[i].runtime_config_version = 1;
	}

	return domain;
}

struct domain *domain_create(const struct vc_channel_entry *channels,
			     size_t count)
{
	struct domain *domain;

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		return NULL;
	}

	if (domain_init(domain, channels, count) == NULL) {
		free(domain);
		return NULL;
	}

	return domain;
}

struct domain *domain_create_static(const struct vc_channel_entry *channels,
				    size_t count)
{
	static struct domain domain;

	return domain_init(&domain, channels, count);
}

enum vc_status domain_get_runtime_config(const struct domain *domain,
						 uint8_t channel,
						 struct vc_runtime_config_snapshot *cfg)
{
	const struct vc_channel_snapshot *snap;
	const struct vc_channel_config *ch_cfg;
	const struct vc_channel_runtime *rt;

	if (!cfg) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	snap = &domain->snapshots[channel];
	ch_cfg = &domain->channels[channel];
	rt = &domain->runtime[channel];

	*cfg = (struct vc_runtime_config_snapshot){
		.channel = channel,
		.version = rt->runtime_config_version,
		.capability_flags = domain->ch_entry[channel].capabilities,
		.output_enable = rt->output_enabled,
		.raw_output_drive = domain->operating_mode == VC_OPERATING_MODE_CALIBRATION ?
				    0 : raw_drive_from_operational_target(ch_cfg,
					    snap->operational_target_voltage),
		.calibration_mode = domain->operating_mode == VC_OPERATING_MODE_CALIBRATION,
		.calibration_output_enable = snap->cal_output_enabled != 0,
		.calibration_raw_output_drive = snap->raw_dac_readback,
		.force_safe_state = !rt->output_enabled &&
				    snap->cal_output_enabled == 0 &&
				    snap->raw_dac_readback == 0,
	};

	return VC_OK;
}

enum vc_operating_mode domain_get_operating_mode(const struct domain *domain)
{
	return domain->operating_mode;
}

enum vc_status domain_set_operating_mode(struct domain *domain,
					    enum vc_operating_mode mode)
{
	enum vc_operating_mode old_mode = domain->operating_mode;
	struct runtime_config_visible_state before[VC_MAX_CHANNELS];
	bool capture_runtime_state;

	if (!is_valid_operating_mode(mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (is_entering_calibration(domain, mode) && !domain->cal_unlocked) {
		return VC_ERR_INVALID_COMMAND;
	}

	if (domain->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    mode == VC_OPERATING_MODE_NORMAL) {
		for (size_t i = 0; i < domain->channel_count; i++) {
			domain->runtime[i].cooldown_remaining_ms = 0;
		}
	}

	capture_runtime_state = mode == VC_OPERATING_MODE_CALIBRATION ||
				old_mode == VC_OPERATING_MODE_CALIBRATION;
	if (capture_runtime_state) {
		for (size_t i = 0; i < domain->channel_count; i++) {
			before[i] = capture_runtime_config_visible_state(domain, i);
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
	if (capture_runtime_state) {
		for (size_t i = 0; i < domain->channel_count; i++) {
			mark_runtime_config_changed_if_visible_state_changed(domain, i,
								      &before[i]);
		}
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
	struct runtime_config_visible_state before[VC_MAX_CHANNELS];
	bool capture_runtime_state;

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
		for (size_t i = 0; i < domain->channel_count; i++) {
			domain->runtime[i].cooldown_remaining_ms = 0;
		}
	}

	capture_runtime_state = cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION ||
				old_mode == VC_OPERATING_MODE_CALIBRATION;
	if (capture_runtime_state) {
		for (size_t i = 0; i < domain->channel_count; i++) {
			before[i] = capture_runtime_config_visible_state(domain, i);
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
	if (capture_runtime_state) {
		for (size_t i = 0; i < domain->channel_count; i++) {
			mark_runtime_config_changed_if_visible_state_changed(domain, i,
								      &before[i]);
		}
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
	enum vc_status status;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	status = validate_channel_capability_config(domain, channel,
						  &domain->channels[channel], cfg);
	if (status != VC_OK) {
		return status;
	}
	if (domain->operating_mode != VC_OPERATING_MODE_CALIBRATION &&
	    calibration_fields_changed(&domain->channels[channel], cfg)) {
		return VC_ERR_INVALID_COMMAND;
	}

	max_v = VC_DEFAULT_MAX_VOLTAGE_RAW;
	min_v = VC_DEFAULT_MIN_VOLTAGE_RAW;

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

enum vc_status domain_consume_measurement(struct domain *domain,
					     const struct vc_measurement_snapshot *meas)
{
	struct vc_channel_config *cfg;
	struct vc_channel_snapshot *snap;
	struct runtime_config_visible_state before;
	uint16_t provider_faults;
	bool hard_provider_fault = false;

	if (!meas) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!channel_valid(domain, meas->channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if ((meas->present_mask & VC_MEAS_PRESENT_VOLTAGE) &&
	    !channel_has_cap(domain, meas->channel, CH_CAP_VOLTAGE_MEASUREMENT)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if ((meas->present_mask & VC_MEAS_PRESENT_CURRENT) &&
	    !channel_has_cap(domain, meas->channel, CH_CAP_CURRENT_MEASUREMENT)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	cfg = &domain->channels[meas->channel];
	snap = &domain->snapshots[meas->channel];

	if (meas->present_mask & VC_MEAS_PRESENT_VOLTAGE) {
		snap->raw_adc_voltage = meas->raw_voltage;
		snap->measured_voltage = clamp_int16_from_i64(
			((int64_t)meas->raw_voltage * cfg->measured_voltage_calib_k) /
			10000 + cfg->measured_voltage_calib_b);
	}
	if (meas->present_mask & VC_MEAS_PRESENT_CURRENT) {
		snap->raw_adc_current = meas->raw_current;
		snap->measured_current = clamp_int16_from_i64(
			((int64_t)meas->raw_current * cfg->measured_current_calib_k) /
			10000 + cfg->measured_current_calib_b);
	}
	if (meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) {
		before = capture_runtime_config_visible_state(domain, meas->channel);
		provider_faults = meas->provider_fault_cause;
		if (provider_faults & (VC_FAULT_HARDWARE | VC_FAULT_INTERLOCK)) {
			hard_provider_fault = true;
		}

		if (meas->provider_status & VC_PROVIDER_STATUS_INTERLOCK) {
			provider_faults |= VC_FAULT_INTERLOCK;
			hard_provider_fault = true;
		}
		if (meas->provider_status & VC_PROVIDER_STATUS_APPLY_FAILED) {
			provider_faults |= VC_FAULT_HARDWARE;
			hard_provider_fault = true;
		}
		if (meas->provider_status & VC_PROVIDER_STATUS_SAMPLE_ERROR) {
			provider_faults |= VC_FAULT_MEASUREMENT;
		}
		if (hard_provider_fault) {
			force_runtime_safe_state(domain, meas->channel);
		}
		if (provider_faults != 0) {
			snap->active_fault_cause |= provider_faults;
			snap->fault_history_cause |= provider_faults;
		}
		mark_runtime_config_changed_if_visible_state_changed(domain,
							      meas->channel, &before);
	}

	return VC_OK;
}

enum vc_status domain_get_system_snapshot(const struct domain *domain,
					     struct vc_system_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->protocol_major = HVB_PROTOCOL_MAJOR;
	snap->protocol_minor = HVB_PROTOCOL_MINOR;
	snap->variant_id = VC_VARIANT_ID;
	snap->system_capability_flags = VC_DEFAULT_SYSTEM_CAPS;
	snap->supported_channel_count = (uint16_t)domain->channel_count;
	snap->active_channel_mask = VC_CHANNEL_MASK(domain->channel_count);
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
	snap->channel_capability_flags = domain->ch_entry[channel].capabilities;
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

static enum vc_status calibration_capability_ready(
	const struct domain *domain, uint8_t channel, uint16_t caps)
{
	enum vc_status status = calibration_channel_ready(domain, channel);

	if (status != VC_OK) {
		return status;
	}
	if (!channel_has_cap(domain, channel, caps)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	return VC_OK;
}

enum vc_status domain_calibration_set_output_enable(struct domain *domain,
						       uint8_t channel,
						       bool enabled)
{
	enum vc_status status = calibration_capability_ready(domain, channel,
							       CH_CAP_RAW_OUTPUT_DRIVE);
	struct runtime_config_visible_state before;

	if (status != VC_OK) {
		return status;
	}
	before = capture_runtime_config_visible_state(domain, channel);

	if (enabled) {
		if (has_hard_safety_fault(domain, channel)) {
			return VC_ERR_UNSAFE_STATE;
		}
		for (size_t ch = 0; ch < domain->channel_count; ch++) {
			if (ch == channel) {
				continue;
			}
			if (domain->snapshots[ch].cal_output_enabled ||
			    domain->snapshots[ch].raw_dac_readback != 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
		domain->snapshots[channel].cal_output_enabled = 1;
		mark_runtime_config_changed_if_visible_state_changed(domain, channel,
								      &before);
		return VC_OK;
	}

	domain->snapshots[channel].cal_output_enabled = 0;
	domain->snapshots[channel].raw_dac_readback = 0;
	domain->snapshots[channel].raw_adc_voltage = 0;
	domain->snapshots[channel].raw_adc_current = 0;
	domain->snapshots[channel].cal_sample_status = VC_CAL_SAMPLE_NONE;
	mark_runtime_config_changed_if_visible_state_changed(domain, channel,
							      &before);
	return VC_OK;
}

enum vc_status domain_calibration_set_raw_dac(struct domain *domain,
						 uint8_t channel,
						 uint16_t code)
{
	enum vc_status status = calibration_capability_ready(domain, channel,
							       CH_CAP_RAW_OUTPUT_DRIVE);
	struct runtime_config_visible_state before;
	uint16_t limit;

	if (status != VC_OK) {
		return status;
	}
	before = capture_runtime_config_visible_state(domain, channel);

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
	mark_runtime_config_changed_if_visible_state_changed(domain, channel,
							      &before);
	return VC_OK;
}

enum vc_status domain_calibration_set_max_raw_dac(struct domain *domain,
						     uint8_t channel,
						     uint16_t limit)
{
	enum vc_status status = calibration_capability_ready(domain, channel,
							       CH_CAP_RAW_OUTPUT_DRIVE);

	if (status != VC_OK) {
		return status;
	}
	if (limit > VC_DEFAULT_MAX_RAW_DAC) {
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
	if ((domain->ch_entry[channel].capabilities &
	     (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)) == 0) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
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
	struct runtime_config_visible_state before;

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
	before = capture_runtime_config_visible_state(domain, channel);

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (snap->active_fault_cause != 0) {
			return VC_ERR_UNSAFE_STATE;
		}
		rt->output_enabled = true;
		rt->ramping = true;
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		rt->output_enabled = false;
		rt->ramping = false;
		snap->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		rt->output_enabled = false;
		rt->ramping = false;
		snap->raw_dac_readback = 0;
		snap->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_NONE:
	default:
		break;
	}
	mark_runtime_config_changed_if_visible_state_changed(domain, channel, &before);

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
	return (uint16_t)domain->channel_count;
}

uint16_t domain_get_active_channel_mask(const struct domain *domain)
{
	return VC_CHANNEL_MASK(domain->channel_count);
}

uint16_t domain_get_variant_id(const struct domain *domain)
{
	return VC_VARIANT_ID;
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
	int16_t max_v = VC_DEFAULT_MAX_VOLTAGE_RAW;
	int16_t min_v = VC_DEFAULT_MIN_VOLTAGE_RAW;
	int16_t max_i = VC_DEFAULT_MAX_CURRENT_RAW;
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

static void apply_protection_action(struct domain *domain, uint8_t channel,
				    enum vc_output_action action,
				    int16_t clamp_limit)
{
	struct vc_channel_snapshot *snap = &domain->snapshots[channel];
	struct vc_channel_runtime *rt = &domain->runtime[channel];
	struct runtime_config_visible_state before =
		capture_runtime_config_visible_state(domain, channel);

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
		rt->output_enabled = false;
		rt->ramping = false;
		snap->raw_dac_readback = 0;
		snap->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		rt->output_enabled = false;
		snap->operational_target_voltage = 0;
		break;
	default:
		break;
	}
	mark_runtime_config_changed_if_visible_state_changed(domain, channel, &before);
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
		apply_protection_action(domain, ch, cur_action,
					cfg->current_limit_threshold);
	} else if (cur_block) {
		snap->active_fault_cause |= VC_FAULT_CURRENT;
		apply_protection_action(domain, ch, cur_action,
					cfg->current_limit_threshold);
	} else {
		snap->active_fault_cause |= VC_FAULT_VOLTAGE;
		apply_protection_action(domain, ch, vol_action,
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
	uint8_t n = (uint8_t)domain->channel_count;

	if (domain->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}

	for (uint8_t ch = 0; ch < n; ch++) {
		struct runtime_config_visible_state before =
			capture_runtime_config_visible_state(domain, ch);
		uint32_t version_before = domain->runtime[ch].runtime_config_version;

		vc_tick_ramp(domain, ch, dt_ms);
		vc_tick_measure(domain, ch, voltage_noise[ch],
				current_noise[ch]);
		vc_tick_protection(domain, ch);
		vc_tick_recovery(domain, ch, dt_ms);
		vc_tick_status_bits(domain, ch);
		if (domain->runtime[ch].runtime_config_version == version_before) {
			mark_runtime_config_changed_if_visible_state_changed(domain, ch,
								      &before);
		}
	}
}

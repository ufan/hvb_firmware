/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_register.h"
#include "voltage_control/vc.h"
#include "sys_status/sys_status.h"
#include "regmap/vc_regs.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t int32_hi(int32_t value)
{
	return (uint16_t)((uint32_t)value >> 16);
}

static uint16_t int32_lo(int32_t value)
{
	return (uint16_t)((uint32_t)value & 0xFFFF);
}

static bool caps_any(uint16_t caps, uint16_t mask)
{
	return (caps & mask) != 0;
}

static bool caps_all(uint16_t caps, uint16_t mask)
{
	return (caps & mask) == mask;
}

/* ------------------------------------------------------------------ */
/* Channel capability / calibration guards                             */
/* ------------------------------------------------------------------ */

static bool is_ch_cal_input_reg(uint16_t off)
{
	/* CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed in v3 */
	return off >= CH_RAW_ADC_VOLTAGE_HI && off <= CH_RAW_ADC_CURRENT_LO;
}

static bool is_ch_cal_holding_reg(uint16_t off)
{
	return off >= CH_CAL_OUTPUT_ENABLE && off <= CH_CAL_MAX_RAW_DAC_LIMIT;
}

static bool is_ch_calibration_coefficient_reg(uint16_t off)
{
	return off >= CH_OUTPUT_CAL_K && off <= CH_MEASURED_I_CAL_B;
}

static bool ch_input_supported(uint16_t caps, uint16_t off)
{
	switch (off) {
	case CH_STATUS_BITS:
	case CH_ACTIVE_FAULT_CAUSE:
	case CH_FAULT_HISTORY_CAUSE:
	case CH_LAST_PROT_OUT_ACTION:
	case CH_AUTO_RETRY_COUNT:
	case CH_AUTO_COOLDOWN_REMAINING:
	case CH_LAST_FAULT_TIMESTAMP_HI:
	case CH_LAST_FAULT_TIMESTAMP_LO:
	case CH_OPER_TARGET_VOLTAGE:
	case CH_CAPABILITY_FLAGS:
		return true;
	case CH_MEASURED_VOLTAGE:
	case CH_RAW_ADC_VOLTAGE_HI:
	case CH_RAW_ADC_VOLTAGE_LO:
		return caps_all(caps, CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_MEASURED_CURRENT:
	case CH_RAW_ADC_CURRENT_HI:
	case CH_RAW_ADC_CURRENT_LO:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	default:
		return off < CH_BLOCK_SIZE;
	}
}

static bool ch_holding_supported(uint16_t caps, uint16_t off)
{
	switch (off) {
	case CH_OUTPUT_ACTION:
	case CH_FAULT_CMD:
	case CH_PARAM_ACTION:
		return true;
	case CH_CFG_TARGET_VOLTAGE:
	case CH_RAMP_UP_STEP:
	case CH_RAMP_UP_INTERVAL:
	case CH_RAMP_DOWN_STEP:
	case CH_RAMP_DOWN_INTERVAL:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
	case CH_RECOVERY_POLICY_MODE:
	case CH_AUTO_RETRY_DELAY:
	case CH_AUTO_RETRY_MAX_COUNT:
	case CH_AUTO_RETRY_WINDOW:
	case CH_CURRENT_SAFE_BAND_PCT:
		return true;
	case CH_CURRENT_PROTECTION_MODE:
	case CH_CURRENT_PROT_OUT_ACTION:
	case CH_CURRENT_LIMIT_THRESHOLD:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	case CH_AUTO_DERATE_STEP:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE |
					     CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_OUTPUT_CAL_K:
	case CH_OUTPUT_CAL_B:
	case CH_CAL_OUTPUT_ENABLE:
	case CH_CAL_DAC_CODE:
	case CH_CAL_MAX_RAW_DAC_LIMIT:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
	case CH_MEASURED_V_CAL_K:
	case CH_MEASURED_V_CAL_B:
		return caps_all(caps, CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_MEASURED_I_CAL_K:
	case CH_MEASURED_I_CAL_B:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	case CH_CAL_SAMPLE_CMD:
		return caps_any(caps, CH_CAP_VOLTAGE_MEASUREMENT |
					     CH_CAP_CURRENT_MEASUREMENT);
	case CH_CAL_COMMIT_CMD:
		return caps_any(caps, CH_CAP_RAW_OUTPUT_DRIVE |
					     CH_CAP_VOLTAGE_MEASUREMENT |
					     CH_CAP_CURRENT_MEASUREMENT);
	default:
		return off < CH_BLOCK_SIZE;
	}
}

/* ------------------------------------------------------------------ */
/* Register-to-field mapping tables                                    */
/* ------------------------------------------------------------------ */

static const enum vc_config_field sys_reg_to_field[] = {
	[SYS_OPERATING_MODE]         = VC_FIELD_OPERATING_MODE,
	[SYS_STARTUP_CHANNEL_POLICY] = VC_FIELD_STARTUP_CHANNEL_POLICY,
};

static const enum vc_config_field ch_reg_to_field[] = {
	[CH_CFG_TARGET_VOLTAGE]      = VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
	[CH_RAMP_UP_STEP]            = VC_FIELD_RAMP_UP_STEP,
	[CH_RAMP_UP_INTERVAL]        = VC_FIELD_RAMP_UP_INTERVAL,
	[CH_RAMP_DOWN_STEP]          = VC_FIELD_RAMP_DOWN_STEP,
	[CH_RAMP_DOWN_INTERVAL]      = VC_FIELD_RAMP_DOWN_INTERVAL,
	[CH_RECOVERY_POLICY_MODE]    = VC_FIELD_RECOVERY_POLICY_MODE,
	[CH_AUTO_RETRY_DELAY]        = VC_FIELD_AUTO_RETRY_DELAY,
	[CH_AUTO_RETRY_MAX_COUNT]    = VC_FIELD_AUTO_RETRY_MAX_COUNT,
	[CH_AUTO_RETRY_WINDOW]       = VC_FIELD_AUTO_RETRY_WINDOW,
	[CH_CURRENT_SAFE_BAND_PCT]   = VC_FIELD_CURRENT_SAFE_BAND_PCT,
	[CH_CURRENT_PROTECTION_MODE] = VC_FIELD_CURRENT_PROTECTION_MODE,
	[CH_CURRENT_PROT_OUT_ACTION] = VC_FIELD_CURRENT_PROT_OUT_ACTION,
	[CH_CURRENT_LIMIT_THRESHOLD] = VC_FIELD_CURRENT_LIMIT_THRESHOLD,
	[CH_AUTO_DERATE_STEP]        = VC_FIELD_AUTO_DERATE_STEP,
};

static const enum vc_cal_field ch_reg_to_cal_field[] = {
	[CH_OUTPUT_CAL_K]     = VC_CAL_FIELD_OUTPUT_K,
	[CH_OUTPUT_CAL_B]     = VC_CAL_FIELD_OUTPUT_B,
	[CH_MEASURED_V_CAL_K] = VC_CAL_FIELD_MEASURED_V_K,
	[CH_MEASURED_V_CAL_B] = VC_CAL_FIELD_MEASURED_V_B,
	[CH_MEASURED_I_CAL_K] = VC_CAL_FIELD_MEASURED_I_K,
	[CH_MEASURED_I_CAL_B] = VC_CAL_FIELD_MEASURED_I_B,
};

/* ------------------------------------------------------------------ */
/* System register read/write                                          */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_read_sys_input(struct vc_ctx *ctx, uint16_t off,
				     uint16_t *reg)
{
	struct vc_system_snapshot snap;

	vc_query(ctx, vc_q_system_snapshot(&snap));

	switch (off) {
	case SYS_PROTOCOL_MAJOR:        *reg = snap.protocol_major; break;
	case SYS_PROTOCOL_MINOR:        *reg = snap.protocol_minor; break;
	case SYS_VARIANT_ID:            *reg = snap.variant_id; break;
	case SYS_CAPABILITY_FLAGS:      *reg = snap.system_capability_flags; break;
	case SYS_SUPPORTED_CHANNELS:    *reg = snap.supported_channel_count; break;
	case SYS_ACTIVE_CHANNEL_MASK:   *reg = snap.active_channel_mask; break;
	case SYS_ACTIVE_OPERATING_MODE: *reg = (uint16_t)snap.active_operating_mode; break;
	case SYS_STATUS:                *reg = snap.system_status; break;
	case SYS_FAULT_CAUSE:           *reg = snap.system_fault_cause; break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}

enum vc_status vc_reg_read_sys_holding(struct vc_ctx *ctx, uint16_t off,
				       uint16_t *reg)
{
	struct vc_system_config cfg;

	vc_query(ctx, vc_q_system_config(&cfg));

	switch (off) {
	case SYS_OPERATING_MODE:         *reg = cfg.operating_mode; break;
	case SYS_STARTUP_CHANNEL_POLICY: *reg = cfg.startup_channel_policy; break;
	case SYS_PARAM_ACTION:           *reg = 0; break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}

enum vc_status vc_reg_write_sys_holding(struct vc_ctx *ctx, uint16_t off,
					uint16_t val, k_timeout_t timeout)
{
	if (off == SYS_PARAM_ACTION) {
		return vc_dispatch(ctx, vc_cmd_sys_param((enum vc_param_action)val),
				   timeout);
	}
	if (off > SYS_STARTUP_CHANNEL_POLICY) {
		return VC_ERR_INVALID_VALUE;
	}
	return vc_dispatch(ctx, vc_cmd_sys_field(sys_reg_to_field[off], val),
			   timeout);
}

/* ------------------------------------------------------------------ */
/* Channel register read/write                                         */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_read_ch_input(struct vc_ctx *ctx, uint8_t ch,
				    uint16_t off, uint16_t *reg)
{
	struct vc_channel_snapshot snap;
	struct vc_system_snapshot sys;

	if (vc_query(ctx, vc_q_channel_snapshot(ch, &snap)) != VC_OK) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	vc_query(ctx, vc_q_system_snapshot(&sys));
	if (is_ch_cal_input_reg(off) &&
	    sys.active_operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	if (!ch_input_supported(snap.channel_capability_flags, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	switch (off) {
	case CH_MEASURED_VOLTAGE:          *reg = (uint16_t)snap.measured_voltage; break;
	case CH_MEASURED_CURRENT:          *reg = (uint16_t)snap.measured_current; break;
	case CH_OPER_TARGET_VOLTAGE:       *reg = (uint16_t)snap.operational_target_voltage; break;
	case CH_STATUS_BITS:               *reg = snap.status_bits; break;
	case CH_ACTIVE_FAULT_CAUSE:         *reg = snap.active_fault_cause; break;
	case CH_FAULT_HISTORY_CAUSE:        *reg = snap.fault_history_cause; break;
	case CH_LAST_PROT_OUT_ACTION:       *reg = snap.last_protection_output_action; break;
	case CH_AUTO_RETRY_COUNT:           *reg = snap.auto_retry_count; break;
	case CH_AUTO_COOLDOWN_REMAINING:    *reg = snap.auto_cooldown_remaining; break;
	case CH_LAST_FAULT_TIMESTAMP_HI:    *reg = (uint16_t)(snap.last_fault_timestamp >> 16); break;
	case CH_LAST_FAULT_TIMESTAMP_LO:    *reg = (uint16_t)(snap.last_fault_timestamp & 0xFFFF); break;
	case CH_CAPABILITY_FLAGS:           *reg = snap.channel_capability_flags; break;
	case CH_RAW_ADC_VOLTAGE_HI:         *reg = int32_hi(snap.raw_adc_voltage); break;
	case CH_RAW_ADC_VOLTAGE_LO:         *reg = int32_lo(snap.raw_adc_voltage); break;
	case CH_RAW_ADC_CURRENT_HI:         *reg = int32_hi(snap.raw_adc_current); break;
	case CH_RAW_ADC_CURRENT_LO:         *reg = int32_lo(snap.raw_adc_current); break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}

enum vc_status vc_reg_read_ch_holding(struct vc_ctx *ctx, uint8_t ch_idx,
				      uint16_t off, uint16_t *reg)
{
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	struct vc_channel_cal_config cal;

	if (vc_query(ctx, vc_q_channel_snapshot(ch_idx, &snap)) != VC_OK) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!ch_holding_supported(snap.channel_capability_flags, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	vc_query(ctx, vc_q_channel_config(ch_idx, &cfg));

	switch (off) {
	/* Commands — always read as 0 */
	case CH_OUTPUT_ACTION:
	case CH_FAULT_CMD:
	case CH_PARAM_ACTION:
	case CH_CAL_SAMPLE_CMD:
	case CH_CAL_COMMIT_CMD:
		*reg = 0;
		break;
	/* Operational config */
	case CH_CFG_TARGET_VOLTAGE:      *reg = (uint16_t)cfg.configured_target_voltage; break;
	case CH_RAMP_UP_STEP:            *reg = cfg.ramp_up_step; break;
	case CH_RAMP_UP_INTERVAL:        *reg = cfg.ramp_up_interval; break;
	case CH_RAMP_DOWN_STEP:          *reg = cfg.ramp_down_step; break;
	case CH_RAMP_DOWN_INTERVAL:      *reg = cfg.ramp_down_interval; break;
	/* Recovery (moved from system block in v3) */
	case CH_RECOVERY_POLICY_MODE:    *reg = cfg.recovery_policy_mode; break;
	case CH_AUTO_RETRY_DELAY:        *reg = cfg.auto_retry_delay; break;
	case CH_AUTO_RETRY_MAX_COUNT:    *reg = cfg.auto_retry_max_count; break;
	case CH_AUTO_RETRY_WINDOW:       *reg = cfg.auto_retry_window; break;
	case CH_CURRENT_SAFE_BAND_PCT:   *reg = cfg.current_safe_band_pct; break;
	/* Protection */
	case CH_CURRENT_PROTECTION_MODE: *reg = cfg.current_protection_mode; break;
	case CH_CURRENT_PROT_OUT_ACTION: *reg = cfg.current_protection_output_action; break;
	case CH_CURRENT_LIMIT_THRESHOLD: *reg = (uint16_t)cfg.current_limit_threshold; break;
	case CH_AUTO_DERATE_STEP:        *reg = cfg.auto_derate_step; break;
	/* Cal config (readable in any mode) */
	case CH_OUTPUT_CAL_K:
	case CH_OUTPUT_CAL_B:
	case CH_MEASURED_V_CAL_K:
	case CH_MEASURED_V_CAL_B:
	case CH_MEASURED_I_CAL_K:
	case CH_MEASURED_I_CAL_B:
		vc_query(ctx, vc_q_channel_cal_config(ch_idx, &cal));
		switch (off) {
		case CH_OUTPUT_CAL_K:     *reg = cal.output_calib_k; break;
		case CH_OUTPUT_CAL_B:     *reg = (uint16_t)cal.output_calib_b; break;
		case CH_MEASURED_V_CAL_K: *reg = cal.measured_voltage_calib_k; break;
		case CH_MEASURED_V_CAL_B: *reg = (uint16_t)cal.measured_voltage_calib_b; break;
		case CH_MEASURED_I_CAL_K: *reg = cal.measured_current_calib_k; break;
		case CH_MEASURED_I_CAL_B: *reg = (uint16_t)cal.measured_current_calib_b; break;
		}
		break;
	/* Cal session state (FC03 readback) */
	case CH_CAL_OUTPUT_ENABLE:     *reg = snap.cal_output_enabled; break;
	case CH_CAL_DAC_CODE:          *reg = snap.raw_dac_readback; break;
	case CH_CAL_MAX_RAW_DAC_LIMIT: *reg = snap.cal_max_raw_dac_limit; break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}

enum vc_status vc_reg_write_ch_holding(struct vc_ctx *ctx, uint8_t ch,
				       uint16_t off, uint16_t val,
				       k_timeout_t timeout)
{
	struct vc_channel_snapshot snap;
	struct vc_system_snapshot sys;

	if (vc_query(ctx, vc_q_channel_snapshot(ch, &snap)) != VC_OK) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!ch_holding_supported(snap.channel_capability_flags, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	switch (off) {
	case CH_OUTPUT_ACTION:
		return vc_dispatch(ctx, vc_cmd_output(ch, (enum vc_output_action)val),
				   timeout);
	case CH_FAULT_CMD:
		return vc_dispatch(ctx,
				   vc_cmd_fault(ch, (enum vc_channel_fault_command)val),
				   timeout);
	case CH_PARAM_ACTION:
		return vc_dispatch(ctx,
				   vc_cmd_ch_param(ch, (enum vc_param_action)val),
				   timeout);
	default:
		break;
	}

	vc_query(ctx, vc_q_system_snapshot(&sys));

	/* Cal coefficient writes — route via SET_CHANNEL_CAL_FIELD (cal mode gated in controller) */
	if (is_ch_calibration_coefficient_reg(off)) {
		return vc_dispatch(ctx,
				   vc_cmd_cal_set_field(ch, ch_reg_to_cal_field[off], val),
				   timeout);
	}

	if (is_ch_cal_holding_reg(off)) {
		if (sys.active_operating_mode != VC_OPERATING_MODE_CALIBRATION) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:
			if (val > 1) {
				return VC_ERR_INVALID_VALUE;
			}
			return vc_dispatch(ctx, vc_cmd_cal_output(ch, val != 0),
					   timeout);
		case CH_CAL_DAC_CODE:
			return vc_dispatch(ctx, vc_cmd_cal_dac(ch, val), timeout);
		case CH_CAL_SAMPLE_CMD:
			if (val == CAL_COMMAND_NONE) {
				return VC_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return VC_ERR_INVALID_VALUE;
			}
			return vc_dispatch(ctx, vc_cmd_cal_sample(ch), timeout);
		case CH_CAL_COMMIT_CMD:
			if (val == CAL_COMMAND_NONE) {
				return VC_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return VC_ERR_INVALID_VALUE;
			}
			return vc_dispatch(ctx, vc_cmd_cal_commit(ch), timeout);
		case CH_CAL_MAX_RAW_DAC_LIMIT:
			return vc_dispatch(ctx, vc_cmd_cal_max_dac(ch, val),
					   timeout);
		default:
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}

	if (off >= ARRAY_SIZE(ch_reg_to_field)) {
		return VC_ERR_INVALID_VALUE;
	}

	return vc_dispatch(ctx, vc_cmd_ch_field(ch, ch_reg_to_field[off], val),
			   timeout);
}

/* ------------------------------------------------------------------ */
/* System status registers                                             */
/* ------------------------------------------------------------------ */

int sys_status_reg_read_input(uint16_t off, uint16_t *reg)
{
	struct sys_status_snapshot ss = sys_status_get();

	switch (off) {
	case SYS_BOARD_TEMPERATURE: *reg = (uint16_t)ss.board_temperature; return 0;
	case SYS_BOARD_HUMIDITY:    *reg = ss.board_humidity; return 0;
	case SYS_UPTIME_HI:        *reg = (uint16_t)(ss.uptime >> 16); return 0;
	case SYS_UPTIME_LO:        *reg = (uint16_t)(ss.uptime & 0xFFFF); return 0;
	case SYS_FW_VERSION_HI:    *reg = ss.fw_version_high; return 0;
	case SYS_FW_VERSION_LO:    *reg = ss.fw_version_low; return 0;
	default: return -1;
	}
}

/* ------------------------------------------------------------------ */
/* Extension block                                                     */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_write_ext(struct vc_ctx *ctx, uint16_t off,
				uint16_t val, k_timeout_t timeout)
{
	if (off == EXT_CAL_UNLOCK_ABS - EXT_BLOCK_BASE) {
		return vc_dispatch(ctx, vc_cmd_cal_unlock(val), timeout);
	}
	return VC_ERR_UNSUPPORTED_CAPABILITY;
}

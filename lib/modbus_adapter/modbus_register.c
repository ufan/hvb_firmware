/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_register.h"
#include "voltage_control/vc.h"
#include "reg_store/reg_store.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

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

enum vc_status vc_reg_read_sys_input(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	reg_store_read_input(SYS_BLOCK_BASE + off, reg);
	return VC_OK;
}

enum vc_status vc_reg_read_sys_holding(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	reg_store_read_holding(SYS_BLOCK_BASE + off, reg);
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
	enum vc_status st = vc_dispatch(ctx,
		vc_cmd_sys_field(sys_reg_to_field[off], val), timeout);
	if (st == VC_OK) {
		reg_store_write_holding(SYS_BLOCK_BASE + off, val);
	}
	return st;
}

/* ------------------------------------------------------------------ */
/* Channel register read/write                                         */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_read_ch_input(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	reg_store_read_input(SYS_BLOCK_BASE + SYS_SUPPORTED_CHANNELS, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	reg_store_read_input(CH_BLOCK_BASE(ch) + CH_CAPABILITY_FLAGS, &caps);
	if (!ch_input_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	reg_store_read_input(SYS_BLOCK_BASE + SYS_ACTIVE_OPERATING_MODE, &mode);
	if (is_ch_cal_input_reg(off) &&
	    (enum vc_operating_mode)mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	reg_store_read_input(CH_BLOCK_BASE(ch) + off, reg);
	return VC_OK;
}

enum vc_status vc_reg_read_ch_holding(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;

	reg_store_read_input(SYS_BLOCK_BASE + SYS_SUPPORTED_CHANNELS, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	reg_store_read_input(CH_BLOCK_BASE(ch) + CH_CAPABILITY_FLAGS, &caps);
	if (!ch_holding_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	reg_store_read_holding(CH_BLOCK_BASE(ch) + off, reg);
	return VC_OK;
}

enum vc_status vc_reg_write_ch_holding(struct vc_ctx *ctx, uint8_t ch,
					uint16_t off, uint16_t val,
					k_timeout_t timeout)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	reg_store_read_input(SYS_BLOCK_BASE + SYS_SUPPORTED_CHANNELS, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	reg_store_read_input(CH_BLOCK_BASE(ch) + CH_CAPABILITY_FLAGS, &caps);
	if (!ch_holding_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	switch (off) {
	case CH_OUTPUT_ACTION:
		return vc_dispatch(ctx,
			vc_cmd_output(ch, (enum vc_output_action)val), timeout);
	case CH_FAULT_CMD:
		return vc_dispatch(ctx,
			vc_cmd_fault(ch, (enum vc_channel_fault_command)val), timeout);
	case CH_PARAM_ACTION:
		return vc_dispatch(ctx,
			vc_cmd_ch_param(ch, (enum vc_param_action)val), timeout);
	default:
		break;
	}

	reg_store_read_input(SYS_BLOCK_BASE + SYS_ACTIVE_OPERATING_MODE, &mode);

	if (is_ch_calibration_coefficient_reg(off)) {
		enum vc_status st = vc_dispatch(ctx,
			vc_cmd_cal_set_field(ch, ch_reg_to_cal_field[off], val),
			timeout);
		if (st == VC_OK) {
			reg_store_write_holding(CH_BLOCK_BASE(ch) + off, val);
		}
		return st;
	}

	if (is_ch_cal_holding_reg(off)) {
		if ((enum vc_operating_mode)mode != VC_OPERATING_MODE_CALIBRATION) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
		enum vc_status st;
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:
			if (val > 1) {
				return VC_ERR_INVALID_VALUE;
			}
			st = vc_dispatch(ctx, vc_cmd_cal_output(ch, val != 0), timeout);
			if (st == VC_OK) {
				reg_store_write_holding(CH_BLOCK_BASE(ch) + off, val);
			}
			return st;
		case CH_CAL_DAC_CODE:
			st = vc_dispatch(ctx, vc_cmd_cal_dac(ch, val), timeout);
			if (st == VC_OK) {
				reg_store_write_holding(CH_BLOCK_BASE(ch) + off, val);
			}
			return st;
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
			st = vc_dispatch(ctx, vc_cmd_cal_max_dac(ch, val), timeout);
			if (st == VC_OK) {
				reg_store_write_holding(CH_BLOCK_BASE(ch) + off, val);
			}
			return st;
		default:
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}

	if (off >= ARRAY_SIZE(ch_reg_to_field)) {
		return VC_ERR_INVALID_VALUE;
	}

	enum vc_status st = vc_dispatch(ctx,
		vc_cmd_ch_field(ch, ch_reg_to_field[off], val), timeout);
	if (st == VC_OK) {
		reg_store_write_holding(CH_BLOCK_BASE(ch) + off, val);
	}
	return st;
}

/* ------------------------------------------------------------------ */
/* Extension block                                                     */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_write_ext(struct vc_ctx *ctx, uint16_t off,
				uint16_t val, k_timeout_t timeout)
{
	if (off == EXT_CAL_UNLOCK) {
		return vc_dispatch(ctx, vc_cmd_cal_unlock(val), timeout);
	}
	if (off == EXT_CAL_EXIT) {
		return vc_dispatch(ctx, vc_cmd_cal_exit(), timeout);
	}
	return VC_ERR_UNSUPPORTED_CAPABILITY;
}

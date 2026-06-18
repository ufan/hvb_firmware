/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/modbus_adapter.h"
#include "voltage_control/domain.h"
#include "regmap/hvb_regs.h"

#define EXT_BLOCK_END 279

struct vc_mb_adapter {
	struct domain *domain;
};

static bool addr_decode(uint16_t addr, bool *is_sys, uint8_t *ch, uint16_t *off)
{
	if (addr < SYS_BLOCK_BASE + CH_BLOCK_SIZE) {
		*is_sys = true;
		*off = addr;
		return true;
	}

	if (addr >= CH_BLOCK_BASE(0) && addr < CH_BLOCK_BASE(4)) {
		uint16_t rel = addr - CH_BLOCK_BASE(0);
		*is_sys = false;
		*ch = (uint8_t)(rel / CH_BLOCK_SIZE);
		*off = rel % CH_BLOCK_SIZE;
		return true;
	}

	return false;
}

static bool is_extension(uint16_t addr)
{
	return addr >= EXT_BLOCK_BASE && addr <= EXT_BLOCK_END;
}

static bool is_calibration_mode(struct domain *d)
{
	return domain_get_operating_mode(d) == VC_OPERATING_MODE_CALIBRATION;
}

static bool is_ch_cal_input_reg(uint16_t off)
{
	return off >= CH_RAW_ADC_VOLTAGE_HI && off <= CH_RAW_DAC_READBACK;
}

static bool is_ch_cal_holding_reg(uint16_t off)
{
	return off >= CH_CAL_OUTPUT_ENABLE && off <= CH_CAL_MAX_RAW_DAC_LIMIT;
}

static bool is_ch_calibration_coefficient_reg(uint16_t off)
{
	return off >= CH_OUTPUT_CAL_K && off <= CH_MEASURED_I_CAL_B;
}

static bool caps_any(uint16_t caps, uint16_t mask)
{
	return (caps & mask) != 0;
}

static bool caps_all(uint16_t caps, uint16_t mask)
{
	return (caps & mask) == mask;
}

static enum vc_mb_result domain_st_to_mb_result(enum vc_status st)
{
	switch (st) {
	case VC_OK:
		return VC_MB_OK;
	case VC_ERR_UNSUPPORTED_CHANNEL:
	case VC_ERR_UNSUPPORTED_CAPABILITY:
		return VC_MB_ILLEGAL_ADDRESS;
	case VC_ERR_INVALID_VALUE:
	case VC_ERR_INVALID_COMMAND:
	case VC_ERR_UNSAFE_STATE:
		return VC_MB_ILLEGAL_VALUE;
	case VC_ERR_STORAGE:
	default:
		return VC_MB_DEVICE_FAILURE;
	}
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
	case CH_CAL_SAMPLE_STATUS:
		return caps_any(caps, CH_CAP_VOLTAGE_MEASUREMENT |
					     CH_CAP_CURRENT_MEASUREMENT);
	case CH_RAW_DAC_READBACK:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
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
	case CH_SAVE_TARGET_POLICY:
	case CH_OUTPUT_CAL_K:
	case CH_OUTPUT_CAL_B:
	case CH_CAL_OUTPUT_ENABLE:
	case CH_RAW_DAC_CODE:
	case CH_CAL_MAX_RAW_DAC_LIMIT:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
	case CH_VOLTAGE_PROTECTION_MODE:
	case CH_VOLTAGE_PROT_OUT_ACTION:
	case CH_VOLTAGE_LIMIT_THRESHOLD:
	case CH_MEASURED_V_CAL_K:
	case CH_MEASURED_V_CAL_B:
		return caps_all(caps, CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_CURRENT_PROTECTION_MODE:
	case CH_CURRENT_PROT_OUT_ACTION:
	case CH_CURRENT_LIMIT_THRESHOLD:
	case CH_MEASURED_I_CAL_K:
	case CH_MEASURED_I_CAL_B:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	case CH_AUTO_DERATE_STEP:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE |
					     CH_CAP_VOLTAGE_MEASUREMENT);
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

static uint16_t int32_hi(int32_t value)
{
	return (uint16_t)((uint32_t)value >> 16);
}

static uint16_t int32_lo(int32_t value)
{
	return (uint16_t)((uint32_t)value & 0xFFFF);
}

static enum vc_mb_result read_sys_input(struct domain *d, uint16_t off, uint16_t *reg)
{
	struct vc_system_snapshot snap;

	domain_get_system_snapshot(d, &snap);

	switch (off) {
	case SYS_PROTOCOL_MAJOR:        *reg = snap.protocol_major; break;
	case SYS_PROTOCOL_MINOR:        *reg = snap.protocol_minor; break;
	case SYS_VARIANT_ID:            *reg = snap.variant_id; break;
	case SYS_CAPABILITY_FLAGS:      *reg = snap.system_capability_flags; break;
	case SYS_SUPPORTED_CHANNELS:    *reg = snap.supported_channel_count; break;
	case SYS_ACTIVE_CHANNEL_MASK:   *reg = snap.active_channel_mask; break;
	case SYS_BOARD_TEMPERATURE:     *reg = (uint16_t)snap.board_temperature; break;
	case SYS_BOARD_HUMIDITY:        *reg = snap.board_humidity; break;
	case SYS_UPTIME_HI:             *reg = (uint16_t)(snap.uptime >> 16); break;
	case SYS_UPTIME_LO:             *reg = (uint16_t)(snap.uptime & 0xFFFF); break;
	case SYS_FW_VERSION_HI:         *reg = snap.fw_version_high; break;
	case SYS_FW_VERSION_LO:         *reg = snap.fw_version_low; break;
	case SYS_ACTIVE_OPERATING_MODE: *reg = (uint16_t)snap.active_operating_mode; break;
	case SYS_STATUS:                *reg = snap.system_status; break;
	case SYS_FAULT_CAUSE:           *reg = snap.system_fault_cause; break;
	default:
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return VC_MB_ILLEGAL_ADDRESS; }
		break;
	}
	return VC_MB_OK;
}

static enum vc_mb_result read_ch_input(struct domain *d, uint8_t ch, uint16_t off,
			 uint16_t *reg)
{
	struct vc_channel_snapshot snap;

	if (!domain_is_channel_supported(d, ch)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	if (is_ch_cal_input_reg(off) && !is_calibration_mode(d)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	domain_get_channel_snapshot(d, ch, &snap);
	if (!ch_input_supported(snap.channel_capability_flags, off)) {
		return VC_MB_ILLEGAL_ADDRESS;
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
	case CH_CAL_SAMPLE_STATUS:          *reg = (uint16_t)snap.cal_sample_status; break;
	case CH_RAW_DAC_READBACK:           *reg = snap.raw_dac_readback; break;
	default:
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return VC_MB_ILLEGAL_ADDRESS; }
		break;
	}
	return VC_MB_OK;
}

static enum vc_mb_result read_sys_holding(struct domain *d, uint16_t off, uint16_t *reg)
{
	struct vc_system_config cfg;

	domain_get_system_config(d, &cfg);

	switch (off) {
	case SYS_OPERATING_MODE:         *reg = cfg.operating_mode; break;
	case SYS_SLAVE_ADDRESS:          *reg = cfg.slave_address; break;
	case SYS_BAUD_RATE_CODE:         *reg = cfg.baud_rate_code; break;
	case SYS_RECOVERY_POLICY_MODE:   *reg = cfg.recovery_policy_mode; break;
	case SYS_AUTO_RETRY_DELAY:       *reg = cfg.auto_retry_delay; break;
	case SYS_AUTO_RETRY_MAX_COUNT:   *reg = cfg.auto_retry_max_count; break;
	case SYS_AUTO_RETRY_WINDOW:      *reg = cfg.auto_retry_window; break;
	case SYS_VOLTAGE_SAFE_BAND_PCT:  *reg = cfg.voltage_safe_band_pct; break;
	case SYS_CURRENT_SAFE_BAND_PCT:  *reg = cfg.current_safe_band_pct; break;
	case SYS_PARAM_ACTION:           *reg = 0; break;
	default:
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return VC_MB_ILLEGAL_ADDRESS; }
		break;
	}
	return VC_MB_OK;
}

static enum vc_mb_result read_ch_holding(struct domain *d, uint8_t ch, uint16_t off,
			   uint16_t *reg)
{
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	if (!domain_is_channel_supported(d, ch)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	domain_get_channel_snapshot(d, ch, &snap);
	if (!ch_holding_supported(snap.channel_capability_flags, off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	if (is_ch_cal_holding_reg(off)) {
		if (!is_calibration_mode(d)) {
			return VC_MB_ILLEGAL_ADDRESS;
		}
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:     *reg = snap.cal_output_enabled; break;
		case CH_RAW_DAC_CODE:          *reg = snap.raw_dac_readback; break;
		case CH_CAL_SAMPLE_CMD:        *reg = 0; break;
		case CH_CAL_COMMIT_CMD:        *reg = 0; break;
		case CH_CAL_MAX_RAW_DAC_LIMIT: *reg = snap.cal_max_raw_dac_limit; break;
		default:
			return VC_MB_ILLEGAL_ADDRESS;
		}
		return VC_MB_OK;
	}

	domain_get_channel_config(d, ch, &cfg);

	switch (off) {
	case CH_CFG_TARGET_VOLTAGE:        *reg = (uint16_t)cfg.configured_target_voltage; break;
	case CH_OUTPUT_ACTION:             *reg = 0; break;
	case CH_FAULT_CMD:                 *reg = 0; break;
	case CH_RAMP_UP_STEP:              *reg = cfg.ramp_up_step; break;
	case CH_RAMP_UP_INTERVAL:          *reg = cfg.ramp_up_interval; break;
	case CH_RAMP_DOWN_STEP:            *reg = cfg.ramp_down_step; break;
	case CH_RAMP_DOWN_INTERVAL:        *reg = cfg.ramp_down_interval; break;
	case CH_VOLTAGE_PROTECTION_MODE:   *reg = cfg.voltage_protection_mode; break;
	case CH_VOLTAGE_PROT_OUT_ACTION:   *reg = cfg.voltage_protection_output_action; break;
	case CH_VOLTAGE_LIMIT_THRESHOLD:   *reg = (uint16_t)cfg.voltage_limit_threshold; break;
	case CH_CURRENT_PROTECTION_MODE:   *reg = cfg.current_protection_mode; break;
	case CH_CURRENT_PROT_OUT_ACTION:   *reg = cfg.current_protection_output_action; break;
	case CH_CURRENT_LIMIT_THRESHOLD:   *reg = (uint16_t)cfg.current_limit_threshold; break;
	case CH_AUTO_DERATE_STEP:          *reg = cfg.auto_derate_step; break;
	case CH_SAVE_TARGET_POLICY:        *reg = cfg.save_target_policy; break;
	case CH_OUTPUT_CAL_K:             *reg = cfg.output_calib_k; break;
	case CH_OUTPUT_CAL_B:             *reg = (uint16_t)cfg.output_calib_b; break;
	case CH_MEASURED_V_CAL_K:         *reg = cfg.measured_voltage_calib_k; break;
	case CH_MEASURED_V_CAL_B:         *reg = (uint16_t)cfg.measured_voltage_calib_b; break;
	case CH_MEASURED_I_CAL_K:         *reg = cfg.measured_current_calib_k; break;
	case CH_MEASURED_I_CAL_B:         *reg = (uint16_t)cfg.measured_current_calib_b; break;
	case CH_PARAM_ACTION:              *reg = 0; break;
	default:
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return VC_MB_ILLEGAL_ADDRESS; }
		break;
	}
	return VC_MB_OK;
}

static enum vc_mb_result write_sys_holding(struct domain *d, uint16_t off, uint16_t val)
{
	struct vc_system_config cfg;

	domain_get_system_config(d, &cfg);

	switch (off) {
	case SYS_OPERATING_MODE:
		if (val > VC_OPERATING_MODE_CALIBRATION) return VC_MB_ILLEGAL_VALUE;
		cfg.operating_mode = (enum vc_operating_mode)val;
		break;
	case SYS_SLAVE_ADDRESS:
		if (val > 247) return VC_MB_ILLEGAL_VALUE;
		cfg.slave_address = val;
		break;
	case SYS_BAUD_RATE_CODE:
		if (val > 1) return VC_MB_ILLEGAL_VALUE;
		cfg.baud_rate_code = (enum vc_baud_rate_code)val;
		break;
	case SYS_RECOVERY_POLICY_MODE:
		if (val > 3) return VC_MB_ILLEGAL_VALUE;
		cfg.recovery_policy_mode = (enum vc_recovery_policy_mode)val;
		break;
	case SYS_AUTO_RETRY_DELAY:       cfg.auto_retry_delay = val; break;
	case SYS_AUTO_RETRY_MAX_COUNT:   cfg.auto_retry_max_count = val; break;
	case SYS_AUTO_RETRY_WINDOW:      cfg.auto_retry_window = val; break;
	case SYS_VOLTAGE_SAFE_BAND_PCT:
		if (val > 50) return VC_MB_ILLEGAL_VALUE;
		cfg.voltage_safe_band_pct = val;
		break;
	case SYS_CURRENT_SAFE_BAND_PCT:
		if (val > 50) return VC_MB_ILLEGAL_VALUE;
		cfg.current_safe_band_pct = val;
		break;
	default:
		return VC_MB_ILLEGAL_ADDRESS;
	}

	return domain_st_to_mb_result(domain_set_system_config(d, &cfg));
}

static enum vc_mb_result write_sys_param_action(struct domain *d, uint16_t val)
{
	if (val > 3 && val != 255) return VC_MB_ILLEGAL_ADDRESS;
	return domain_st_to_mb_result(domain_system_param_action(d, (enum vc_param_action)val));
}

static enum vc_mb_result write_ch_holding(struct domain *d, uint8_t ch, uint16_t off,
			    uint16_t val)
{
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	if (!domain_is_channel_supported(d, ch)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	domain_get_channel_snapshot(d, ch, &snap);
	if (!ch_holding_supported(snap.channel_capability_flags, off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	if (is_ch_calibration_coefficient_reg(off) && !is_calibration_mode(d)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}
	if (is_ch_cal_holding_reg(off)) {
		if (!is_calibration_mode(d)) {
			return VC_MB_ILLEGAL_ADDRESS;
		}
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:
			if (val > 1) return VC_MB_ILLEGAL_VALUE;
			return domain_st_to_mb_result(
				domain_calibration_set_output_enable(d, ch, val != 0));
		case CH_RAW_DAC_CODE:
			return domain_st_to_mb_result(
				domain_calibration_set_raw_dac(d, ch, val));
		case CH_CAL_SAMPLE_CMD:
			if (val == CAL_COMMAND_NONE) return VC_MB_OK;
			if (val != CAL_COMMAND_EXECUTE) return VC_MB_ILLEGAL_ADDRESS;
			return domain_st_to_mb_result(domain_calibration_sample(d, ch));
		case CH_CAL_COMMIT_CMD:
			if (val == CAL_COMMAND_NONE) return VC_MB_OK;
			if (val != CAL_COMMAND_EXECUTE) return VC_MB_ILLEGAL_ADDRESS;
			return domain_st_to_mb_result(domain_calibration_commit(d, ch));
		case CH_CAL_MAX_RAW_DAC_LIMIT:
			return domain_st_to_mb_result(
				domain_calibration_set_max_raw_dac(d, ch, val));
		default:
			return VC_MB_ILLEGAL_ADDRESS;
		}
	}

	domain_get_channel_config(d, ch, &cfg);

	switch (off) {
	case CH_CFG_TARGET_VOLTAGE:      cfg.configured_target_voltage = (int16_t)val; break;
	case CH_RAMP_UP_STEP:            cfg.ramp_up_step = val; break;
	case CH_RAMP_UP_INTERVAL:        cfg.ramp_up_interval = val; break;
	case CH_RAMP_DOWN_STEP:          cfg.ramp_down_step = val; break;
	case CH_RAMP_DOWN_INTERVAL:      cfg.ramp_down_interval = val; break;
	case CH_VOLTAGE_PROTECTION_MODE:
		if (val > 2) return VC_MB_ILLEGAL_VALUE;
		cfg.voltage_protection_mode = (enum vc_protection_mode)val;
		break;
	case CH_VOLTAGE_PROT_OUT_ACTION:
		if (val > 5) return VC_MB_ILLEGAL_ADDRESS;
		if (!(val == 0 || val == 2 || val == 3 || val == 4 || val == 5))
			return VC_MB_ILLEGAL_ADDRESS;
		cfg.voltage_protection_output_action = (enum vc_output_action)val;
		break;
	case CH_VOLTAGE_LIMIT_THRESHOLD: cfg.voltage_limit_threshold = (int16_t)val; break;
	case CH_CURRENT_PROTECTION_MODE:
		if (val > 2) return VC_MB_ILLEGAL_VALUE;
		cfg.current_protection_mode = (enum vc_protection_mode)val;
		break;
	case CH_CURRENT_PROT_OUT_ACTION:
		if (val > 5) return VC_MB_ILLEGAL_ADDRESS;
		if (!(val == 0 || val == 2 || val == 3 || val == 4))
			return VC_MB_ILLEGAL_ADDRESS;
		cfg.current_protection_output_action = (enum vc_output_action)val;
		break;
	case CH_CURRENT_LIMIT_THRESHOLD: cfg.current_limit_threshold = (int16_t)val; break;
	case CH_AUTO_DERATE_STEP:        cfg.auto_derate_step = val; break;
	case CH_SAVE_TARGET_POLICY:
		if (val > 1) return VC_MB_ILLEGAL_VALUE;
		cfg.save_target_policy = val;
		break;
	case CH_OUTPUT_CAL_K:            cfg.output_calib_k = val; break;
	case CH_OUTPUT_CAL_B:            cfg.output_calib_b = (int16_t)val; break;
	case CH_MEASURED_V_CAL_K:        cfg.measured_voltage_calib_k = val; break;
	case CH_MEASURED_V_CAL_B:        cfg.measured_voltage_calib_b = (int16_t)val; break;
	case CH_MEASURED_I_CAL_K:        cfg.measured_current_calib_k = val; break;
	case CH_MEASURED_I_CAL_B:        cfg.measured_current_calib_b = (int16_t)val; break;
	default:
		return VC_MB_ILLEGAL_ADDRESS;
	}

	return domain_st_to_mb_result(domain_set_channel_config(d, ch, &cfg));
}

static enum vc_mb_result write_ch_output_action(struct domain *d, uint8_t ch,
				  uint16_t val)
{
	if (val > 3) return VC_MB_ILLEGAL_VALUE;
	return domain_st_to_mb_result(
		domain_channel_output_action(d, ch, (enum vc_output_action)val));
}

static enum vc_mb_result write_ch_fault_cmd(struct domain *d, uint8_t ch, uint16_t val)
{
	if (val > 2) return VC_MB_ILLEGAL_VALUE;
	return domain_st_to_mb_result(
		domain_channel_fault_command(d, ch,
			(enum vc_channel_fault_command)val));
}

static enum vc_mb_result write_ch_param_action(struct domain *d, uint8_t ch, uint16_t val)
{
	if (val > 3 && val != 255) return VC_MB_ILLEGAL_ADDRESS;
	return domain_st_to_mb_result(
		domain_channel_param_action(d, ch,
			(enum vc_param_action)val));
}

enum vc_mb_result vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		return read_sys_input(a->domain, off, reg);
	}

	return read_ch_input(a->domain, ch, off, reg);
}

enum vc_mb_result vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		*reg = 0;
		return VC_MB_OK;
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		return read_sys_holding(a->domain, off, reg);
	}

	return read_ch_holding(a->domain, ch, off, reg);
}

enum vc_mb_result vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr, uint16_t val)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		if (addr == EXT_CAL_UNLOCK_ABS) {
			return domain_st_to_mb_result(
				domain_calibration_unlock(a->domain, val));
		}
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		if (off == SYS_PARAM_ACTION) {
			return write_sys_param_action(a->domain, val);
		}
		return write_sys_holding(a->domain, off, val);
	}

	if (!domain_is_channel_supported(a->domain, ch)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	switch (off) {
	case CH_OUTPUT_ACTION:
		return write_ch_output_action(a->domain, ch, val);
	case CH_FAULT_CMD:
		return write_ch_fault_cmd(a->domain, ch, val);
	case CH_PARAM_ACTION:
		return write_ch_param_action(a->domain, ch, val);
	default:
		break;
	}

	return write_ch_holding(a->domain, ch, off, val);
}

struct vc_mb_adapter *vc_mb_adapter_create(struct domain *domain)
{
	static struct vc_mb_adapter adapter;
	adapter.domain = domain;
	return &adapter;
}

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/domain.h"
#include "regmap/hvb_regs.h"

#define EXT_BLOCK_END 279

struct vc_mb_adapter {
	struct vc_domain *domain;
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

static int read_sys_input(struct vc_domain *d, uint16_t off, uint16_t *reg)
{
	struct vc_system_snapshot snap;

	vc_domain_get_system_snapshot(d, &snap);

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
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return -1; }
		break;
	}
	return 0;
}

static int read_ch_input(struct vc_domain *d, uint8_t ch, uint16_t off,
			 uint16_t *reg)
{
	struct vc_channel_snapshot snap;

	if (!vc_domain_is_channel_supported(d, ch)) {
		return -1;
	}

	vc_domain_get_channel_snapshot(d, ch, &snap);

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
	default:
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return -1; }
		break;
	}
	return 0;
}

static int read_sys_holding(struct vc_domain *d, uint16_t off, uint16_t *reg)
{
	struct vc_system_config cfg;

	vc_domain_get_system_config(d, &cfg);

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
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return -1; }
		break;
	}
	return 0;
}

static int read_ch_holding(struct vc_domain *d, uint8_t ch, uint16_t off,
			   uint16_t *reg)
{
	struct vc_channel_config cfg;

	if (!vc_domain_is_channel_supported(d, ch)) {
		return -1;
	}

	vc_domain_get_channel_config(d, ch, &cfg);

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
		if (off < CH_BLOCK_SIZE) { *reg = 0; } else { return -1; }
		break;
	}
	return 0;
}

static int write_sys_holding(struct vc_domain *d, uint16_t off, uint16_t val)
{
	struct vc_system_config cfg;

	vc_domain_get_system_config(d, &cfg);

	switch (off) {
	case SYS_OPERATING_MODE:
		if (val > 1) return -1;
		cfg.operating_mode = (enum vc_operating_mode)val;
		break;
	case SYS_SLAVE_ADDRESS:
		if (val > 247) return -1;
		cfg.slave_address = val;
		break;
	case SYS_BAUD_RATE_CODE:
		if (val > 1) return -1;
		cfg.baud_rate_code = (enum vc_baud_rate_code)val;
		break;
	case SYS_RECOVERY_POLICY_MODE:
		if (val > 3) return -1;
		cfg.recovery_policy_mode = (enum vc_recovery_policy_mode)val;
		break;
	case SYS_AUTO_RETRY_DELAY:       cfg.auto_retry_delay = val; break;
	case SYS_AUTO_RETRY_MAX_COUNT:   cfg.auto_retry_max_count = val; break;
	case SYS_AUTO_RETRY_WINDOW:      cfg.auto_retry_window = val; break;
	case SYS_VOLTAGE_SAFE_BAND_PCT:
		if (val > 50) return -1;
		cfg.voltage_safe_band_pct = val;
		break;
	case SYS_CURRENT_SAFE_BAND_PCT:
		if (val > 50) return -1;
		cfg.current_safe_band_pct = val;
		break;
	default:
		return -1;
	}

	return vc_domain_set_system_config(d, &cfg) == VC_OK ? 0 : -1;
}

static int write_sys_param_action(struct vc_domain *d, uint16_t val)
{
	if (val > 3 && val != 255) return -1;
	return vc_domain_system_param_action(d, (enum vc_param_action)val) == VC_OK ? 0 : -1;
}

static int write_ch_holding(struct vc_domain *d, uint8_t ch, uint16_t off,
			    uint16_t val)
{
	struct vc_channel_config cfg;

	if (!vc_domain_is_channel_supported(d, ch)) {
		return -1;
	}

	vc_domain_get_channel_config(d, ch, &cfg);

	switch (off) {
	case CH_CFG_TARGET_VOLTAGE:      cfg.configured_target_voltage = (int16_t)val; break;
	case CH_RAMP_UP_STEP:            cfg.ramp_up_step = val; break;
	case CH_RAMP_UP_INTERVAL:        cfg.ramp_up_interval = val; break;
	case CH_RAMP_DOWN_STEP:          cfg.ramp_down_step = val; break;
	case CH_RAMP_DOWN_INTERVAL:      cfg.ramp_down_interval = val; break;
	case CH_VOLTAGE_PROTECTION_MODE:
		if (val > 2) return -1;
		cfg.voltage_protection_mode = (enum vc_protection_mode)val;
		break;
	case CH_VOLTAGE_PROT_OUT_ACTION:
		if (val > 5) return -1;
		if (!(val == 0 || val == 2 || val == 3 || val == 4 || val == 5))
			return -1;
		cfg.voltage_protection_output_action = (enum vc_output_action)val;
		break;
	case CH_VOLTAGE_LIMIT_THRESHOLD: cfg.voltage_limit_threshold = (int16_t)val; break;
	case CH_CURRENT_PROTECTION_MODE:
		if (val > 2) return -1;
		cfg.current_protection_mode = (enum vc_protection_mode)val;
		break;
	case CH_CURRENT_PROT_OUT_ACTION:
		if (val > 5) return -1;
		if (!(val == 0 || val == 2 || val == 3 || val == 4))
			return -1;
		cfg.current_protection_output_action = (enum vc_output_action)val;
		break;
	case CH_CURRENT_LIMIT_THRESHOLD: cfg.current_limit_threshold = (int16_t)val; break;
	case CH_AUTO_DERATE_STEP:        cfg.auto_derate_step = val; break;
	case CH_SAVE_TARGET_POLICY:
		if (val > 1) return -1;
		cfg.save_target_policy = val;
		break;
	case CH_OUTPUT_CAL_K:            cfg.output_calib_k = val; break;
	case CH_OUTPUT_CAL_B:            cfg.output_calib_b = (int16_t)val; break;
	case CH_MEASURED_V_CAL_K:        cfg.measured_voltage_calib_k = val; break;
	case CH_MEASURED_V_CAL_B:        cfg.measured_voltage_calib_b = (int16_t)val; break;
	case CH_MEASURED_I_CAL_K:        cfg.measured_current_calib_k = val; break;
	case CH_MEASURED_I_CAL_B:        cfg.measured_current_calib_b = (int16_t)val; break;
	default:
		return -1;
	}

	return vc_domain_set_channel_config(d, ch, &cfg) == VC_OK ? 0 : -1;
}

static int write_ch_output_action(struct vc_domain *d, uint8_t ch,
				  uint16_t val)
{
	if (val > 3) return -1;
	return vc_domain_channel_output_action(d, ch, (enum vc_output_action)val)
		== VC_OK ? 0 : -1;
}

static int write_ch_fault_cmd(struct vc_domain *d, uint8_t ch, uint16_t val)
{
	if (val > 2) return -1;
	return vc_domain_channel_fault_command(d, ch,
		(enum vc_channel_fault_command)val) == VC_OK ? 0 : -1;
}

static int write_ch_param_action(struct vc_domain *d, uint8_t ch, uint16_t val)
{
	if (val > 3 && val != 255) return -1;
	return vc_domain_channel_param_action(d, ch,
		(enum vc_param_action)val) == VC_OK ? 0 : -1;
}

int vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return -1;
	}

	if (is_sys) {
		return read_sys_input(a->domain, off, reg);
	}

	return read_ch_input(a->domain, ch, off, reg);
}

int vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		*reg = 0;
		return 0;
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return -1;
	}

	if (is_sys) {
		return read_sys_holding(a->domain, off, reg);
	}

	return read_ch_holding(a->domain, ch, off, reg);
}

int vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr, uint16_t val)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		return -1;
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return -1;
	}

	if (is_sys) {
		if (off == SYS_PARAM_ACTION) {
			return write_sys_param_action(a->domain, val);
		}
		return write_sys_holding(a->domain, off, val);
	}

	if (!vc_domain_is_channel_supported(a->domain, ch)) {
		return -1;
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

struct vc_mb_adapter *vc_mb_adapter_create(struct vc_domain *domain)
{
	static struct vc_mb_adapter adapter;
	adapter.domain = domain;
	return &adapter;
}

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_register.h"
#include "voltage_control/vc.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"

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
/* Generated wire views                                                 */
/* ------------------------------------------------------------------ */

struct wire_reg {
	reg_id_t id;
	uint8_t word;
};

static struct {
	reg_id_t id;
	uint32_t value;
	bool valid;
} word_latch;

#define SYS_INPUT_16(name, field, offset) \
	[offset] = { REG_SYS_ID(REG_SYS_FIELD_##name), 0 },
#define SYS_INPUT_32(name, field, offset) \
	[offset] = { REG_SYS_ID(REG_SYS_FIELD_##name), 0 }, \
	[(offset) + 1] = { REG_SYS_ID(REG_SYS_FIELD_##name), 1 },
#define SYS_HOLDING_16(name, field, offset)
#define SYS_HOLDING_32(name, field, offset)
#define SYS_REG16(name, field, type, access, category, bank, offset) \
	SYS_##bank##_16(name, field, offset)
#define SYS_REG32(name, field, type, access, category, bank, offset) \
	SYS_##bank##_32(name, field, offset)
static const struct wire_reg sys_input_view[CH_BLOCK_SIZE] = {
#include "reg_store/system_regs.def"
};
#undef SYS_REG16
#undef SYS_REG32
#undef SYS_INPUT_16
#undef SYS_INPUT_32
#undef SYS_HOLDING_16
#undef SYS_HOLDING_32

#define SYS_INPUT_16(name, field, offset)
#define SYS_INPUT_32(name, field, offset)
#define SYS_HOLDING_16(name, field, offset) \
	[offset] = { REG_SYS_ID(REG_SYS_FIELD_##name), 0 },
#define SYS_HOLDING_32(name, field, offset) \
	[offset] = { REG_SYS_ID(REG_SYS_FIELD_##name), 0 }, \
	[(offset) + 1] = { REG_SYS_ID(REG_SYS_FIELD_##name), 1 },
#define SYS_REG16(name, field, type, access, category, bank, offset) \
	SYS_##bank##_16(name, field, offset)
#define SYS_REG32(name, field, type, access, category, bank, offset) \
	SYS_##bank##_32(name, field, offset)
static const struct wire_reg sys_holding_view[CH_BLOCK_SIZE] = {
#include "reg_store/system_regs.def"
};
#undef SYS_REG16
#undef SYS_REG32
#undef SYS_INPUT_16
#undef SYS_INPUT_32
#undef SYS_HOLDING_16
#undef SYS_HOLDING_32

#define VC_INPUT_16(name, field, offset) \
	[offset] = { REG_VC_FIELD_##name, 0 },
#define VC_INPUT_32(name, field, offset) \
	[offset] = { REG_VC_FIELD_##name, 0 }, \
	[(offset) + 1] = { REG_VC_FIELD_##name, 1 },
#define VC_HOLDING_16(name, field, offset)
#define VC_HOLDING_32(name, field, offset)
#define VC_REG16(name, field, type, access, category, bank, offset) \
	VC_##bank##_16(name, field, offset)
#define VC_REG32(name, field, type, access, category, bank, offset) \
	VC_##bank##_32(name, field, offset)
static const struct wire_reg ch_input_view[CH_BLOCK_SIZE] = {
#include "reg_store/vc_regs.def"
};
#undef VC_REG16
#undef VC_REG32
#undef VC_INPUT_16
#undef VC_INPUT_32
#undef VC_HOLDING_16
#undef VC_HOLDING_32

#define VC_INPUT_16(name, field, offset)
#define VC_INPUT_32(name, field, offset)
#define VC_HOLDING_16(name, field, offset) \
	[offset] = { REG_VC_FIELD_##name, 0 },
#define VC_HOLDING_32(name, field, offset) \
	[offset] = { REG_VC_FIELD_##name, 0 }, \
	[(offset) + 1] = { REG_VC_FIELD_##name, 1 },
#define VC_REG16(name, field, type, access, category, bank, offset) \
	VC_##bank##_16(name, field, offset)
#define VC_REG32(name, field, type, access, category, bank, offset) \
	VC_##bank##_32(name, field, offset)
static const struct wire_reg ch_holding_view[CH_BLOCK_SIZE] = {
#include "reg_store/vc_regs.def"
};
#undef VC_REG16
#undef VC_REG32
#undef VC_INPUT_16
#undef VC_INPUT_32
#undef VC_HOLDING_16
#undef VC_HOLDING_32

static enum vc_status catalog_status(enum reg_status status)
{
	switch (status) {
	case REG_OK: return VC_OK;
	case REG_NOT_FOUND:
	case REG_UNSUPPORTED: return VC_ERR_UNSUPPORTED_CAPABILITY;
	case REG_INVALID_VALUE: return VC_ERR_INVALID_VALUE;
	case REG_IO_ERROR: return VC_ERR_STORAGE;
	case REG_READ_ONLY:
	case REG_WRITE_ONLY: return VC_ERR_INVALID_COMMAND;
	default: return VC_ERR_UNSAFE_STATE;
	}
}

static enum vc_status read_wire(const struct wire_reg *wire, uint8_t ch,
				bool channel, uint16_t *reg)
{
	const struct reg_descriptor *desc;
	union reg_value value = {};
	reg_id_t id;
	enum reg_status status;
	uint32_t raw;

	if (wire->id == 0U) {
		*reg = 0U;
		return VC_OK;
	}
	id = channel ? REG_VC_ID(ch, wire->id) : wire->id;
	desc = reg_describe(id);
	if (desc == NULL) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if ((desc->type == REG_S32 || desc->type == REG_U32) &&
	    wire->word == 1U && word_latch.valid && word_latch.id == id) {
		*reg = (uint16_t)word_latch.value;
		word_latch.valid = false;
		return VC_OK;
	}
	status = reg_read_descriptor(desc, &value);
	if (status != REG_OK) {
		return catalog_status(status);
	}

	switch (desc->type) {
	case REG_S16:
		word_latch.valid = false;
		*reg = (uint16_t)value.s16;
		return VC_OK;
	case REG_U16:
	case REG_ENUM:
		word_latch.valid = false;
		*reg = value.u16;
		return VC_OK;
	case REG_BOOL:
		word_latch.valid = false;
		*reg = value.boolean ? 1U : 0U;
		return VC_OK;
	case REG_S32: raw = (uint32_t)value.s32; break;
	case REG_U32: raw = value.u32; break;
	default: return VC_ERR_INVALID_VALUE;
	}
	if (wire->word == 0U) {
		word_latch.id = id;
		word_latch.value = raw;
		word_latch.valid = true;
		*reg = (uint16_t)(raw >> 16);
	} else {
		word_latch.valid = false;
		*reg = (uint16_t)raw;
	}
	return VC_OK;
}

static enum vc_status write_wire(const struct wire_reg *wire, uint8_t ch,
				 bool channel, uint16_t reg,
				 k_timeout_t timeout)
{
	const struct reg_descriptor *desc;
	union reg_value value = {};
	reg_id_t id;

	if (wire->id == 0U) {
		return VC_ERR_INVALID_VALUE;
	}
	id = channel ? REG_VC_ID(ch, wire->id) : wire->id;
	desc = reg_describe(id);
	if (desc == NULL) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if (desc->type == REG_S16) {
		value.s16 = (int16_t)reg;
	} else {
		value.u16 = reg;
	}
	return catalog_status(reg_write_descriptor(desc, value, timeout));
}

/* ------------------------------------------------------------------ */
/* System register read/write                                          */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_read_sys_input(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	return read_wire(&sys_input_view[off], 0, false, reg);
}

enum vc_status vc_reg_read_sys_holding(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	return read_wire(&sys_holding_view[off], 0, false, reg);
}

enum vc_status vc_reg_write_sys_holding(struct vc_ctx *ctx, uint16_t off,
					uint16_t val, k_timeout_t timeout)
{
	ARG_UNUSED(ctx);
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	return write_wire(&sys_holding_view[off], 0, false, val, timeout);
}

/* ------------------------------------------------------------------ */
/* Channel register read/write                                         */
/* ------------------------------------------------------------------ */

enum vc_status vc_reg_read_ch_input(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_input_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	(void)read_wire(&sys_input_view[SYS_ACTIVE_OPERATING_MODE], 0, false, &mode);
	if (is_ch_cal_input_reg(off) &&
	    (enum vc_operating_mode)mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	return read_wire(&ch_input_view[off], ch, true, reg);
}

enum vc_status vc_reg_read_ch_holding(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;

	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (off >= CH_BLOCK_SIZE) {
		return VC_ERR_INVALID_VALUE;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_holding_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	return read_wire(&ch_holding_view[off], ch, true, reg);
}

enum vc_status vc_reg_write_ch_holding(struct vc_ctx *ctx, uint8_t ch,
					uint16_t off, uint16_t val,
					k_timeout_t timeout)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	ARG_UNUSED(ctx);
	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_holding_supported(caps, off)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	switch (off) {
	case CH_OUTPUT_ACTION:
		return write_wire(&ch_holding_view[off], ch, true, val, timeout);
	case CH_FAULT_CMD:
		return write_wire(&ch_holding_view[off], ch, true, val, timeout);
	case CH_PARAM_ACTION:
		return write_wire(&ch_holding_view[off], ch, true, val, timeout);
	default:
		break;
	}

	(void)read_wire(&sys_input_view[SYS_ACTIVE_OPERATING_MODE], 0, false, &mode);

	if (is_ch_calibration_coefficient_reg(off)) {
		return write_wire(&ch_holding_view[off], ch, true, val, timeout);
	}

	if (is_ch_cal_holding_reg(off)) {
		if ((enum vc_operating_mode)mode != VC_OPERATING_MODE_CALIBRATION) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:
			if (val > 1) {
				return VC_ERR_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_DAC_CODE:
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_SAMPLE_CMD:
			if (val == CAL_COMMAND_NONE) {
				return VC_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return VC_ERR_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_COMMIT_CMD:
			if (val == CAL_COMMAND_NONE) {
				return VC_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return VC_ERR_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_MAX_RAW_DAC_LIMIT:
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		default:
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}

	if (off >= ARRAY_SIZE(ch_holding_view)) {
		return VC_ERR_INVALID_VALUE;
	}
	return write_wire(&ch_holding_view[off], ch, true, val, timeout);
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

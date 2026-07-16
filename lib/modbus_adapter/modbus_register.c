/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_register.h"
#include "voltage_control/vc_types.h"
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
	return off >= CH_CAL_OUTPUT_ENABLE && off <= CH_CAL_COMMIT_CMD;
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
	reg_handle_t handle;
	uint8_t ordinal;
	uint8_t word;
	bool mapped;
};

extern const struct reg_descriptor vc_catalog_global_regs[];
extern const struct reg_descriptor modbus_protocol_major_reg;
extern const struct reg_descriptor modbus_protocol_minor_reg;
extern const struct reg_descriptor modbus_NEXT_BOOT_SLAVE_ADDRESS_reg;
extern const struct reg_descriptor modbus_NEXT_BOOT_BAUD_RATE_CODE_reg;

/* Uptime and firmware version have no hardware dependency (see
 * sys_uptime.c) and are always compiled in, unlike the SYS_RUN-LED/SHT3x
 * pieces of sys_status.c which require CONFIG_SYS_STATUS. */
extern const struct reg_descriptor sys_status_uptime_reg;
extern const struct reg_descriptor sys_status_firmware_version_reg;

#if defined(CONFIG_SYS_STATUS)
extern const struct reg_descriptor sys_status_temperature_reg;
extern const struct reg_descriptor sys_status_humidity_reg;
#define SYS_STATUS_HANDLE(name) (&sys_status_##name##_reg)
#else
#define SYS_STATUS_HANDLE(name) NULL
#endif

#define VC_GLOBAL_HANDLE(name) \
	(&vc_catalog_global_regs[REG_VC_GLOBAL_ORD_##name])

static struct {
	reg_id_t id;
	uint32_t value;
	bool valid;
} word_latch;

#define SYS_VIEW_HANDLE_PROTOCOL_MAJOR (&modbus_protocol_major_reg)
#define SYS_VIEW_HANDLE_PROTOCOL_MINOR (&modbus_protocol_minor_reg)
#define SYS_VIEW_HANDLE_VARIANT_ID VC_GLOBAL_HANDLE(VARIANT_ID)
#define SYS_VIEW_HANDLE_CAPABILITY_FLAGS VC_GLOBAL_HANDLE(CAPABILITY_FLAGS)
#define SYS_VIEW_HANDLE_SUPPORTED_CHANNELS VC_GLOBAL_HANDLE(SUPPORTED_CHANNELS)
#define SYS_VIEW_HANDLE_ACTIVE_CHANNEL_MASK VC_GLOBAL_HANDLE(ACTIVE_CHANNEL_MASK)
#define SYS_VIEW_HANDLE_CURRENT_UNIT_EXP VC_GLOBAL_HANDLE(CURRENT_UNIT_EXP)
#define SYS_VIEW_HANDLE_BOARD_TEMPERATURE SYS_STATUS_HANDLE(temperature)
#define SYS_VIEW_HANDLE_BOARD_HUMIDITY SYS_STATUS_HANDLE(humidity)
#define SYS_VIEW_HANDLE_UPTIME (&sys_status_uptime_reg)
#define SYS_VIEW_HANDLE_FW_VERSION (&sys_status_firmware_version_reg)
#define SYS_VIEW_HANDLE_ACTIVE_OPERATING_MODE VC_GLOBAL_HANDLE(ACTIVE_OPERATING_MODE)
#define SYS_VIEW_HANDLE_STATUS VC_GLOBAL_HANDLE(STATUS)
#define SYS_VIEW_HANDLE_FAULT_CAUSE VC_GLOBAL_HANDLE(FAULT_CAUSE)
#define SYS_VIEW_HANDLE_OPERATING_MODE VC_GLOBAL_HANDLE(OPERATING_MODE)
#define SYS_VIEW_HANDLE_STARTUP_CHANNEL_POLICY VC_GLOBAL_HANDLE(STARTUP_CHANNEL_POLICY)
#define SYS_VIEW_HANDLE_SLAVE_ADDRESS (&modbus_NEXT_BOOT_SLAVE_ADDRESS_reg)
#define SYS_VIEW_HANDLE_BAUD_RATE_CODE (&modbus_NEXT_BOOT_BAUD_RATE_CODE_reg)
#define SYS_VIEW_HANDLE_PARAM_ACTION VC_GLOBAL_HANDLE(PARAM_ACTION)

#define SYS_INPUT_16(name, offset) \
	[offset] = { SYS_VIEW_HANDLE_##name, 0, 0, true },
#define SYS_INPUT_32(name, offset) \
	[offset] = { SYS_VIEW_HANDLE_##name, 0, 0, true }, \
	[(offset) + 1] = { SYS_VIEW_HANDLE_##name, 0, 1, true },
#define SYS_HOLDING_16(name, offset)
#define SYS_HOLDING_32(name, offset)
#define MODBUS_SYS16(name, bank, offset, poll_cat) SYS_##bank##_16(name, offset)
#define MODBUS_SYS32(name, bank, offset, poll_cat) SYS_##bank##_32(name, offset)
#define MODBUS_VC16(name, bank, offset, poll_cat)
#define MODBUS_VC32(name, bank, offset, poll_cat)
static const struct wire_reg sys_input_view[CH_BLOCK_SIZE] = {
#include "reg_store/modbus_view.def"
};
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
#undef SYS_INPUT_16
#undef SYS_INPUT_32
#undef SYS_HOLDING_16
#undef SYS_HOLDING_32

#define SYS_INPUT_16(name, offset)
#define SYS_INPUT_32(name, offset)
#define SYS_HOLDING_16(name, offset) \
	[offset] = { SYS_VIEW_HANDLE_##name, 0, 0, true },
#define SYS_HOLDING_32(name, offset) \
	[offset] = { SYS_VIEW_HANDLE_##name, 0, 0, true }, \
	[(offset) + 1] = { SYS_VIEW_HANDLE_##name, 0, 1, true },
#define MODBUS_SYS16(name, bank, offset, poll_cat) SYS_##bank##_16(name, offset)
#define MODBUS_SYS32(name, bank, offset, poll_cat) SYS_##bank##_32(name, offset)
#define MODBUS_VC16(name, bank, offset, poll_cat)
#define MODBUS_VC32(name, bank, offset, poll_cat)
static const struct wire_reg sys_holding_view[CH_BLOCK_SIZE] = {
#include "reg_store/modbus_view.def"
};
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
#undef SYS_INPUT_16
#undef SYS_INPUT_32
#undef SYS_HOLDING_16
#undef SYS_HOLDING_32

#define VC_INPUT_16(name, offset) \
	[offset] = { NULL, REG_VC_ORD_##name, 0, true },
#define VC_INPUT_32(name, offset) \
	[offset] = { NULL, REG_VC_ORD_##name, 0, true }, \
	[(offset) + 1] = { NULL, REG_VC_ORD_##name, 1, true },
#define VC_HOLDING_16(name, offset)
#define VC_HOLDING_32(name, offset)
#define MODBUS_SYS16(name, bank, offset, poll_cat)
#define MODBUS_SYS32(name, bank, offset, poll_cat)
#define MODBUS_VC16(name, bank, offset, poll_cat) VC_##bank##_16(name, offset)
#define MODBUS_VC32(name, bank, offset, poll_cat) VC_##bank##_32(name, offset)
static const struct wire_reg ch_input_view[CH_BLOCK_SIZE] = {
#include "reg_store/modbus_view.def"
};
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
#undef VC_INPUT_16
#undef VC_INPUT_32
#undef VC_HOLDING_16
#undef VC_HOLDING_32

#define VC_INPUT_16(name, offset)
#define VC_INPUT_32(name, offset)
#define VC_HOLDING_16(name, offset) \
	[offset] = { NULL, REG_VC_ORD_##name, 0, true },
#define VC_HOLDING_32(name, offset) \
	[offset] = { NULL, REG_VC_ORD_##name, 0, true }, \
	[(offset) + 1] = { NULL, REG_VC_ORD_##name, 1, true },
#define MODBUS_SYS16(name, bank, offset, poll_cat)
#define MODBUS_SYS32(name, bank, offset, poll_cat)
#define MODBUS_VC16(name, bank, offset, poll_cat) VC_##bank##_16(name, offset)
#define MODBUS_VC32(name, bank, offset, poll_cat) VC_##bank##_32(name, offset)
static const struct wire_reg ch_holding_view[CH_BLOCK_SIZE] = {
#include "reg_store/modbus_view.def"
};
#undef MODBUS_SYS16
#undef MODBUS_SYS32
#undef MODBUS_VC16
#undef MODBUS_VC32
#undef VC_INPUT_16
#undef VC_INPUT_32
#undef VC_HOLDING_16
#undef VC_HOLDING_32

static enum reg_status read_wire(const struct wire_reg *wire, uint8_t ch,
				bool channel, uint16_t *reg)
{
	const struct reg_descriptor *desc;
	union reg_value value = {};
	enum reg_status status;
	uint32_t raw;

	if (!wire->mapped) {
		*reg = 0U;
		return REG_OK;
	}
	desc = channel ? reg_vc_channel_handle(ch, wire->ordinal) : wire->handle;
	if (desc == NULL) {
		*reg = 0U;
		return REG_OK;
	}
	if ((desc->type == REG_S32 || desc->type == REG_U32) &&
	    wire->word == 1U && word_latch.valid && word_latch.id == desc->id) {
		*reg = (uint16_t)word_latch.value;
		word_latch.valid = false;
		return REG_OK;
	}
	status = reg_read_descriptor(desc, &value);
	if (status == REG_WRITE_ONLY) {
		*reg = 0U;
		return REG_OK;
	}
	if (status != REG_OK) {
		return status;
	}

	switch (desc->type) {
	case REG_S16:
		word_latch.valid = false;
		*reg = (uint16_t)value.s16;
		return REG_OK;
	case REG_U16:
	case REG_ENUM:
		word_latch.valid = false;
		*reg = value.u16;
		return REG_OK;
	case REG_BOOL:
		word_latch.valid = false;
		*reg = value.boolean ? 1U : 0U;
		return REG_OK;
	case REG_S32: raw = (uint32_t)value.s32; break;
	case REG_U32: raw = value.u32; break;
	default: return REG_INVALID_VALUE;
	}
	if (wire->word == 0U) {
		word_latch.id = desc->id;
		word_latch.value = raw;
		word_latch.valid = true;
		*reg = (uint16_t)(raw >> 16);
	} else {
		word_latch.valid = false;
		*reg = (uint16_t)raw;
	}
	return REG_OK;
}

static enum reg_status write_wire(const struct wire_reg *wire, uint8_t ch,
				 bool channel, uint16_t reg,
				 k_timeout_t timeout)
{
	const struct reg_descriptor *desc;
	union reg_value value = {};

	if (!wire->mapped) {
		return REG_INVALID_VALUE;
	}
	desc = channel ? reg_vc_channel_handle(ch, wire->ordinal) : wire->handle;
	if (desc == NULL) {
		return REG_UNSUPPORTED;
	}
	if (desc->type == REG_S16) {
		value.s16 = (int16_t)reg;
	} else {
		value.u16 = reg;
	}
	return reg_write_descriptor(desc, value, timeout);
}

/* ------------------------------------------------------------------ */
/* System register read/write                                          */
/* ------------------------------------------------------------------ */

enum reg_status vc_reg_read_sys_input(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return REG_INVALID_ARGUMENT;
	}
	return read_wire(&sys_input_view[off], 0, false, reg);
}

enum reg_status vc_reg_read_sys_holding(uint16_t off, uint16_t *reg)
{
	if (off >= CH_BLOCK_SIZE) {
		return REG_INVALID_ARGUMENT;
	}
	return read_wire(&sys_holding_view[off], 0, false, reg);
}

enum reg_status vc_reg_write_sys_holding(uint16_t off, uint16_t val,
					k_timeout_t timeout)
{
	if (off >= CH_BLOCK_SIZE) {
		return REG_INVALID_ARGUMENT;
	}
	return write_wire(&sys_holding_view[off], 0, false, val, timeout);
}

/* ------------------------------------------------------------------ */
/* Channel register read/write                                         */
/* ------------------------------------------------------------------ */

enum reg_status vc_reg_read_ch_input(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return REG_UNSUPPORTED;
	}
	if (off >= CH_BLOCK_SIZE) {
		return REG_INVALID_ARGUMENT;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_input_supported(caps, off)) {
		return REG_UNSUPPORTED;
	}
	(void)read_wire(&sys_input_view[SYS_ACTIVE_OPERATING_MODE], 0, false, &mode);
	if (is_ch_cal_input_reg(off) &&
	    (enum vc_operating_mode)mode != VC_OPERATING_MODE_CALIBRATION) {
		return REG_UNSUPPORTED;
	}
	return read_wire(&ch_input_view[off], ch, true, reg);
}

enum reg_status vc_reg_read_ch_holding(uint8_t ch, uint16_t off, uint16_t *reg)
{
	uint16_t count = 0;
	uint16_t caps = 0;

	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return REG_UNSUPPORTED;
	}
	if (off >= CH_BLOCK_SIZE) {
		return REG_INVALID_ARGUMENT;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_holding_supported(caps, off)) {
		return REG_UNSUPPORTED;
	}
	return read_wire(&ch_holding_view[off], ch, true, reg);
}

enum reg_status vc_reg_write_ch_holding(uint8_t ch, uint16_t off, uint16_t val,
					k_timeout_t timeout)
{
	uint16_t count = 0;
	uint16_t caps = 0;
	uint16_t mode = 0;

	(void)read_wire(&sys_input_view[SYS_SUPPORTED_CHANNELS], 0, false, &count);
	if (ch >= count) {
		return REG_UNSUPPORTED;
	}
	(void)read_wire(&ch_input_view[CH_CAPABILITY_FLAGS], ch, true, &caps);
	if (!ch_holding_supported(caps, off)) {
		return REG_UNSUPPORTED;
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
			return REG_UNSUPPORTED;
		}
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:
			if (val > 1) {
				return REG_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_DAC_CODE:
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_SAMPLE_CMD:
			if (val == CAL_COMMAND_NONE) {
				return REG_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return REG_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		case CH_CAL_COMMIT_CMD:
			if (val == CAL_COMMAND_NONE) {
				return REG_OK;
			}
			if (val != CAL_COMMAND_EXECUTE) {
				return REG_INVALID_VALUE;
			}
			return write_wire(&ch_holding_view[off], ch, true, val, timeout);
		default:
			return REG_UNSUPPORTED;
		}
	}

	if (off >= ARRAY_SIZE(ch_holding_view)) {
		return REG_INVALID_ARGUMENT;
	}
	return write_wire(&ch_holding_view[off], ch, true, val, timeout);
}

/* ------------------------------------------------------------------ */
/* Extension block                                                     */
/* ------------------------------------------------------------------ */

enum reg_status vc_reg_write_ext(uint16_t off, uint16_t val,
				k_timeout_t timeout)
{
	union reg_value value = { .u16 = val };
	enum reg_status status;

	if (off == EXT_CAL_UNLOCK) {
		status = reg_write(REG_VC_GLOBAL_ID(
			REG_VC_GLOBAL_FIELD_CAL_UNLOCK), value, timeout);
		return status;
	}
	if (off == EXT_CAL_EXIT) {
		status = reg_write(REG_VC_GLOBAL_ID(
			REG_VC_GLOBAL_FIELD_CAL_EXIT), value, timeout);
		return status;
	}
	return REG_UNSUPPORTED;
}

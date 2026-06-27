/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include "reg_store/reg_map.h"
#include "modbus_adapter/modbus_adapter.h"
#include "voltage_control/vc.h"

static struct vc_ctx *make_ctx(void)
{
	return vc_init();
}

static void destroy_ctx(struct vc_ctx *ctx)
{
	vc_destroy(ctx);
}

ZTEST_SUITE(modbus_adapter, NULL, NULL, NULL, NULL, NULL);

/* ---- Address decode ---- */

ZTEST(modbus_adapter, test_sys_input_reads_protocol_version)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t major, minor;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, SYS_PROTOCOL_MAJOR, &major), VC_MB_OK);
	zassert_equal(vc_mb_input_rd(mb, SYS_PROTOCOL_MINOR, &minor), VC_MB_OK);
	zassert_equal(major, VC_PROTOCOL_MAJOR);
	zassert_equal(minor, VC_PROTOCOL_MINOR);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_input_reads_channel_count)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t count;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, SYS_SUPPORTED_CHANNELS, &count), VC_MB_OK);
	zassert_equal(count, 2);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_ch_input_reads_capability_flags)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t caps;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_CAPABILITY_FLAGS,
				     &caps), VC_MB_OK);
	zassert_equal(caps, CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			    CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_unsupported_channel_returns_illegal_address)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(2) + CH_STATUS_BITS, &reg),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(3) + CH_OUTPUT_ACTION, &reg),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_invalid_address_returns_illegal_address)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, 999, &reg), VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

/* ---- System holding read/write round-trip ---- */

ZTEST(modbus_adapter, test_sys_holding_write_slave_address)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 42), VC_MB_OK);
	zassert_equal(vc_mb_holding_rd(mb, SYS_SLAVE_ADDRESS, &reg), VC_MB_OK);
	zassert_equal(reg, 42);

	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 0),
		      VC_MB_ILLEGAL_VALUE);
	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 248),
		      VC_MB_ILLEGAL_VALUE);
	zassert_equal(vc_mb_holding_rd(mb, SYS_SLAVE_ADDRESS, &reg), VC_MB_OK);
	zassert_equal(reg, 42);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_holding_write_baud_rate_code)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_rd(mb, SYS_BAUD_RATE_CODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_BAUD_RATE_115200);

	zassert_equal(vc_mb_holding_wr(mb, SYS_BAUD_RATE_CODE, VC_BAUD_RATE_9600),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_rd(mb, SYS_BAUD_RATE_CODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_BAUD_RATE_9600);

	zassert_equal(vc_mb_holding_wr(mb, SYS_BAUD_RATE_CODE, 99),
		      VC_MB_ILLEGAL_VALUE);
	zassert_equal(vc_mb_holding_rd(mb, SYS_BAUD_RATE_CODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_BAUD_RATE_9600);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_holding_write_startup_policy)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	/* Default is 0 (load NVS op-config) */
	zassert_equal(vc_mb_holding_rd(mb, SYS_STARTUP_CHANNEL_POLICY, &reg), VC_MB_OK);
	zassert_equal(reg, 0);

	/* Write 1 (factory reset op-config on startup) */
	zassert_equal(vc_mb_holding_wr(mb, SYS_STARTUP_CHANNEL_POLICY, 1), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_holding_rd(mb, SYS_STARTUP_CHANNEL_POLICY, &reg), VC_MB_OK);
	zassert_equal(reg, 1);

	/* Write back to 0 */
	zassert_equal(vc_mb_holding_wr(mb, SYS_STARTUP_CHANNEL_POLICY, 0), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_holding_rd(mb, SYS_STARTUP_CHANNEL_POLICY, &reg), VC_MB_OK);
	zassert_equal(reg, 0);

	destroy_ctx(ctx);
}

/* ---- Channel holding read/write round-trip ---- */

ZTEST(modbus_adapter, test_ch_holding_write_target_voltage)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_CFG_TARGET_VOLTAGE,
				       5000), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_CFG_TARGET_VOLTAGE,
				       &reg), VC_MB_OK);
	zassert_equal(reg, 5000);

	destroy_ctx(ctx);
}

/* ---- Output action round-trip ---- */

ZTEST(modbus_adapter, test_ch_output_action_enable_disable)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t bits;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_OUTPUT_ACTION,
				       VC_OUTPUT_ACTION_ENABLE), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_STATUS_BITS, &bits),
		      VC_MB_OK);
	zassert_true(bits & 0x0002);

	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_OUTPUT_ACTION,
				       VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_STATUS_BITS, &bits),
		      VC_MB_OK);
	zassert_false(bits & 0x0002);

	destroy_ctx(ctx);
}

/* ---- Calibration mode restrictions ---- */

ZTEST(modbus_adapter, test_cal_registers_rejected_outside_calibration)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	/* Cal input registers (raw ADC) are gated behind calibration mode */
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_ADC_VOLTAGE_HI,
				     &reg), VC_MB_ILLEGAL_ADDRESS);
	/* Cal session holding regs are readable in any mode (FC03 readback) */
	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_CAL_DAC_CODE, &reg),
		      VC_MB_OK);
	/* But writes to cal session holding regs are gated behind calibration mode */
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_CAL_DAC_CODE, 100),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_cal_unlock_and_mode_entry)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP1),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP2),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, SYS_OPERATING_MODE,
				       VC_OPERATING_MODE_CALIBRATION), VC_MB_OK);
	k_msleep(50);

	zassert_equal(vc_mb_input_rd(mb, SYS_ACTIVE_OPERATING_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_CAL_DAC_CODE, &reg),
		      VC_MB_OK);

	destroy_ctx(ctx);
}

/* ---- Extension block ---- */

ZTEST(modbus_adapter, test_extension_read_returns_zero)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg = 0xFFFF;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_rd(mb, EXT_BLOCK_BASE, &reg), VC_MB_OK);
	zassert_equal(reg, 0);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_extension_write_non_unlock_rejected)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	/* Offset 2 is not assigned — must be rejected */
	zassert_equal(vc_mb_holding_wr(mb, EXT_BLOCK_BASE + 2, 0),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_cal_exit_via_ext_register)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);

	/* Unlock + enter CAL */
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP1),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP2),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, SYS_OPERATING_MODE,
				       VC_OPERATING_MODE_CALIBRATION), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, SYS_ACTIVE_OPERATING_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_OPERATING_MODE_CALIBRATION);

	/* Exit via EXT_CAL_EXIT — must restore to NORMAL (the pre-cal mode) */
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_EXIT_ABS, 1), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, SYS_ACTIVE_OPERATING_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_OPERATING_MODE_NORMAL);

	destroy_ctx(ctx);
}

/* ---- Error mapping ---- */

ZTEST(modbus_adapter, test_fault_cmd_maps_to_mb_result)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_FAULT_CMD,
				       VC_CHANNEL_FAULT_COMMAND_NONE), VC_MB_OK);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_param_action_storage_returns_device_failure)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_PARAM_ACTION, VC_PARAM_ACTION_SAVE),
		      VC_MB_DEVICE_FAILURE);

	destroy_ctx(ctx);
}

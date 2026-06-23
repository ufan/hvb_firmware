/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/modbus_adapter.h"
#include "voltage_control/vc.h"

struct vc_ctx {
	struct vc_runtime *runtime;
};

static const struct vc_channel_entry full_cap_channels[] = {
	{ .dev = NULL, .index = 0,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
	{ .dev = NULL, .index = 1,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
};

static const struct vc_channel_entry onoff_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE },
};

static struct vc_ctx test_ctx;

static struct vc_ctx *make_ctx(const struct vc_channel_entry *channels,
			       size_t count)
{
	test_ctx.runtime = vc_domain_runtime_create(channels, count);
	return test_ctx.runtime ? &test_ctx : NULL;
}

static void destroy_ctx(struct vc_ctx *ctx)
{
	if (ctx && ctx->runtime) {
		vc_runtime_destroy(ctx->runtime);
		ctx->runtime = NULL;
	}
}

ZTEST_SUITE(modbus_adapter, NULL, NULL, NULL, NULL, NULL);

/* ---- Address decode ---- */

ZTEST(modbus_adapter, test_sys_input_reads_protocol_version)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 2);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t major, minor;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, SYS_PROTOCOL_MAJOR, &major), VC_MB_OK);
	zassert_equal(vc_mb_input_rd(mb, SYS_PROTOCOL_MINOR, &minor), VC_MB_OK);
	zassert_equal(major, HVB_PROTOCOL_MAJOR);
	zassert_equal(minor, HVB_PROTOCOL_MINOR);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_input_reads_channel_count)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 2);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t count;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, SYS_SUPPORTED_CHANNELS, &count), VC_MB_OK);
	zassert_equal(count, 2);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_ch_input_reads_capability_flags)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 2);
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
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 2);
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
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, 999, &reg), VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

/* ---- System holding read/write round-trip ---- */

ZTEST(modbus_adapter, test_sys_holding_write_slave_address)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 42), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_holding_rd(mb, SYS_SLAVE_ADDRESS, &reg), VC_MB_OK);
	zassert_equal(reg, 42);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_holding_write_recovery_policy)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_RECOVERY_POLICY_MODE,
				       VC_RECOVERY_AUTO_RETRY), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_holding_rd(mb, SYS_RECOVERY_POLICY_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_RECOVERY_AUTO_RETRY);

	destroy_ctx(ctx);
}

/* ---- Channel holding read/write round-trip ---- */

ZTEST(modbus_adapter, test_ch_holding_write_target_voltage)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
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
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
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

/* ---- Capability gating ---- */

ZTEST(modbus_adapter, test_onoff_channel_rejects_measurement_read)
{
	struct vc_ctx *ctx = make_ctx(onoff_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_MEASURED_VOLTAGE, &reg),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_MEASURED_CURRENT, &reg),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_STATUS_BITS, &reg),
		      VC_MB_OK);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_onoff_channel_rejects_protection_write)
{
	struct vc_ctx *ctx = make_ctx(onoff_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb,
				       CH_BLOCK_BASE(0) + CH_VOLTAGE_PROTECTION_MODE,
				       VC_PROTECTION_MODE_FLAG_ONLY),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

/* ---- Calibration mode restrictions ---- */

ZTEST(modbus_adapter, test_cal_registers_rejected_outside_calibration)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_ADC_VOLTAGE_HI,
				     &reg), VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_DAC_CODE, &reg),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_cal_unlock_and_mode_entry)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 2);
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

	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_DAC_CODE, &reg),
		      VC_MB_OK);

	destroy_ctx(ctx);
}

/* ---- Extension block ---- */

ZTEST(modbus_adapter, test_extension_read_returns_zero)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg = 0xFFFF;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_rd(mb, EXT_BLOCK_BASE, &reg), VC_MB_OK);
	zassert_equal(reg, 0);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_extension_write_non_unlock_rejected)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, EXT_BLOCK_BASE + 1, 0),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

/* ---- Error mapping ---- */

ZTEST(modbus_adapter, test_fault_cmd_maps_to_mb_result)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_FAULT_CMD,
				       VC_CHANNEL_FAULT_COMMAND_NONE), VC_MB_OK);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_param_action_storage_returns_device_failure)
{
	struct vc_ctx *ctx = make_ctx(full_cap_channels, 1);
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_PARAM_ACTION, VC_PARAM_ACTION_SAVE),
		      VC_MB_DEVICE_FAILURE);

	destroy_ctx(ctx);
}

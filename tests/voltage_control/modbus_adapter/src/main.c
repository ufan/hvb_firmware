/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#if defined(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include "reg_store/reg_map.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"
#include "modbus_adapter/modbus_adapter.h"
#include "voltage_control/vc.h"

static struct vc_ctx *make_ctx(void)
{
	return vc_init();
}

#if defined(CONFIG_SYS_RESET)
static struct k_sem reset_called;

void sys_status_platform_reset(void)
{
	k_sem_give(&reset_called);
}
#endif

static void destroy_ctx(struct vc_ctx *ctx)
{
	vc_destroy(ctx);
}

#if defined(CONFIG_SETTINGS)
static uint16_t read_modbus_u16(enum reg_modbus_field field)
{
	union reg_value value = {};

	zassert_equal(reg_read(REG_MODBUS_ID(field), &value), REG_OK);
	return value.u16;
}
#endif

ZTEST_SUITE(modbus_adapter, NULL, NULL, NULL, NULL, NULL);

ZTEST(modbus_adapter, test_adapter_config_is_exposed_by_owner_registers)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	union reg_value value = {};

	zassert_not_null(mb);
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 1U);
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 1U);
	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_adapter_active_write_routes_through_owner)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	union reg_value value = { .u16 = 42U };
	enum reg_status status;

	zassert_not_null(mb);
	status = reg_write(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), value, K_NO_WAIT);
	zassert_equal(status, REG_OK, "catalog write returned %d", status);
	value.u16 = 0U;
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 42U);
	destroy_ctx(ctx);
}

#if defined(CONFIG_SETTINGS)
ZTEST(modbus_adapter, test_adapter_next_boot_write_routes_through_owner)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	union reg_value value = { .u16 = 77U };

	zassert_not_null(mb);
	zassert_equal(reg_write(REG_MODBUS_ID(
		REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), value, K_NO_WAIT),
		REG_OK);
	value.u16 = 0U;
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 77U);
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 1U);
	destroy_ctx(ctx);
}
#endif

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

#if !defined(CONFIG_SETTINGS)
ZTEST(modbus_adapter, test_sys_holding_slave_address_is_read_only_without_settings)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 42),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, SYS_SLAVE_ADDRESS, &reg), VC_MB_OK);
	zassert_equal(reg, 1);

	destroy_ctx(ctx);
}

ZTEST(modbus_adapter, test_sys_holding_baud_is_read_only_without_settings)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_rd(mb, SYS_BAUD_RATE_CODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_BAUD_RATE_115200);

	zassert_equal(vc_mb_holding_wr(mb, SYS_BAUD_RATE_CODE, VC_BAUD_RATE_9600),
		      VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, SYS_BAUD_RATE_CODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_BAUD_RATE_115200);

	destroy_ctx(ctx);
}
#endif

ZTEST(modbus_adapter, test_channel_param_action_rejects_system_reset)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_PARAM_ACTION,
				       SYS_PARAM_ACTION_SOFTWARE_RESET),
		      VC_MB_ILLEGAL_VALUE);

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
		      IS_ENABLED(CONFIG_SETTINGS) ? VC_MB_OK : VC_MB_DEVICE_FAILURE);

	destroy_ctx(ctx);
}

#if defined(CONFIG_SETTINGS)
struct settings_read_ctx {
	struct mb_adapter_config cfg;
	bool found;
};

static int read_mb_config(const char *name, size_t len,
			  settings_read_cb read_cb, void *cb_arg, void *param)
{
	struct settings_read_ctx *read = param;

	if (len != sizeof(read->cfg) ||
	    read_cb(cb_arg, &read->cfg, sizeof(read->cfg)) < 0) {
		return -EIO;
	}
	read->found = true;
	return 0;
}

ZTEST(modbus_adapter, test_modbus_config_is_persisted_for_next_boot)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb;
	struct settings_read_ctx stored = {};

	zassert_equal(settings_delete("mb/cfg"), 0);
	mb = vc_mb_adapter_create(ctx);
	zassert_not_null(mb);

	zassert_equal(vc_mb_holding_wr(mb, SYS_SLAVE_ADDRESS, 42), VC_MB_OK);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), 1);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), 42);

	mb = vc_mb_adapter_create(ctx);
	zassert_not_null(mb);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), 42);

	union reg_value value = { .u16 = 77U };
	zassert_equal(reg_write(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), value, K_NO_WAIT), REG_OK);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), 77);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), 42);
	value.u16 = 1U;
	zassert_equal(reg_write(REG_MODBUS_ID(REG_MODBUS_FIELD_CONFIG_SAVE),
				value, K_NO_WAIT), REG_OK);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), 77);

	zassert_equal(vc_mb_holding_wr(mb, SYS_PARAM_ACTION,
				       VC_PARAM_ACTION_FACTORY_RESET), VC_MB_OK);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), 77);
	zassert_equal(read_modbus_u16(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), 1);
	zassert_equal(settings_load_subtree_direct("mb/cfg", read_mb_config,
						   &stored), 0);
	zassert_true(stored.found);
	zassert_equal(stored.cfg.slave_address, 1);

	destroy_ctx(ctx);
}
#endif

ZTEST(modbus_adapter, test_system_reset_requires_system_reset_service)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
#if defined(CONFIG_SYS_RESET)
	k_sem_init(&reset_called, 0, 1);
	zassert_equal(vc_mb_holding_wr(mb, SYS_PARAM_ACTION, 255), VC_MB_OK);
	zassert_equal(k_sem_take(&reset_called, K_NO_WAIT), -EBUSY);
	zassert_equal(k_sem_take(&reset_called, K_MSEC(100)), 0);
#else
	zassert_equal(vc_mb_holding_wr(mb, SYS_PARAM_ACTION, 255),
		      VC_MB_DEVICE_FAILURE);
#endif

	destroy_ctx(ctx);
}

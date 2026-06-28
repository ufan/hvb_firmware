/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"
#include "voltage_control/vc.h"

static struct vc_ctx *ctx;

static void write_u16(reg_id_t id, uint16_t input)
{
	union reg_value value = { .u16 = input };

	zassert_equal(reg_write(id, value, K_SECONDS(1)), REG_OK);
}

static uint16_t read_u16(reg_id_t id)
{
	union reg_value value = {};

	zassert_equal(reg_read(id, &value), REG_OK);
	return value.u16;
}

static void *suite_setup(void)
{
	ctx = vc_init();
	zassert_not_null(ctx);
	return ctx;
}

ZTEST_SUITE(vc_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(vc_api, test_init_returns_non_null)
{
	zassert_not_null(ctx);
}

ZTEST(vc_api, test_start_returns_ok)
{
	zassert_equal(vc_ctx_start(ctx), VC_OK);
}

ZTEST(vc_api, test_catalog_sets_mode)
{
	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_OPERATING_MODE),
		  VC_OPERATING_MODE_AUTOMATIC);

	k_msleep(50);
	zassert_equal(read_u16(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_ACTIVE_OPERATING_MODE)),
		VC_OPERATING_MODE_AUTOMATIC);

	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_OPERATING_MODE),
		  VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_api, test_catalog_dispatches_output_action)
{
	write_u16(REG_VC_ID(0, REG_VC_FIELD_OUTPUT_ACTION),
		  VC_OUTPUT_ACTION_ENABLE);

	k_msleep(50);
	zassert_true(read_u16(REG_VC_ID(0, REG_VC_FIELD_STATUS_BITS)) & 0x0002);

	write_u16(REG_VC_ID(0, REG_VC_FIELD_OUTPUT_ACTION),
		  VC_OUTPUT_ACTION_DISABLE_IMMEDIATE);
}

ZTEST(vc_api, test_catalog_sets_channel_field)
{
	write_u16(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE), 5000);

	k_msleep(50);
	zassert_equal(read_u16(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE)),
		5000);
}

ZTEST(vc_api, test_catalog_sets_system_field)
{
	write_u16(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_STARTUP_CHANNEL_POLICY), 1);

	k_msleep(50);
	zassert_equal(read_u16(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_STARTUP_CHANNEL_POLICY)), 1);
}

ZTEST(vc_api, test_catalog_reads_vc_fixed_and_config_state)
{
	union reg_value value = {};
	write_u16(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE), 5000);

	zassert_equal(reg_read(REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MAJOR), &value),
		      REG_OK);
	zassert_equal(value.u16, VC_PROTOCOL_MAJOR);
	zassert_equal(reg_read(REG_VC_GLOBAL_ID(
			       REG_VC_GLOBAL_FIELD_SUPPORTED_CHANNELS),
			       &value), REG_OK);
	zassert_equal(value.u16, 2U);
	zassert_equal(reg_read(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
			       &value), REG_OK);
	zassert_equal(value.s16, 5000);
}

ZTEST(vc_api, test_catalog_write_is_validated_and_committed_by_vc_owner)
{
	union reg_value value = { .s16 = 3200 };

	zassert_equal(reg_write(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
				value, K_SECONDS(1)), REG_OK);
	zassert_equal(reg_read(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
			       &value), REG_OK);
	zassert_equal(value.s16, 3200);

	value.s16 = 21000;
	zassert_equal(reg_write(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
				value, K_SECONDS(1)), REG_INVALID_VALUE);
	zassert_equal(reg_read(REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE),
			       &value), REG_OK);
	zassert_equal(value.s16, 3200);
}

ZTEST(vc_api, test_plain_scalars_are_bound_to_canonical_storage)
{
	const struct reg_descriptor *target = reg_describe(
		REG_VC_ID(0, REG_VC_FIELD_CFG_TARGET_VOLTAGE));
	const struct reg_descriptor *status = reg_describe(
		REG_VC_ID(0, REG_VC_FIELD_STATUS_BITS));

	zassert_not_null(target);
	zassert_not_null(target->value);
	zassert_not_null(status);
	zassert_not_null(status->value);
}

ZTEST(vc_api, test_read_observes_state_after_command_completion)
{
	write_u16(REG_VC_ID(1, REG_VC_FIELD_CFG_TARGET_VOLTAGE), 4100);
	zassert_equal(read_u16(REG_VC_ID(1, REG_VC_FIELD_CFG_TARGET_VOLTAGE)),
		4100);
}

ZTEST(vc_api, test_catalog_command_routes_through_runtime)
{
	union reg_value value = { .u16 = VC_OUTPUT_ACTION_ENABLE };

	zassert_equal(reg_write(REG_VC_ID(0, REG_VC_FIELD_OUTPUT_ACTION),
				value, K_SECONDS(1)), REG_OK);
	k_msleep(20);
	zassert_equal(reg_read(REG_VC_ID(0, REG_VC_FIELD_STATUS_BITS), &value),
		      REG_OK);
	zassert_true(value.u16 & 0x0002);

	value.u16 = VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
	zassert_equal(reg_write(REG_VC_ID(0, REG_VC_FIELD_OUTPUT_ACTION),
				value, K_SECONDS(1)), REG_OK);
}

ZTEST(vc_api, test_catalog_dispatches_calibration)
{
	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_CAL_UNLOCK),
		  CAL_UNLOCK_STEP1);
	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_CAL_UNLOCK),
		  CAL_UNLOCK_STEP2);
	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_OPERATING_MODE),
		  VC_OPERATING_MODE_CALIBRATION);

	k_msleep(50);
	zassert_equal(read_u16(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_ACTIVE_OPERATING_MODE)),
		VC_OPERATING_MODE_CALIBRATION);

	write_u16(REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_OPERATING_MODE),
		  VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_api, test_catalog_reads_supported_channel_count)
{
	zassert_equal(read_u16(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_SUPPORTED_CHANNELS)), 2);
}

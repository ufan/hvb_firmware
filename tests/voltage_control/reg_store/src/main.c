/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"

#ifdef CONFIG_VC_RUNTIME
#include "voltage_control/vc.h"
#endif

static uint16_t mutable_value = 7U;
static const uint16_t fixed_value = 42U;

static enum reg_status test_write(const struct reg_descriptor *desc,
				  union reg_value value,
				  k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	if (value.u16 > 100U) {
		return REG_INVALID_VALUE;
	}
	*(uint16_t *)desc->value = value.u16;
	return REG_OK;
}

static const struct reg_owner test_owner = {
	.write = test_write,
};

REG_DESCRIPTOR_DEFINE(test_fixed_reg,
	REG_ID(1, 0, 1), REG_U16, REG_RO, REG_FIXED,
	&fixed_value, NULL);
REG_DESCRIPTOR_DEFINE(test_mutable_reg,
	REG_ID(1, 0, 2), REG_U16, REG_RW, REG_CONFIG,
	&mutable_value, &test_owner);
REG_DESCRIPTOR_DEFINE(test_command_reg,
	REG_ID(1, 0, 3), REG_U16, REG_WO, REG_COMMAND,
	NULL, &test_owner);

ZTEST_SUITE(reg_store, NULL, NULL, NULL, NULL, NULL);

ZTEST(reg_store, test_structured_register_id)
{
	reg_id_t id = REG_ID(0x12, 0x34, 0x5678);

	zassert_equal(REG_ID_MODULE(id), 0x12);
	zassert_equal(REG_ID_INSTANCE(id), 0x34);
	zassert_equal(REG_ID_FIELD(id), 0x5678);
}

ZTEST(reg_store, test_semantic_ids_are_protocol_neutral_and_stable)
{
	zassert_equal(REG_ID_MODULE(REG_SYS_PROTOCOL_MAJOR_ID), REG_MODULE_SYSTEM);
	zassert_equal(REG_ID_INSTANCE(REG_SYS_PROTOCOL_MAJOR_ID), 0U);
	zassert_equal(REG_ID_FIELD(REG_SYS_PROTOCOL_MAJOR_ID),
		      REG_SYS_FIELD_PROTOCOL_MAJOR);
	zassert_equal(REG_ID_MODULE(REG_VC_STATUS_BITS_ID(15)),
		      REG_MODULE_VOLTAGE_CONTROL);
	zassert_equal(REG_ID_INSTANCE(REG_VC_STATUS_BITS_ID(15)), 15U);
	zassert_equal(REG_ID_FIELD(REG_VC_STATUS_BITS_ID(15)),
		      REG_VC_FIELD_STATUS_BITS);
}

ZTEST(reg_store, test_modbus_v3_view_keeps_fixed_wire_layout)
{
	zassert_equal(SYS_PROTOCOL_MAJOR, 0U);
	zassert_equal(SYS_OPERATING_MODE, 0U);
	zassert_equal(CH_BLOCK_BASE(15), 640U);
	zassert_equal(CH_MEASURED_VOLTAGE, 10U);
	zassert_equal(CH_CFG_TARGET_VOLTAGE, 3U);
	zassert_equal(EXT_BLOCK_BASE, 680U);
}

ZTEST(reg_store, test_catalog_reads_fixed_and_mutable_values)
{
	union reg_value value = {};

	zassert_equal(reg_read(REG_ID(1, 0, 1), &value), REG_OK);
	zassert_equal(value.u16, 42U);
	zassert_equal(reg_read(REG_ID(1, 0, 2), &value), REG_OK);
	zassert_equal(value.u16, 7U);
}

ZTEST(reg_store, test_catalog_enforces_access_and_missing_ids)
{
	union reg_value value = { .u16 = 9U };

	zassert_equal(reg_write(REG_ID(1, 0, 1), value, K_NO_WAIT),
		      REG_READ_ONLY);
	zassert_equal(reg_read(REG_ID(1, 0, 3), &value), REG_WRITE_ONLY);
	zassert_equal(reg_read(REG_ID(9, 9, 9), &value), REG_NOT_FOUND);
}

ZTEST(reg_store, test_owner_write_commits_only_valid_values)
{
	union reg_value value = { .u16 = 99U };

	zassert_equal(reg_write(REG_ID(1, 0, 2), value, K_MSEC(1)), REG_OK);
	zassert_equal(mutable_value, 99U);
	value.u16 = 101U;
	zassert_equal(reg_write(REG_ID(1, 0, 2), value, K_MSEC(1)),
		      REG_INVALID_VALUE);
	zassert_equal(mutable_value, 99U);
}

#ifdef CONFIG_VC_RUNTIME
ZTEST(reg_store, test_sixteen_channel_catalog_is_statically_composed)
{
	struct vc_ctx *ctx = vc_init();
	union reg_value value = {};

	zassert_not_null(ctx);
	zassert_not_null(reg_describe(
		REG_VC_ID(15, REG_VC_FIELD_STATUS_BITS)));
	zassert_equal(reg_read(REG_SYS_ID(REG_SYS_FIELD_SUPPORTED_CHANNELS),
			       &value), REG_OK);
	zassert_equal(value.u16, 16U);
	vc_destroy(ctx);
}
#endif

/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"

#ifdef CONFIG_VC_RUNTIME
#include "modbus_adapter/modbus_adapter.h"
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
	REG_ID(0xfe, 0, 1), REG_U16, REG_RO, REG_FIXED,
	&fixed_value, NULL);
REG_DESCRIPTOR_DEFINE(test_mutable_reg,
	REG_ID(0xfe, 0, 2), REG_U16, REG_RW, REG_CONFIG,
	&mutable_value, &test_owner);
REG_DESCRIPTOR_DEFINE(test_command_reg,
	REG_ID(0xfe, 0, 3), REG_U16, REG_WO, REG_COMMAND,
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
	zassert_not_equal(REG_MODULE_SYSTEM_STATUS,
			  REG_MODULE_MODBUS_ADAPTER);
	zassert_not_equal(REG_MODULE_SYSTEM_STATUS,
			  REG_MODULE_VOLTAGE_CONTROL);
	zassert_equal(REG_ID_MODULE(REG_MODBUS_PROTOCOL_MAJOR_ID),
		      REG_MODULE_MODBUS_ADAPTER);
	zassert_equal(REG_ID_MODULE(REG_SYS_STATUS_UPTIME_ID),
		      REG_MODULE_SYSTEM_STATUS);
	zassert_equal(REG_ID_INSTANCE(REG_VC_GLOBAL_SUPPORTED_CHANNELS_ID),
		      REG_GLOBAL_INSTANCE);
	zassert_equal(REG_ID_MODULE(REG_VC_STATUS_BITS_ID(15)),
		      REG_MODULE_VOLTAGE_CONTROL);
	zassert_equal(REG_ID_INSTANCE(REG_VC_STATUS_BITS_ID(15)), 15U);
	zassert_equal(REG_ID_FIELD(REG_VC_STATUS_BITS_ID(15)),
		      REG_VC_FIELD_STATUS_BITS);
}

ZTEST(reg_store, test_descriptor_handle_avoids_id_lookup_at_access_time)
{
	reg_handle_t handle = reg_describe(REG_ID(0xfe, 0, 2));
	union reg_value value = {};

	zassert_not_null(handle);
	zassert_equal(reg_handle_read(handle, &value), REG_OK);
	zassert_equal(value.u16, mutable_value);
	value.u16 = 23U;
	zassert_equal(reg_handle_write(handle, value, K_NO_WAIT), REG_OK);
	zassert_equal(mutable_value, 23U);
}

ZTEST(reg_store, test_catalog_ids_are_unique)
{
	STRUCT_SECTION_FOREACH(reg_descriptor, lhs) {
		STRUCT_SECTION_FOREACH(reg_descriptor, rhs) {
			if (lhs != rhs) {
				zassert_not_equal(lhs->id, rhs->id,
					  "duplicate register id 0x%08x", lhs->id);
			}
		}
	}
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

	zassert_equal(reg_read(REG_ID(0xfe, 0, 1), &value), REG_OK);
	zassert_equal(value.u16, 42U);
	zassert_equal(reg_read(REG_ID(0xfe, 0, 2), &value), REG_OK);
	zassert_equal(value.u16, 7U);
}

ZTEST(reg_store, test_catalog_enforces_access_and_missing_ids)
{
	union reg_value value = { .u16 = 9U };

	zassert_equal(reg_write(REG_ID(0xfe, 0, 1), value, K_NO_WAIT),
		      REG_READ_ONLY);
	zassert_equal(reg_read(REG_ID(0xfe, 0, 3), &value), REG_WRITE_ONLY);
	zassert_equal(reg_read(REG_ID(9, 9, 9), &value), REG_NOT_FOUND);
}

ZTEST(reg_store, test_owner_write_commits_only_valid_values)
{
	union reg_value value = { .u16 = 99U };

	zassert_equal(reg_write(REG_ID(0xfe, 0, 2), value, K_MSEC(1)), REG_OK);
	zassert_equal(mutable_value, 99U);
	value.u16 = 101U;
	zassert_equal(reg_write(REG_ID(0xfe, 0, 2), value, K_MSEC(1)),
		      REG_INVALID_VALUE);
	zassert_equal(mutable_value, 99U);
}

#ifdef CONFIG_VC_RUNTIME
K_THREAD_STACK_DEFINE(post_destroy_writer_stack, 1024);
static struct k_thread post_destroy_writer_thread;
static struct k_sem post_destroy_writer_done;
static enum reg_status post_destroy_writer_status;

static void post_destroy_writer(void *p1, void *p2, void *p3)
{
	union reg_value value = { .u16 = 1U };

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	post_destroy_writer_status = reg_write(REG_VC_GLOBAL_ID(
		REG_VC_GLOBAL_FIELD_STARTUP_CHANNEL_POLICY), value, K_NO_WAIT);
	k_sem_give(&post_destroy_writer_done);
}

ZTEST(reg_store, test_vc_catalog_is_unavailable_after_destroy)
{
	struct vc_ctx *ctx = vc_init();
	union reg_value value = {};
	int completed;

	zassert_not_null(ctx);
	vc_destroy(ctx);
	zassert_equal(reg_read(REG_VC_GLOBAL_SUPPORTED_CHANNELS_ID, &value),
		      REG_BUSY);

	k_sem_init(&post_destroy_writer_done, 0, 1);
	(void)k_thread_create(&post_destroy_writer_thread,
		post_destroy_writer_stack,
		K_THREAD_STACK_SIZEOF(post_destroy_writer_stack),
		post_destroy_writer, NULL, NULL, NULL,
		K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	completed = k_sem_take(&post_destroy_writer_done, K_MSEC(100));
	if (completed != 0) {
		k_thread_abort(&post_destroy_writer_thread);
	}
	zassert_equal(completed, 0, "catalog write blocked after destroy");
	zassert_equal(post_destroy_writer_status, REG_BUSY);
}

ZTEST(reg_store, test_vc_singleton_rejects_second_create)
{
	struct vc_ctx *first = vc_init();
	struct vc_ctx *second;

	zassert_not_null(first);
	second = vc_init();
	zassert_is_null(second);
	vc_destroy(first);
}

ZTEST(reg_store, test_fixed_vc_globals_are_directly_bound)
{
	const enum reg_vc_global_ordinal fixed_ordinals[] = {
		REG_VC_GLOBAL_ORD_VARIANT_ID,
		REG_VC_GLOBAL_ORD_CAPABILITY_FLAGS,
		REG_VC_GLOBAL_ORD_SUPPORTED_CHANNELS,
		REG_VC_GLOBAL_ORD_ACTIVE_CHANNEL_MASK,
	};

	for (size_t i = 0; i < ARRAY_SIZE(fixed_ordinals); i++) {
		reg_handle_t handle = reg_vc_global_handle(fixed_ordinals[i]);

		zassert_not_null(handle);
		zassert_not_null(handle->value, "fixed ordinal %u is callback-only",
				 fixed_ordinals[i]);
	}
}

ZTEST(reg_store, test_sixteen_channel_catalog_is_statically_composed)
{
	struct vc_ctx *ctx = vc_init();
	struct vc_mb_adapter *mb;
	reg_handle_t handle;
	union reg_value value = {};
	uint16_t word;

	zassert_not_null(ctx);
	mb = vc_mb_adapter_create();
	zassert_not_null(mb);
	handle = reg_vc_channel_handle(15, REG_VC_ORD_CFG_TARGET_VOLTAGE);
	zassert_not_null(handle);
	zassert_equal(handle->id,
		REG_VC_ID(15, REG_VC_FIELD_CFG_TARGET_VOLTAGE));
	zassert_not_null(reg_describe(
		REG_VC_ID(15, REG_VC_FIELD_STATUS_BITS)));
	zassert_equal(reg_read(REG_VC_GLOBAL_ID(
			       REG_VC_GLOBAL_FIELD_SUPPORTED_CHANNELS),
			       &value), REG_OK);
	zassert_equal(value.u16, 16U);
	zassert_equal(vc_mb_input_rd(mb,
		CH_BLOCK_BASE(15) + CH_CAPABILITY_FLAGS, &word), VC_MB_OK);
	zassert_not_equal(word, 0U);
	zassert_equal(vc_mb_holding_wr(mb,
		CH_BLOCK_BASE(15) + CH_CFG_TARGET_VOLTAGE, 1234U), VC_MB_OK);
	zassert_equal(vc_mb_holding_rd(mb,
		CH_BLOCK_BASE(15) + CH_CFG_TARGET_VOLTAGE, &word), VC_MB_OK);
	zassert_equal(word, 1234U);
	vc_destroy(ctx);
}
#endif

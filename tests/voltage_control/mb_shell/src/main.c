/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "modbus_adapter/modbus_adapter.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"
#include "voltage_control/vc.h"

static struct vc_ctx *ctx;

static void expect_command_result(const char *command, int expected)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	int ret = shell_execute_cmd(shell, command);

	zassert_equal(ret, expected, "command '%s' returned %d, expected %d",
		      command, ret, expected);
}

ZTEST(mb_shell, test_mb_status_is_registered)
{
	expect_command_result("mb status", 0);
}

/* mb set slave: out-of-range address must be rejected before adapter init. */
ZTEST(mb_shell, test_mb_set_slave_validates_range)
{
	expect_command_result("mb set slave 0", -EINVAL);
	expect_command_result("mb set slave 248", -EINVAL);
}

ZTEST(mb_shell, test_mb_set_slave_changes_active_config_only)
{
	union reg_value value = {};

	expect_command_result("mb set slave 10", 0);
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 10);
	zassert_equal(reg_read(REG_MODBUS_ID(
		REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS), &value), REG_OK);
	zassert_equal(value.u16, 1);
}

/* mb set baud: unknown code must be rejected before adapter init. */
ZTEST(mb_shell, test_mb_set_baud_validates_code)
{
	expect_command_result("mb set baud 5", -EINVAL);
}

/* mb save / mb load return -EIO without adapter init. */
ZTEST(mb_shell, test_mb_save_load_need_init)
{
	expect_command_result("mb save", -EIO);
	expect_command_result("mb load", -EIO);
}

ZTEST(mb_shell, test_mb_factory_is_registered)
{
	expect_command_result("mb factory", 0);
}

static void *mb_shell_setup(void)
{
	ctx = vc_init();
	zassert_not_null(ctx);
	zassert_not_null(vc_mb_adapter_create());
	return NULL;
}

static void mb_shell_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_destroy(ctx);
}

ZTEST_SUITE(mb_shell, NULL, mb_shell_setup, NULL, NULL, mb_shell_teardown);

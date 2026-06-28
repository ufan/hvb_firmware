/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

static struct k_sem reset_called;

void sys_status_platform_reset(void)
{
	k_sem_give(&reset_called);
}

static void expect_command_result(const char *command, int expected)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	int ret = shell_execute_cmd(shell, command);

	zassert_equal(ret, expected, "command '%s' returned %d, expected %d",
		      command, ret, expected);
}

ZTEST(ss_shell, test_ss_is_registered)
{
	expect_command_result("ss", 0);
}

ZTEST(ss_shell, test_system_status_is_exposed_through_catalog)
{
	union reg_value value = {};

	zassert_equal(reg_read(REG_SYS_ID(REG_SYS_FIELD_FW_VERSION), &value),
		      REG_OK);
	zassert_equal(value.u32, 1U);
	zassert_equal(reg_read(REG_SYS_ID(REG_SYS_FIELD_UPTIME), &value), REG_OK);
	zassert_equal(reg_read(REG_SYS_ID(REG_SYS_FIELD_BOARD_TEMPERATURE),
			       &value), REG_OK);
	zassert_equal(reg_read(REG_SYS_ID(REG_SYS_FIELD_BOARD_HUMIDITY),
			       &value), REG_OK);
}

ZTEST(ss_shell, test_reset_is_acknowledged_before_execution)
{
	k_sem_init(&reset_called, 0, 1);
	expect_command_result("ss reset", 0);
	zassert_equal(k_sem_take(&reset_called, K_NO_WAIT), -EBUSY);
	zassert_equal(k_sem_take(&reset_called, K_MSEC(100)), 0);
}

ZTEST_SUITE(ss_shell, NULL, NULL, NULL, NULL, NULL);

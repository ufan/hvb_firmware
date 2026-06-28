/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

#include "voltage_control/vc.h"
#include "voltage_control/vc_shell.h"

static struct vc_ctx *ctx;

static void expect_command_result(const char *command, int expected)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	int ret = shell_execute_cmd(shell, command);

	zassert_equal(ret, expected, "command '%s' returned %d, expected %d",
		      command, ret, expected);
}

ZTEST(vc_shell, test_cal_exit_is_registered)
{
	expect_command_result("vc cal exit", -EIO);
}

ZTEST(vc_shell, test_status_reads_catalog)
{
	expect_command_result("vc status", 0);
}

ZTEST(vc_shell, test_reset_is_not_registered)
{
	expect_command_result("vc reset", SHELL_CMD_HELP_PRINTED);
}

ZTEST(vc_shell, test_sys_reset_is_not_registered)
{
	expect_command_result("vc sys reset", SHELL_CMD_HELP_PRINTED);
}

static void *vc_shell_setup(void)
{
	ctx = vc_init();
	zassert_not_null(ctx);
	vc_shell_init();
	return NULL;
}

static void vc_shell_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_destroy(ctx);
}

ZTEST_SUITE(vc_shell, NULL, vc_shell_setup, NULL, NULL,
	    vc_shell_teardown);

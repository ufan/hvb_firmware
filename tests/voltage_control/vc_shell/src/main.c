/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

static void expect_command_result(const char *command, int expected)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	int ret = shell_execute_cmd(shell, command);

	zassert_equal(ret, expected, "command '%s' returned %d, expected %d",
		      command, ret, expected);
}

ZTEST(vc_shell, test_cal_exit_is_registered)
{
	expect_command_result("vc cal exit", -ENODEV);
}

ZTEST(vc_shell, test_reset_is_registered)
{
	expect_command_result("vc reset", -ENODEV);
}

ZTEST(vc_shell, test_sys_reset_is_registered)
{
	expect_command_result("vc sys reset", -ENODEV);
}

ZTEST_SUITE(vc_shell, NULL, NULL, NULL, NULL, NULL);

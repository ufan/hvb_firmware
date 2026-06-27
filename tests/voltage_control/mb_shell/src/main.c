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

/* mb status — returns 0 even without adapter init (cfg zeroed). */
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

/* mb set slave with a valid address returns -EIO (no adapter initialized). */
ZTEST(mb_shell, test_mb_set_slave_needs_init)
{
	expect_command_result("mb set slave 10", -EIO);
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

/* mb factory — returns 0 even without adapter init (no-op path). */
ZTEST(mb_shell, test_mb_factory_is_registered)
{
	expect_command_result("mb factory", 0);
}

ZTEST_SUITE(mb_shell, NULL, NULL, NULL, NULL, NULL);

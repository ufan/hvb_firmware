/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

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

static const char *execute_and_get_output(const char *command)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();
	size_t size;
	int ret;

	shell_backend_dummy_clear_output(shell);
	ret = shell_execute_cmd(shell, command);
	zassert_equal(ret, 0, "command '%s' returned %d", command, ret);
	k_msleep(50);
	return shell_backend_dummy_get_output(shell, &size);
}

ZTEST(vc_shell, test_cal_exit_is_registered)
{
	expect_command_result("vc cal exit", -EIO);
}

ZTEST(vc_shell, test_status_reads_catalog)
{
	expect_command_result("vc status", 0);
}

ZTEST(vc_shell, test_partial_capability_status_reads_supported_measurement)
{
	const char *output = execute_and_get_output("vc ch 1 status");

	zassert_not_null(strstr(output, "current:"), "output: %s", output);
	zassert_is_null(strstr(output, "voltage:"));
}

ZTEST(vc_shell, test_partial_capability_config_omits_output_fields)
{
	const char *output = execute_and_get_output("vc ch 1 config");

	zassert_not_null(strstr(output, "protection:"), "output: %s", output);
	zassert_is_null(strstr(output, "target:"));
	zassert_is_null(strstr(output, "ramp_up:"));
}

ZTEST(vc_shell, test_partial_capability_cal_config_reads_current_only)
{
	const char *output = execute_and_get_output("vc cal config 1");

	zassert_not_null(strstr(output, "i_cal:"), "output: %s", output);
	zassert_is_null(strstr(output, "out_cal:"));
	zassert_is_null(strstr(output, "v_cal:"));
}

ZTEST(vc_shell, test_reset_is_not_registered)
{
	expect_command_result("vc reset", SHELL_CMD_HELP_PRINTED);
}

ZTEST(vc_shell, test_sys_reset_is_not_registered)
{
	expect_command_result("vc sys reset", SHELL_CMD_HELP_PRINTED);
}

ZTEST(vc_shell, test_cal_unlock_enters_cal_mode)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	const char *output;
	size_t size;

	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc cal unlock"), 0,
		      "vc cal unlock failed");
	k_msleep(50);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "Calibration session started"),
			 "missing session start banner: %s", output);

	/* verify mode changed to CAL */
	shell_backend_dummy_clear_output(sh);
	zassert_equal(shell_execute_cmd(sh, "vc sys status"), 0);
	k_msleep(50);
	output = shell_backend_dummy_get_output(sh, &size);
	zassert_not_null(strstr(output, "Mode:        CAL"),
			 "mode not CAL: %s", output);

	/* cleanup */
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}

ZTEST(vc_shell, test_cal_set_max_dac_field)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	(void)shell_execute_cmd(sh, "vc cal unlock");
	k_msleep(50);
	expect_command_result("vc cal set 0 max_dac 1000", 0);
	/* cleanup */
	(void)shell_execute_cmd(sh, "vc cal exit");
	k_msleep(50);
}

ZTEST(vc_shell, test_mode_cal_is_rejected)
{
	expect_command_result("vc mode cal", -EINVAL);
}

static void *vc_shell_setup(void)
{
	const struct shell *shell = shell_backend_dummy_get_ptr();

	ctx = vc_init();
	zassert_not_null(ctx);
	vc_shell_init();
	(void)shell_execute_cmd(shell, "vc status");
	k_msleep(50);
	shell_backend_dummy_clear_output(shell);
	return NULL;
}

static void vc_shell_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_destroy(ctx);
}

ZTEST_SUITE(vc_shell, NULL, vc_shell_setup, NULL, NULL,
	    vc_shell_teardown);

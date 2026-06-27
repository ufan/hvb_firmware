/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "sys_status/sys_status.h"

static int cmd_ss(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct sys_status_snapshot s = sys_status_get();

	shell_print(sh, "uptime:  %u s", s.uptime);
	shell_print(sh, "temp:    %d.%d C",
		    s.board_temperature / 10, abs(s.board_temperature) % 10);
	shell_print(sh, "humid:   %d.%d %%RH",
		    s.board_humidity / 10, s.board_humidity % 10);
	shell_print(sh, "fw:      %d.%d",
		    s.fw_version_high, s.fw_version_low);
	return 0;
}

#if defined(CONFIG_SYS_RESET)
static int cmd_ss_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = sys_status_request_reset();

	if (ret == 0) {
		shell_print(sh, "system reset requested");
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ss,
	SHELL_CMD(reset, NULL, "Reset system", cmd_ss_reset),
	SHELL_SUBCMD_SET_END
);
#define SS_SUBCOMMANDS (&sub_ss)
#else
#define SS_SUBCOMMANDS NULL
#endif

SHELL_CMD_REGISTER(ss, SS_SUBCOMMANDS,
		   "System status (uptime, temp, humidity, fw)", cmd_ss);

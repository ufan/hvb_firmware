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

SHELL_CMD_REGISTER(ss, NULL, "System status (uptime, temp, humidity, fw)", cmd_ss);

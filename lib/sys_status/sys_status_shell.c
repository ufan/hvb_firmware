/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "sys_status/sys_status.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

static int cmd_ss(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	union reg_value uptime = {};
	union reg_value version = {};

	/* Uptime and firmware version have no hardware dependency and are
	 * always available (see sys_uptime.c) — a genuine failure here is
	 * a real error, unlike temp/humidity below. */
	if (reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME),
		     &uptime) != REG_OK ||
	    reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_FW_VERSION),
		     &version) != REG_OK) {
		return -EIO;
	}

	shell_print(sh, "uptime:  %u s", uptime.u32);

	if (IS_ENABLED(CONFIG_SYS_STATUS)) {
		union reg_value temp = {};
		union reg_value humidity = {};

		if (reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_TEMPERATURE),
			     &temp) == REG_OK &&
		    reg_read(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_HUMIDITY),
			     &humidity) == REG_OK) {
			shell_print(sh, "temp:    %d.%d C",
				    temp.s16 / 10, abs(temp.s16) % 10);
			shell_print(sh, "humid:   %d.%d %%RH",
				    humidity.u16 / 10, humidity.u16 % 10);
		}
	}

	shell_print(sh, "fw:      %u.%u.%u",
		    (unsigned)((version.u32 >> 24) & 0xFFU),
		    (unsigned)((version.u32 >> 16) & 0xFFU),
		    (unsigned)(version.u32 & 0xFFFFU));
	return 0;
}

#if defined(CONFIG_SYS_RESET)
static int cmd_ss_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	union reg_value value = { .u16 = 1U };
	int ret = reg_write(REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_RESET),
			    value, K_NO_WAIT) == REG_OK ? 0 : -EIO;

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
		   "System status (uptime, fw, +temp/humidity if fitted)", cmd_ss);

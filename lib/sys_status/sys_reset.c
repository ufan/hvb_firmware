/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "sys_status/sys_status.h"

__weak void sys_status_platform_reset(void)
{
	sys_reboot(SYS_REBOOT_COLD);
}

static void reset_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_status_platform_reset();
}

K_WORK_DELAYABLE_DEFINE(reset_work, reset_work_handler);

int sys_status_request_reset(void)
{
	(void)k_work_reschedule(&reset_work,
				 K_MSEC(CONFIG_SYS_RESET_DELAY_MS));
	return 0;
}

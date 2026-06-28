/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "sys_status/sys_status.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

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

static enum reg_status reset_reg_write(const struct reg_descriptor *desc,
				       union reg_value value,
				       k_timeout_t timeout)
{
	ARG_UNUSED(desc);
	ARG_UNUSED(timeout);

	if (value.u16 != 1U) {
		return REG_INVALID_VALUE;
	}
	return sys_status_request_reset() == 0 ? REG_OK : REG_IO_ERROR;
}

static const struct reg_owner reset_reg_owner = {
	.write = reset_reg_write,
};

REG_DESCRIPTOR_DEFINE(sys_status_reset_reg,
	REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_RESET),
	REG_U16, REG_WO, REG_COMMAND, NULL, &reset_reg_owner);

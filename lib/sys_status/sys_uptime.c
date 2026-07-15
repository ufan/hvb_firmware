/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uptime and firmware version are plain software facts with no hardware
 * dependency — unlike the rest of sys_status.c (SYS_RUN LED, SHT3x sensor),
 * they must stay available on every board variant, including ones without
 * an environment sensor or status LED (e.g. jw_lvb). Kept as a separate,
 * unconditionally-compiled translation unit for exactly that reason.
 */

#include <zephyr/kernel.h>

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

#define SYS_STATUS_FW_VERSION_HIGH 0
#define SYS_STATUS_FW_VERSION_LOW  1

static const uint32_t firmware_version =
	((uint32_t)SYS_STATUS_FW_VERSION_HIGH << 16) | SYS_STATUS_FW_VERSION_LOW;

static enum reg_status sys_uptime_reg_read(const struct reg_descriptor *desc,
					   union reg_value *value)
{
	switch (REG_ID_FIELD(desc->id)) {
	case REG_SYS_STATUS_FIELD_UPTIME:
		value->u32 = (uint32_t)(k_uptime_get() / 1000);
		return REG_OK;
	default:
		return REG_NOT_FOUND;
	}
}

static const struct reg_owner sys_uptime_reg_owner = {
	.read = sys_uptime_reg_read,
};

REG_DESCRIPTOR_DEFINE(sys_status_uptime_reg,
	REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME),
	REG_U32, REG_RO, REG_RUNTIME_STATE, NULL, &sys_uptime_reg_owner);
REG_DESCRIPTOR_DEFINE(sys_status_firmware_version_reg,
	REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_FW_VERSION),
	REG_U32, REG_RO, REG_FIXED, &firmware_version, NULL);

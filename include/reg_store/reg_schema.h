/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef REG_STORE_REG_SCHEMA_H
#define REG_STORE_REG_SCHEMA_H

#include "reg_store/reg_catalog.h"

enum reg_module_id {
	REG_MODULE_INVALID = 0,
	REG_MODULE_SYSTEM_STATUS = 1,
	REG_MODULE_VOLTAGE_CONTROL = 2,
	REG_MODULE_MODBUS_ADAPTER = 3,
	/* Transitional alias removed after mixed-system callers migrate. */
	REG_MODULE_SYSTEM = REG_MODULE_SYSTEM_STATUS,
};

#define REG_GLOBAL_INSTANCE UINT8_MAX

enum reg_system_field {
#define SYS_REG16(name, field, type, access, category, bank, offset) \
	REG_SYS_FIELD_##name = field,
#define SYS_REG32(name, field, type, access, category, bank, offset) \
	REG_SYS_FIELD_##name = field,
#include "reg_store/system_regs.def"
#undef SYS_REG16
#undef SYS_REG32
};

enum reg_vc_field {
#define VC_REG16(name, field, type, access, category, bank, offset) \
	REG_VC_FIELD_##name = field,
#define VC_REG32(name, field, type, access, category, bank, offset) \
	REG_VC_FIELD_##name = field,
#include "reg_store/vc_regs.def"
#undef VC_REG16
#undef VC_REG32
};

enum reg_sys_status_field {
#define SYS_STATUS_REG(name, field, type, access, category) \
	REG_SYS_STATUS_FIELD_##name = field,
#include "reg_store/sys_status_regs.def"
#undef SYS_STATUS_REG
};

#define REG_SYS_ID(field) REG_ID(REG_MODULE_SYSTEM, 0, (field))
#define REG_VC_ID(channel, field) \
	REG_ID(REG_MODULE_VOLTAGE_CONTROL, (channel), (field))
#define REG_SYS_STATUS_ID(field) \
	REG_ID(REG_MODULE_SYSTEM_STATUS, 0, (field))

#define REG_MODBUS_PROTOCOL_MAJOR_ID \
	REG_ID(REG_MODULE_MODBUS_ADAPTER, 0, REG_SYS_FIELD_PROTOCOL_MAJOR)
#define REG_SYS_STATUS_UPTIME_ID \
	REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME)
#define REG_VC_GLOBAL_SUPPORTED_CHANNELS_ID \
	REG_ID(REG_MODULE_VOLTAGE_CONTROL, REG_GLOBAL_INSTANCE, \
	       REG_SYS_FIELD_SUPPORTED_CHANNELS)

#define REG_SYS_PROTOCOL_MAJOR_ID \
	REG_SYS_ID(REG_SYS_FIELD_PROTOCOL_MAJOR)
#define REG_VC_STATUS_BITS_ID(channel) \
	REG_VC_ID((channel), REG_VC_FIELD_STATUS_BITS)

#endif /* REG_STORE_REG_SCHEMA_H */

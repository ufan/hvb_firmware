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
};

#define REG_GLOBAL_INSTANCE UINT8_MAX

enum reg_vc_field {
#define VC_REG16(name, field, type, access, category) \
	REG_VC_FIELD_##name = field,
#define VC_REG32(name, field, type, access, category) \
	REG_VC_FIELD_##name = field,
#include "reg_store/vc_regs.def"
#undef VC_REG16
#undef VC_REG32
};

enum reg_vc_ordinal {
#define VC_REG16(name, field, type, access, category) \
	REG_VC_ORD_##name,
#define VC_REG32 VC_REG16
#include "reg_store/vc_regs.def"
#undef VC_REG16
#undef VC_REG32
	REG_VC_ORD_COUNT,
};

enum reg_vc_global_field {
#define VC_GLOBAL_REG(name, field, type, access, category) \
	REG_VC_GLOBAL_FIELD_##name = field,
#include "reg_store/vc_global_regs.def"
#undef VC_GLOBAL_REG
};

enum reg_vc_global_ordinal {
#define VC_GLOBAL_REG(name, field, type, access, category) \
	REG_VC_GLOBAL_ORD_##name,
#include "reg_store/vc_global_regs.def"
#undef VC_GLOBAL_REG
	REG_VC_GLOBAL_ORD_COUNT,
};

enum reg_sys_status_field {
#define SYS_STATUS_REG(name, field, type, access, category) \
	REG_SYS_STATUS_FIELD_##name = field,
#include "reg_store/sys_status_regs.def"
#undef SYS_STATUS_REG
};

enum reg_modbus_field {
#define MODBUS_REG(name, field, type, access, category) \
	REG_MODBUS_FIELD_##name = field,
#include "reg_store/modbus_regs.def"
#undef MODBUS_REG
};

#define REG_VC_ID(channel, field) \
	REG_ID(REG_MODULE_VOLTAGE_CONTROL, (channel), (field))
#define REG_VC_GLOBAL_ID(field) \
	REG_ID(REG_MODULE_VOLTAGE_CONTROL, REG_GLOBAL_INSTANCE, (field))
#define REG_SYS_STATUS_ID(field) \
	REG_ID(REG_MODULE_SYSTEM_STATUS, 0, (field))
#define REG_MODBUS_ID(field) \
	REG_ID(REG_MODULE_MODBUS_ADAPTER, 0, (field))

#define REG_MODBUS_PROTOCOL_MAJOR_ID \
	REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MAJOR)
#define REG_SYS_STATUS_UPTIME_ID \
	REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME)
#define REG_VC_GLOBAL_SUPPORTED_CHANNELS_ID \
	REG_VC_GLOBAL_ID(REG_VC_GLOBAL_FIELD_SUPPORTED_CHANNELS)

#define REG_VC_STATUS_BITS_ID(channel) \
	REG_VC_ID((channel), REG_VC_FIELD_STATUS_BITS)

reg_handle_t reg_vc_channel_handle(uint8_t channel,
				   enum reg_vc_ordinal ordinal);
reg_handle_t reg_vc_global_handle(enum reg_vc_global_ordinal ordinal);

#endif /* REG_STORE_REG_SCHEMA_H */

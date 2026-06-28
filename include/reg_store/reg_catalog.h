/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef REG_STORE_REG_CATALOG_H
#define REG_STORE_REG_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>

typedef uint32_t reg_id_t;

#define REG_ID(module, instance, field) \
	((((reg_id_t)(module) & 0xffU) << 24) | \
	 (((reg_id_t)(instance) & 0xffU) << 16) | \
	 ((reg_id_t)(field) & 0xffffU))
#define REG_ID_MODULE(id)   (((id) >> 24) & 0xffU)
#define REG_ID_INSTANCE(id) (((id) >> 16) & 0xffU)
#define REG_ID_FIELD(id)    ((id) & 0xffffU)

enum reg_type {
	REG_U16,
	REG_S16,
	REG_U32,
	REG_S32,
	REG_BOOL,
	REG_ENUM,
};

enum reg_access {
	REG_RO,
	REG_RW,
	REG_WO,
};

enum reg_category {
	REG_MEASUREMENT_RAW,
	REG_MEASUREMENT_DERIVED,
	REG_RUNTIME_STATE,
	REG_FIXED,
	REG_CONFIG,
	REG_COMMAND,
};

enum reg_status {
	REG_OK = 0,
	REG_NOT_FOUND = -1,
	REG_INVALID_ARGUMENT = -2,
	REG_INVALID_VALUE = -3,
	REG_READ_ONLY = -4,
	REG_WRITE_ONLY = -5,
	REG_UNSUPPORTED = -6,
	REG_BUSY = -7,
	REG_IO_ERROR = -8,
};

union reg_value {
	uint16_t u16;
	int16_t s16;
	uint32_t u32;
	int32_t s32;
	bool boolean;
};

struct reg_descriptor;
typedef const struct reg_descriptor *reg_handle_t;

struct reg_owner {
	enum reg_status (*read)(const struct reg_descriptor *desc,
				union reg_value *value);
	enum reg_status (*write)(const struct reg_descriptor *desc,
				 union reg_value value,
				 k_timeout_t timeout);
};

struct reg_descriptor {
	reg_id_t id;
	enum reg_type type;
	enum reg_access access;
	enum reg_category category;
	void *value;
	const struct reg_owner *owner;
};

#define REG_DESCRIPTOR_DEFINE(name, reg_id, reg_type_, reg_access_, \
			      reg_category_, value_ptr_, owner_ptr_) \
	const STRUCT_SECTION_ITERABLE(reg_descriptor, name) = { \
		.id = (reg_id), \
		.type = (reg_type_), \
		.access = (reg_access_), \
		.category = (reg_category_), \
		.value = (void *)(value_ptr_), \
		.owner = (owner_ptr_), \
	}

const struct reg_descriptor *reg_describe(reg_id_t id);
enum reg_status reg_read_descriptor(const struct reg_descriptor *desc,
				    union reg_value *out);
enum reg_status reg_write_descriptor(const struct reg_descriptor *desc,
				     union reg_value value,
				     k_timeout_t timeout);
enum reg_status reg_read(reg_id_t id, union reg_value *out);
enum reg_status reg_write(reg_id_t id, union reg_value value,
			  k_timeout_t timeout);

static inline enum reg_status reg_handle_read(reg_handle_t handle,
					       union reg_value *out)
{
	return reg_read_descriptor(handle, out);
}

static inline enum reg_status reg_handle_write(reg_handle_t handle,
						union reg_value value,
						k_timeout_t timeout)
{
	return reg_write_descriptor(handle, value, timeout);
}

#endif /* REG_STORE_REG_CATALOG_H */

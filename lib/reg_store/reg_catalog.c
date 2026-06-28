/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "reg_store/reg_catalog.h"

const struct reg_descriptor *reg_describe(reg_id_t id)
{
	STRUCT_SECTION_FOREACH(reg_descriptor, desc) {
		if (desc->id == id) {
			return desc;
		}
	}

	return NULL;
}

static enum reg_status read_direct(const struct reg_descriptor *desc,
				   union reg_value *out)
{
	if (desc->value == NULL) {
		return REG_INVALID_ARGUMENT;
	}

	memset(out, 0, sizeof(*out));
	switch (desc->type) {
	case REG_U16:
	case REG_ENUM:
		out->u16 = *(const uint16_t *)desc->value;
		break;
	case REG_S16:
		out->s16 = *(const int16_t *)desc->value;
		break;
	case REG_U32:
		out->u32 = *(const uint32_t *)desc->value;
		break;
	case REG_S32:
		out->s32 = *(const int32_t *)desc->value;
		break;
	case REG_BOOL:
		out->boolean = *(const bool *)desc->value;
		break;
	default:
		return REG_INVALID_ARGUMENT;
	}

	return REG_OK;
}

enum reg_status reg_read_descriptor(const struct reg_descriptor *desc,
				    union reg_value *out)
{
	if (desc == NULL || out == NULL) {
		return REG_INVALID_ARGUMENT;
	}
	if (desc->access == REG_WO) {
		return REG_WRITE_ONLY;
	}
	if (desc->owner != NULL && desc->owner->read != NULL) {
		return desc->owner->read(desc, out);
	}

	return read_direct(desc, out);
}

enum reg_status reg_read(reg_id_t id, union reg_value *out)
{
	const struct reg_descriptor *desc = reg_describe(id);

	if (desc == NULL) {
		return REG_NOT_FOUND;
	}

	return reg_read_descriptor(desc, out);
}

enum reg_status reg_write_descriptor(const struct reg_descriptor *desc,
				     union reg_value value,
				     k_timeout_t timeout)
{
	if (desc == NULL) {
		return REG_INVALID_ARGUMENT;
	}
	if (desc->access == REG_RO) {
		return REG_READ_ONLY;
	}
	if (desc->owner == NULL || desc->owner->write == NULL) {
		return REG_UNSUPPORTED;
	}

	return desc->owner->write(desc, value, timeout);
}

enum reg_status reg_write(reg_id_t id, union reg_value value,
			  k_timeout_t timeout)
{
	const struct reg_descriptor *desc = reg_describe(id);

	if (desc == NULL) {
		return REG_NOT_FOUND;
	}

	return reg_write_descriptor(desc, value, timeout);
}

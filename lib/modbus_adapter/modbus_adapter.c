/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/sys/util.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#ifdef CONFIG_MODBUS
#include <zephyr/drivers/uart.h>
#include <zephyr/modbus/modbus.h>
#endif

#include "modbus_adapter/modbus_adapter.h"
#include "modbus_register.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"
#include "sys_status/sys_status.h"

#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif

#ifndef CONFIG_VC_MODBUS_UNIT_ID
#define CONFIG_VC_MODBUS_UNIT_ID 1
#endif

#ifndef CONFIG_VC_MODBUS_BAUD_RATE
#define CONFIG_VC_MODBUS_BAUD_RATE 115200
#endif

#define EXT_BLOCK_END (EXT_BLOCK_BASE + EXT_BLOCK_SIZE - 1)
#define MB_CMD_TIMEOUT K_SECONDS(1)

struct vc_mb_adapter {
	struct vc_ctx *ctx;
	struct k_mutex lock;
	struct mb_adapter_config active_cfg;
	struct mb_adapter_config boot_cfg;
};

static struct vc_mb_adapter adapter;
static struct vc_mb_adapter *mb;
static const uint16_t protocol_major = VC_PROTOCOL_MAJOR;
static const uint16_t protocol_minor = VC_PROTOCOL_MINOR;
static int adapter_write_next_boot_field(uint16_t field, uint16_t value);
static int modbus_adapter_set_slave_address(uint16_t addr);
static int modbus_adapter_set_baud_rate_code(uint16_t code);
static int modbus_adapter_config_save(void);
static int modbus_adapter_config_load(void);
static int modbus_adapter_config_factory(void);

static enum reg_status errno_to_reg_status(int ret)
{
	if (ret == 0) {
		return REG_OK;
	}
	if (ret == -EINVAL) {
		return REG_INVALID_VALUE;
	}
	if (ret == -ENOTSUP || ret == -ENODEV) {
		return REG_UNSUPPORTED;
	}
	return REG_IO_ERROR;
}

static enum reg_status modbus_reg_read(const struct reg_descriptor *desc,
				       union reg_value *value)
{
	if (mb == NULL) {
		return REG_BUSY;
	}

	k_mutex_lock(&mb->lock, K_FOREVER);
	switch (REG_ID_FIELD(desc->id)) {
	case REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS:
		value->u16 = mb->active_cfg.slave_address;
		break;
	case REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE:
		value->u16 = mb->active_cfg.baud_rate_code;
		break;
	case REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS:
		value->u16 = mb->boot_cfg.slave_address;
		break;
	case REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE:
		value->u16 = mb->boot_cfg.baud_rate_code;
		break;
	default:
		k_mutex_unlock(&mb->lock);
		return REG_NOT_FOUND;
	}
	k_mutex_unlock(&mb->lock);
	return REG_OK;
}

static enum reg_status modbus_reg_write(const struct reg_descriptor *desc,
					union reg_value value,
					k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	switch (REG_ID_FIELD(desc->id)) {
	case REG_MODBUS_FIELD_ACTIVE_SLAVE_ADDRESS:
		return errno_to_reg_status(
			modbus_adapter_set_slave_address(value.u16));
	case REG_MODBUS_FIELD_ACTIVE_BAUD_RATE_CODE:
		return errno_to_reg_status(
			modbus_adapter_set_baud_rate_code(value.u16));
	case REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS:
	case REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE:
		return errno_to_reg_status(adapter_write_next_boot_field(
			(uint16_t)REG_ID_FIELD(desc->id), value.u16));
	case REG_MODBUS_FIELD_CONFIG_SAVE:
		return value.u16 == 1U
			? errno_to_reg_status(modbus_adapter_config_save())
			: REG_INVALID_VALUE;
	case REG_MODBUS_FIELD_CONFIG_LOAD:
		return value.u16 == 1U
			? errno_to_reg_status(modbus_adapter_config_load())
			: REG_INVALID_VALUE;
	case REG_MODBUS_FIELD_CONFIG_FACTORY:
		return value.u16 == 1U
			? errno_to_reg_status(modbus_adapter_config_factory())
			: REG_INVALID_VALUE;
	default:
		return REG_UNSUPPORTED;
	}
}

static const struct reg_owner modbus_reg_owner = {
	.read = modbus_reg_read,
	.write = modbus_reg_write,
};

REG_DESCRIPTOR_DEFINE(modbus_protocol_major_reg,
	REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MAJOR),
	REG_U16, REG_RO, REG_FIXED, &protocol_major, NULL);
REG_DESCRIPTOR_DEFINE(modbus_protocol_minor_reg,
	REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MINOR),
	REG_U16, REG_RO, REG_FIXED, &protocol_minor, NULL);

#define MODBUS_CONFIG_DESCRIPTOR(name, type_, access_, category_) \
	REG_DESCRIPTOR_DEFINE(modbus_##name##_reg, \
		REG_MODBUS_ID(REG_MODBUS_FIELD_##name), REG_##type_, \
		REG_##access_, REG_##category_, NULL, &modbus_reg_owner)

MODBUS_CONFIG_DESCRIPTOR(ACTIVE_SLAVE_ADDRESS, U16, RW, CONFIG);
MODBUS_CONFIG_DESCRIPTOR(ACTIVE_BAUD_RATE_CODE, U16, RW, CONFIG);
MODBUS_CONFIG_DESCRIPTOR(NEXT_BOOT_SLAVE_ADDRESS, U16, RW, CONFIG);
MODBUS_CONFIG_DESCRIPTOR(NEXT_BOOT_BAUD_RATE_CODE, U16, RW, CONFIG);
MODBUS_CONFIG_DESCRIPTOR(CONFIG_SAVE, U16, WO, COMMAND);
MODBUS_CONFIG_DESCRIPTOR(CONFIG_LOAD, U16, WO, COMMAND);
MODBUS_CONFIG_DESCRIPTOR(CONFIG_FACTORY, U16, WO, COMMAND);

#if defined(CONFIG_MODBUS)
static uint32_t baud_code_to_rate(uint16_t code)
{
	switch (code) {
	case VC_BAUD_RATE_9600:   return 9600;
	default:                  return 115200;
	}
}
#endif

BUILD_ASSERT(CONFIG_VC_MODBUS_BAUD_RATE == 9600 ||
	     CONFIG_VC_MODBUS_BAUD_RATE == 115200,
	     "CONFIG_VC_MODBUS_BAUD_RATE must be 9600 or 115200");

static struct mb_adapter_config adapter_default_config(void)
{
	return (struct mb_adapter_config){
		.slave_address = CONFIG_VC_MODBUS_UNIT_ID,
		.baud_rate_code = CONFIG_VC_MODBUS_BAUD_RATE == 9600
			? VC_BAUD_RATE_9600 : VC_BAUD_RATE_115200,
	};
}

static bool adapter_config_valid(const struct mb_adapter_config *cfg)
{
	return cfg->slave_address >= 1 && cfg->slave_address <= 247 &&
		(cfg->baud_rate_code == VC_BAUD_RATE_115200 ||
		 cfg->baud_rate_code == VC_BAUD_RATE_9600);
}

#if defined(CONFIG_SETTINGS)
static int adapter_save_config(const struct mb_adapter_config *cfg);
#endif

/* ------------------------------------------------------------------ */
/* Address decode                                                      */
/* ------------------------------------------------------------------ */

static bool addr_decode(uint16_t addr, bool *is_sys, uint8_t *ch, uint16_t *off)
{
	if (addr < SYS_BLOCK_BASE + CH_BLOCK_SIZE) {
		*is_sys = true;
		*off = addr;
		return true;
	}

	if (addr >= CH_BLOCK_BASE(0) && addr < CH_BLOCK_BASE(16)) {
		uint16_t rel = addr - CH_BLOCK_BASE(0);
		*is_sys = false;
		*ch = (uint8_t)(rel / CH_BLOCK_SIZE);
		*off = rel % CH_BLOCK_SIZE;
		return true;
	}

	return false;
}

static bool is_extension(uint16_t addr)
{
	return addr >= EXT_BLOCK_BASE && addr <= EXT_BLOCK_END;
}

/* ------------------------------------------------------------------ */
/* vc_status → vc_mb_result mapping                                    */
/* ------------------------------------------------------------------ */

static enum vc_mb_result domain_st_to_mb_result(enum vc_status st)
{
	switch (st) {
	case VC_OK:
		return VC_MB_OK;
	case VC_ERR_UNSUPPORTED_CHANNEL:
	case VC_ERR_UNSUPPORTED_CAPABILITY:
		return VC_MB_ILLEGAL_ADDRESS;
	case VC_ERR_INVALID_VALUE:
	case VC_ERR_INVALID_COMMAND:
	case VC_ERR_UNSAFE_STATE:
		return VC_MB_ILLEGAL_VALUE;
	case VC_ERR_STORAGE:
	default:
		return VC_MB_DEVICE_FAILURE;
	}
}

static enum vc_mb_result reg_st_to_mb_result(enum reg_status st)
{
	switch (st) {
	case REG_OK:
		return VC_MB_OK;
	case REG_NOT_FOUND:
	case REG_READ_ONLY:
	case REG_WRITE_ONLY:
	case REG_UNSUPPORTED:
		return VC_MB_ILLEGAL_ADDRESS;
	case REG_INVALID_ARGUMENT:
	case REG_INVALID_VALUE:
		return VC_MB_ILLEGAL_VALUE;
	default:
		return VC_MB_DEVICE_FAILURE;
	}
}

/* ------------------------------------------------------------------ */
/* System register multiplexing                                        */
/* ------------------------------------------------------------------ */

static enum vc_mb_result read_sys_input(struct vc_mb_adapter *a, uint16_t off,
					uint16_t *reg)
{
	ARG_UNUSED(a);
	reg_id_t id;
	union reg_value value = {};

	switch (off) {
	case SYS_PROTOCOL_MAJOR:
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MAJOR);
		break;
	case SYS_PROTOCOL_MINOR:
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_PROTOCOL_MINOR);
		break;
	case SYS_BOARD_TEMPERATURE:
		id = REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_TEMPERATURE);
		break;
	case SYS_BOARD_HUMIDITY:
		id = REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_BOARD_HUMIDITY);
		break;
	case SYS_UPTIME_HI:
	case SYS_UPTIME_LO:
		id = REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_UPTIME);
		break;
	case SYS_FW_VERSION_HI:
	case SYS_FW_VERSION_LO:
		id = REG_SYS_STATUS_ID(REG_SYS_STATUS_FIELD_FW_VERSION);
		break;
	default:
		return domain_st_to_mb_result(vc_reg_read_sys_input(off, reg));
	}

	enum reg_status status = reg_read(id, &value);

	if (status != REG_OK) {
		return reg_st_to_mb_result(status);
	}
	if (off == SYS_UPTIME_HI || off == SYS_FW_VERSION_HI) {
		*reg = (uint16_t)(value.u32 >> 16);
	} else if (off == SYS_UPTIME_LO || off == SYS_FW_VERSION_LO) {
		*reg = (uint16_t)value.u32;
	} else if (off == SYS_BOARD_TEMPERATURE) {
		*reg = (uint16_t)value.s16;
	} else {
		*reg = value.u16;
	}
	return VC_MB_OK;
}

static enum vc_mb_result read_sys_holding(struct vc_mb_adapter *a,
					  uint16_t off, uint16_t *reg)
{
	union reg_value value = {};
	reg_id_t id;

	if (off == SYS_SLAVE_ADDRESS) {
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS);
		goto read_config;
	}
	if (off == SYS_BAUD_RATE_CODE) {
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE);
		goto read_config;
	}
	return domain_st_to_mb_result(vc_reg_read_sys_holding(off, reg));

read_config:
	ARG_UNUSED(a);
	enum reg_status status = reg_read(id, &value);

	if (status == REG_OK) {
		*reg = value.u16;
	}
	return reg_st_to_mb_result(status);
}

static enum vc_mb_result write_sys_holding(struct vc_mb_adapter *a,
					   uint16_t off, uint16_t val)
{
	union reg_value value = { .u16 = val };
	reg_id_t id;

	if (off == SYS_SLAVE_ADDRESS) {
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS);
		goto write_config;
	}
	if (off == SYS_BAUD_RATE_CODE) {
		id = REG_MODBUS_ID(REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE);
		goto write_config;
	}
	return domain_st_to_mb_result(
		vc_reg_write_sys_holding(a->ctx, off, val, MB_CMD_TIMEOUT));

write_config:
	return reg_st_to_mb_result(reg_write(id, value, MB_CMD_TIMEOUT));
}

/* ------------------------------------------------------------------ */
/* Adapter config persistence                                          */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_SETTINGS

static int adapter_save_config(const struct mb_adapter_config *cfg)
{
	return settings_save_one("mb/cfg", cfg, sizeof(*cfg));
}

struct adapter_load_ctx {
	void *dst;
	size_t len;
	bool found;
};

static int adapter_settings_loader(const char *name, size_t len,
				   settings_read_cb read_cb, void *cb_arg,
				   void *param)
{
	struct adapter_load_ctx *ctx = param;

	if (len != ctx->len) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, ctx->dst, len) < 0) {
		return -EIO;
	}
	ctx->found = true;
	return 0;
}

static int adapter_load_config(struct mb_adapter_config *cfg)
{
	struct adapter_load_ctx ctx = {
		.dst = cfg,
		.len = sizeof(*cfg),
		.found = false,
	};
	int rc = settings_load_subtree_direct("mb/cfg", adapter_settings_loader,
					      &ctx);

	if (rc < 0) {
		return rc;
	}
	return ctx.found ? 0 : -ENOENT;
}

#else /* !CONFIG_SETTINGS */

static int adapter_load_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENOENT;
}

#endif /* CONFIG_SETTINGS */

static int adapter_write_next_boot_field(uint16_t field, uint16_t value)
{
#if !defined(CONFIG_SETTINGS)
	ARG_UNUSED(field);
	ARG_UNUSED(value);
	return -ENOTSUP;
#else
	if (mb == NULL) {
		return -ENODEV;
	}

	struct mb_adapter_config candidate;

	k_mutex_lock(&mb->lock, K_FOREVER);
	candidate = mb->boot_cfg;
	if (field == REG_MODBUS_FIELD_NEXT_BOOT_SLAVE_ADDRESS) {
		candidate.slave_address = value;
	} else if (field == REG_MODBUS_FIELD_NEXT_BOOT_BAUD_RATE_CODE) {
		candidate.baud_rate_code = value;
	} else {
		k_mutex_unlock(&mb->lock);
		return -EINVAL;
	}
	if (!adapter_config_valid(&candidate)) {
		k_mutex_unlock(&mb->lock);
		return -EINVAL;
	}
	int ret = adapter_save_config(&candidate);

	if (ret == 0) {
		mb->boot_cfg = candidate;
	}
	k_mutex_unlock(&mb->lock);
	return ret;
#endif
}

/* ------------------------------------------------------------------ */
/* Param action: adapter saves its own config, then delegates to VC    */
/* ------------------------------------------------------------------ */

static enum vc_mb_result handle_sys_param_action(struct vc_mb_adapter *a,
						 uint16_t val)
{
	if (val == SYS_PARAM_ACTION_SOFTWARE_RESET) {
#if defined(CONFIG_SYS_RESET)
		return sys_status_request_reset() == 0
			? VC_MB_OK : VC_MB_DEVICE_FAILURE;
#else
		return VC_MB_DEVICE_FAILURE;
#endif
	}

	enum vc_param_action action = (enum vc_param_action)val;

	switch (action) {
	case VC_PARAM_ACTION_SAVE:
		break;
	case VC_PARAM_ACTION_LOAD:
	{
		struct mb_adapter_config candidate;

		if (adapter_load_config(&candidate) < 0 ||
		    !adapter_config_valid(&candidate)) {
			return VC_MB_DEVICE_FAILURE;
		}
		k_mutex_lock(&a->lock, K_FOREVER);
		a->boot_cfg = candidate;
		k_mutex_unlock(&a->lock);
		break;
	}
	case VC_PARAM_ACTION_FACTORY_RESET:
	{
#if defined(CONFIG_SETTINGS)
		struct mb_adapter_config defaults = adapter_default_config();

		if (adapter_save_config(&defaults) < 0) {
			return VC_MB_DEVICE_FAILURE;
		}
		k_mutex_lock(&a->lock, K_FOREVER);
		a->boot_cfg = defaults;
		k_mutex_unlock(&a->lock);
#endif
		break;
	}
	default:
		break;
	}

	enum vc_mb_result res = domain_st_to_mb_result(
		vc_reg_write_sys_holding(a->ctx, SYS_PARAM_ACTION, val,
					 MB_CMD_TIMEOUT));

	return res;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

enum vc_mb_result vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr,
				 uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		return read_sys_input(a, off, reg);
	}

	return domain_st_to_mb_result(
		vc_reg_read_ch_input(ch, off, reg));
}

enum vc_mb_result vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t *reg)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		*reg = 0;
		return VC_MB_OK;
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		return read_sys_holding(a, off, reg);
	}

	return domain_st_to_mb_result(
		vc_reg_read_ch_holding(ch, off, reg));
}

enum vc_mb_result vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t val)
{
	bool is_sys;
	uint8_t ch;
	uint16_t off;

	if (is_extension(addr)) {
		return domain_st_to_mb_result(
			vc_reg_write_ext(a->ctx, addr - EXT_BLOCK_BASE, val,
					 MB_CMD_TIMEOUT));
	}

	if (!addr_decode(addr, &is_sys, &ch, &off)) {
		return VC_MB_ILLEGAL_ADDRESS;
	}

	if (is_sys) {
		if (off == SYS_PARAM_ACTION) {
			return handle_sys_param_action(a, val);
		}
		return write_sys_holding(a, off, val);
	}

	return domain_st_to_mb_result(
		vc_reg_write_ch_holding(a->ctx, ch, off, val, MB_CMD_TIMEOUT));
}

struct vc_mb_adapter *vc_mb_adapter_create(struct vc_ctx *ctx)
{
	struct mb_adapter_config defaults = adapter_default_config();

	mb = &adapter;
	k_mutex_init(&mb->lock);
	mb->ctx = ctx;
	mb->boot_cfg = defaults;
	if (adapter_load_config(&mb->boot_cfg) < 0 ||
	    !adapter_config_valid(&mb->boot_cfg)) {
		mb->boot_cfg = defaults;
#if defined(CONFIG_SETTINGS)
		if (adapter_save_config(&mb->boot_cfg) < 0) {
			return NULL;
		}
#endif
	}
	mb->active_cfg = mb->boot_cfg;

	return mb;
}

/* ------------------------------------------------------------------ */
/* Modbus RTU server init                                              */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MODBUS

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static int mb_iface = -1;

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	return vc_mb_input_rd(mb, addr, reg) ? -EINVAL : 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	return vc_mb_holding_rd(mb, addr, reg) ? -EINVAL : 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	return vc_mb_holding_wr(mb, addr, reg) ? -EINVAL : 0;
}

static struct modbus_user_callbacks modbus_callbacks = {
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static int modbus_server_start(uint8_t unit_id, uint32_t baud)
{
	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RTU,
		.server = {
			.user_cb = &modbus_callbacks,
			.unit_id = unit_id,
		},
		.serial = {
			.baud = baud,
			.parity = UART_CFG_PARITY_NONE,
		},
	};

	return modbus_init_server(mb_iface, param);
}

static int modbus_adapter_apply_config(const struct mb_adapter_config *cfg)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (mb_iface < 0) {
		mb->active_cfg = *cfg;
		return 0;
	}

	struct mb_adapter_config previous = mb->active_cfg;
	(void)modbus_disable(mb_iface);

	int ret = modbus_server_start(cfg->slave_address,
				      baud_code_to_rate(cfg->baud_rate_code));
	if (ret < 0) {
		(void)modbus_server_start(previous.slave_address,
					  baud_code_to_rate(previous.baud_rate_code));
		printk("modbus reconfig failed: %d\n", ret);
		return ret;
	}
	mb->active_cfg = *cfg;

	printk("modbus_adapter: reconfig slave=%d baud=%d\n",
	       cfg->slave_address, baud_code_to_rate(cfg->baud_rate_code));
	return 0;
}

int modbus_adapter_init(struct vc_ctx *ctx)
{
	mb = vc_mb_adapter_create(ctx);
	if (!mb) {
		printk("Failed to create Modbus adapter\n");
		return -ENOMEM;
	}

	const char iface_name[] = DEVICE_DT_NAME(MODBUS_NODE);

	mb_iface = modbus_iface_get_by_name(iface_name);
	if (mb_iface < 0) {
		printk("Failed to get Modbus iface %s: %d\n",
		       iface_name, mb_iface);
		return mb_iface;
	}

	uint8_t unit_id = mb->active_cfg.slave_address;
	uint32_t baud = baud_code_to_rate(mb->active_cfg.baud_rate_code);
	int ret = modbus_server_start(unit_id, baud);

	if (ret < 0) {
		printk("Modbus RTU server init failed: %d\n", ret);
		return ret;
	}

	printk("modbus_adapter: slave=%d baud=%d\n", unit_id, baud);
	return 0;
}

static int modbus_adapter_set_slave_address(uint16_t addr)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (addr < 1 || addr > 247) {
		return -EINVAL;
	}
	k_mutex_lock(&mb->lock, K_FOREVER);
	struct mb_adapter_config candidate = mb->active_cfg;

	candidate.slave_address = addr;
	int ret = modbus_adapter_apply_config(&candidate);
	k_mutex_unlock(&mb->lock);
	return ret;
}

static int modbus_adapter_set_baud_rate_code(uint16_t code)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (code > VC_BAUD_RATE_9600) {
		return -EINVAL;
	}
	struct mb_adapter_config candidate;

	k_mutex_lock(&mb->lock, K_FOREVER);
	candidate = mb->active_cfg;
	candidate.baud_rate_code = code;
	int ret = modbus_adapter_apply_config(&candidate);
	k_mutex_unlock(&mb->lock);
	return ret;
}

static int modbus_adapter_config_save(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
#if !defined(CONFIG_SETTINGS)
	return -ENOTSUP;
#else
	k_mutex_lock(&mb->lock, K_FOREVER);
	int ret = adapter_save_config(&mb->active_cfg);

	if (ret == 0) {
		mb->boot_cfg = mb->active_cfg;
	}
	k_mutex_unlock(&mb->lock);
	return ret;
#endif
}

static int modbus_adapter_config_load(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	struct mb_adapter_config candidate;
	int ret = adapter_load_config(&candidate);

	if (ret < 0) {
		return ret;
	}
	if (!adapter_config_valid(&candidate)) {
		return -EINVAL;
	}
	k_mutex_lock(&mb->lock, K_FOREVER);
	ret = modbus_adapter_apply_config(&candidate);
	if (ret == 0) {
		mb->boot_cfg = candidate;
	}
	k_mutex_unlock(&mb->lock);
	return ret;
}

static int modbus_adapter_config_factory(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	struct mb_adapter_config defaults = adapter_default_config();
	int ret;

	k_mutex_lock(&mb->lock, K_FOREVER);
#if defined(CONFIG_SETTINGS)
	ret = adapter_save_config(&defaults);
	if (ret < 0) {
		k_mutex_unlock(&mb->lock);
		return ret;
	}
	mb->boot_cfg = defaults;
#endif
	ret = modbus_adapter_apply_config(&defaults);
	k_mutex_unlock(&mb->lock);
	return ret;
}

#else /* !CONFIG_MODBUS */

static int modbus_adapter_set_slave_address(uint16_t addr)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (addr < 1U || addr > 247U) {
		return -EINVAL;
	}
	k_mutex_lock(&mb->lock, K_FOREVER);
	mb->active_cfg.slave_address = addr;
	k_mutex_unlock(&mb->lock);
	return 0;
}

static int modbus_adapter_set_baud_rate_code(uint16_t code)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (code > VC_BAUD_RATE_9600) {
		return -EINVAL;
	}
	k_mutex_lock(&mb->lock, K_FOREVER);
	mb->active_cfg.baud_rate_code = code;
	k_mutex_unlock(&mb->lock);
	return 0;
}

static int modbus_adapter_config_save(void)
{
	return -ENODEV;
}

static int modbus_adapter_config_load(void)
{
	return -ENODEV;
}

static int modbus_adapter_config_factory(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	k_mutex_lock(&mb->lock, K_FOREVER);
	mb->active_cfg = adapter_default_config();
	k_mutex_unlock(&mb->lock);
	return 0;
}

#endif /* CONFIG_MODBUS */

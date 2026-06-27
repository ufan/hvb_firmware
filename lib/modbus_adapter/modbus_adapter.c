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
#include "reg_store/reg_map.h"
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
	struct mb_adapter_config active_cfg;
	struct mb_adapter_config boot_cfg;
};

static struct vc_mb_adapter adapter;
static struct vc_mb_adapter *mb;

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

/* ------------------------------------------------------------------ */
/* System register multiplexing                                        */
/* ------------------------------------------------------------------ */

static enum vc_mb_result read_sys_input(struct vc_mb_adapter *a, uint16_t off,
					uint16_t *reg)
{
	ARG_UNUSED(a);
	return domain_st_to_mb_result(vc_reg_read_sys_input(off, reg));
}

static enum vc_mb_result read_sys_holding(struct vc_mb_adapter *a,
					  uint16_t off, uint16_t *reg)
{
	if (off == SYS_SLAVE_ADDRESS) {
		*reg = a->boot_cfg.slave_address;
		return VC_MB_OK;
	}
	if (off == SYS_BAUD_RATE_CODE) {
		*reg = a->boot_cfg.baud_rate_code;
		return VC_MB_OK;
	}
	return domain_st_to_mb_result(vc_reg_read_sys_holding(off, reg));
}

static enum vc_mb_result write_sys_holding(struct vc_mb_adapter *a,
					   uint16_t off, uint16_t val)
{
	if (off == SYS_SLAVE_ADDRESS) {
#if !defined(CONFIG_SETTINGS)
		return VC_MB_ILLEGAL_ADDRESS;
#else
		if (val < 1 || val > 247) {
			return VC_MB_ILLEGAL_VALUE;
		}
		a->boot_cfg.slave_address = val;
		return VC_MB_OK;
#endif
	}
	if (off == SYS_BAUD_RATE_CODE) {
#if !defined(CONFIG_SETTINGS)
		return VC_MB_ILLEGAL_ADDRESS;
#else
		if (val > VC_BAUD_RATE_9600) {
			return VC_MB_ILLEGAL_VALUE;
		}
		a->boot_cfg.baud_rate_code = val;
		return VC_MB_OK;
#endif
	}
	return domain_st_to_mb_result(
		vc_reg_write_sys_holding(a->ctx, off, val, MB_CMD_TIMEOUT));
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

static int adapter_erase_config(void)
{
	return settings_delete("mb/cfg");
}

#else /* !CONFIG_SETTINGS */

static int adapter_load_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENOENT;
}

static int adapter_erase_config(void)
{
	return -ENOTSUP;
}

#endif /* CONFIG_SETTINGS */

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
		if (adapter_load_config(&a->boot_cfg) < 0 ||
		    !adapter_config_valid(&a->boot_cfg)) {
			a->boot_cfg = adapter_default_config();
		}
		break;
	case VC_PARAM_ACTION_FACTORY_RESET:
		(void)adapter_erase_config();
		a->boot_cfg = adapter_default_config();
		break;
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
	mb->ctx = ctx;
	mb->boot_cfg = defaults;
	if (adapter_load_config(&mb->boot_cfg) < 0 ||
	    !adapter_config_valid(&mb->boot_cfg)) {
		mb->boot_cfg = defaults;
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

int modbus_adapter_get_active_config(struct mb_adapter_config *cfg)
{
	if (mb == NULL || cfg == NULL) {
		return -ENODEV;
	}
	*cfg = mb->active_cfg;
	return 0;
}

int modbus_adapter_get_next_boot_config(struct mb_adapter_config *cfg)
{
	if (mb == NULL || cfg == NULL) {
		return -ENODEV;
	}
	*cfg = mb->boot_cfg;
	return 0;
}

bool modbus_adapter_config_is_persistent(void)
{
	return IS_ENABLED(CONFIG_SETTINGS);
}

int modbus_adapter_set_slave_address(uint16_t addr)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (addr < 1 || addr > 247) {
		return -EINVAL;
	}
	struct mb_adapter_config candidate = mb->active_cfg;

	candidate.slave_address = addr;
	return modbus_adapter_apply_config(&candidate);
}

int modbus_adapter_set_baud_rate_code(uint16_t code)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (code > VC_BAUD_RATE_9600) {
		return -EINVAL;
	}
	struct mb_adapter_config candidate = mb->active_cfg;

	candidate.baud_rate_code = code;
	return modbus_adapter_apply_config(&candidate);
}

int modbus_adapter_config_save(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
#if !defined(CONFIG_SETTINGS)
	return -ENOTSUP;
#else
	int ret = adapter_save_config(&mb->active_cfg);

	if (ret == 0) {
		mb->boot_cfg = mb->active_cfg;
	}
	return ret;
#endif
}

int modbus_adapter_config_load(void)
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
	ret = modbus_adapter_apply_config(&candidate);
	if (ret == 0) {
		mb->boot_cfg = candidate;
	}
	return ret;
}

int modbus_adapter_config_factory(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	struct mb_adapter_config defaults = adapter_default_config();
	int ret = adapter_erase_config();

	if (ret != 0 && ret != -ENOTSUP) {
		return ret;
	}
	ret = modbus_adapter_apply_config(&defaults);
	if (ret == 0 && IS_ENABLED(CONFIG_SETTINGS)) {
		mb->boot_cfg = defaults;
	}
	return ret;
}

#else /* !CONFIG_MODBUS */

int modbus_adapter_get_active_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENODEV;
}

int modbus_adapter_get_next_boot_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENODEV;
}

bool modbus_adapter_config_is_persistent(void)
{
	return IS_ENABLED(CONFIG_SETTINGS);
}

int modbus_adapter_set_slave_address(uint16_t addr)
{
	ARG_UNUSED(addr);
	return -ENODEV;
}

int modbus_adapter_set_baud_rate_code(uint16_t code)
{
	ARG_UNUSED(code);
	return -ENODEV;
}

int modbus_adapter_config_save(void)
{
	return -ENODEV;
}

int modbus_adapter_config_load(void)
{
	return -ENODEV;
}

int modbus_adapter_config_factory(void)
{
	return -ENODEV;
}

#endif /* CONFIG_MODBUS */

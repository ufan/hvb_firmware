/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

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
	struct mb_adapter_config cfg;
	bool needs_reconfig;
};

static uint32_t baud_code_to_rate(uint16_t code)
{
	switch (code) {
	case VC_BAUD_RATE_9600:   return 9600;
	default:                  return 115200;
	}
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
		*reg = a->cfg.slave_address;
		return VC_MB_OK;
	}
	if (off == SYS_BAUD_RATE_CODE) {
		*reg = a->cfg.baud_rate_code;
		return VC_MB_OK;
	}
	return domain_st_to_mb_result(vc_reg_read_sys_holding(off, reg));
}

static enum vc_mb_result write_sys_holding(struct vc_mb_adapter *a,
					   uint16_t off, uint16_t val)
{
	if (off == SYS_SLAVE_ADDRESS) {
		if (val < 1 || val > 247) {
			return VC_MB_ILLEGAL_VALUE;
		}
		if (a->cfg.slave_address != val) {
			a->cfg.slave_address = val;
			a->needs_reconfig = true;
		}
		return VC_MB_OK;
	}
	if (off == SYS_BAUD_RATE_CODE) {
		if (val > VC_BAUD_RATE_9600) {
			return VC_MB_ILLEGAL_VALUE;
		}
		if (a->cfg.baud_rate_code != val) {
			a->cfg.baud_rate_code = val;
			a->needs_reconfig = true;
		}
		return VC_MB_OK;
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

static void adapter_erase_config(void)
{
	(void)settings_delete("mb/cfg");
}

#else /* !CONFIG_SETTINGS */

static int adapter_save_config(const struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return 0;
}

static int adapter_load_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
	return -ENOENT;
}

static void adapter_erase_config(void)
{
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
		adapter_save_config(&a->cfg);
		break;
	case VC_PARAM_ACTION_LOAD:
		if (adapter_load_config(&a->cfg) < 0) {
			a->cfg.slave_address = CONFIG_VC_MODBUS_UNIT_ID;
			a->cfg.baud_rate_code = VC_BAUD_RATE_115200;
		}
		a->needs_reconfig = true;
		break;
	case VC_PARAM_ACTION_FACTORY_RESET:
		adapter_erase_config();
		a->cfg.slave_address = CONFIG_VC_MODBUS_UNIT_ID;
		a->cfg.baud_rate_code = VC_BAUD_RATE_115200;
		a->needs_reconfig = true;
		break;
	default:
		break;
	}

	enum vc_mb_result res = domain_st_to_mb_result(
		vc_reg_write_sys_holding(a->ctx, SYS_PARAM_ACTION, val,
					 MB_CMD_TIMEOUT));

	if (a->needs_reconfig) {
		modbus_adapter_apply_config();
	}

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
	static struct vc_mb_adapter adapter;

	adapter.ctx = ctx;
	adapter.cfg.slave_address = CONFIG_VC_MODBUS_UNIT_ID;
	adapter.cfg.baud_rate_code = VC_BAUD_RATE_115200;

	adapter_load_config(&adapter.cfg);

	return &adapter;
}

/* ------------------------------------------------------------------ */
/* Modbus RTU server init                                              */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MODBUS

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static struct vc_mb_adapter *mb;
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

void modbus_adapter_apply_config(void)
{
	if (mb == NULL || mb_iface < 0) {
		return;
	}

	mb->needs_reconfig = false;
	modbus_disable(mb_iface);

	int ret = modbus_server_start(mb->cfg.slave_address,
				      baud_code_to_rate(mb->cfg.baud_rate_code));
	if (ret < 0) {
		printk("modbus reconfig failed: %d\n", ret);
		return;
	}

	printk("modbus_adapter: reconfig slave=%d baud=%d\n",
	       mb->cfg.slave_address,
	       baud_code_to_rate(mb->cfg.baud_rate_code));
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

	uint8_t unit_id = mb->cfg.slave_address;
	uint32_t baud = baud_code_to_rate(mb->cfg.baud_rate_code);
	int ret = modbus_server_start(unit_id, baud);

	if (ret < 0) {
		printk("Modbus RTU server init failed: %d\n", ret);
		return ret;
	}

	printk("modbus_adapter: slave=%d baud=%d\n", unit_id, baud);
	return 0;
}

void modbus_adapter_get_config(struct mb_adapter_config *cfg)
{
	if (mb == NULL || cfg == NULL) {
		return;
	}
	*cfg = mb->cfg;
}

int modbus_adapter_set_slave_address(uint16_t addr)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (addr < 1 || addr > 247) {
		return -EINVAL;
	}
	if (mb->cfg.slave_address != addr) {
		mb->cfg.slave_address = addr;
		mb->needs_reconfig = true;
		modbus_adapter_apply_config();
	}
	return 0;
}

int modbus_adapter_set_baud_rate_code(uint16_t code)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (code > VC_BAUD_RATE_9600) {
		return -EINVAL;
	}
	if (mb->cfg.baud_rate_code != code) {
		mb->cfg.baud_rate_code = code;
		mb->needs_reconfig = true;
		modbus_adapter_apply_config();
	}
	return 0;
}

int modbus_adapter_config_save(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	return adapter_save_config(&mb->cfg);
}

int modbus_adapter_config_load(void)
{
	if (mb == NULL) {
		return -ENODEV;
	}
	if (adapter_load_config(&mb->cfg) < 0) {
		mb->cfg.slave_address = CONFIG_VC_MODBUS_UNIT_ID;
		mb->cfg.baud_rate_code = VC_BAUD_RATE_115200;
	}
	mb->needs_reconfig = true;
	modbus_adapter_apply_config();
	return 0;
}

void modbus_adapter_config_factory(void)
{
	if (mb == NULL) {
		return;
	}
	adapter_erase_config();
	mb->cfg.slave_address = CONFIG_VC_MODBUS_UNIT_ID;
	mb->cfg.baud_rate_code = VC_BAUD_RATE_115200;
	mb->needs_reconfig = true;
	modbus_adapter_apply_config();
}

#else /* !CONFIG_MODBUS */

void modbus_adapter_apply_config(void)
{
}

void modbus_adapter_get_config(struct mb_adapter_config *cfg)
{
	ARG_UNUSED(cfg);
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

void modbus_adapter_config_factory(void)
{
}

#endif /* CONFIG_MODBUS */

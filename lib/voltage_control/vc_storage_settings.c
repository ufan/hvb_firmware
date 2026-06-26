/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "voltage_control/vc_storage.h"

struct settings_read_ctx {
	void *dst;
	size_t len;
	bool found;
};

/* Zephyr settings callback: read exactly ctx->len bytes into ctx->dst. */
static int settings_direct_loader(const char *name, size_t len,
				  settings_read_cb read_cb, void *cb_arg,
				  void *param)
{
	struct settings_read_ctx *ctx = param;

	if (len != ctx->len) {
		return -EINVAL;
	}

	if (read_cb(cb_arg, ctx->dst, len) < 0) {
		return -EIO;
	}

	ctx->found = true;
	return 0;
}

/* Load a settings key directly into a buffer; returns -ENOENT if not found. */
static int settings_load_key(const char *key, void *dst, size_t len)
{
	struct settings_read_ctx ctx = {
		.dst = dst,
		.len = len,
		.found = false,
	};
	int rc = settings_load_subtree_direct(key, settings_direct_loader, &ctx);

	if (rc < 0) {
		return rc;
	}
	return ctx.found ? 0 : -ENOENT;
}

/* Persist system config to NVS key "vc/sys". */
static int settings_save_sys(const struct vc_system_config *cfg)
{
	return settings_save_one("vc/sys", cfg, sizeof(*cfg));
}

/* Load system config from NVS key "vc/sys". */
static int settings_load_sys(struct vc_system_config *cfg)
{
	return settings_load_key("vc/sys", cfg, sizeof(*cfg));
}

/* Persist channel operational config to "vc/chN/cfg" (direct struct, no cal). */
static int settings_save_ch_cfg(uint8_t ch, const struct vc_channel_config *cfg)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	return settings_save_one(key, cfg, sizeof(*cfg));
}

/* Load channel operational config from "vc/chN/cfg". */
static int settings_load_ch_cfg(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	return settings_load_key(key, cfg, sizeof(*cfg));
}

/* Persist channel calibration config to "vc/chN/cal". */
static int settings_save_ch_cal(uint8_t ch, const struct vc_channel_cal_config *cal)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	return settings_save_one(key, cal, sizeof(*cal));
}

/* Load channel calibration config from "vc/chN/cal". */
static int settings_load_ch_cal(uint8_t ch, struct vc_channel_cal_config *cal)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	return settings_load_key(key, cal, sizeof(*cal));
}

/* Delete all vc settings keys (system config + all channel config/cal). */
static int settings_erase(void)
{
	(void)settings_delete("vc/sys");
	for (uint8_t ch = 0; ch < VC_MAX_CHANNELS; ch++) {
		char key[16];

		snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
		(void)settings_delete(key);
		snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
		(void)settings_delete(key);
	}
	return 0;
}

const struct vc_storage_backend vc_settings_storage = {
	.save_system_config = settings_save_sys,
	.load_system_config = settings_load_sys,
	.save_channel_config = settings_save_ch_cfg,
	.load_channel_config = settings_load_ch_cfg,
	.save_channel_cal = settings_save_ch_cal,
	.load_channel_cal = settings_load_ch_cal,
	.erase_all = settings_erase,
};

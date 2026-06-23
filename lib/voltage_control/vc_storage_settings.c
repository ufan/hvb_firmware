/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "voltage_control/vc_storage.h"

struct vc_channel_config_no_cal {
	int16_t configured_target_voltage;
	uint16_t ramp_up_step;
	uint16_t ramp_up_interval;
	uint16_t ramp_down_step;
	uint16_t ramp_down_interval;
	enum vc_protection_mode voltage_protection_mode;
	enum vc_output_action voltage_protection_output_action;
	int16_t voltage_limit_threshold;
	enum vc_protection_mode current_protection_mode;
	enum vc_output_action current_protection_output_action;
	int16_t current_limit_threshold;
	uint16_t auto_derate_step;
	uint16_t save_target_policy;
};

struct vc_channel_cal {
	uint16_t output_calib_k;
	int16_t output_calib_b;
	uint16_t measured_voltage_calib_k;
	int16_t measured_voltage_calib_b;
	uint16_t measured_current_calib_k;
	int16_t measured_current_calib_b;
};

/* Pack operational fields (no calibration coefficients) for NVS storage. */
static void pack_no_cal(struct vc_channel_config_no_cal *dst,
			const struct vc_channel_config *src)
{
	dst->configured_target_voltage = src->configured_target_voltage;
	dst->ramp_up_step = src->ramp_up_step;
	dst->ramp_up_interval = src->ramp_up_interval;
	dst->ramp_down_step = src->ramp_down_step;
	dst->ramp_down_interval = src->ramp_down_interval;
	dst->voltage_protection_mode = src->voltage_protection_mode;
	dst->voltage_protection_output_action = src->voltage_protection_output_action;
	dst->voltage_limit_threshold = src->voltage_limit_threshold;
	dst->current_protection_mode = src->current_protection_mode;
	dst->current_protection_output_action = src->current_protection_output_action;
	dst->current_limit_threshold = src->current_limit_threshold;
	dst->auto_derate_step = src->auto_derate_step;
	dst->save_target_policy = src->save_target_policy;
}

/* Unpack operational fields from NVS into a full channel config struct. */
static void unpack_no_cal(struct vc_channel_config *dst,
			  const struct vc_channel_config_no_cal *src)
{
	dst->configured_target_voltage = src->configured_target_voltage;
	dst->ramp_up_step = src->ramp_up_step;
	dst->ramp_up_interval = src->ramp_up_interval;
	dst->ramp_down_step = src->ramp_down_step;
	dst->ramp_down_interval = src->ramp_down_interval;
	dst->voltage_protection_mode = src->voltage_protection_mode;
	dst->voltage_protection_output_action = src->voltage_protection_output_action;
	dst->voltage_limit_threshold = src->voltage_limit_threshold;
	dst->current_protection_mode = src->current_protection_mode;
	dst->current_protection_output_action = src->current_protection_output_action;
	dst->current_limit_threshold = src->current_limit_threshold;
	dst->auto_derate_step = src->auto_derate_step;
	dst->save_target_policy = src->save_target_policy;
}

/* Pack calibration coefficients (K/B for output, voltage, current) for NVS storage. */
static void pack_cal(struct vc_channel_cal *dst,
		     const struct vc_channel_config *src)
{
	dst->output_calib_k = src->output_calib_k;
	dst->output_calib_b = src->output_calib_b;
	dst->measured_voltage_calib_k = src->measured_voltage_calib_k;
	dst->measured_voltage_calib_b = src->measured_voltage_calib_b;
	dst->measured_current_calib_k = src->measured_current_calib_k;
	dst->measured_current_calib_b = src->measured_current_calib_b;
}

/* Unpack calibration coefficients from NVS into a full channel config struct. */
static void unpack_cal(struct vc_channel_config *dst,
		       const struct vc_channel_cal *src)
{
	dst->output_calib_k = src->output_calib_k;
	dst->output_calib_b = src->output_calib_b;
	dst->measured_voltage_calib_k = src->measured_voltage_calib_k;
	dst->measured_voltage_calib_b = src->measured_voltage_calib_b;
	dst->measured_current_calib_k = src->measured_current_calib_k;
	dst->measured_current_calib_b = src->measured_current_calib_b;
}

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

/* Persist channel operational config (excluding cal coefficients) to "vc/chN/cfg". */
static int settings_save_ch_cfg(uint8_t ch, const struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_config_no_cal packed;

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	pack_no_cal(&packed, cfg);
	return settings_save_one(key, &packed, sizeof(packed));
}

/* Load channel operational config (excluding cal coefficients) from "vc/chN/cfg". */
static int settings_load_ch_cfg(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_config_no_cal packed;
	int rc;

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	rc = settings_load_key(key, &packed, sizeof(packed));
	if (rc < 0) {
		return rc;
	}
	unpack_no_cal(cfg, &packed);
	return 0;
}

/* Persist channel calibration coefficients to "vc/chN/cal". */
static int settings_save_ch_cal(uint8_t ch, const struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_cal packed;

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	pack_cal(&packed, cfg);
	return settings_save_one(key, &packed, sizeof(packed));
}

/* Load channel calibration coefficients from "vc/chN/cal". */
static int settings_load_ch_cal(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_cal packed;
	int rc;

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	rc = settings_load_key(key, &packed, sizeof(packed));
	if (rc < 0) {
		return rc;
	}
	unpack_cal(cfg, &packed);
	return 0;
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

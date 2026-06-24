/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_controller.h"
#include "regmap/vc_regs.h"
#include <string.h>
#include <zephyr/sys/reboot.h>

#define VC_VARIANT_ID 1
#define VC_DEFAULT_SYSTEM_CAPS \
	(SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR | SYS_CAP_CALIBRATION_MODE)
#define VC_CHANNEL_MASK(c) ((1U << (c)) - 1)

static struct vc_system_config default_system_config(void)
{
	return (struct vc_system_config){
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.slave_address = 1,
		.baud_rate_code = VC_BAUD_RATE_115200,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
		.voltage_safe_band_pct = 10,
		.current_safe_band_pct = 10,
	};
}

static bool channel_valid(const struct vc_controller *ctrl, uint8_t ch)
{
	return ch < ctrl->channel_count;
}

static void drain_pending(struct vc_controller *ctrl, uint8_t ch)
{
	(void)vc_channel_take_pending_command(&ctrl->channels[ch]);
}

struct vc_controller *vc_controller_init_static(
	const struct vc_channel_entry *entries, size_t count)
{
	static struct vc_controller ctrl;

	if (entries == NULL || count == 0 || count > VC_MAX_CHANNELS) {
		return NULL;
	}

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.channel_count = count;
	ctrl.operating_mode = VC_OPERATING_MODE_NORMAL;
	ctrl.sys_cfg = default_system_config();

	for (size_t i = 0; i < count; i++) {
		vc_channel_init(&ctrl.channels[i], entries[i].index,
				entries[i].capabilities);
	}

	return &ctrl;
}

enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;
	enum vc_status st = vc_channel_output_action(&ctrl->channels[ch],
						     action, cal);

	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	enum vc_status st = vc_channel_fault_command(&ctrl->channels[ch], cmd,
						     &ctrl->sys_cfg);

	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;
	enum vc_status st = vc_channel_set_field(&ctrl->channels[ch],
						 field, value, cal);

	drain_pending(ctrl, ch);
	return st;
}

void vc_controller_consume_voltage(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_voltage)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_voltage(&ctrl->channels[ch], raw_voltage);
	drain_pending(ctrl, ch);
}

void vc_controller_consume_current(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_current)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_current(&ctrl->channels[ch], raw_current);
	drain_pending(ctrl, ch);
}

void vc_controller_consume_fault(
	struct vc_controller *ctrl, uint8_t ch, uint16_t fault_cause)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_fault(&ctrl->channels[ch], fault_cause);
	drain_pending(ctrl, ch);
}

void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms)
{
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		vc_channel_tick_ramp(&ctrl->channels[i], dt_ms, &ctrl->sys_cfg);
		drain_pending(ctrl, i);
	}
}

enum vc_status vc_controller_set_operating_mode(
	struct vc_controller *ctrl, enum vc_operating_mode mode)
{
	if (mode != VC_OPERATING_MODE_NORMAL &&
	    mode != VC_OPERATING_MODE_AUTOMATIC &&
	    mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_VALUE;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION &&
	    mode == VC_OPERATING_MODE_CALIBRATION && !ctrl->cal_unlocked) {
		return VC_ERR_INVALID_COMMAND;
	}

	if (ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    mode == VC_OPERATING_MODE_NORMAL) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			ctrl->channels[i].cooldown_remaining_ms = 0;
		}
	}

	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], true);
			drain_pending(ctrl, i);
		}
	} else if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], false);
			drain_pending(ctrl, i);
		}
	}

	ctrl->operating_mode = mode;
	if (mode != VC_OPERATING_MODE_CALIBRATION) {
		ctrl->sys_cfg.operating_mode = mode;
	}
	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->cal_unlock_step = 0;
		ctrl->cal_unlocked = false;
	}

	return VC_OK;
}

enum vc_operating_mode vc_controller_get_operating_mode(
	const struct vc_controller *ctrl)
{
	return ctrl->operating_mode;
}

enum vc_status vc_controller_calibration_unlock(
	struct vc_controller *ctrl, uint16_t value)
{
	if (value == CAL_UNLOCK_STEP1) {
		ctrl->cal_unlock_step = 1;
		ctrl->cal_unlocked = false;
		return VC_OK;
	}
	if (value == CAL_UNLOCK_STEP2 && ctrl->cal_unlock_step == 1) {
		ctrl->cal_unlock_step = 0;
		ctrl->cal_unlocked = true;
		return VC_OK;
	}
	ctrl->cal_unlock_step = 0;
	ctrl->cal_unlocked = false;
	return VC_ERR_INVALID_COMMAND;
}

enum vc_status vc_controller_get_system_config(
	const struct vc_controller *ctrl, struct vc_system_config *cfg)
{
	*cfg = ctrl->sys_cfg;
	return VC_OK;
}

enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg)
{
	if (cfg->slave_address > 247) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->baud_rate_code > VC_BAUD_RATE_9600) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_safe_band_pct > 50 || cfg->current_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}

	enum vc_operating_mode old_cfg_mode = ctrl->sys_cfg.operating_mode;

	ctrl->sys_cfg = *cfg;
	if (cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->sys_cfg.operating_mode = old_cfg_mode;
	}

	if (cfg->operating_mode != ctrl->operating_mode) {
		return vc_controller_set_operating_mode(ctrl, cfg->operating_mode);
	}
	return VC_OK;
}

void vc_controller_get_system_snapshot(
	const struct vc_controller *ctrl, struct vc_system_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->protocol_major = VC_PROTOCOL_MAJOR;
	snap->protocol_minor = VC_PROTOCOL_MINOR;
	snap->variant_id = VC_VARIANT_ID;
	snap->system_capability_flags = VC_DEFAULT_SYSTEM_CAPS;
	snap->supported_channel_count = (uint16_t)ctrl->channel_count;
	snap->active_channel_mask = VC_CHANNEL_MASK(ctrl->channel_count);
	snap->active_operating_mode = ctrl->operating_mode;
}

enum vc_status vc_controller_get_channel_snapshot(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_snapshot *snap)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	vc_channel_get_snapshot(&ctrl->channels[ch], snap);
	return VC_OK;
}

enum vc_status vc_controller_get_channel_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_config *cfg)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_get_config(&ctrl->channels[ch], cfg);
}

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend)
{
	ctrl->storage = backend;
}

size_t vc_controller_channel_count(const struct vc_controller *ctrl)
{
	return ctrl->channel_count;
}

uint16_t vc_controller_channel_capabilities(
	const struct vc_controller *ctrl, uint8_t ch)
{
	if (!channel_valid(ctrl, ch)) {
		return 0;
	}
	return ctrl->channels[ch].capabilities;
}

enum vc_status vc_controller_system_param_action(
	struct vc_controller *ctrl, enum vc_param_action action)
{
	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
		if (ctrl->storage == NULL ||
		    ctrl->storage->save_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		return ctrl->storage->save_system_config(&ctrl->sys_cfg) < 0
			? VC_ERR_STORAGE : VC_OK;
	case VC_PARAM_ACTION_LOAD: {
		struct vc_system_config cfg;

		if (ctrl->storage == NULL ||
		    ctrl->storage->load_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		if (ctrl->storage->load_system_config(&cfg) < 0) {
			return VC_ERR_STORAGE;
		}
		return vc_controller_set_system_config(ctrl, &cfg);
	}
	case VC_PARAM_ACTION_FACTORY_RESET:
		if (ctrl->storage != NULL && ctrl->storage->erase_all != NULL) {
			(void)ctrl->storage->erase_all();
		}
		ctrl->sys_cfg = default_system_config();
		ctrl->operating_mode = VC_OPERATING_MODE_NORMAL;
		return VC_OK;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
#ifdef CONFIG_REBOOT
		sys_reboot(SYS_REBOOT_COLD);
#endif
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

enum vc_status vc_controller_channel_param_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_param_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE: {
		struct vc_channel_config cfg;

		if (ctrl->storage == NULL ||
		    ctrl->storage->save_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		return ctrl->storage->save_channel_config(ch, &cfg) < 0
			? VC_ERR_STORAGE : VC_OK;
	}
	case VC_PARAM_ACTION_LOAD: {
		struct vc_channel_config cfg, loaded;

		if (ctrl->storage == NULL ||
		    ctrl->storage->load_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		loaded = cfg;
		if (ctrl->storage->load_channel_config(ch, &loaded) < 0) {
			return VC_ERR_STORAGE;
		}
		loaded.output_calib_k = cfg.output_calib_k;
		loaded.output_calib_b = cfg.output_calib_b;
		loaded.measured_voltage_calib_k = cfg.measured_voltage_calib_k;
		loaded.measured_voltage_calib_b = cfg.measured_voltage_calib_b;
		loaded.measured_current_calib_k = cfg.measured_current_calib_k;
		loaded.measured_current_calib_b = cfg.measured_current_calib_b;
		bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;

		return vc_channel_set_config(&ctrl->channels[ch], &loaded, cal);
	}
	case VC_PARAM_ACTION_FACTORY_RESET: {
		struct vc_channel_config cfg;
		struct vc_channel_config defaults = {
			.voltage_limit_threshold = 20000,
			.current_limit_threshold = 32767,
			.output_calib_k = 10000,
			.measured_voltage_calib_k = 10000,
			.measured_current_calib_k = 10000,
		};

		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		defaults.output_calib_k = cfg.output_calib_k;
		defaults.output_calib_b = cfg.output_calib_b;
		defaults.measured_voltage_calib_k = cfg.measured_voltage_calib_k;
		defaults.measured_voltage_calib_b = cfg.measured_voltage_calib_b;
		defaults.measured_current_calib_k = cfg.measured_current_calib_k;
		defaults.measured_current_calib_b = cfg.measured_current_calib_b;
		bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;

		return vc_channel_set_config(&ctrl->channels[ch], &defaults, cal);
	}
	case VC_PARAM_ACTION_SOFTWARE_RESET:
#ifdef CONFIG_REBOOT
		sys_reboot(SYS_REBOOT_COLD);
#endif
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

enum vc_status vc_controller_set_system_field(
	struct vc_controller *ctrl, enum vc_config_field field, uint16_t value)
{
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);

	switch (field) {
	case VC_FIELD_OPERATING_MODE:
		return vc_controller_set_operating_mode(ctrl,
						       (enum vc_operating_mode)value);
	case VC_FIELD_SLAVE_ADDRESS:
		cfg.slave_address = value;
		break;
	case VC_FIELD_BAUD_RATE_CODE:
		cfg.baud_rate_code = (enum vc_baud_rate_code)value;
		break;
	case VC_FIELD_RECOVERY_POLICY_MODE:
		cfg.recovery_policy_mode = (enum vc_recovery_policy_mode)value;
		break;
	case VC_FIELD_AUTO_RETRY_DELAY:
		cfg.auto_retry_delay = value;
		break;
	case VC_FIELD_AUTO_RETRY_MAX_COUNT:
		cfg.auto_retry_max_count = value;
		break;
	case VC_FIELD_AUTO_RETRY_WINDOW:
		cfg.auto_retry_window = value;
		break;
	case VC_FIELD_VOLTAGE_SAFE_BAND_PCT:
		cfg.voltage_safe_band_pct = value;
		break;
	case VC_FIELD_CURRENT_SAFE_BAND_PCT:
		cfg.current_safe_band_pct = value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_controller_set_system_config(ctrl, &cfg);
}

enum vc_status vc_controller_channel_cal_output_enable(
	struct vc_controller *ctrl, uint8_t ch, bool enable)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	enum vc_status st = vc_channel_cal_set_output_enable(
		&ctrl->channels[ch], enable, ctrl->channels, ctrl->channel_count);

	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_cal_raw_dac(
	struct vc_controller *ctrl, uint8_t ch, uint16_t code)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	enum vc_status st = vc_channel_cal_set_raw_dac(&ctrl->channels[ch], code);

	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_cal_sample(
	struct vc_controller *ctrl, uint8_t ch)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_channel_cal_sample(&ctrl->channels[ch]);
}

enum vc_status vc_controller_channel_cal_commit(
	struct vc_controller *ctrl, uint8_t ch)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_channel_cal_commit(&ctrl->channels[ch]);
}

enum vc_status vc_controller_channel_cal_max_raw_dac(
	struct vc_controller *ctrl, uint8_t ch, uint16_t limit)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_channel_cal_set_max_raw_dac(&ctrl->channels[ch], limit);
}

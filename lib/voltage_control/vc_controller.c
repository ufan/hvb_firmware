/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_controller.h"
#include "reg_store/reg_map.h"
#include <string.h>
#include <errno.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(vc_controller, LOG_LEVEL_WRN);

#ifdef CONFIG_VC_CHANNEL_CONTROLLER
#include <zephyr/sys/iterable_sections.h>
#include <dt-bindings/voltage_control/capabilities.h>

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define MEAS_ENTRY(node_id)                                             \
	STRUCT_SECTION_ITERABLE(vc_channel_buffer,                         \
		VC_CHANNEL_BUFFER_NAME(node_id)) = {                       \
		.channel_id = DT_REG_ADDR(node_id),                     \
	};

DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, MEAS_ENTRY)
#endif

#define VC_VARIANT_ID CONFIG_VC_VARIANT_ID
#define VC_CAL_WATCHDOG_MS (CONFIG_VC_CAL_WATCHDOG_TIMEOUT_S * 1000U)

static void cal_watchdog_reset(struct vc_controller *ctrl)
{
	ctrl->cal_watchdog_ms = VC_CAL_WATCHDOG_MS;
}

void vc_controller_cal_heartbeat(struct vc_controller *ctrl)
{
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		cal_watchdog_reset(ctrl);
	}
}

#define VC_DEFAULT_SYSTEM_CAPS \
	(SYS_CAP_AUTOMATIC_MODE | SYS_CAP_CALIBRATION_MODE)
#define VC_CHANNEL_MASK(c) ((1U << (c)) - 1)

struct vc_controller vc_controller_canonical_state;

static struct vc_system_config default_system_config(void)
{
	return (struct vc_system_config){
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.startup_channel_policy = 0,
	};
}

static bool channel_valid(const struct vc_controller *ctrl, uint8_t ch)
{
	return ch < ctrl->channel_count;
}

static void build_meas_index(struct vc_controller *ctrl)
{
	memset(ctrl->meas_index, 0, sizeof(ctrl->meas_index));
#ifdef CONFIG_VC_CHANNEL_CONTROLLER
	STRUCT_SECTION_FOREACH(vc_channel_buffer, entry) {
		if (entry->channel_id < VC_MAX_CHANNELS) {
			ctrl->meas_index[entry->channel_id] = entry;
		}
	}
#endif
}

struct vc_controller *vc_controller_init(
	vc_wake_fn_t wake_fn, void *wake_user_data)
{
	struct vc_controller *ctrl = &vc_controller_canonical_state;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->operating_mode = VC_OPERATING_MODE_NORMAL;
	ctrl->sys_cfg = default_system_config();

#ifdef CONFIG_VC_CHANNEL_CONTROLLER
	ctrl->channel_count = DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE);
	build_meas_index(ctrl);

#define INIT_CHANNEL(node_id)                                            \
	vc_channel_init(&ctrl->channels[DT_REG_ADDR(node_id)],          \
			DEVICE_DT_GET(node_id),                          \
			DT_REG_ADDR(node_id),                            \
			DT_PROP(node_id, capabilities),                  \
			ctrl->meas_index[DT_REG_ADDR(node_id)],         \
			wake_fn, wake_user_data);

	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, INIT_CHANNEL)
#endif

	return ctrl;
}

/* ---- Thin wrappers: route to vc_channel ---- */

enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_channel_output_action(&ctrl->channels[ch], action);
}

enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_fault_command(&ctrl->channels[ch], cmd);
}

enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	enum vc_status st = vc_channel_set_field(&ctrl->channels[ch], field, value);

	/* AUTO mode: setting a non-zero target auto-enables the channel */
	if (st == VC_OK &&
	    field == VC_FIELD_CONFIGURED_TARGET_VOLTAGE &&
	    ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    (int16_t)value != 0 &&
	    ctrl->channels[ch].active_fault_cause == 0) {
		(void)vc_channel_output_action(&ctrl->channels[ch],
					       VC_OUTPUT_ACTION_ENABLE);
	}
	return st;
}

void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms)
{
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		if (dt_ms >= ctrl->cal_watchdog_ms) {
			LOG_WRN("calibration watchdog expired, session terminated");
			(void)vc_controller_cal_exit(ctrl);
		} else {
			ctrl->cal_watchdog_ms -= dt_ms;
		}
		return;
	}
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		vc_channel_run(&ctrl->channels[i], dt_ms, &ctrl->sys_cfg);
	}
}

/* ---- Operating mode (collective behavior) ---- */

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
	if (mode == ctrl->operating_mode) {
		if (mode == VC_OPERATING_MODE_CALIBRATION) {
			cal_watchdog_reset(ctrl);
		}
		return VC_OK;
	}

	/* NORMAL → AUTOMATIC: refuse while any channel is still faulted, so a
	 * fault latched in Normal mode can never ride along into Automatic
	 * recovery eligibility. The reverse direction (→ NORMAL) is always
	 * allowed, since it's the safe-retreat move an operator would want
	 * to make because of a fault, not in spite of one.
	 */
	if (mode == VC_OPERATING_MODE_AUTOMATIC &&
	    ctrl->operating_mode == VC_OPERATING_MODE_NORMAL) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			if (ctrl->channels[i].active_fault_cause != 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}

	/* AUTO → NORMAL: gracefully disable all outputs */
	if (mode == VC_OPERATING_MODE_NORMAL &&
	    ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			(void)vc_channel_output_action(&ctrl->channels[i],
						       VC_OUTPUT_ACTION_DISABLE_GRACEFUL);
		}
	}

	/* → AUTOMATIC: enable all non-faulted channels with non-zero configured target */
	if (mode == VC_OPERATING_MODE_AUTOMATIC) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			if (ctrl->channels[i].config.configured_target_voltage != 0 &&
			    ctrl->channels[i].active_fault_cause == 0) {
				(void)vc_channel_output_action(&ctrl->channels[i],
							       VC_OUTPUT_ACTION_ENABLE);
			}
		}
	}

	/* → CAL: reset all channels into calibration state */
	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->pre_cal_mode = ctrl->operating_mode;
		cal_watchdog_reset(ctrl);
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], true);
		}
	} else if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		/* CAL → anything: reset channels back to normal state */
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], false);
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

enum vc_status vc_controller_cal_exit(struct vc_controller *ctrl)
{
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_controller_set_operating_mode(ctrl, ctrl->pre_cal_mode);
}

/* ---- System config ---- */

enum vc_status vc_controller_get_system_config(
	const struct vc_controller *ctrl, struct vc_system_config *cfg)
{
	*cfg = ctrl->sys_cfg;
	return VC_OK;
}

enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg)
{
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

enum vc_status vc_controller_set_system_field(
	struct vc_controller *ctrl, enum vc_config_field field, uint16_t value)
{
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);

	switch (field) {
	case VC_FIELD_OPERATING_MODE:
		return vc_controller_set_operating_mode(ctrl,
						       (enum vc_operating_mode)value);
	case VC_FIELD_STARTUP_CHANNEL_POLICY:
		cfg.startup_channel_policy = value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_controller_set_system_config(ctrl, &cfg);
}

/* ---- Snapshots ---- */

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
	snap->cal_watchdog_remaining_ms = ctrl->cal_watchdog_ms;
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

enum vc_status vc_controller_get_channel_cal_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_cal_config *cal)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_get_cal_config(&ctrl->channels[ch], cal);
}

enum vc_status vc_controller_channel_set_cal_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_cal_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	enum vc_status st = vc_channel_set_cal_field(&ctrl->channels[ch], field, value);

	if (st == VC_OK) {
		cal_watchdog_reset(ctrl);
	}
	return st;
}

/* ---- Storage ---- */

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend)
{
	ctrl->storage = backend;
}

size_t vc_controller_channel_count(const struct vc_controller *ctrl)
{
	return ctrl->channel_count;
}

/* ---- Param actions ---- */

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
		int ret = ctrl->storage->load_system_config(&cfg);

		if (ret == -ENOENT) {
			return VC_OK;
		}
		if (ret < 0) {
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
		struct vc_channel_config cfg;

		if (ctrl->storage == NULL ||
		    ctrl->storage->load_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		int cfg_ret = ctrl->storage->load_channel_config(ch, &cfg);

		if (cfg_ret < 0 && cfg_ret != -ENOENT) {
			return VC_ERR_STORAGE;
		}
		if (cfg_ret == 0) {
			enum vc_status st = vc_channel_set_config(&ctrl->channels[ch], &cfg);

			if (st != VC_OK) {
				return st;
			}
		}
		/* Load cal from NVS; -ENOENT keeps current cal */
		if (ctrl->storage->load_channel_cal != NULL) {
			struct vc_channel_cal_config cal;

			vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
			if (ctrl->storage->load_channel_cal(ch, &cal) == 0) {
				vc_channel_load_cal(&ctrl->channels[ch], &cal);
			}
		}
		return VC_OK;
	}
	case VC_PARAM_ACTION_FACTORY_RESET: {
		struct vc_channel_config defaults = vc_channel_default_config();
		enum vc_status st = vc_channel_set_config(&ctrl->channels[ch], &defaults);

		if (st != VC_OK) {
			return st;
		}
		/* Load cal from NVS; -ENOENT keeps default k=10000/b=0 */
		if (ctrl->storage != NULL && ctrl->storage->load_channel_cal != NULL) {
			struct vc_channel_cal_config cal;

			vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
			if (ctrl->storage->load_channel_cal(ch, &cal) == 0) {
				vc_channel_load_cal(&ctrl->channels[ch], &cal);
			}
		}
		return VC_OK;
	}
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

/* ---- Calibration (cross-channel check lives here) ---- */

enum vc_status vc_controller_channel_cal_output_enable(
	struct vc_controller *ctrl, uint8_t ch, bool enable)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (enable) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			if (i == (size_t)ch) {
				continue;
			}
			if (ctrl->channels[i].cal_output_enabled ||
			    ctrl->channels[i].raw_dac_readback != 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	enum vc_status st = vc_channel_cal_set_output_enable(&ctrl->channels[ch], enable);

	if (st == VC_OK) {
		cal_watchdog_reset(ctrl);
	}
	return st;
}

enum vc_status vc_controller_channel_cal_raw_dac(
	struct vc_controller *ctrl, uint8_t ch, uint32_t code)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	enum vc_status st = vc_channel_cal_set_raw_dac(&ctrl->channels[ch], code);

	if (st == VC_OK) {
		cal_watchdog_reset(ctrl);
	}
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
	enum vc_status st = vc_channel_cal_sample(&ctrl->channels[ch]);

	if (st == VC_OK) {
		cal_watchdog_reset(ctrl);
	}
	return st;
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
	enum vc_status ret = vc_channel_cal_commit(&ctrl->channels[ch]);

	if (ret != VC_OK) {
		return ret;
	}
	cal_watchdog_reset(ctrl);
	if (ctrl->storage != NULL && ctrl->storage->save_channel_cal != NULL) {
		struct vc_channel_cal_config cal;

		vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
		if (ctrl->storage->save_channel_cal(ch, &cal) < 0) {
			return VC_ERR_STORAGE;
		}
	}
	return VC_OK;
}


/* ---- Start sampling ---- */

enum vc_status vc_controller_start_sampling(struct vc_controller *ctrl)
{
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		const struct device *dev = ctrl->channels[i].dev;

		if (dev == NULL || dev->api == NULL) {
			continue;
		}
		const struct vc_channel_api *api = dev->api;

		if (api->start_sampling) {
			int ret = api->start_sampling(dev);

			if (ret < 0 && ret != -ENOTSUP) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	return VC_OK;
}

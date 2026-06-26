/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "voltage_control/vc_controller.h"
#include "regmap/vc_regs.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

static struct vc_controller *ctrl;
static int wake_count;

static void test_wake_fn(void *user_data)
{
	ARG_UNUSED(user_data);
	wake_count++;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	wake_count = 0;
	ctrl = vc_controller_init(test_wake_fn, NULL);
	zassert_not_null(ctrl);
}

ZTEST_SUITE(vc_controller, NULL, NULL, before_each, NULL, NULL);

ZTEST(vc_controller, test_init)
{
	zassert_equal(vc_controller_channel_count(ctrl), 2);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_channels_have_devices)
{
	zassert_not_null(ctrl->channels[0].dev);
	zassert_not_null(ctrl->channels[1].dev);
}

ZTEST(vc_controller, test_channel_output_action_routes)
{
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
		VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ctrl->channels[0].output_enabled);
	zassert_false(ctrl->channels[1].output_enabled);
}

ZTEST(vc_controller, test_channel_output_action_rejects_invalid_channel)
{
	zassert_equal(vc_controller_channel_output_action(ctrl, 5,
		VC_OUTPUT_ACTION_ENABLE), VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_controller, test_tick_ramps)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_controller_tick(ctrl, 100);

	zassert_equal(ctrl->channels[0].operational_target_voltage, 1000);
}

ZTEST(vc_controller, test_calibration_unlock_and_mode_entry)
{
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1),
		      VC_OK);
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2),
		      VC_OK);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
		VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(vc_controller, test_calibration_rejected_without_unlock)
{
	zassert_equal(vc_controller_set_operating_mode(ctrl,
		VC_OPERATING_MODE_CALIBRATION), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_controller, test_system_snapshot)
{
	struct vc_system_snapshot snap;

	vc_controller_get_system_snapshot(ctrl, &snap);
	zassert_equal(snap.protocol_major, VC_PROTOCOL_MAJOR);
	zassert_equal(snap.protocol_minor, VC_PROTOCOL_MINOR);
	zassert_equal(snap.supported_channel_count, 2);
	zassert_equal(snap.active_channel_mask, 0x0003);
}

ZTEST(vc_controller, test_system_config_defaults)
{
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);
	zassert_equal(cfg.operating_mode, VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_channel_set_field_routes)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 5000), VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_controller, test_channel_set_field_rejects_invalid_channel)
{
	zassert_equal(vc_controller_channel_set_field(ctrl, 99,
		VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 100),
		VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_controller, test_mode_transition_auto_to_normal_clears_cooldown)
{
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_AUTOMATIC);

	ctrl->channels[0].cooldown_remaining_ms = 5000;
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_NORMAL);
	zassert_equal(ctrl->channels[0].cooldown_remaining_ms, 0);
}

ZTEST(vc_controller, test_calibration_entry_resets_channels)
{
	struct vc_channel_snapshot snap;

	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_CALIBRATION);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
}

ZTEST(vc_controller, test_cal_single_output_across_channels)
{
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, false), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true), VC_OK);
}

ZTEST(vc_controller, test_system_param_action_no_storage)
{
	zassert_equal(vc_controller_system_param_action(ctrl, VC_PARAM_ACTION_SAVE),
		      VC_ERR_STORAGE);
	zassert_equal(vc_controller_system_param_action(ctrl, VC_PARAM_ACTION_NONE),
		      VC_OK);
}

ZTEST(vc_controller, test_channel_param_action_no_storage)
{
	zassert_equal(vc_controller_channel_param_action(ctrl, 0, VC_PARAM_ACTION_SAVE),
		      VC_ERR_STORAGE);
}

ZTEST(vc_controller, test_channel_param_reset_restores_operational_defaults)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_RAMP_UP_STEP, 123), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_RAMP_UP_INTERVAL, 4), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_RAMP_DOWN_STEP, 456), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_RAMP_DOWN_INTERVAL, 7), VC_OK);

	zassert_equal(vc_controller_channel_param_action(ctrl, 0,
		VC_PARAM_ACTION_FACTORY_RESET), VC_OK);

	zassert_equal(vc_controller_get_channel_config(ctrl, 0, &cfg), VC_OK);
	zassert_equal(cfg.ramp_up_step, 50000);
	zassert_equal(cfg.ramp_up_interval, 10);
	zassert_equal(cfg.ramp_down_step, 50000);
	zassert_equal(cfg.ramp_down_interval, 10);
}

ZTEST(vc_controller, test_start_sampling)
{
	zassert_equal(vc_controller_start_sampling(ctrl), VC_OK);
}

static void enter_cal(struct vc_controller *c)
{
	vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(c, VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(vc_controller, test_cal_exit_rejected_when_not_in_cal)
{
	zassert_equal(vc_controller_cal_exit(ctrl), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_controller, test_cal_exit_from_normal_restores_normal)
{
	enter_cal(ctrl);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_cal_exit_from_auto_restores_auto)
{
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_AUTOMATIC);
	enter_cal(ctrl);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_AUTOMATIC);
}

ZTEST(vc_controller, test_reentering_cal_keeps_pre_cal_mode)
{
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_AUTOMATIC);
	enter_cal(ctrl);

	zassert_equal(vc_controller_set_operating_mode(ctrl,
		VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_AUTOMATIC);
}

ZTEST(vc_controller, test_cal_exit_resets_channels_to_disabled_safe)
{
	enter_cal(ctrl);
	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);

	struct vc_channel_snapshot snap;

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(snap.raw_dac_readback, 0);
}

ZTEST(vc_controller, test_cal_watchdog_exits_after_inactivity)
{
	enter_cal(ctrl);

	vc_controller_tick(ctrl, 29999);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	vc_controller_tick(ctrl, 1);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_cal_watchdog_resets_on_cal_activity)
{
	enter_cal(ctrl);

	vc_controller_tick(ctrl, 25000);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 1234), VC_OK);
	vc_controller_tick(ctrl, 5000);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	vc_controller_tick(ctrl, 25000);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

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

static const struct vc_channel_entry test_entries[] = {
	{ .dev = NULL, .index = 0, .capabilities = FULL_CAPS },
	{ .dev = NULL, .index = 1, .capabilities = FULL_CAPS },
};

static struct vc_controller *ctrl;

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	ctrl = vc_controller_init_static(test_entries, 2);
	zassert_not_null(ctrl);
}

ZTEST_SUITE(vc_controller, NULL, NULL, before_each, NULL, NULL);

ZTEST(vc_controller, test_init)
{
	zassert_equal(vc_controller_channel_count(ctrl), 2);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
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

ZTEST(vc_controller, test_consume_voltage_routes_and_drains)
{
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_consume_voltage(ctrl, 0, 1200);
	zassert_equal(ctrl->channels[0].measured_voltage, 1200);
	zassert_false(ctrl->channels[0].pending.valid);
}

ZTEST(vc_controller, test_consume_voltage_triggers_protection)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_tick(ctrl, 100);

	vc_controller_consume_voltage(ctrl, 0, 3100);

	zassert_true(ctrl->channels[0].active_fault_cause & VC_FAULT_VOLTAGE);
	zassert_false(ctrl->channels[0].output_enabled);
	zassert_false(ctrl->channels[0].pending.valid);
}

ZTEST(vc_controller, test_tick_ramps_and_drains)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_controller_tick(ctrl, 100);

	zassert_equal(ctrl->channels[0].operational_target_voltage, 1000);
	zassert_false(ctrl->channels[0].pending.valid);
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
	zassert_equal(cfg.slave_address, 1);
	zassert_equal(cfg.baud_rate_code, VC_BAUD_RATE_115200);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
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
	struct vc_system_config sys;

	vc_controller_get_system_config(ctrl, &sys);
	sys.operating_mode = VC_OPERATING_MODE_AUTOMATIC;
	sys.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	sys.auto_retry_delay = 60;
	vc_controller_set_system_config(ctrl, &sys);

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

ZTEST(vc_controller, test_consume_fault_routes)
{
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_consume_fault(ctrl, 0, VC_FAULT_HARDWARE);
	zassert_true(ctrl->channels[0].active_fault_cause & VC_FAULT_HARDWARE);
	zassert_false(ctrl->channels[0].output_enabled);
	zassert_false(ctrl->channels[0].pending.valid);
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

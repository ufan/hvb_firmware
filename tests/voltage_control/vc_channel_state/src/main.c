/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "voltage_control/vc_channel_state.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

static struct vc_channel ch;
static const struct vc_system_config default_sys = {
	.current_safe_band_pct = 10,
};

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_channel_init(&ch, 0, FULL_CAPS);
}

ZTEST_SUITE(vc_channel_state, NULL, NULL, before_each, NULL, NULL);

/* ---- Init + defaults ---- */

ZTEST(vc_channel_state, test_init_defaults)
{
	zassert_equal(ch.index, 0);
	zassert_equal(ch.capabilities, FULL_CAPS);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(ch.active_fault_cause, 0);
	zassert_equal(ch.measured_voltage, 0);
	zassert_equal(ch.measured_current, 0);
	zassert_false(ch.pending.valid);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_channel_state, test_default_config)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_get_config(&ch, &cfg), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.output_calib_k, 10000);
	zassert_equal(cfg.output_calib_b, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);
	zassert_equal(cfg.measured_current_calib_k, 10000);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
}

ZTEST(vc_channel_state, test_snapshot_defaults)
{
	struct vc_channel_snapshot snap;

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.measured_voltage, 0);
	zassert_equal(snap.measured_current, 0);
	zassert_equal(snap.active_fault_cause, 0);
	zassert_equal(snap.fault_history_cause, 0);
	zassert_equal(snap.status_bits, 0);
	zassert_equal(snap.channel_capability_flags, FULL_CAPS);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
}

/* ---- Output action ---- */

ZTEST(vc_channel_state, test_output_action_enable)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false), VC_OK);
	zassert_true(ch.output_enabled);
	zassert_true(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RAMPING);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_output_action_disable_immediate)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE, false),
		      VC_OK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_true(ch.pending.valid);
	zassert_equal(ch.pending.output_code, 0);
	zassert_false(ch.pending.output_enable);
}

ZTEST(vc_channel_state, test_output_action_rejected_in_calibration)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, true),
		      VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_output_action_rejected_with_active_fault)
{
	ch.active_fault_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(vc_channel_state, test_output_action_invalid_host_action)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO, false),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Fault command ---- */

ZTEST(vc_channel_state, test_fault_command_clear_history)
{
	ch.fault_history_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_fault_command(&ch, VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY,
					       &default_sys), VC_OK);
	zassert_equal(ch.fault_history_cause, 0);
}

ZTEST(vc_channel_state, test_fault_command_invalid)
{
	zassert_equal(vc_channel_fault_command(&ch, 3, &default_sys),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Consume voltage ---- */

ZTEST(vc_channel_state, test_consume_voltage_applies_calibration)
{
	vc_channel_consume_voltage(&ch, 1200);
	zassert_equal(ch.measured_voltage, 1200);
	zassert_equal(ch.raw_adc_voltage, 1200);
}

ZTEST(vc_channel_state, test_consume_voltage_with_calibration_gain)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.measured_voltage_calib_k = 20000;
	vc_channel_set_config(&ch, &cfg, true);

	vc_channel_consume_voltage(&ch, 100);
	zassert_equal(ch.measured_voltage, 200);
}

ZTEST(vc_channel_state, test_consume_voltage_clamps_to_int16)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.measured_voltage_calib_k = 65535;
	vc_channel_set_config(&ch, &cfg, true);

	vc_channel_consume_voltage(&ch, 20000);
	zassert_equal(ch.measured_voltage, INT16_MAX);
}

/* ---- Consume current ---- */

ZTEST(vc_channel_state, test_consume_current_applies_calibration)
{
	vc_channel_consume_current(&ch, 500);
	zassert_equal(ch.measured_current, 500);
	zassert_equal(ch.raw_adc_current, 500);
}

ZTEST(vc_channel_state, test_current_protection_triggers_fault)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;
	vc_channel_tick_ramp(&ch, 100, &default_sys);
	ch.pending.valid = false;

	vc_channel_consume_current(&ch, 200);

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_true(ch.pending.valid);
	zassert_false(ch.pending.output_enable);
}

/* ---- Consume fault ---- */

ZTEST(vc_channel_state, test_consume_fault_sets_hardware_fault)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);
	zassert_true(ch.fault_history_cause & VC_FAULT_HARDWARE);
	zassert_false(ch.output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_consume_fault_interlock)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_consume_fault(&ch, VC_FAULT_INTERLOCK);

	zassert_true(ch.active_fault_cause & VC_FAULT_INTERLOCK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
}

/* ---- Tick ramp ---- */

ZTEST(vc_channel_state, test_tick_ramp_instant)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
	zassert_true(ch.pending.valid);
	zassert_equal(ch.pending.output_code, 5000);
}

ZTEST(vc_channel_state, test_tick_ramp_gradual)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 100);
	zassert_true(ch.ramping);
}

ZTEST(vc_channel_state, test_tick_ramp_no_output_enabled)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	vc_channel_set_config(&ch, &cfg, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 0);
}

ZTEST(vc_channel_state, test_tick_ramp_at_target_stops_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_equal(ch.operational_target_voltage, 100);
	zassert_false(ch.ramping);

	ch.pending.valid = false;
	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_false(ch.pending.valid);
}

/* ---- Set config validation ---- */

ZTEST(vc_channel_state, test_set_config_target_voltage_range)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_set_config_calibration_blocked_outside_cal_mode)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.output_calib_k++;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_set_config_calibration_allowed_in_cal_mode)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.output_calib_k = 20000;
	cfg.output_calib_b = 5;
	zassert_equal(vc_channel_set_config(&ch, &cfg, true), VC_OK);
	vc_channel_get_config(&ch, &cfg);
	zassert_equal(cfg.output_calib_k, 20000);
	zassert_equal(cfg.output_calib_b, 5);
}

ZTEST(vc_channel_state, test_set_field_target_voltage)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					   5000, false), VC_OK);
	vc_channel_get_config(&ch, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_channel_state, test_set_field_rejects_system_field)
{
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_OPERATING_MODE, 0, false),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_onoff_channel_rejects_output_drive_config)
{
	struct vc_channel onoff_ch;
	struct vc_channel_config cfg;

	vc_channel_init(&onoff_ch, 0, CH_CAP_OUTPUT_ENABLE);
	vc_channel_get_config(&onoff_ch, &cfg);
	cfg.configured_target_voltage = 100;
	zassert_equal(vc_channel_set_config(&onoff_ch, &cfg, false),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

/* ---- Calibration ---- */

ZTEST(vc_channel_state, test_cal_set_output_enable)
{
	zassert_equal(vc_channel_cal_set_output_enable(&ch, true, NULL, 0), VC_OK);
	zassert_equal(ch.cal_output_enabled, 1);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_cal_set_raw_dac_requires_output_enabled)
{
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_OK);
	zassert_equal(ch.raw_dac_readback, 100);
}

ZTEST(vc_channel_state, test_cal_max_raw_dac_limit)
{
	vc_channel_cal_set_max_raw_dac(&ch, 50);
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 51), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 50), VC_OK);
}

ZTEST(vc_channel_state, test_cal_sample_captures_raw)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	vc_channel_cal_set_raw_dac(&ch, 123);
	zassert_equal(vc_channel_cal_sample(&ch), VC_OK);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_VALID);
	zassert_equal(ch.raw_adc_voltage, 123);
}

ZTEST(vc_channel_state, test_cal_commit_requires_output_disabled)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_commit(&ch), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, false, NULL, 0);
	zassert_equal(vc_channel_cal_commit(&ch), VC_OK);
}

ZTEST(vc_channel_state, test_cal_disable_clears_sample_state)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	vc_channel_cal_set_raw_dac(&ch, 50);
	vc_channel_cal_sample(&ch);
	vc_channel_cal_set_output_enable(&ch, false, NULL, 0);
	zassert_equal(ch.raw_adc_voltage, 0);
	zassert_equal(ch.raw_adc_current, 0);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_NONE);
}

ZTEST(vc_channel_state, test_cal_single_output_enabled_across_channels)
{
	struct vc_channel channels[2];

	vc_channel_init(&channels[0], 0, FULL_CAPS);
	vc_channel_init(&channels[1], 1, FULL_CAPS);

	zassert_equal(vc_channel_cal_set_output_enable(&channels[0], true, channels, 2), VC_OK);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[1], true, channels, 2),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[0], false, channels, 2), VC_OK);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[1], true, channels, 2), VC_OK);
}

ZTEST(vc_channel_state, test_reset_calibration_entering)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	vc_channel_cal_set_raw_dac(&ch, 100);
	vc_channel_reset_calibration(&ch, true);

	zassert_equal(ch.raw_dac_readback, 0);
	zassert_equal(ch.cal_output_enabled, 0);
	zassert_equal(ch.cal_max_raw_dac_limit, 0xFFFF);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
}

ZTEST(vc_channel_state, test_reset_calibration_exiting)
{
	vc_channel_reset_calibration(&ch, false);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- Pending command ---- */

ZTEST(vc_channel_state, test_take_pending_command_clears_valid)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	zassert_true(ch.pending.valid);

	struct vc_pending_command cmd = vc_channel_take_pending_command(&ch);
	zassert_true(cmd.valid);
	zassert_true(cmd.output_enable);
	zassert_false(ch.pending.valid);
}

ZTEST(vc_channel_state, test_current_protection_skipped_during_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	/* Ramp partway — channel is still ramping */
	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_true(ch.ramping);
	ch.pending.valid = false;

	/* Inject overcurrent while ramping */
	vc_channel_consume_current(&ch, 200);

	zassert_equal(ch.active_fault_cause, 0,
		      "current protection must not fire during ramping");
	zassert_equal(ch.fault_history_cause, 0,
		      "current protection must not flag during ramping");
}

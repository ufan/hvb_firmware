/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "voltage_control/vc_channel.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

struct vc_stub_data {
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	uint16_t last_output_code;
	bool last_enable;
};

static struct vc_channel ch;
static const struct vc_system_config default_sys = {
	.operating_mode = VC_OPERATING_MODE_NORMAL,
};
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
	vc_channel_init(&ch, NULL, 0, FULL_CAPS, NULL, test_wake_fn, &ch);
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
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_is_null(ch.dev);
	zassert_is_null(ch.meas);
}

ZTEST(vc_channel_state, test_init_with_device)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, test_wake_fn, &hw_ch);
	zassert_equal(hw_ch.dev, dev);
}

ZTEST(vc_channel_state, test_default_config)
{
	struct vc_channel_config cfg;
	struct vc_channel_cal_config cal;

	zassert_equal(vc_channel_get_config(&ch, &cfg), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);

	zassert_equal(vc_channel_get_cal_config(&ch, &cal), VC_OK);
	zassert_equal(cal.output_calib_k, 10000);
	zassert_equal(cal.output_calib_b, 0);
	zassert_equal(cal.measured_voltage_calib_k, 10000);
	zassert_equal(cal.measured_current_calib_k, 10000);
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
	zassert_equal(ch.cal_config.max_raw_dac_limit, 0xFFFF);
}

/* ---- Output action ---- */

ZTEST(vc_channel_state, test_output_action_enable)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ch.output_enabled);
	zassert_true(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RAMPING);
}

ZTEST(vc_channel_state, test_output_action_disable_immediate)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE),
		      VC_OK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_HV_ON);
}

ZTEST(vc_channel_state, test_output_action_disable_force)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_FORCE),
		      VC_OK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_channel_state, test_output_action_rejected_with_active_fault)
{
	ch.active_fault_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(vc_channel_state, test_output_action_invalid_host_action)
{
	zassert_equal(vc_channel_output_action(&ch, (enum vc_output_action)99),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Fault command ---- */

ZTEST(vc_channel_state, test_fault_command_clear_history)
{
	ch.fault_history_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_fault_command(&ch, VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY),
		      VC_OK);
	zassert_equal(ch.fault_history_cause, 0);
}

ZTEST(vc_channel_state, test_fault_command_invalid)
{
	zassert_equal(vc_channel_fault_command(&ch, 3),
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
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 20000),
		      VC_OK);
	vc_channel_consume_voltage(&ch, 100);
	zassert_equal(ch.measured_voltage, 200);
}

ZTEST(vc_channel_state, test_consume_voltage_clamps_to_int16)
{
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 65535),
		      VC_OK);
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
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_FORCE;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_tick_ramp(&ch, 100, &default_sys);

	vc_channel_consume_current(&ch, 200);

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_false(ch.output_enabled);
}

/* ---- Consume fault ---- */

ZTEST(vc_channel_state, test_consume_fault_sets_hardware_fault)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);
	zassert_true(ch.fault_history_cause & VC_FAULT_HARDWARE);
	zassert_false(ch.output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_channel_state, test_consume_fault_interlock)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

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
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
}

ZTEST(vc_channel_state, test_tick_ramp_gradual)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 100);
	zassert_true(ch.ramping);
}

ZTEST(vc_channel_state, test_tick_ramp_no_output_enabled)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	vc_channel_set_config(&ch, &cfg);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 0);
}

ZTEST(vc_channel_state, test_tick_ramp_at_target_stops_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_equal(ch.operational_target_voltage, 100);
	zassert_false(ch.ramping);
}

/* ---- Set config validation ---- */

ZTEST(vc_channel_state, test_set_config_target_voltage_range)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(vc_channel_set_config(&ch, &cfg), VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(vc_channel_set_config(&ch, &cfg), VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(vc_channel_set_config(&ch, &cfg), VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_set_cal_field_works_at_channel_level)
{
	struct vc_channel_cal_config cal;

	/* vc_channel_set_cal_field has no mode guard — mode gating is at controller level */
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_OUTPUT_K, 20000), VC_OK);
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_OUTPUT_B, (uint16_t)(int16_t)-5), VC_OK);
	zassert_equal(vc_channel_get_cal_config(&ch, &cal), VC_OK);
	zassert_equal(cal.output_calib_k, 20000);
	zassert_equal(cal.output_calib_b, (int16_t)-5);
}

ZTEST(vc_channel_state, test_set_cal_field_all_fields)
{
	struct vc_channel_cal_config cal;

	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 11000), VC_OK);
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_B, (uint16_t)(int16_t)-10), VC_OK);
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_K, 12000), VC_OK);
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_B, 3), VC_OK);
	zassert_equal(vc_channel_get_cal_config(&ch, &cal), VC_OK);
	zassert_equal(cal.measured_voltage_calib_k, 11000);
	zassert_equal(cal.measured_voltage_calib_b, (int16_t)-10);
	zassert_equal(cal.measured_current_calib_k, 12000);
	zassert_equal(cal.measured_current_calib_b, 3);
}

ZTEST(vc_channel_state, test_set_field_target_voltage)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					   5000), VC_OK);
	vc_channel_get_config(&ch, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_channel_state, test_set_field_rejects_system_field)
{
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_OPERATING_MODE, 0),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_onoff_channel_rejects_output_drive_config)
{
	struct vc_channel onoff_ch;
	struct vc_channel_config cfg;

	vc_channel_init(&onoff_ch, NULL, 0, CH_CAP_OUTPUT_ENABLE, NULL, NULL, NULL);
	vc_channel_get_config(&onoff_ch, &cfg);
	cfg.configured_target_voltage = 100;
	zassert_equal(vc_channel_set_config(&onoff_ch, &cfg),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

/* ---- Calibration ---- */

ZTEST(vc_channel_state, test_cal_set_output_enable)
{
	zassert_equal(vc_channel_cal_set_output_enable(&ch, true), VC_OK);
	zassert_equal(ch.cal_output_enabled, 1);
}

ZTEST(vc_channel_state, test_cal_set_raw_dac_requires_output_enabled)
{
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_OK);
	zassert_equal(ch.raw_dac_readback, 100);
}

ZTEST(vc_channel_state, test_cal_max_raw_dac_limit)
{
	vc_channel_cal_set_max_raw_dac(&ch, 50);
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 51), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 50), VC_OK);
}

ZTEST(vc_channel_state, test_cal_sample_captures_raw)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 123);
	zassert_equal(vc_channel_cal_sample(&ch), VC_OK);
	zassert_equal(ch.raw_adc_voltage, 123);
}

ZTEST(vc_channel_state, test_cal_commit_requires_output_disabled)
{
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_commit(&ch), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, false);
	zassert_equal(vc_channel_cal_commit(&ch), VC_OK);
}

ZTEST(vc_channel_state, test_cal_disable_clears_sample_state)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 50);
	vc_channel_cal_sample(&ch);
	vc_channel_cal_set_output_enable(&ch, false);
	zassert_equal(ch.raw_adc_voltage, 0);
	zassert_equal(ch.raw_adc_current, 0);
}

ZTEST(vc_channel_state, test_reset_calibration_entering)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 100);
	vc_channel_reset_calibration(&ch, true);

	zassert_equal(ch.raw_dac_readback, 0);
	zassert_equal(ch.cal_output_enabled, 0);
	zassert_equal(ch.cal_config.max_raw_dac_limit, 0xFFFF);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
}

ZTEST(vc_channel_state, test_reset_calibration_exiting)
{
	vc_channel_reset_calibration(&ch, false);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- vc_channel_run ---- */

ZTEST(vc_channel_state, test_run_consumes_meas_buffer)
{
	struct vc_channel_buffer meas = {
		.channel_id = 0,
	};
	struct vc_channel run_ch;

	vc_channel_buffer_publish_voltage(&meas, 1500, 100);
	vc_channel_buffer_publish_current(&meas, 300, 100);
	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, &meas, NULL, NULL);

	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 1500);
	zassert_equal(run_ch.measured_current, 300);
	zassert_equal(run_ch.last_consumed_voltage_ts, 100);
	zassert_equal(run_ch.last_consumed_current_ts, 100);
}

ZTEST(vc_channel_state, test_run_skips_unchanged_timestamps)
{
	struct vc_channel_buffer meas = {
		.channel_id = 0,
		.raw_voltage = 1500,
		.voltage_timestamp_ms = 100,
		.raw_current = 300,
		.current_timestamp_ms = 100,
	};
	struct vc_channel run_ch;

	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, &meas, NULL, NULL);
	vc_channel_run(&run_ch, 100, &default_sys);

	meas.raw_voltage = 9999;
	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 1500,
		      "should not re-consume with same timestamp");
}

ZTEST(vc_channel_state, test_run_null_meas_is_safe)
{
	struct vc_channel run_ch;

	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, NULL, NULL, NULL);
	vc_channel_run(&run_ch, 100, &default_sys);
	zassert_equal(run_ch.measured_voltage, 0);
}

/* ---- apply_hw with stub driver ---- */

ZTEST(vc_channel_state, test_apply_hw_calls_driver)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, NULL, NULL);

	struct vc_channel_config cfg;

	vc_channel_get_config(&hw_ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&hw_ch, &cfg);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_tick_ramp(&hw_ch, 100, &default_sys);

	struct vc_stub_data *stub = dev->data;

	zassert_true(stub->last_enable, "hw should be enabled");
	zassert_equal(stub->last_output_code, 5000, "hw should have DAC code 5000");
}

ZTEST(vc_channel_state, test_apply_hw_disable_immediate_hv_on_dac_zero)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, NULL, NULL);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE);

	struct vc_stub_data *stub = dev->data;

	zassert_true(stub->last_enable, "HV must stay on after disable_immediate");
	zassert_equal(stub->last_output_code, 0, "DAC must be zero after disable_immediate");
}

ZTEST(vc_channel_state, test_apply_hw_disable_force_hv_off)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, NULL, NULL);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_DISABLE_FORCE);

	struct vc_stub_data *stub = dev->data;

	zassert_false(stub->last_enable, "HV must be off after disable_force");
	zassert_equal(stub->last_output_code, 0, "DAC must be zero after disable_force");
}

ZTEST(vc_channel_state, test_current_protection_skipped_during_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_FORCE;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_true(ch.ramping);

	vc_channel_consume_current(&ch, 200);

	zassert_equal(ch.active_fault_cause, 0,
		      "current protection must not fire during ramping");
	zassert_equal(ch.fault_history_cause, 0,
		      "current protection must not flag during ramping");
}

/* ---- Meas callback registration ---- */

ZTEST(vc_channel_state, test_meas_callback_registered_with_hw)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, test_wake_fn, &hw_ch);

	struct vc_stub_data *stub = dev->data;

	zassert_not_null(stub->meas_cb, "init should register meas callback on hw");

	wake_count = 0;
	stub->meas_cb(0, stub->meas_cb_user_data);
	zassert_equal(wake_count, 1, "meas callback should invoke wake_fn");
}

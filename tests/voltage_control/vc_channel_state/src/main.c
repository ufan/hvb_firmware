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
	zassert_equal(cfg.current_limit_threshold, 10000);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);

	zassert_equal(vc_channel_get_cal_config(&ch, &cal), VC_OK);
	zassert_equal(cal.output_calib_k, 32768);
	zassert_equal(cal.output_calib_b, 0);
	zassert_equal(cal.measured_voltage_calib_k, 1);
	zassert_equal(cal.measured_current_calib_k, 1);
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
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 50000),
		      VC_OK);
	vc_channel_consume_voltage(&ch, 24000);
	zassert_equal(ch.measured_voltage, 1200);
	zassert_equal(ch.raw_adc_voltage, 24000);
}

ZTEST(vc_channel_state, test_consume_voltage_with_calibration_gain)
{
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 20000),
		      VC_OK);
	vc_channel_consume_voltage(&ch, 10000);
	zassert_equal(ch.measured_voltage, 200);
}

ZTEST(vc_channel_state, test_consume_voltage_clamps_to_int16)
{
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_V_K, 65535),
		      VC_OK);
	vc_channel_consume_voltage(&ch, 600000);
	zassert_equal(ch.measured_voltage, INT16_MAX);
}

/* ---- Consume current ---- */

ZTEST(vc_channel_state, test_consume_current_applies_calibration)
{
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_K, 50000),
		      VC_OK);
	vc_channel_consume_current(&ch, 10000);
	zassert_equal(ch.measured_current, 500);
	zassert_equal(ch.raw_adc_current, 10000);
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

	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_K, 50000),
		      VC_OK);
	vc_channel_consume_current(&ch, 10000);

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_current_protection_graceful_disable_ramps_to_zero)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_K, 50000),
		      VC_OK);

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_GRACEFUL;
	cfg.ramp_up_step = 5000;
	cfg.ramp_up_interval = 1;
	cfg.ramp_down_step = 1000;
	cfg.ramp_down_interval = 1;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_tick_ramp(&ch, 1000, &default_sys);
	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.ramping);

	vc_channel_consume_current(&ch, 5000); /* measured = 250, exceeds threshold 100 */
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_true(ch.ramping, "graceful disable must arm a ramp-to-zero");
	zassert_true(ch.output_enabled, "graceful disable must not cut output instantly");

	/* Drive the ramp-down. Without the fix this never progresses, because
	 * vc_channel_tick_ramp() bails out while active_fault_cause is set --
	 * exactly the fault this ramp-down exists to resolve. */
	for (int i = 0; i < 10; i++) {
		vc_channel_tick_ramp(&ch, 1000, &default_sys);
	}

	zassert_equal(ch.operational_target_voltage, 0,
		      "graceful disable must actually ramp to zero despite the active fault "
		      "that triggered it");
	zassert_false(ch.output_enabled);
	zassert_false(ch.ramping);
}

/* ---- Automatic recovery ---- */

static void arm_current_fault(struct vc_channel *ch, enum vc_recovery_policy_mode recovery)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_cal_field(ch, VC_CAL_FIELD_MEASURED_I_K, 50000), VC_OK);

	vc_channel_get_config(ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
	cfg.recovery_policy_mode = recovery;
	cfg.auto_retry_delay = 1;
	cfg.auto_retry_max_count = 3;
	cfg.auto_retry_window = 60;
	cfg.ramp_up_step = 5000;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(ch, &cfg);
	vc_channel_output_action(ch, VC_OUTPUT_ACTION_ENABLE);
	vc_channel_tick_ramp(ch, 1000, &default_sys);

	vc_channel_consume_current(ch, 5000); /* measured = 250, exceeds threshold 100 */
	zassert_true(ch->active_fault_cause & VC_FAULT_CURRENT);
}

ZTEST(vc_channel_state, test_recovery_stays_latched_in_normal_mode)
{
	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &default_sys); /* default_sys = NORMAL mode */
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "Automatic-only recovery must never act in Normal mode");
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_stays_latched_with_manual_policy)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_MANUAL_LATCH);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "MANUAL_LATCH must never auto-retry even in Automatic mode");
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_ignores_non_current_fault)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	cfg.auto_retry_delay = 1;
	cfg.auto_retry_max_count = 3;
	cfg.auto_retry_window = 60;
	vc_channel_set_config(&ch, &cfg);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);
	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);

	for (int i = 0; i < 20; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE,
		     "hardware faults are never auto-recoverable, regardless of policy");
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_auto_retry_clears_fault_after_cooldown_and_safe_band)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	zassert_false(ch.output_enabled);

	/* Still above the safe band (threshold 100, 10% band -> safe at <=90).
	 * Ticking through the 1s cooldown must not clear the fault yet. */
	for (int i = 0; i < 3; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "still unsafe -- must not retry even after cooldown elapses");
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RETRY_COOLDOWN);

	/* Current drops to a safe level (raw 100 -> measured 5, well under 90). */
	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_equal(ch.active_fault_cause, 0, "safe now -- retry must clear the fault");
	zassert_true(ch.output_enabled);
	zassert_equal(ch.recovery_target, 5000, "AUTO_RETRY targets the full configured value");

	/* Drive the recovery ramp to completion. */
	for (int i = 0; i < 5; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}
	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.recovering);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
}

ZTEST(vc_channel_state, test_recovery_exhausts_after_max_retries)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	ch.config.auto_retry_max_count = 2;

	for (int attempt = 0; attempt < 2; attempt++) {
		vc_channel_consume_current(&ch, 100); /* safe */
		vc_channel_run(&ch, 1000, &auto_sys);  /* cooldown elapses, retries */
		zassert_equal(ch.active_fault_cause, 0,
			      "attempt %d must succeed (under max count)", attempt);

		/* Re-fault immediately so the next attempt has something to retry from. */
		vc_channel_consume_current(&ch, 5000);
		zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	}

	/* Third attempt: max_count (2) already used up inside the window. */
	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys);

	zassert_true(ch.active_fault_cause & VC_FAULT_RETRY_EXHAUST,
		     "third attempt must exhaust and latch, not retry again");
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_false(ch.output_enabled);
}

ZTEST(vc_channel_state, test_recovery_window_expiry_resets_count)
{
	struct vc_system_config auto_sys = { .operating_mode = VC_OPERATING_MODE_AUTOMATIC };
	struct vc_channel_snapshot snap;

	arm_current_fault(&ch, VC_RECOVERY_AUTO_RETRY);
	ch.config.auto_retry_max_count = 1;
	ch.config.auto_retry_window = 5; /* seconds */

	vc_channel_consume_current(&ch, 100);
	vc_channel_run(&ch, 1000, &auto_sys); /* first (and only allowed) retry */
	zassert_equal(ch.active_fault_cause, 0);

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.auto_retry_count, 1);

	/* Advance well past the 5s window with no further faults. */
	for (int i = 0; i < 10; i++) {
		vc_channel_run(&ch, 1000, &auto_sys);
	}

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.auto_retry_count, 0,
		      "retry timestamps older than the window must age out");
}

ZTEST(vc_channel_state, test_set_field_does_not_evaluate_protection_synchronously)
{
	/* A real Modbus write of mode+action+threshold lands as three separate
	 * single-register writes. Evaluating protection after each individual
	 * field write risks latching a fault against a stale/partial config --
	 * exactly the race that let a fault fire using the wrong action before
	 * the caller's intended config had fully landed. */
	zassert_equal(vc_channel_set_cal_field(&ch, VC_CAL_FIELD_MEASURED_I_K, 50000),
		      VC_OK);
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_CURRENT_LIMIT_THRESHOLD, 100),
		      VC_OK);
	vc_channel_consume_current(&ch, 5000); /* measured = 250, already above 100 */
	zassert_equal(ch.active_fault_cause, 0, "mode is still Disabled, must not fault yet");

	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_CURRENT_PROTECTION_MODE,
					   VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION),
		      VC_OK);

	zassert_equal(ch.active_fault_cause, 0,
		      "a config write alone must never trigger protection -- only a fresh "
		      "current sample may, so mode/action/threshold always land as a "
		      "consistent whole before evaluation happens");

	vc_channel_consume_current(&ch, 5000);
	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT,
		     "the next real sample must evaluate against the now-armed config");
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

	vc_channel_tick_ramp(&ch, 1000, &default_sys);

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

ZTEST(vc_channel_state, test_set_field_rejects_retry_max_count_above_history_cap)
{
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_AUTO_RETRY_MAX_COUNT,
					   CONFIG_VC_MAX_RETRY_HISTORY),
		      VC_OK, "exactly at the cap must be accepted");
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_AUTO_RETRY_MAX_COUNT,
					   CONFIG_VC_MAX_RETRY_HISTORY + 1),
		      VC_ERR_INVALID_VALUE, "above the cap must be rejected");
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


ZTEST(vc_channel_state, test_cal_sample_captures_raw)
{
	struct vc_channel_buffer meas = {
		.channel_id = 0,
	};
	struct vc_channel cal_ch;

	vc_channel_init(&cal_ch, NULL, 0, FULL_CAPS, &meas, NULL, NULL);
	vc_channel_buffer_publish_voltage(&meas, 4567, 10);
	vc_channel_buffer_publish_current(&meas, 89, 10);

	vc_channel_cal_set_output_enable(&cal_ch, true);
	vc_channel_cal_set_raw_dac(&cal_ch, 123);

	zassert_equal(vc_channel_cal_sample(&cal_ch), VC_OK);
	zassert_equal(cal_ch.raw_adc_voltage, 4567,
		      "cal sample must read the real ADC buffer, not echo the DAC code");
	zassert_equal(cal_ch.raw_adc_current, 89);
}

ZTEST(vc_channel_state, test_cal_sample_respects_capability_mask)
{
	struct vc_channel_buffer meas = {
		.channel_id = 0,
	};
	struct vc_channel cal_ch;
	uint16_t voltage_only_caps = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
				      CH_CAP_VOLTAGE_MEASUREMENT;

	vc_channel_init(&cal_ch, NULL, 0, voltage_only_caps, &meas, NULL, NULL);
	vc_channel_buffer_publish_voltage(&meas, 4567, 10);
	vc_channel_buffer_publish_current(&meas, 89, 10);

	vc_channel_cal_set_output_enable(&cal_ch, true);
	vc_channel_cal_set_raw_dac(&cal_ch, 123);

	zassert_equal(vc_channel_cal_sample(&cal_ch), VC_OK);
	zassert_equal(cal_ch.raw_adc_voltage, 4567);
	zassert_equal(cal_ch.raw_adc_current, 0,
		      "channel without CH_CAP_CURRENT_MEASUREMENT must not surface buffer current");
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
	zassert_equal(vc_channel_set_cal_field(&run_ch, VC_CAL_FIELD_MEASURED_V_K, 50000), VC_OK);
	zassert_equal(vc_channel_set_cal_field(&run_ch, VC_CAL_FIELD_MEASURED_I_K, 50000), VC_OK);

	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 75);
	zassert_equal(run_ch.measured_current, 15);
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
	zassert_equal(vc_channel_set_cal_field(&run_ch, VC_CAL_FIELD_MEASURED_V_K, 50000), VC_OK);
	vc_channel_run(&run_ch, 100, &default_sys);

	meas.raw_voltage = 9999;
	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 75,
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
	zassert_equal(stub->last_output_code, 16384, "hw should have DAC code 16384");
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

/* ---- vc_channel_no_dac: on/off-only channel (LVB topology) ---- */

#define LVB_CAPS (CH_CAP_OUTPUT_ENABLE | \
		  CH_CAP_VOLTAGE_MEASUREMENT | \
		  CH_CAP_CURRENT_MEASUREMENT)

static struct vc_channel no_dac_ch;

static void before_each_no_dac(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_channel_init(&no_dac_ch, NULL, 1, LVB_CAPS, NULL, NULL, NULL);
}

ZTEST_SUITE(vc_channel_no_dac, NULL, NULL, before_each_no_dac, NULL, NULL);

ZTEST(vc_channel_no_dac, test_cal_output_enable_rejected)
{
	zassert_equal(vc_channel_cal_set_output_enable(&no_dac_ch, true),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

ZTEST(vc_channel_no_dac, test_output_enable_action_accepted)
{
	zassert_equal(vc_channel_output_action(&no_dac_ch, VC_OUTPUT_ACTION_ENABLE),
		      VC_OK);
	zassert_true(no_dac_ch.output_enabled);
}

ZTEST(vc_channel_no_dac, test_disable_immediate_accepted)
{
	vc_channel_output_action(&no_dac_ch, VC_OUTPUT_ACTION_ENABLE);
	zassert_equal(vc_channel_output_action(&no_dac_ch,
					       VC_OUTPUT_ACTION_DISABLE_IMMEDIATE),
		      VC_OK);
	zassert_false(no_dac_ch.output_enabled);
}

ZTEST(vc_channel_no_dac, test_voltage_measurement_consumed)
{
	struct vc_channel_snapshot snap;

	zassert_equal(vc_channel_set_cal_field(&no_dac_ch, VC_CAL_FIELD_MEASURED_V_K, 50000),
		      VC_OK);
	vc_channel_consume_voltage(&no_dac_ch, 70000);
	vc_channel_get_snapshot(&no_dac_ch, &snap);
	zassert_equal(snap.measured_voltage, 3500);
}

ZTEST(vc_channel_no_dac, test_current_measurement_consumed)
{
	struct vc_channel_snapshot snap;

	zassert_equal(vc_channel_set_cal_field(&no_dac_ch, VC_CAL_FIELD_MEASURED_I_K, 50000),
		      VC_OK);
	vc_channel_consume_current(&no_dac_ch, 4000);
	vc_channel_get_snapshot(&no_dac_ch, &snap);
	zassert_equal(snap.measured_current, 200);
}

ZTEST(vc_channel_no_dac, test_capabilities_reported)
{
	struct vc_channel_snapshot snap;

	vc_channel_get_snapshot(&no_dac_ch, &snap);
	zassert_equal(snap.channel_capability_flags, LVB_CAPS);
}

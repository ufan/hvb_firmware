/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voltage-control domain unit tests.
 * Runs on native_posix.  No hardware required.
 */

#include <zephyr/ztest.h>
#include <stdlib.h>

#include "voltage_control/domain.h"
#include "voltage_control/variant.h"

static void *domain_setup_fresh(void)
{
	const struct vc_variant_profile *variant = vc_hvb_get_variant();
	zassert_not_null(variant);
	struct vc_domain *d = vc_domain_create(variant);
	zassert_not_null(d);
	return d;
}

ZTEST_SUITE(voltage_control_domain, NULL, NULL, NULL, NULL, NULL);

/* ---- Variant profile ---- */

ZTEST(voltage_control_domain, test_variant_id)
{
	struct vc_domain *d = domain_setup_fresh();
	zassert_equal(vc_domain_get_variant_id(d), 1);
	free(d);
}

ZTEST(voltage_control_domain, test_supported_channels)
{
	struct vc_domain *d = domain_setup_fresh();
	zassert_equal(vc_domain_get_supported_channel_count(d), 2);
	zassert_equal(vc_domain_get_active_channel_mask(d), 0x0003);
	free(d);
}

ZTEST(voltage_control_domain, test_channel0_supported)
{
	struct vc_domain *d = domain_setup_fresh();
	zassert_true(vc_domain_is_channel_supported(d, 0));
	zassert_true(vc_domain_is_channel_supported(d, 1));
	zassert_false(vc_domain_is_channel_supported(d, 2));
	free(d);
}

/* ---- System config defaults ---- */

ZTEST(voltage_control_domain, test_system_config_defaults)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_system_config cfg;

	vc_domain_get_system_config(d, &cfg);
	zassert_equal(cfg.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_equal(cfg.slave_address, 1);
	zassert_equal(cfg.baud_rate_code, VC_BAUD_RATE_115200);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
	zassert_equal(cfg.voltage_safe_band_pct, 10);
	zassert_equal(cfg.current_safe_band_pct, 10);
	free(d);
}

/* ---- Channel config defaults ---- */

ZTEST(voltage_control_domain, test_channel_config_defaults)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	vc_domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.voltage_protection_mode, VC_PROTECTION_MODE_DISABLED);
	zassert_equal(cfg.voltage_protection_output_action,
		      VC_OUTPUT_ACTION_NONE);
	zassert_equal(cfg.voltage_limit_threshold, 20000);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.output_calib_k, 10000);
	zassert_equal(cfg.output_calib_b, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);
	zassert_equal(cfg.measured_voltage_calib_b, 0);
	zassert_equal(cfg.measured_current_calib_k, 10000);
	zassert_equal(cfg.measured_current_calib_b, 0);
	free(d);
}

/* ---- Config validation ---- */

ZTEST(voltage_control_domain, test_target_voltage_range)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(vc_domain_set_channel_config(d, 0, &cfg), VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(vc_domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(vc_domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

ZTEST(voltage_control_domain, test_unsupported_channel)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	vc_domain_get_channel_config(d, 0, &cfg);
	zassert_equal(vc_domain_set_channel_config(d, 2, &cfg),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	free(d);
}

ZTEST(voltage_control_domain, test_operating_mode_validation)
{
	struct vc_domain *d = domain_setup_fresh();

	zassert_equal(vc_domain_set_operating_mode(d,
			 VC_OPERATING_MODE_AUTOMATIC), VC_OK);
	zassert_equal(vc_domain_get_operating_mode(d),
		      VC_OPERATING_MODE_AUTOMATIC);

	zassert_equal(vc_domain_set_operating_mode(d,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	zassert_equal(vc_domain_get_operating_mode(d),
		      VC_OPERATING_MODE_NORMAL);

	zassert_equal(vc_domain_set_operating_mode(d, 2),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

/* ---- System snapshot ---- */

ZTEST(voltage_control_domain, test_system_snapshot)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_system_snapshot snap;

	vc_domain_get_system_snapshot(d, &snap);
	zassert_equal(snap.protocol_major, 2);
	zassert_equal(snap.protocol_minor, 0);
	zassert_equal(snap.variant_id, 1);
	zassert_equal(snap.supported_channel_count, 2);
	zassert_equal(snap.active_channel_mask, 0x0003);
	free(d);
}

/* ---- Channel snapshot ---- */

ZTEST(voltage_control_domain, test_channel_snapshot_unsupported)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	zassert_equal(vc_domain_get_channel_snapshot(d, 2, &snap),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	free(d);
}

/* ---- Output action ---- */

ZTEST(voltage_control_domain, test_output_action_valid)
{
	struct vc_domain *d = domain_setup_fresh();

	zassert_equal(vc_domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(vc_domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_equal(vc_domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_DISABLE_GRACEFUL), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_output_action_host_context_invalid)
{
	struct vc_domain *d = domain_setup_fresh();

	zassert_equal(vc_domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_CLAMP),
		      VC_ERR_INVALID_COMMAND);
	free(d);
}

/* ---- Fault command ---- */

ZTEST(voltage_control_domain, test_fault_command_clear_history)
{
	struct vc_domain *d = domain_setup_fresh();

	zassert_equal(vc_domain_channel_fault_command(d, 0,
			 VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_fault_command_invalid)
{
	struct vc_domain *d = domain_setup_fresh();

	zassert_equal(vc_domain_channel_fault_command(d, 0, 3),
		      VC_ERR_INVALID_COMMAND);
	free(d);
}

/* ---- Domain tick: ramp ---- */

ZTEST(voltage_control_domain, test_tick_instant_ramp)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	cfg.ramp_up_interval = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 5000,
		      "instant ramp must reach target in one tick");
	free(d);
}

/* ---- Domain tick: measured values ---- */

ZTEST(voltage_control_domain, test_tick_measured_with_noise)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {7, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);
	zassert_equal(snap.measured_voltage, 1007,
		      "measured = target + noise");
	free(d);
}

/* ---- Domain tick: protection triggers ---- */

ZTEST(voltage_control_domain, test_tick_protection_triggers_fault)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_true(snap.active_fault_cause & VC_FAULT_VOLTAGE,
		     "fault must trigger when measured > limit");
	zassert_true(snap.fault_history_cause & VC_FAULT_VOLTAGE,
		     "fault history must record event");
	zassert_equal(snap.operational_target_voltage, 0,
		      "Force Output Zero must zero target");
	free(d);
}

/* ---- Domain tick: flag only does not create active fault ---- */

ZTEST(voltage_control_domain, test_tick_flag_only)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_FLAG_ONLY;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.active_fault_cause, 0,
		      "flag-only must not create active fault");
	zassert_true(snap.fault_history_cause & VC_FAULT_VOLTAGE,
		     "flag-only must record fault history");
	free(d);
}

/* ---- Uptime ---- */

ZTEST(voltage_control_domain, test_uptime)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_system_snapshot snap;

	vc_domain_set_uptime(d, 42);
	vc_domain_get_system_snapshot(d, &snap);
	zassert_equal(snap.uptime, 42);
	free(d);
}

/* ---- Protection output action: clamp ---- */

ZTEST(voltage_control_domain, test_protection_action_clamp)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_CLAMP;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 3000,
		      "clamp must set target to limit threshold");
	free(d);
}

/* ---- Mode transition: Automatic→Normal cancels cooldown ---- */

ZTEST(voltage_control_domain, test_mode_transition_cancels_cooldown)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	struct vc_system_config sys;
	int16_t v_noise[VC_MAX_CHANNELS] = {100, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_system_config(d, &sys);
	sys.operating_mode = VC_OPERATING_MODE_AUTOMATIC;
	sys.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	sys.auto_retry_delay = 60;
	vc_domain_set_system_config(d, &sys);

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_true(snap.auto_cooldown_remaining > 0,
		     "cooldown must start after fault in automatic mode");

	vc_domain_set_operating_mode(d, VC_OPERATING_MODE_NORMAL);
	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.auto_cooldown_remaining, 0,
		      "automatic→normal must cancel cooldown");
	free(d);
}

/* ---- Enable rejected when active fault present ---- */

ZTEST(voltage_control_domain, test_enable_rejected_with_active_fault)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_domain_tick(d, 500, v_noise, c_noise);

	zassert_equal(vc_domain_channel_output_action(d, 0,
		VC_OUTPUT_ACTION_ENABLE), VC_ERR_UNSAFE_STATE,
		"enable must be rejected when active fault is present");
	free(d);
}

/* ---- Simultaneous current+voltage fault: current priority, both bits ---- */

ZTEST(voltage_control_domain, test_dual_fault_current_priority)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {10, 0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.current_limit_threshold = 100;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_CLAMP;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_true(snap.active_fault_cause & VC_FAULT_CURRENT,
		"current fault bit must be set");
	zassert_true(snap.active_fault_cause & VC_FAULT_VOLTAGE,
		"voltage fault bit must also be set");
	zassert_true(snap.fault_history_cause & VC_FAULT_CURRENT,
		"current fault history must be set");
	zassert_true(snap.fault_history_cause & VC_FAULT_VOLTAGE,
		"voltage fault history must also be set");
	zassert_equal(snap.operational_target_voltage, 0,
		"current determines action: ForceOutputZero");
	zassert_equal(snap.last_protection_output_action,
		VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO,
		"current action recorded as last protection output action");
	free(d);
}

/* ---- Current-only fault with voltage flag-only: both histories, current action ---- */

ZTEST(voltage_control_domain, test_current_fault_voltage_flag_only)
{
	struct vc_domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {10, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {10, 0};

	vc_domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.current_limit_threshold = 100;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_FLAG_ONLY;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_domain_set_channel_config(d, 0, &cfg);
	vc_domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_domain_tick(d, 500, v_noise, c_noise);

	vc_domain_get_channel_snapshot(d, 0, &snap);
	zassert_true(snap.active_fault_cause & VC_FAULT_CURRENT,
		"current fault active");
	zassert_false(snap.active_fault_cause & VC_FAULT_VOLTAGE,
		"voltage flag-only must not create active fault");
	zassert_true(snap.fault_history_cause & VC_FAULT_CURRENT,
		"current fault history");
	zassert_true(snap.fault_history_cause & VC_FAULT_VOLTAGE,
		"voltage flag-only still records fault history");
	free(d);
}

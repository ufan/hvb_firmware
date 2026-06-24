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

#include "regmap/vc_regs.h"
#include "voltage_control/domain.h"
#include "modbus_adapter/modbus_adapter.h"
#include "voltage_control/runtime.h"
#include "sys_status/sys_status.h"

struct sys_status_snapshot sys_status_get(void)
{
	return (struct sys_status_snapshot){
		.board_temperature = 250,
		.board_humidity = 500,
	};
}

struct vc_ctx {
	struct vc_runtime *runtime;
};

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
	{ .dev = NULL, .index = 1,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
};

static const struct vc_channel_entry onoff_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE },
};

static void *domain_setup_fresh(void)
{
	struct domain *d = domain_create(test_channels, 2);

	zassert_not_null(d);
	return d;
}

static void *domain_setup_on_off_only(void)
{
	struct domain *d = domain_create(onoff_channels, 1);

	zassert_not_null(d);
	return d;
}

static void sim_tick(struct domain *d, uint32_t dt_ms,
		     const int16_t *v_noise, const int16_t *c_noise)
{
	uint8_t n = domain_get_supported_channel_count(d);

	domain_process_periodic(d, dt_ms);

	if (domain_get_operating_mode(d) == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}

	for (uint8_t ch = 0; ch < n; ch++) {
		struct vc_channel_snapshot snap;

		if (domain_get_channel_snapshot(d, ch, &snap) == VC_OK) {
			int32_t mv = snap.operational_target_voltage + v_noise[ch];
			int32_t mi = mv / 2 + c_noise[ch];
			struct vc_measurement_snapshot meas = {
				.channel = ch,
				.generation = 1,
				.present_mask = VC_MEAS_PRESENT_VOLTAGE |
						VC_MEAS_PRESENT_CURRENT,
				.raw_voltage = mv,
				.raw_current = mi,
			};

			(void)domain_consume_measurement(d, &meas);
		}
	}

	domain_process_periodic(d, 0);
}

static void enter_calibration_mode(struct domain *d)
{
	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP2), VC_OK);
	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_CALIBRATION), VC_OK);
}

ZTEST_SUITE(voltage_control_domain, NULL, NULL, NULL, NULL, NULL);

ZTEST(voltage_control_domain, test_runtime_contract_defaults_are_zeroable)
{
	struct vc_runtime_config_snapshot cfg = {0};
	struct vc_measurement_snapshot meas = {0};

	zassert_equal(cfg.channel, 0);
	zassert_equal(cfg.version, 0);
	zassert_equal(cfg.capability_flags, 0);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_false(cfg.calibration_mode);
	zassert_false(cfg.calibration_output_enable);
	zassert_equal(cfg.calibration_raw_output_drive, 0);
	zassert_false(cfg.force_safe_state);

	zassert_equal(meas.channel, 0);
	zassert_equal(meas.generation, 0);
	zassert_equal(meas.timestamp_ms, 0);
	zassert_equal(meas.present_mask, 0);
	zassert_equal(meas.raw_voltage, 0);
	zassert_equal(meas.raw_current, 0);
	zassert_equal(meas.provider_status, 0);
	zassert_equal(meas.provider_fault_cause, 0);
}

ZTEST(voltage_control_domain, test_domain_create_static_initializes_domain_without_heap)
{
	struct domain *d = domain_create_static(test_channels, 2);

	zassert_not_null(d);
	zassert_equal(domain_get_supported_channel_count(d), 2);
	zassert_equal(domain_get_operating_mode(d), VC_OPERATING_MODE_NORMAL);
}

ZTEST(voltage_control_domain, test_initial_runtime_config_is_safe)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_equal(cfg.channel, 0);
	zassert_equal(cfg.version, 1);
	zassert_equal(cfg.capability_flags,
		      CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
		      CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_false(cfg.calibration_mode);
	zassert_false(cfg.calibration_output_enable);
	zassert_equal(cfg.calibration_raw_output_drive, 0);
	zassert_true(cfg.force_safe_state);

	free(d);
}

ZTEST(voltage_control_domain, test_runtime_config_rejects_unsupported_channel)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_get_runtime_config(d, 2, &cfg),
		      VC_ERR_UNSUPPORTED_CHANNEL);

	free(d);
}

ZTEST(voltage_control_domain, test_runtime_config_rejects_null_snapshot)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_get_runtime_config(d, 0, NULL), VC_ERR_INVALID_VALUE);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consumes_measurement_snapshot)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 1234,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE |
				      VC_MEAS_PRESENT_CURRENT |
				      VC_MEAS_PRESENT_PROVIDER_STATUS,
		.raw_voltage = 1200,
		.raw_current = 34,
		.provider_status = VC_PROVIDER_STATUS_READY,
	};
	struct vc_channel_snapshot snap;

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 1200);
	zassert_equal(snap.raw_adc_current, 34);
	zassert_equal(snap.measured_voltage, 1200);
	zassert_equal(snap.measured_current, 34);
	zassert_equal(snap.active_fault_cause, 0);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consume_measurement_rejects_null)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_consume_measurement(d, NULL), VC_ERR_INVALID_VALUE);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consume_measurement_rejects_unsupported_channel)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 2,
	};

	zassert_equal(domain_consume_measurement(d, &meas),
		      VC_ERR_UNSUPPORTED_CHANNEL);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consume_measurement_rejects_unsupported_voltage)
{
	struct domain *d = domain_setup_on_off_only();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 1200,
	};
	struct vc_channel_snapshot snap;

	zassert_equal(domain_consume_measurement(d, &meas),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 0);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consume_measurement_sample_error_sets_fault)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_SAMPLE_ERROR,
	};
	struct vc_channel_snapshot snap;

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_true((snap.active_fault_cause & VC_FAULT_MEASUREMENT) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_MEASUREMENT) != 0);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_consume_measurement_clamps_calibrated_voltage)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 20000,
	};
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.measured_voltage_calib_k = 65535;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_NORMAL), VC_OK);

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.measured_voltage, INT16_MAX);

	free(d);
}

ZTEST(voltage_control_domain, test_runtime_config_calibration_output_is_not_safe_state)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot cfg;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 123), VC_OK);

	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_true(cfg.calibration_mode);
	zassert_true(cfg.calibration_output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_equal(cfg.calibration_raw_output_drive, 123);
	zassert_false(cfg.force_safe_state);

	free(d);
}

ZTEST(voltage_control_domain, test_runtime_config_version_bumps_on_output_enable)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;

	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);

	zassert_equal(after.version, before.version + 1);
	zassert_true(after.output_enable);
	zassert_false(after.force_safe_state);

	free(d);
}

ZTEST(voltage_control_domain, test_runtime_config_version_ignores_idempotent_output_actions)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;

	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(after.version, before.version);
	zassert_true(after.output_enable);

	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(after.version, before.version);
	zassert_false(after.output_enable);
	zassert_true(after.force_safe_state);

	free(d);
}

ZTEST(voltage_control_domain, test_tick_updates_normal_raw_output_drive_intent)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;
	struct vc_channel_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(before.raw_output_drive, 0);

	sim_tick(d, 100, quiet, quiet);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(after.version, before.version + 1);
	zassert_equal(snap.operational_target_voltage, 1000);
	zassert_equal(after.raw_output_drive, 1000);

	free(d);
}

ZTEST(voltage_control_domain, test_tick_without_visible_output_change_keeps_runtime_version)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);

	sim_tick(d, 100, quiet, quiet);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(after.version, before.version);
	zassert_equal(after.raw_output_drive, before.raw_output_drive);

	free(d);
}

ZTEST(voltage_control_domain, test_normal_raw_output_drive_applies_output_calibration_gain)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	enter_calibration_mode(d);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.output_calib_k = 20000;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_NORMAL), VC_OK);

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);

	zassert_equal(domain_get_runtime_config(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_output_drive, 200);

	free(d);
}

ZTEST(voltage_control_domain, test_normal_raw_output_drive_clamps_low)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	enter_calibration_mode(d);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.output_calib_b = -1000;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_NORMAL), VC_OK);

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);

	zassert_equal(domain_get_runtime_config(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_output_drive, 0);

	free(d);
}

ZTEST(voltage_control_domain, test_normal_raw_output_drive_clamps_high)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	enter_calibration_mode(d);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.output_calib_k = UINT16_MAX;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_NORMAL), VC_OK);

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 20000;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);

	zassert_equal(domain_get_runtime_config(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_output_drive, UINT16_MAX);

	free(d);
}

ZTEST(voltage_control_domain, test_apply_failed_forces_safe_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;
	struct vc_channel_snapshot snap;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_APPLY_FAILED,
	};
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_true(before.output_enable);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.operational_target_voltage, 1000);

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(after.version, before.version + 1);
	zassert_false(after.output_enable);
	zassert_true(after.force_safe_state);
	zassert_equal(after.raw_output_drive, 0);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true((snap.active_fault_cause & VC_FAULT_HARDWARE) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_HARDWARE) != 0);

	free(d);
}

ZTEST(voltage_control_domain, test_provider_fault_cause_hardware_forces_safe_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;
	struct vc_channel_snapshot snap;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_fault_cause = VC_FAULT_HARDWARE,
	};
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_true(before.output_enable);

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(after.version, before.version + 1);
	zassert_false(after.output_enable);
	zassert_true(after.force_safe_state);
	zassert_equal(after.raw_output_drive, 0);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true((snap.active_fault_cause & VC_FAULT_HARDWARE) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_HARDWARE) != 0);

	free(d);
}

ZTEST(voltage_control_domain, test_interlock_forces_safe_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;
	struct vc_channel_snapshot snap;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_INTERLOCK,
	};
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	sim_tick(d, 100, quiet, quiet);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_true(before.output_enable);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.operational_target_voltage, 1000);

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(after.version, before.version + 1);
	zassert_false(after.output_enable);
	zassert_true(after.force_safe_state);
	zassert_equal(after.raw_output_drive, 0);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true((snap.active_fault_cause & VC_FAULT_INTERLOCK) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_INTERLOCK) != 0);

	free(d);
}

ZTEST(voltage_control_domain, test_calibration_raw_dac_change_bumps_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(before.calibration_raw_output_drive, 0);

	zassert_equal(domain_calibration_set_raw_dac(d, 0, 123), VC_OK);

	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);
	zassert_equal(after.version, before.version + 1);
	zassert_equal(after.calibration_raw_output_drive, 123);

	free(d);
}

/* ---- Variant profile ---- */

ZTEST(voltage_control_domain, test_variant_id)
{
	struct domain *d = domain_setup_fresh();
	zassert_equal(domain_get_variant_id(d), 1);
	free(d);
}

ZTEST(voltage_control_domain, test_supported_channels)
{
	struct domain *d = domain_setup_fresh();
	zassert_equal(domain_get_supported_channel_count(d), 2);
	zassert_equal(domain_get_active_channel_mask(d), 0x0003);
	free(d);
}

ZTEST(voltage_control_domain, test_channel0_supported)
{
	struct domain *d = domain_setup_fresh();
	zassert_true(domain_is_channel_supported(d, 0));
	zassert_true(domain_is_channel_supported(d, 1));
	zassert_false(domain_is_channel_supported(d, 2));
	free(d);
}

ZTEST(voltage_control_domain, test_modbus_input_core_readable_without_measurement_caps)
{
	struct vc_runtime *rt = vc_domain_runtime_create(onoff_channels, 1);
	struct vc_ctx test_ctx = { .runtime = rt };
	struct vc_mb_adapter *mb = vc_mb_adapter_create(&test_ctx);
	uint16_t reg;

	zassert_not_null(rt);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_STATUS_BITS,
				      &reg), VC_MB_OK);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_CAPABILITY_FLAGS,
				      &reg), VC_MB_OK);
	zassert_equal(reg, CH_CAP_OUTPUT_ENABLE);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_domain, test_modbus_input_rejects_unsupported_measurement_tail)
{
	struct vc_runtime *rt = vc_domain_runtime_create(onoff_channels, 1);
	struct vc_ctx test_ctx = { .runtime = rt };
	struct vc_mb_adapter *mb = vc_mb_adapter_create(&test_ctx);
	uint16_t reg;

	zassert_not_null(rt);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_MEASURED_VOLTAGE,
				      &reg), VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_MEASURED_CURRENT,
				      &reg), VC_MB_ILLEGAL_ADDRESS);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_domain, test_modbus_holding_rejects_unsupported_policy_tail)
{
	struct vc_runtime *rt = vc_domain_runtime_create(onoff_channels, 1);
	struct vc_ctx test_ctx = { .runtime = rt };
	struct vc_mb_adapter *mb = vc_mb_adapter_create(&test_ctx);

	zassert_not_null(rt);
	zassert_equal(vc_mb_holding_wr(mb,
					CH_BLOCK_BASE(0) + CH_CURRENT_PROTECTION_MODE,
					VC_PROTECTION_MODE_FLAG_ONLY), VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_wr(mb,
					CH_BLOCK_BASE(0) + CH_OUTPUT_ACTION,
					VC_OUTPUT_ACTION_NONE), VC_MB_OK);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_domain, test_modbus_rejects_unsupported_calibration_tail)
{
	struct vc_runtime *rt = vc_domain_runtime_create(onoff_channels, 1);
	struct vc_ctx test_ctx = { .runtime = rt };
	struct vc_mb_adapter *mb = vc_mb_adapter_create(&test_ctx);
	uint16_t reg;

	zassert_not_null(rt);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP1),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP2),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, SYS_OPERATING_MODE,
				       VC_OPERATING_MODE_CALIBRATION), VC_MB_OK);

	zassert_equal(vc_mb_input_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_ADC_VOLTAGE_HI,
				      &reg), VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_rd(mb, CH_BLOCK_BASE(0) + CH_RAW_DAC_CODE,
					&reg), VC_MB_ILLEGAL_ADDRESS);
	zassert_equal(vc_mb_holding_wr(mb, CH_BLOCK_BASE(0) + CH_CAL_SAMPLE_CMD,
					CAL_COMMAND_EXECUTE), VC_MB_ILLEGAL_ADDRESS);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_domain, test_domain_rejects_unsupported_measurement_policy)
{
	struct domain *d = domain_setup_on_off_only();
	struct vc_channel_config cfg;

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.current_protection_mode = VC_PROTECTION_MODE_FLAG_ONLY;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_UNSUPPORTED_CAPABILITY);

	free(d);
}

ZTEST(voltage_control_domain, test_domain_rejects_unsupported_calibration_paths)
{
	struct domain *d = domain_setup_on_off_only();
	struct vc_channel_config cfg;

	enter_calibration_mode(d);

	zassert_equal(domain_calibration_set_output_enable(d, 0, true),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(domain_calibration_set_max_raw_dac(d, 0, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(domain_calibration_sample(d, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);

	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	cfg.output_calib_k++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_UNSUPPORTED_CAPABILITY);

	free(d);
}

/* ---- System config defaults ---- */

ZTEST(voltage_control_domain, test_system_config_defaults)
{
	struct domain *d = domain_setup_fresh();
	struct vc_system_config cfg;

	domain_get_system_config(d, &cfg);
	zassert_equal(cfg.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
	zassert_equal(cfg.current_safe_band_pct, 10);
	free(d);
}

/* ---- Channel config defaults ---- */

ZTEST(voltage_control_domain, test_channel_config_defaults)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 0);
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
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

ZTEST(voltage_control_domain, test_unsupported_channel)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(domain_set_channel_config(d, 2, &cfg),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	free(d);
}

ZTEST(voltage_control_domain, test_operating_mode_validation)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_AUTOMATIC), VC_OK);
	zassert_equal(domain_get_operating_mode(d),
		      VC_OPERATING_MODE_AUTOMATIC);

	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	zassert_equal(domain_get_operating_mode(d),
		      VC_OPERATING_MODE_NORMAL);

	zassert_equal(domain_set_operating_mode(d, 3),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_mode_rejected_before_unlock)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_CALIBRATION), VC_ERR_INVALID_COMMAND);
	zassert_equal(domain_get_operating_mode(d),
		      VC_OPERATING_MODE_NORMAL);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_unlock_allows_mode_entry)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP2), VC_OK);
	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(domain_get_operating_mode(d),
		      VC_OPERATING_MODE_CALIBRATION);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_unlock_wrong_value_clears_sequence)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(domain_calibration_unlock(d, 0), VC_ERR_INVALID_COMMAND);
	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP2),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_CALIBRATION), VC_ERR_INVALID_COMMAND);
	zassert_equal(domain_get_operating_mode(d),
		      VC_OPERATING_MODE_NORMAL);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_entry_clears_raw_outputs)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 1234), VC_OK);

	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	enter_calibration_mode(d);

	for (uint8_t ch = 0; ch < domain_get_supported_channel_count(d); ch++) {
		zassert_equal(domain_get_channel_snapshot(d, ch, &snap), VC_OK);
		zassert_equal(snap.raw_dac_readback, 0);
		zassert_equal(snap.cal_output_enabled, 0);
		zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);
		zassert_equal(snap.raw_adc_voltage, 0);
		zassert_equal(snap.raw_adc_current, 0);
		zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
	}
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_raw_dac_requires_output_enable)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 1),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 0), VC_OK);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 1), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_dac_readback, 1);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_disable_clears_sample_state)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_sample(d, 0), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_VALID);

	zassert_equal(domain_calibration_set_output_enable(d, 0, false), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 0);
	zassert_equal(snap.raw_adc_current, 0);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_single_output_enabled)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_output_enable(d, 1, true),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(domain_calibration_set_output_enable(d, 0, false), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(domain_calibration_set_output_enable(d, 1, true), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_raw_dac_limit_validation)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_max_raw_dac(d, 0, 100), VC_OK);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 101),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 100), VC_OK);
	zassert_equal(domain_calibration_set_max_raw_dac(d, 0, 99),
		      VC_ERR_UNSAFE_STATE);

	zassert_equal(domain_calibration_set_raw_dac(d, 0, 0), VC_OK);
	zassert_equal(domain_calibration_set_max_raw_dac(d, 0, 0), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.cal_max_raw_dac_limit, 0);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 1),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 0), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_entry_disables_normal_output)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	domain_set_channel_config(d, 0, &cfg);
	domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);
	sim_tick(d, 500, v_noise, c_noise);

	enter_calibration_mode(d);

	domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.status_bits & 0x0007, 0,
		      "calibration entry must clear normal output/ramp status");
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_tick_does_not_ramp_to_target)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {100, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {100, 0};

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	domain_set_channel_config(d, 0, &cfg);

	enter_calibration_mode(d);
	sim_tick(d, 500, v_noise, c_noise);

	domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.measured_voltage, 0);
	zassert_equal(snap.measured_current, 0);
	zassert_equal(snap.active_fault_cause, 0);
	zassert_equal(snap.fault_history_cause, 0);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_rejects_normal_output_action)
{
	struct domain *d = domain_setup_fresh();

	enter_calibration_mode(d);
	zassert_equal(domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_ERR_INVALID_COMMAND);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_exit_clears_raw_output)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_max_raw_dac(d, 0, 0), VC_OK);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 0), VC_OK);
	zassert_equal(domain_set_operating_mode(d,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
	zassert_equal(snap.raw_adc_voltage, 0);
	zassert_equal(snap.raw_adc_current, 0);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);

	enter_calibration_mode(d);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
	free(d);
}

ZTEST(voltage_control_domain, test_reject_variant_with_too_many_channels)
{
	zassert_is_null(domain_create(test_channels, VC_MAX_CHANNELS + 1));
}

ZTEST(voltage_control_domain, test_calibration_coefficients_require_calibration_mode)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg, baseline;

	zassert_equal(domain_get_channel_config(d, 0, &baseline), VC_OK);
	cfg = baseline;
	cfg.output_calib_k++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.output_calib_b++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_voltage_calib_k++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_voltage_calib_b++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_current_calib_k++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_current_calib_b++;
	zassert_equal(domain_set_channel_config(d, 0, &cfg),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(cfg.output_calib_k, baseline.output_calib_k);
	zassert_equal(cfg.output_calib_b, baseline.output_calib_b);
	zassert_equal(cfg.measured_voltage_calib_k,
		      baseline.measured_voltage_calib_k);
	zassert_equal(cfg.measured_voltage_calib_b,
		      baseline.measured_voltage_calib_b);
	zassert_equal(cfg.measured_current_calib_k,
		      baseline.measured_current_calib_k);
	zassert_equal(cfg.measured_current_calib_b,
		      baseline.measured_current_calib_b);

	cfg.configured_target_voltage = 1000;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 1000);

	enter_calibration_mode(d);
	cfg.output_calib_k = 10001;
	cfg.output_calib_b = 1;
	cfg.measured_voltage_calib_k = 10002;
	cfg.measured_voltage_calib_b = 2;
	cfg.measured_current_calib_k = 10003;
	cfg.measured_current_calib_b = 3;
	zassert_equal(domain_set_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(domain_get_channel_config(d, 0, &cfg), VC_OK);
	zassert_equal(cfg.output_calib_k, 10001);
	zassert_equal(cfg.output_calib_b, 1);
	zassert_equal(cfg.measured_voltage_calib_k, 10002);
	zassert_equal(cfg.measured_voltage_calib_b, 2);
	zassert_equal(cfg.measured_current_calib_k, 10003);
	zassert_equal(cfg.measured_current_calib_b, 3);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_commit_preconditions)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_commit(d, 2),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(domain_calibration_commit(d, 0),
		      VC_ERR_INVALID_COMMAND);

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_commit(d, 0), VC_ERR_UNSAFE_STATE);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 10), VC_OK);
	zassert_equal(domain_calibration_set_output_enable(d, 0, false), VC_OK);
	zassert_equal(domain_calibration_commit(d, 0), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_sample_requires_calibration_mode)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_sample(d, 2),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(domain_calibration_sample(d, 0),
		      VC_ERR_INVALID_COMMAND);
	free(d);
}

ZTEST(voltage_control_domain, test_calibration_sample_captures_raw_values)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(d);
	zassert_equal(domain_calibration_set_output_enable(d, 0, true), VC_OK);
	zassert_equal(domain_calibration_set_raw_dac(d, 0, 123), VC_OK);
	zassert_equal(domain_calibration_sample(d, 0), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_VALID);
	zassert_equal(snap.raw_adc_voltage, 123);
	zassert_equal(snap.raw_adc_current, 0);
	free(d);
}

/* ---- System snapshot ---- */

ZTEST(voltage_control_domain, test_system_snapshot)
{
	struct domain *d = domain_setup_fresh();
	struct vc_system_snapshot snap;

	domain_get_system_snapshot(d, &snap);
	zassert_equal(snap.protocol_major, VC_PROTOCOL_MAJOR);
	zassert_equal(snap.protocol_minor, VC_PROTOCOL_MINOR);
	zassert_equal(snap.variant_id, 1);
	zassert_equal(snap.supported_channel_count, 2);
	zassert_equal(snap.active_channel_mask, 0x0003);
	free(d);
}

/* ---- Channel snapshot ---- */

ZTEST(voltage_control_domain, test_channel_snapshot_unsupported)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_snapshot snap;

	zassert_equal(domain_get_channel_snapshot(d, 2, &snap),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	free(d);
}

/* ---- Output action ---- */

ZTEST(voltage_control_domain, test_output_action_valid)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_DISABLE_GRACEFUL), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_output_action_host_context_invalid)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_channel_output_action(d, 0,
			 VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO),
		      VC_ERR_INVALID_COMMAND);
	free(d);
}

/* ---- Fault command ---- */

ZTEST(voltage_control_domain, test_fault_command_clear_history)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_channel_fault_command(d, 0,
			 VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY), VC_OK);
	free(d);
}

ZTEST(voltage_control_domain, test_fault_command_invalid)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_channel_fault_command(d, 0, 3),
		      VC_ERR_INVALID_COMMAND);
	free(d);
}

/* ---- Domain tick: ramp ---- */

ZTEST(voltage_control_domain, test_tick_instant_ramp)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	cfg.ramp_up_interval = 0;
	domain_set_channel_config(d, 0, &cfg);
	domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	sim_tick(d, 500, v_noise, c_noise);

	domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 5000,
		      "instant ramp must reach target in one tick");
	free(d);
}

/* ---- Domain tick: measured values ---- */

ZTEST(voltage_control_domain, test_tick_measured_with_noise)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {7, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	domain_set_channel_config(d, 0, &cfg);
	domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	sim_tick(d, 500, v_noise, c_noise);

	domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);
	zassert_equal(snap.measured_voltage, 1007,
		      "measured = target + noise");
	free(d);
}

ZTEST(voltage_control_domain, test_smf_preserves_calibration_output_rejection)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(domain_calibration_unlock(d, CAL_UNLOCK_STEP2), VC_OK);
	zassert_equal(domain_set_operating_mode(d, VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE),
		      VC_ERR_INVALID_COMMAND);

	free(d);
}

ZTEST(voltage_control_domain, test_smf_preserves_fault_safe_runtime_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_INTERLOCK,
		.provider_fault_cause = VC_FAULT_INTERLOCK,
	};
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_true(cfg.force_safe_state);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);

	free(d);
}

/* ---- Per-field config setters ---- */

ZTEST(voltage_control_domain, test_set_system_field_operating_mode)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_system_field(d, VC_FIELD_OPERATING_MODE,
					      VC_OPERATING_MODE_AUTOMATIC), VC_OK);
	zassert_equal(domain_get_operating_mode(d), VC_OPERATING_MODE_AUTOMATIC);
	free(d);
}

ZTEST(voltage_control_domain, test_set_system_field_rejects_invalid)
{
	struct domain *d = domain_setup_fresh();

	free(d);
}

ZTEST(voltage_control_domain, test_set_system_field_rejects_channel_field)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_system_field(d, VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 100),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

ZTEST(voltage_control_domain, test_set_channel_field_target_voltage)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
	free(d);
}

ZTEST(voltage_control_domain, test_set_channel_field_ramp_params)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_RAMP_UP_STEP, 100), VC_OK);
	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_RAMP_UP_INTERVAL, 5), VC_OK);
	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_RAMP_DOWN_STEP, 200), VC_OK);
	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_RAMP_DOWN_INTERVAL, 3), VC_OK);
	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.ramp_up_step, 100);
	zassert_equal(cfg.ramp_up_interval, 5);
	zassert_equal(cfg.ramp_down_step, 200);
	zassert_equal(cfg.ramp_down_interval, 3);
	free(d);
}

ZTEST(voltage_control_domain, test_set_channel_field_rejects_unsupported_channel)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_channel_field(d, 99, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       100), VC_ERR_UNSUPPORTED_CHANNEL);
	free(d);
}

ZTEST(voltage_control_domain, test_set_channel_field_rejects_system_field)
{
	struct domain *d = domain_setup_fresh();

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_OPERATING_MODE, 0),
		      VC_ERR_INVALID_VALUE);
	free(d);
}

ZTEST(voltage_control_domain, test_set_channel_field_bumps_runtime_config_version)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before, after;

	domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);
	domain_get_runtime_config(d, 0, &before);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	domain_get_runtime_config(d, 0, &after);
	zassert_true(after.version > before.version);
	free(d);
}

ZTEST(voltage_control_domain, test_current_protection_skipped_during_ramping)
{
	struct domain *d = domain_setup_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	domain_get_channel_config(d, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	domain_set_channel_config(d, 0, &cfg);
	domain_channel_output_action(d, 0, VC_OUTPUT_ACTION_ENABLE);

	/* Ramp partway -- channel is still ramping */
	domain_process_periodic(d, 100);

	/* Inject overcurrent measurement while ramping */
	{
		struct vc_measurement_snapshot meas = {
			.channel = 0,
			.generation = 1,
			.present_mask = VC_MEAS_PRESENT_CURRENT,
			.raw_current = 200,
		};
		domain_consume_measurement(d, &meas);
	}
	domain_process_periodic(d, 0);

	domain_get_channel_snapshot(d, 0, &snap);
	zassert_equal(snap.active_fault_cause, 0,
		      "current protection must not fire during ramping");
	zassert_equal(snap.fault_history_cause, 0,
		      "current protection must not flag during ramping");
	free(d);
}

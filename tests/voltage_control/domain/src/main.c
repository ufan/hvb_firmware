/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voltage-control domain unit tests — vc_controller API.
 * Runs on native_posix.  No hardware required.
 */

#include <zephyr/ztest.h>

#include "voltage_control/vc_controller.h"
#include "voltage_control/vc_runtime.h"
#include "regmap/vc_regs.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = FULL_CAPS },
	{ .dev = NULL, .index = 1, .capabilities = FULL_CAPS },
};

static const struct vc_channel_entry onoff_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE },
};

static struct vc_controller *ctrl;

static struct vc_controller *make_fresh(void)
{
	ctrl = vc_controller_init_static(test_channels, 2);
	zassert_not_null(ctrl);
	return ctrl;
}

static struct vc_controller *make_onoff(void)
{
	ctrl = vc_controller_init_static(onoff_channels, 1);
	zassert_not_null(ctrl);
	return ctrl;
}

static bool is_cal_mode(const struct vc_controller *c)
{
	return c->operating_mode == VC_OPERATING_MODE_CALIBRATION;
}

static void sim_tick(struct vc_controller *c, uint32_t dt_ms,
		     const int16_t *v_noise, const int16_t *c_noise_arr)
{
	size_t n = vc_controller_channel_count(c);

	vc_controller_tick(c, dt_ms);

	if (is_cal_mode(c)) {
		return;
	}

	for (uint8_t ch = 0; ch < n; ch++) {
		struct vc_channel_snapshot snap;

		if (vc_controller_get_channel_snapshot(c, ch, &snap) == VC_OK) {
			int32_t mv = snap.operational_target_voltage + v_noise[ch];
			int32_t mi = mv / 2 + c_noise_arr[ch];

			vc_controller_consume_voltage(c, ch, mv);
			vc_controller_consume_current(c, ch, mi);
		}
	}

	vc_controller_tick(c, 0);
}

static void enter_calibration_mode(struct vc_controller *c)
{
	zassert_equal(vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP1),
		      VC_OK);
	zassert_equal(vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP2),
		      VC_OK);
	zassert_equal(vc_controller_set_operating_mode(c,
			 VC_OPERATING_MODE_CALIBRATION), VC_OK);
}

ZTEST_SUITE(vc_domain, NULL, NULL, NULL, NULL, NULL);

/* ---- Runtime contract structs ---- */

ZTEST(vc_domain, test_runtime_contract_defaults_are_zeroable)
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

ZTEST(vc_domain, test_controller_init_static)
{
	struct vc_controller *c = vc_controller_init_static(test_channels, 2);

	zassert_not_null(c);
	zassert_equal(vc_controller_channel_count(c), 2);
	zassert_equal(vc_controller_get_operating_mode(c),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_domain, test_initial_channel_state_is_safe)
{
	make_fresh();

	zassert_false(ctrl->channels[0].output_enabled);
	zassert_equal(ctrl->channels[0].capabilities, FULL_CAPS);
	zassert_equal(ctrl->channels[0].operational_target_voltage, 0);
	zassert_false(is_cal_mode(ctrl));
	zassert_equal(ctrl->channels[0].cal_output_enabled, 0);
	zassert_equal(ctrl->channels[0].raw_dac_readback, 0);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_domain, test_snapshot_rejects_unsupported_channel)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 2, &snap),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}

/* ---- Measurement consumption ---- */

ZTEST(vc_domain, test_consume_voltage_and_current_updates_snapshot)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	vc_controller_consume_voltage(ctrl, 0, 1200);
	vc_controller_consume_current(ctrl, 0, 34);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.raw_adc_voltage, 1200);
	zassert_equal(snap.raw_adc_current, 34);
	zassert_equal(snap.measured_voltage, 1200);
	zassert_equal(snap.measured_current, 34);
	zassert_equal(snap.active_fault_cause, 0);
}

ZTEST(vc_domain, test_consume_fault_measurement_sets_fault)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	vc_controller_consume_fault(ctrl, 0, VC_FAULT_MEASUREMENT);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_true((snap.active_fault_cause & VC_FAULT_MEASUREMENT) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_MEASUREMENT) != 0);
}

ZTEST(vc_domain, test_consume_voltage_clamps_calibrated)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.measured_voltage_calib_k = 65535;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, true),
		      VC_OK);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_NORMAL), VC_OK);

	vc_controller_consume_voltage(ctrl, 0, 20000);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.measured_voltage, INT16_MAX);
}

/* ---- Calibration output state ---- */

ZTEST(vc_domain, test_calibration_output_state)
{
	make_fresh();

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 123), VC_OK);

	zassert_true(is_cal_mode(ctrl));
	zassert_equal(ctrl->channels[0].cal_output_enabled, 1);
	zassert_equal(ctrl->channels[0].raw_dac_readback, 123);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
}

/* ---- Output enable/disable state changes ---- */

ZTEST(vc_domain, test_output_enable_changes_state)
{
	make_fresh();

	zassert_false(ctrl->channels[0].output_enabled);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
				VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ctrl->channels[0].output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_RAMPING);
}

ZTEST(vc_domain, test_idempotent_output_actions)
{
	make_fresh();

	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
				VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ctrl->channels[0].output_enabled);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
				VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ctrl->channels[0].output_enabled);

	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
				VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_false(ctrl->channels[0].output_enabled);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
				VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_false(ctrl->channels[0].output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- Tick / ramp ---- */

ZTEST(vc_domain, test_tick_ramps_to_target_voltage)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	struct vc_pending_command pending;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ctrl->channels[0], 100, &ctrl->sys_cfg);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);

	pending = vc_channel_take_pending_command(&ctrl->channels[0]);
	zassert_true(pending.valid);
	zassert_equal(pending.output_code, 1000);
}

ZTEST(vc_domain, test_tick_zero_target_no_pending_change)
{
	make_fresh();

	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ctrl->channels[0], 100, &ctrl->sys_cfg);

	zassert_false(vc_channel_has_pending_command(&ctrl->channels[0]));
	zassert_equal(ctrl->channels[0].operational_target_voltage, 0);
}

ZTEST(vc_domain, test_output_calibration_gain)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_pending_command pending;

	enter_calibration_mode(ctrl);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.output_calib_k = 20000;
	vc_channel_set_config(&ctrl->channels[0], &cfg, true);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_NORMAL);

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ctrl->channels[0], 100, &ctrl->sys_cfg);
	pending = vc_channel_take_pending_command(&ctrl->channels[0]);

	zassert_true(pending.valid);
	zassert_equal(pending.output_code, 200);
}

ZTEST(vc_domain, test_output_drive_clamps_low)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_pending_command pending;

	enter_calibration_mode(ctrl);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.output_calib_b = -1000;
	vc_channel_set_config(&ctrl->channels[0], &cfg, true);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_NORMAL);

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ctrl->channels[0], 100, &ctrl->sys_cfg);
	pending = vc_channel_take_pending_command(&ctrl->channels[0]);

	zassert_true(pending.valid);
	zassert_equal(pending.output_code, 0);
}

ZTEST(vc_domain, test_output_drive_clamps_high)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_pending_command pending;

	enter_calibration_mode(ctrl);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.output_calib_k = UINT16_MAX;
	vc_channel_set_config(&ctrl->channels[0], &cfg, true);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_NORMAL);

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 20000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_channel_tick_ramp(&ctrl->channels[0], 100, &ctrl->sys_cfg);
	pending = vc_channel_take_pending_command(&ctrl->channels[0]);

	zassert_true(pending.valid);
	zassert_equal(pending.output_code, UINT16_MAX);
}

/* ---- Fault → safe state ---- */

ZTEST(vc_domain, test_hardware_fault_forces_safe_state)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	sim_tick(ctrl, 100, quiet, quiet);
	zassert_true(ctrl->channels[0].output_enabled);
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);

	vc_controller_consume_fault(ctrl, 0, VC_FAULT_HARDWARE);

	zassert_false(ctrl->channels[0].output_enabled);
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true((snap.active_fault_cause & VC_FAULT_HARDWARE) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_HARDWARE) != 0);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_domain, test_interlock_forces_safe_state)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	sim_tick(ctrl, 100, quiet, quiet);
	zassert_true(ctrl->channels[0].output_enabled);
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);

	vc_controller_consume_fault(ctrl, 0, VC_FAULT_INTERLOCK);

	zassert_false(ctrl->channels[0].output_enabled);
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true((snap.active_fault_cause & VC_FAULT_INTERLOCK) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_INTERLOCK) != 0);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- Calibration raw DAC ---- */

ZTEST(vc_domain, test_calibration_raw_dac_change)
{
	make_fresh();

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(ctrl->channels[0].raw_dac_readback, 0);

	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 123), VC_OK);
	zassert_equal(ctrl->channels[0].raw_dac_readback, 123);
}

/* ---- Variant profile ---- */

ZTEST(vc_domain, test_variant_id)
{
	make_fresh();
	struct vc_system_snapshot snap;

	vc_controller_get_system_snapshot(ctrl, &snap);
	zassert_equal(snap.variant_id, 1);
}

ZTEST(vc_domain, test_supported_channels)
{
	make_fresh();
	struct vc_system_snapshot snap;

	zassert_equal(vc_controller_channel_count(ctrl), 2);
	vc_controller_get_system_snapshot(ctrl, &snap);
	zassert_equal(snap.active_channel_mask, 0x0003);
}

ZTEST(vc_domain, test_channel_supported)
{
	make_fresh();

	zassert_true(0 < vc_controller_channel_count(ctrl));
	zassert_true(1 < vc_controller_channel_count(ctrl));
	zassert_false(2 < vc_controller_channel_count(ctrl));
}

/* ---- Capability gating ---- */

ZTEST(vc_domain, test_onoff_rejects_measurement_policy)
{
	make_onoff();
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.current_protection_mode = VC_PROTECTION_MODE_FLAG_ONLY;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

ZTEST(vc_domain, test_onoff_rejects_calibration_paths)
{
	make_onoff();
	struct vc_channel_config cfg;

	enter_calibration_mode(ctrl);

	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(vc_controller_channel_cal_max_raw_dac(ctrl, 0, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
	zassert_equal(vc_controller_channel_cal_sample(ctrl, 0),
		      VC_ERR_UNSUPPORTED_CAPABILITY);

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.output_calib_k++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, true),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

/* ---- System config defaults ---- */

ZTEST(vc_domain, test_system_config_defaults)
{
	make_fresh();
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);
	zassert_equal(cfg.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
	zassert_equal(cfg.current_safe_band_pct, 10);
}

/* ---- Channel config defaults ---- */

ZTEST(vc_domain, test_channel_config_defaults)
{
	make_fresh();
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.output_calib_k, 10000);
	zassert_equal(cfg.output_calib_b, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);
	zassert_equal(cfg.measured_voltage_calib_b, 0);
	zassert_equal(cfg.measured_current_calib_k, 10000);
	zassert_equal(cfg.measured_current_calib_b, 0);
}

/* ---- Config validation ---- */

ZTEST(vc_domain, test_target_voltage_range)
{
	make_fresh();
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_domain, test_unsupported_channel)
{
	make_fresh();
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(vc_controller_channel_set_field(ctrl, 2,
			 VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
			 cfg.configured_target_voltage),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_domain, test_operating_mode_validation)
{
	make_fresh();

	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_AUTOMATIC), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_AUTOMATIC);

	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);

	zassert_equal(vc_controller_set_operating_mode(ctrl, 3),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_domain, test_calibration_mode_rejected_before_unlock)
{
	make_fresh();

	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_CALIBRATION),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_domain, test_calibration_unlock_allows_mode_entry)
{
	make_fresh();

	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1),
		      VC_OK);
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2),
		      VC_OK);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(vc_domain, test_calibration_unlock_wrong_value_clears_sequence)
{
	make_fresh();

	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1),
		      VC_OK);
	zassert_equal(vc_controller_calibration_unlock(ctrl, 0),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_CALIBRATION),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_domain, test_calibration_entry_clears_raw_outputs)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 1234),
		      VC_OK);

	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	enter_calibration_mode(ctrl);

	for (uint8_t ch = 0; ch < vc_controller_channel_count(ctrl); ch++) {
		zassert_equal(vc_controller_get_channel_snapshot(ctrl, ch,
				 &snap), VC_OK);
		zassert_equal(snap.raw_dac_readback, 0);
		zassert_equal(snap.cal_output_enabled, 0);
		zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);
		zassert_equal(snap.raw_adc_voltage, 0);
		zassert_equal(snap.raw_adc_current, 0);
		zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
	}
}

ZTEST(vc_domain, test_calibration_raw_dac_requires_output_enable)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 1),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 0), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 1), VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.raw_dac_readback, 1);
}

ZTEST(vc_domain, test_calibration_disable_clears_sample_state)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_sample(ctrl, 0), VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_VALID);

	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, false),
		      VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.raw_adc_voltage, 0);
	zassert_equal(snap.raw_adc_current, 0);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);
}

ZTEST(vc_domain, test_calibration_single_output_enabled)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, false),
		      VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true),
		      VC_OK);
}

ZTEST(vc_domain, test_calibration_raw_dac_limit_validation)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_max_raw_dac(ctrl, 0, 100),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 101),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 100),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_max_raw_dac(ctrl, 0, 99),
		      VC_ERR_UNSAFE_STATE);

	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 0), VC_OK);
	zassert_equal(vc_controller_channel_cal_max_raw_dac(ctrl, 0, 0),
		      VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.cal_max_raw_dac_limit, 0);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 1),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 0), VC_OK);
}

ZTEST(vc_domain, test_calibration_entry_disables_normal_output)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	sim_tick(ctrl, 500, quiet, quiet);

	enter_calibration_mode(ctrl);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.status_bits & 0x0007, 0,
		      "calibration entry must clear normal output/ramp status");
}

ZTEST(vc_domain, test_calibration_tick_does_not_ramp)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {100, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {100, 0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);

	enter_calibration_mode(ctrl);
	sim_tick(ctrl, 500, v_noise, c_noise);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.measured_voltage, 0);
	zassert_equal(snap.measured_current, 0);
	zassert_equal(snap.active_fault_cause, 0);
	zassert_equal(snap.fault_history_cause, 0);
}

ZTEST(vc_domain, test_calibration_rejects_normal_output_action)
{
	make_fresh();

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_domain, test_calibration_exit_clears_raw_output)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_max_raw_dac(ctrl, 0, 0),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 0), VC_OK);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
			 VC_OPERATING_MODE_NORMAL), VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
	zassert_equal(snap.raw_adc_voltage, 0);
	zassert_equal(snap.raw_adc_current, 0);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_NONE);

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
}

ZTEST(vc_domain, test_reject_too_many_channels)
{
	zassert_is_null(vc_controller_init_static(test_channels,
						  VC_MAX_CHANNELS + 1));
}

ZTEST(vc_domain, test_calibration_coefficients_require_calibration_mode)
{
	make_fresh();
	struct vc_channel_config cfg, baseline;

	vc_controller_get_channel_config(ctrl, 0, &baseline);
	cfg = baseline;
	cfg.output_calib_k++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.output_calib_b++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_voltage_calib_k++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_voltage_calib_b++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_current_calib_k++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	cfg = baseline;
	cfg.measured_current_calib_b++;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_ERR_INVALID_COMMAND);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
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
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, false),
		      VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 1000);

	enter_calibration_mode(ctrl);
	cfg.output_calib_k = 10001;
	cfg.output_calib_b = 1;
	cfg.measured_voltage_calib_k = 10002;
	cfg.measured_voltage_calib_b = 2;
	cfg.measured_current_calib_k = 10003;
	cfg.measured_current_calib_b = 3;
	zassert_equal(vc_channel_set_config(&ctrl->channels[0], &cfg, true),
		      VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.output_calib_k, 10001);
	zassert_equal(cfg.output_calib_b, 1);
	zassert_equal(cfg.measured_voltage_calib_k, 10002);
	zassert_equal(cfg.measured_voltage_calib_b, 2);
	zassert_equal(cfg.measured_current_calib_k, 10003);
	zassert_equal(cfg.measured_current_calib_b, 3);
}

ZTEST(vc_domain, test_calibration_commit_preconditions)
{
	make_fresh();

	zassert_equal(vc_controller_channel_cal_commit(ctrl, 2),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(vc_controller_channel_cal_commit(ctrl, 0),
		      VC_ERR_INVALID_COMMAND);

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_commit(ctrl, 0),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 10), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, false),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_commit(ctrl, 0), VC_OK);
}

ZTEST(vc_domain, test_calibration_sample_requires_calibration_mode)
{
	make_fresh();

	zassert_equal(vc_controller_channel_cal_sample(ctrl, 2),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(vc_controller_channel_cal_sample(ctrl, 0),
		      VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_domain, test_calibration_sample_captures_raw_values)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true),
		      VC_OK);
	zassert_equal(vc_controller_channel_cal_raw_dac(ctrl, 0, 123), VC_OK);
	zassert_equal(vc_controller_channel_cal_sample(ctrl, 0), VC_OK);
	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 0, &snap),
		      VC_OK);
	zassert_equal(snap.cal_sample_status, VC_CAL_SAMPLE_VALID);
	zassert_equal(snap.raw_adc_voltage, 123);
	zassert_equal(snap.raw_adc_current, 0);
}

/* ---- System snapshot ---- */

ZTEST(vc_domain, test_system_snapshot)
{
	make_fresh();
	struct vc_system_snapshot snap;

	vc_controller_get_system_snapshot(ctrl, &snap);
	zassert_equal(snap.protocol_major, VC_PROTOCOL_MAJOR);
	zassert_equal(snap.protocol_minor, VC_PROTOCOL_MINOR);
	zassert_equal(snap.variant_id, 1);
	zassert_equal(snap.supported_channel_count, 2);
	zassert_equal(snap.active_channel_mask, 0x0003);
}

/* ---- Channel snapshot ---- */

ZTEST(vc_domain, test_channel_snapshot_unsupported)
{
	make_fresh();
	struct vc_channel_snapshot snap;

	zassert_equal(vc_controller_get_channel_snapshot(ctrl, 2, &snap),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}

/* ---- Output action ---- */

ZTEST(vc_domain, test_output_action_valid)
{
	make_fresh();

	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_DISABLE_IMMEDIATE), VC_OK);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_DISABLE_GRACEFUL), VC_OK);
}

ZTEST(vc_domain, test_output_action_host_context_invalid)
{
	make_fresh();

	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Fault command ---- */

ZTEST(vc_domain, test_fault_command_clear_history)
{
	make_fresh();

	zassert_equal(vc_controller_channel_fault_command(ctrl, 0,
			 VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY), VC_OK);
}

ZTEST(vc_domain, test_fault_command_invalid)
{
	make_fresh();

	zassert_equal(vc_controller_channel_fault_command(ctrl, 0, 3),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Domain tick: ramp ---- */

ZTEST(vc_domain, test_tick_instant_ramp)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t quiet[VC_MAX_CHANNELS] = {0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	cfg.ramp_up_interval = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	sim_tick(ctrl, 500, quiet, quiet);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 5000,
		      "instant ramp must reach target in one tick");
}

/* ---- Domain tick: measured values ---- */

ZTEST(vc_domain, test_tick_measured_with_noise)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	int16_t v_noise[VC_MAX_CHANNELS] = {7, 0};
	int16_t c_noise[VC_MAX_CHANNELS] = {0};

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	sim_tick(ctrl, 500, v_noise, c_noise);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 1000);
	zassert_equal(snap.measured_voltage, 1007,
		      "measured = target + noise");
}

ZTEST(vc_domain, test_smf_preserves_calibration_output_rejection)
{
	make_fresh();

	enter_calibration_mode(ctrl);
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
			 VC_OUTPUT_ACTION_ENABLE), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_domain, test_smf_preserves_fault_safe_state)
{
	make_fresh();

	vc_controller_consume_fault(ctrl, 0, VC_FAULT_INTERLOCK);

	zassert_false(ctrl->channels[0].output_enabled);
	zassert_equal(ctrl->channels[0].operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ctrl->channels[0]),
		      VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- Per-field config setters ---- */

ZTEST(vc_domain, test_set_system_field_operating_mode)
{
	make_fresh();

	zassert_equal(vc_controller_set_system_field(ctrl,
			 VC_FIELD_OPERATING_MODE,
			 VC_OPERATING_MODE_AUTOMATIC), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_AUTOMATIC);
}

ZTEST(vc_domain, test_set_system_field_rejects_channel_field)
{
	make_fresh();

	zassert_equal(vc_controller_set_system_field(ctrl,
			 VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 100),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_domain, test_set_channel_field_target_voltage)
{
	make_fresh();
	struct vc_channel_config cfg;

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 5000), VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_domain, test_set_channel_field_ramp_params)
{
	make_fresh();
	struct vc_channel_config cfg;

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_RAMP_UP_STEP, 100), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_RAMP_UP_INTERVAL, 5), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_RAMP_DOWN_STEP, 200), VC_OK);
	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_RAMP_DOWN_INTERVAL, 3), VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.ramp_up_step, 100);
	zassert_equal(cfg.ramp_up_interval, 5);
	zassert_equal(cfg.ramp_down_step, 200);
	zassert_equal(cfg.ramp_down_interval, 3);
}

ZTEST(vc_domain, test_set_channel_field_rejects_unsupported_channel)
{
	make_fresh();

	zassert_equal(vc_controller_channel_set_field(ctrl, 99,
			 VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 100),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_domain, test_set_channel_field_rejects_system_field)
{
	make_fresh();

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_OPERATING_MODE, 0),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_domain, test_set_channel_field_updates_config)
{
	make_fresh();
	struct vc_channel_config cfg;

	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
			 VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 5000), VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_domain, test_current_protection_skipped_during_ramping)
{
	make_fresh();
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_controller_tick(ctrl, 100);

	vc_controller_consume_current(ctrl, 0, 200);
	vc_controller_tick(ctrl, 0);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.active_fault_cause, 0,
		      "current protection must not fire during ramping");
	zassert_equal(snap.fault_history_cause, 0,
		      "current protection must not flag during ramping");
}

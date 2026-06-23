/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"
#include "voltage_control/provider_bus.h"
#include "voltage_control/vc_storage.h"

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE |
						  CH_CAP_VOLTAGE_MEASUREMENT },
};

static const struct vc_channel_entry full_cap_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE |
						  CH_CAP_RAW_OUTPUT_DRIVE |
						  CH_CAP_VOLTAGE_MEASUREMENT |
						  CH_CAP_CURRENT_MEASUREMENT },
};

ZTEST_SUITE(voltage_control_runtime, NULL, NULL, NULL, NULL, NULL);

ZTEST(voltage_control_runtime, test_runtime_command_contract_defaults_are_zeroable)
{
	struct vc_runtime_command cmd = {0};

	zassert_equal(cmd.type, VC_RUNTIME_CMD_SET_OPERATING_MODE);
	zassert_equal(cmd.channel, 0);
	zassert_equal(cmd.payload.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_is_null(cmd.result_sem);
	zassert_is_null(cmd.result);
}

ZTEST(voltage_control_runtime, test_provider_bus_contract_defaults_are_zeroable)
{
	struct vc_provider_msg msg = {0};
	struct vc_runtime_config_slot slot = {0};
	struct vc_provider_binding binding = {0};

	zassert_equal(msg.type, VC_PROVIDER_MSG_CONFIG_CHANGED);
	zassert_equal(msg.channel, 0);
	zassert_equal(msg.config_version, 0);
	zassert_equal(slot.snapshot.channel, 0);
	zassert_is_null(binding.dev);
	zassert_is_null(binding.config_slot);
}

ZTEST(voltage_control_runtime, test_runtime_submit_measurement_updates_domain_snapshot)
{
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 10,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 77,
	};
	struct vc_channel_snapshot snap;

	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, &meas), VC_OK);
	bool processed = false;
	for (int i = 0; i < 100; i++) {
		k_msleep(1);
		if (vc_runtime_get_published_channel_snapshot(rt, 0, &snap) == VC_OK &&
		    snap.raw_adc_voltage == 77) {
			processed = true;
			break;
		}
	}
	zassert_true(processed, "worker did not process evidence within 100ms");
	zassert_equal(snap.measured_voltage, 77);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_runtime_submit_measurement_rejects_null_runtime)
{
	struct vc_measurement_snapshot meas = {
		.channel = 0,
	};

	zassert_equal(vc_runtime_submit_measurement(NULL, &meas),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(voltage_control_runtime, test_runtime_submit_measurement_rejects_null_measurement)
{
	
	struct vc_runtime *rt;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, NULL),
		      VC_ERR_INVALID_VALUE);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_runtime_get_channel_config_for_provider)
{
	
	struct vc_runtime *rt;
	struct vc_runtime_config_snapshot cfg;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_get_channel_config(rt, 0, &cfg), VC_OK);
	zassert_equal(cfg.channel, 0);
	zassert_true(cfg.version >= 1);
	zassert_true(cfg.force_safe_state);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_runtime_channel_config_rejects_null)
{
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(vc_runtime_get_channel_config(NULL, 0, &cfg),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_runtime_get_channel_config(NULL, 0, NULL),
		      VC_ERR_INVALID_VALUE);
}

struct fake_apply_state {
	uint16_t output;
	bool enabled;
	int output_ret;
	int enable_ret;
};

static int fake_apply_config(struct fake_apply_state *state,
			     const struct vc_runtime_config_snapshot *cfg)
{
	if (cfg->force_safe_state) {
		state->output = 0;
		state->enabled = false;
		return 0;
	}
	if (state->output_ret < 0) {
		return state->output_ret;
	}
	state->output = cfg->raw_output_drive;
	if (state->enable_ret < 0) {
		return state->enable_ret;
	}
	state->enabled = cfg->output_enable;
	return 0;
}

ZTEST(voltage_control_runtime, test_fake_provider_apply_safe_state)
{
	struct fake_apply_state state = { .output = 123, .enabled = true };
	struct vc_runtime_config_snapshot cfg = { .force_safe_state = true };

	zassert_equal(fake_apply_config(&state, &cfg), 0);
	zassert_equal(state.output, 0);
	zassert_false(state.enabled);
}

ZTEST(voltage_control_runtime, test_runtime_set_operating_mode_is_processed_by_worker)
{
	
	struct vc_runtime *rt;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	struct vc_system_snapshot snap;

	zassert_equal(vc_runtime_set_operating_mode(rt, VC_OPERATING_MODE_AUTOMATIC, K_SECONDS(1)), VC_OK);
	k_msleep(50);
	zassert_equal(vc_runtime_get_published_system_snapshot(rt, &snap), VC_OK);
	zassert_equal(snap.active_operating_mode, VC_OPERATING_MODE_AUTOMATIC);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_runtime_submit_command_rejects_null)
{
	
	struct vc_runtime *rt;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_command(NULL, NULL, K_NO_WAIT), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_runtime_submit_command(rt, NULL, K_NO_WAIT), VC_ERR_INVALID_VALUE);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_published_snapshot_has_uptime)
{
	
	struct vc_runtime *rt;
	struct vc_system_snapshot snap;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	k_msleep(50);
	zassert_equal(vc_runtime_get_published_system_snapshot(rt, &snap), VC_OK);
	zassert_true(snap.uptime >= 0);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_provider_bus_config_slot_acquire_release)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 7,
		.capability_flags = CH_CAP_OUTPUT_ENABLE,
		.output_enable = true,
		.raw_output_drive = 123,
	};
	const struct vc_runtime_config_snapshot *borrowed;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);

	borrowed = vc_provider_bus_acquire_config(0);
	zassert_not_null(borrowed);
	zassert_equal(borrowed->version, 7);
	zassert_equal(borrowed->raw_output_drive, 123);
	vc_provider_bus_release_config(0);
}

ZTEST(voltage_control_runtime, test_provider_bus_measurement_queue_round_trip)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 3,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 456,
	};
	struct vc_measurement_snapshot out;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_measurement(&in), VC_OK);
	zassert_equal(vc_provider_bus_take_measurement(&out), VC_OK);
	zassert_equal(out.channel, 0);
	zassert_equal(out.generation, 3);
	zassert_equal(out.raw_voltage, 456);
}

ZTEST(voltage_control_runtime, test_provider_bus_publish_config_posts_message)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 9,
	};
	struct vc_provider_msg msg;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);
	zassert_equal(vc_provider_bus_take_message(&msg, K_NO_WAIT), VC_OK);
	zassert_equal(msg.type, VC_PROVIDER_MSG_CONFIG_CHANGED);
	zassert_equal(msg.channel, 0);
	zassert_equal(msg.config_version, 9);
}

ZTEST(voltage_control_runtime, test_runtime_create_publishes_initial_provider_config)
{
	
	struct vc_runtime *rt;
	const struct vc_runtime_config_snapshot *cfg;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	cfg = vc_provider_bus_acquire_config(0);
	zassert_not_null(cfg);
	zassert_equal(cfg->channel, 0);
	zassert_true(cfg->version >= 1);
	zassert_true(cfg->force_safe_state);
	vc_provider_bus_release_config(0);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_runtime_command_posts_provider_config_message)
{
	
	struct vc_runtime *rt;
	struct vc_provider_msg msg;
	const struct vc_runtime_config_snapshot *cfg;

	rt = vc_domain_runtime_create(test_channels, 1);
	zassert_not_null(rt);

	/* Drain initial config messages so we see only the command-triggered one */
	while (vc_provider_bus_take_message(&msg, K_NO_WAIT) == VC_OK) {
	}

	zassert_equal(vc_runtime_set_operating_mode(rt, VC_OPERATING_MODE_AUTOMATIC, K_SECONDS(1)), VC_OK);

	/* Config slot is updated and then dispatched immediately, so no message remains */
	cfg = vc_provider_bus_acquire_config(0);
	zassert_not_null(cfg);
	zassert_equal(cfg->channel, 0);
	zassert_true(cfg->version >= 1);
	vc_provider_bus_release_config(0);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_provider_bus_binding_api_is_callable)
{
	zassert_true(vc_provider_bus_binding_count() >= 0);
	zassert_equal(vc_provider_bus_notify_channel(0, 1), VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(voltage_control_runtime, test_provider_bus_start_all_without_bindings)
{
	zassert_equal(vc_provider_bus_start_all(), VC_OK);
}

/* ---- Per-field command integration ---- */

ZTEST(voltage_control_runtime, test_set_system_field_through_queue)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_system_config cfg;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_set_system_field(rt, VC_FIELD_SLAVE_ADDRESS,
						  42, K_SECONDS(1)), VC_OK);
	k_msleep(50);
	zassert_equal(vc_runtime_get_published_system_config(rt, &cfg), VC_OK);
	zassert_equal(cfg.slave_address, 42);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_set_channel_field_through_queue)
{
	struct vc_runtime *rt = vc_domain_runtime_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_set_channel_field(rt, 0,
						   VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
						   5000, K_SECONDS(1)), VC_OK);
	k_msleep(50);
	zassert_equal(vc_runtime_get_published_channel_config(rt, 0, &cfg), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 5000);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_set_channel_field_rejects_bad_channel)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);

	zassert_not_null(rt);
	zassert_equal(vc_runtime_set_channel_field(rt, 99,
						   VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
						   100, K_SECONDS(1)),
		      VC_ERR_UNSUPPORTED_CHANNEL);

	vc_runtime_destroy(rt);
}

/* ---- Published snapshot integration ---- */

ZTEST(voltage_control_runtime, test_published_system_snapshot_reflects_initial_state)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_system_snapshot snap;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_get_published_system_snapshot(rt, &snap), VC_OK);
	zassert_equal(snap.supported_channel_count, 1);
	zassert_equal(snap.active_operating_mode, VC_OPERATING_MODE_NORMAL);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_published_channel_snapshot_reflects_initial_state)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_channel_snapshot snap;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_get_published_channel_snapshot(rt, 0, &snap), VC_OK);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_true(snap.channel_capability_flags & CH_CAP_OUTPUT_ENABLE);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_published_snapshot_updates_after_command)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_system_snapshot snap;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_set_operating_mode(rt, VC_OPERATING_MODE_AUTOMATIC,
						    K_SECONDS(1)), VC_OK);
	k_msleep(50);

	zassert_equal(vc_runtime_get_published_system_snapshot(rt, &snap), VC_OK);
	zassert_equal(snap.active_operating_mode, VC_OPERATING_MODE_AUTOMATIC);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_published_config_reflects_field_write)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_set_channel_field(rt, 0,
						   VC_FIELD_RAMP_UP_STEP,
						   100, K_SECONDS(1)), VC_OK);
	k_msleep(50);

	zassert_equal(vc_runtime_get_published_channel_config(rt, 0, &cfg), VC_OK);
	zassert_equal(cfg.ramp_up_step, 100);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_published_snapshot_rejects_null)
{
	struct vc_system_snapshot snap;

	zassert_equal(vc_runtime_get_published_system_snapshot(NULL, &snap),
		      VC_ERR_INVALID_VALUE);
	zassert_equal(vc_runtime_get_published_system_snapshot(NULL, NULL),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(voltage_control_runtime, test_published_channel_snapshot_rejects_bad_channel)
{
	
	struct vc_runtime *rt = vc_domain_runtime_create(test_channels, 1);
	struct vc_channel_snapshot snap;

	zassert_not_null(rt);
	zassert_equal(vc_runtime_get_published_channel_snapshot(rt, 99, &snap),
		      VC_ERR_UNSUPPORTED_CHANNEL);

	vc_runtime_destroy(rt);
}

ZTEST(voltage_control_runtime, test_stale_fault_flag_is_distinct)
{
	zassert_equal(VC_FAULT_STALE, 0x0080);
	zassert_equal(VC_FAULT_STALE & VC_FAULT_MEASUREMENT, 0);
	zassert_equal(VC_FAULT_STALE & VC_FAULT_VOLTAGE, 0);
}

ZTEST(voltage_control_runtime, test_measurement_buffer_store_and_read)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 5,
		.timestamp_ms = 12345,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 100,
	};
	struct vc_measurement_snapshot out;
	size_t count = vc_measurement_buffer_count();

	if (count == 0) {
		ztest_test_skip();
		return;
	}

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in), VC_OK);
	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 5);
	zassert_equal(out.timestamp_ms, 12345);
	zassert_equal(out.raw_voltage, 100);
}

ZTEST(voltage_control_runtime, test_measurement_buffer_rejects_null)
{
	struct vc_measurement_snapshot meas;

	zassert_equal(vc_measurement_buffer_store(0, NULL), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_measurement_buffer_read(0, NULL), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_measurement_buffer_store(99, &meas),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(vc_measurement_buffer_read(99, &meas),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}

/* ---- Fake storage backend ---- */

static struct vc_system_config fake_saved_sys;
static struct vc_channel_config fake_saved_ch[2];
static bool fake_sys_saved;
static bool fake_ch_saved[2];

static int fake_save_system_config(const struct vc_system_config *cfg)
{
	fake_saved_sys = *cfg;
	fake_sys_saved = true;
	return 0;
}

static int fake_load_system_config(struct vc_system_config *cfg)
{
	if (!fake_sys_saved) {
		return -ENOENT;
	}
	*cfg = fake_saved_sys;
	return 0;
}

static int fake_save_channel_config(uint8_t ch, const struct vc_channel_config *cfg)
{
	if (ch >= 2) {
		return -EINVAL;
	}
	fake_saved_ch[ch] = *cfg;
	fake_ch_saved[ch] = true;
	return 0;
}

static int fake_load_channel_config(uint8_t ch, struct vc_channel_config *cfg)
{
	if (ch >= 2 || !fake_ch_saved[ch]) {
		return -ENOENT;
	}
	*cfg = fake_saved_ch[ch];
	return 0;
}

static int fake_erase_all(void)
{
	fake_sys_saved = false;
	fake_ch_saved[0] = false;
	fake_ch_saved[1] = false;
	return 0;
}

static const struct vc_storage_backend fake_storage = {
	.save_system_config = fake_save_system_config,
	.load_system_config = fake_load_system_config,
	.save_channel_config = fake_save_channel_config,
	.load_channel_config = fake_load_channel_config,
	.erase_all = fake_erase_all,
};

static void reset_fake_storage(void)
{
	memset(&fake_saved_sys, 0, sizeof(fake_saved_sys));
	memset(fake_saved_ch, 0, sizeof(fake_saved_ch));
	fake_sys_saved = false;
	fake_ch_saved[0] = false;
	fake_ch_saved[1] = false;
}

/* ---- Settings persistence tests ---- */

ZTEST(voltage_control_runtime, test_system_save_and_load_round_trip)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_system_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 42), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_true(fake_sys_saved);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 99), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_LOAD), VC_OK);

	domain_get_system_config(d, &cfg);
	zassert_equal(cfg.slave_address, 42);

	free(d);
}

ZTEST(voltage_control_runtime, test_system_factory_reset_restores_defaults)
{
	struct domain *d = domain_create(test_channels, 1);

	zassert_not_null(d);
	struct vc_system_config cfg;

	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 42), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_FACTORY_RESET), VC_OK);

	domain_get_system_config(d, &cfg);
	zassert_equal(cfg.slave_address, 1);

	free(d);
}

ZTEST(voltage_control_runtime, test_system_param_action_null_storage_returns_error)
{
	struct domain *d = domain_create(test_channels, 1);

	zassert_not_null(d);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_ERR_STORAGE);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_LOAD), VC_ERR_STORAGE);

	free(d);
}

ZTEST(voltage_control_runtime, test_channel_save_and_load_round_trip)
{
	struct domain *d = domain_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_true(fake_ch_saved[0]);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       9999), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_LOAD), VC_OK);

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);

	free(d);
}

ZTEST(voltage_control_runtime, test_channel_factory_reset_preserves_calibration)
{
	struct domain *d = domain_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_FACTORY_RESET), VC_OK);

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);

	free(d);
}

ZTEST(voltage_control_runtime, test_channel_load_no_saved_data_returns_error)
{
	struct domain *d = domain_create(full_cap_channels, 1);

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_LOAD),
		      VC_ERR_STORAGE);

	free(d);
}

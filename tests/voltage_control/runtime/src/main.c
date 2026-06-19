/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0, .capabilities = CH_CAP_OUTPUT_ENABLE |
						  CH_CAP_VOLTAGE_MEASUREMENT },
};

ZTEST_SUITE(voltage_control_runtime, NULL, NULL, NULL, NULL, NULL);

ZTEST(voltage_control_runtime, test_runtime_create_rejects_null_domain)
{
	zassert_is_null(vc_runtime_create(NULL));
}

ZTEST(voltage_control_runtime, test_runtime_create_and_destroy)
{
	struct domain *domain = domain_create(test_channels, 1);
	struct vc_runtime *runtime;

	zassert_not_null(domain);

	runtime = vc_runtime_create(domain);
	zassert_not_null(runtime);

	vc_runtime_destroy(runtime);
	zassert_equal(domain_get_supported_channel_count(domain), 1);

	free(domain);
}

ZTEST(voltage_control_runtime, test_runtime_command_contract_defaults_are_zeroable)
{
	struct vc_runtime_command cmd = {0};

	zassert_equal(cmd.type, VC_RUNTIME_CMD_SET_OPERATING_MODE);
	zassert_equal(cmd.channel, 0);
	zassert_equal(cmd.payload.operating_mode, VC_OPERATING_MODE_NORMAL);
	zassert_is_null(cmd.result_sem);
	zassert_is_null(cmd.result);
}

ZTEST(voltage_control_runtime, test_runtime_submit_measurement_updates_domain_snapshot)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 10,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 77,
	};
	struct vc_channel_snapshot snap;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 77);
	zassert_equal(snap.measured_voltage, 77);

	vc_runtime_destroy(rt);
	free(d);
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
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, NULL),
		      VC_ERR_INVALID_VALUE);

	vc_runtime_destroy(rt);
	free(d);
}

ZTEST(voltage_control_runtime, test_runtime_get_channel_config_for_provider)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_runtime_config_snapshot cfg;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_get_channel_config(rt, 0, &cfg), VC_OK);
	zassert_equal(cfg.channel, 0);
	zassert_true(cfg.version >= 1);
	zassert_true(cfg.force_safe_state);

	vc_runtime_destroy(rt);
	free(d);
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

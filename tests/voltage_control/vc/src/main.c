/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "reg_store/reg_map.h"
#include "voltage_control/vc.h"

static struct vc_ctx *ctx;

static void *suite_setup(void)
{
	ctx = vc_init();
	zassert_not_null(ctx);
	return ctx;
}

ZTEST_SUITE(vc_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(vc_api, test_init_returns_non_null)
{
	zassert_not_null(ctx);
}

ZTEST(vc_api, test_start_returns_ok)
{
	zassert_equal(vc_ctx_start(ctx), VC_OK);
}

ZTEST(vc_api, test_dispatch_set_mode)
{
	zassert_equal(vc_dispatch(ctx,
				  vc_cmd_set_mode(VC_OPERATING_MODE_AUTOMATIC),
				  K_SECONDS(1)), VC_OK);

	struct vc_system_snapshot snap;

	k_msleep(50);
	zassert_equal(vc_query(ctx, vc_q_system_snapshot(&snap)), VC_OK);
	zassert_equal(snap.active_operating_mode, VC_OPERATING_MODE_AUTOMATIC);

	vc_dispatch(ctx, vc_cmd_set_mode(VC_OPERATING_MODE_NORMAL),
		    K_SECONDS(1));
}

ZTEST(vc_api, test_dispatch_output_action)
{
	zassert_equal(vc_dispatch(ctx,
				  vc_cmd_output(0, VC_OUTPUT_ACTION_ENABLE),
				  K_SECONDS(1)), VC_OK);

	struct vc_channel_snapshot snap;

	k_msleep(50);
	zassert_equal(vc_query(ctx, vc_q_channel_snapshot(0, &snap)), VC_OK);
	zassert_true(snap.status_bits & 0x0002);

	vc_dispatch(ctx, vc_cmd_output(0, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE),
		    K_SECONDS(1));
}

ZTEST(vc_api, test_dispatch_channel_field)
{
	zassert_equal(vc_dispatch(ctx,
				  vc_cmd_ch_field(0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 5000),
				  K_SECONDS(1)), VC_OK);

	struct vc_channel_config cfg;

	k_msleep(50);
	zassert_equal(vc_query(ctx, vc_q_channel_config(0, &cfg)), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_api, test_dispatch_system_field)
{
	zassert_equal(vc_dispatch(ctx,
				  vc_cmd_sys_field(VC_FIELD_RECOVERY_POLICY_MODE,
						   VC_RECOVERY_AUTO_RETRY),
				  K_SECONDS(1)), VC_OK);

	struct vc_system_config cfg;

	k_msleep(50);
	zassert_equal(vc_query(ctx, vc_q_system_config(&cfg)), VC_OK);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_AUTO_RETRY);
}

ZTEST(vc_api, test_dispatch_calibration)
{
	zassert_equal(vc_dispatch(ctx, vc_cmd_cal_unlock(CAL_UNLOCK_STEP1),
				  K_SECONDS(1)), VC_OK);
	zassert_equal(vc_dispatch(ctx, vc_cmd_cal_unlock(CAL_UNLOCK_STEP2),
				  K_SECONDS(1)), VC_OK);
	zassert_equal(vc_dispatch(ctx,
				  vc_cmd_set_mode(VC_OPERATING_MODE_CALIBRATION),
				  K_SECONDS(1)), VC_OK);

	struct vc_system_snapshot snap;

	k_msleep(50);
	zassert_equal(vc_query(ctx, vc_q_system_snapshot(&snap)), VC_OK);
	zassert_equal(snap.active_operating_mode, VC_OPERATING_MODE_CALIBRATION);

	vc_dispatch(ctx, vc_cmd_set_mode(VC_OPERATING_MODE_NORMAL),
		    K_SECONDS(1));
}

ZTEST(vc_api, test_query_system_snapshot)
{
	struct vc_system_snapshot snap;

	zassert_equal(vc_query(ctx, vc_q_system_snapshot(&snap)), VC_OK);
	zassert_equal(snap.supported_channel_count, 2);
}

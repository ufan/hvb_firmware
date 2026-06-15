/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/variant.h"

#define HVB_VARIANT_ID 1

#define HVB_SYS_CAP_FLAGS (0x0003U)
#define HVB_CHAN_CAP_FLAGS (0x0007U)

#define HVB_NUM_CHANNELS 2
#define HVB_CHANNEL_MASK 0x0003U
#define HVB_VOLTAGE_SCALE 100
#define HVB_CURRENT_SCALE 1
#define HVB_MAX_VOLTAGE_RAW 20000
#define HVB_MIN_VOLTAGE_RAW 0
#define HVB_MAX_CURRENT_RAW 32767

static const struct vc_variant_profile hvb_profile = {
	.variant_id = HVB_VARIANT_ID,
	.system_capability_flags = HVB_SYS_CAP_FLAGS,
	.channel_capability_flags = HVB_CHAN_CAP_FLAGS,
	.num_channels = HVB_NUM_CHANNELS,
	.channel_mask = HVB_CHANNEL_MASK,
	.voltage_scale = HVB_VOLTAGE_SCALE,
	.current_scale = HVB_CURRENT_SCALE,
	.max_voltage_raw = HVB_MAX_VOLTAGE_RAW,
	.min_voltage_raw = HVB_MIN_VOLTAGE_RAW,
	.max_current_raw = HVB_MAX_CURRENT_RAW,
	.default_channel_config = {
		.configured_target_voltage = 0,
		.ramp_up_step = 0,
		.ramp_up_interval = 0,
		.ramp_down_step = 0,
		.ramp_down_interval = 0,
		.voltage_protection_mode = VC_PROTECTION_MODE_DISABLED,
		.voltage_protection_output_action = VC_OUTPUT_ACTION_NONE,
		.voltage_limit_threshold = HVB_MAX_VOLTAGE_RAW,
		.current_protection_mode = VC_PROTECTION_MODE_DISABLED,
		.current_protection_output_action = VC_OUTPUT_ACTION_NONE,
		.current_limit_threshold = HVB_MAX_CURRENT_RAW,
		.auto_derate_step = 0,
		.save_target_policy = 0,
		.output_calib_k = 10000,
		.output_calib_b = 0,
		.measured_voltage_calib_k = 10000,
		.measured_voltage_calib_b = 0,
		.measured_current_calib_k = 10000,
		.measured_current_calib_b = 0,
	},
	.default_system_config = {
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.slave_address = 1,
		.baud_rate_code = VC_BAUD_RATE_115200,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
		.auto_retry_delay = 0,
		.auto_retry_max_count = 0,
		.auto_retry_window = 0,
		.voltage_safe_band_pct = 10,
		.current_safe_band_pct = 10,
	},
};

const struct vc_variant_profile *vc_hvb_get_variant(void)
{
	return &hvb_profile;
}

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VARIANT_H
#define VOLTAGE_CONTROL_VARIANT_H

#include "domain.h"

struct vc_variant_profile {
	uint16_t variant_id;
	uint16_t system_capability_flags;
	uint16_t channel_capability_flags;
	uint8_t num_channels;
	uint16_t channel_mask;
	uint16_t voltage_scale;
	uint16_t current_scale;
	int16_t max_voltage_raw;
	int16_t min_voltage_raw;
	int16_t max_current_raw;
	uint16_t max_raw_dac_code;
	bool calibration_output_disable_confirmed;
	struct vc_channel_config default_channel_config;
	struct vc_system_config default_system_config;
};

const struct vc_variant_profile *vc_hvb_get_variant(void);

#endif

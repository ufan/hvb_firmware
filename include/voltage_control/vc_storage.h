/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_STORAGE_H
#define VOLTAGE_CONTROL_VC_STORAGE_H

#include "voltage_control/domain.h"

struct vc_storage_backend {
	int (*save_system_config)(const struct vc_system_config *cfg);
	int (*load_system_config)(struct vc_system_config *cfg);
	int (*save_channel_config)(uint8_t ch, const struct vc_channel_config *cfg);
	int (*load_channel_config)(uint8_t ch, struct vc_channel_config *cfg);
	int (*save_channel_cal)(uint8_t ch, const struct vc_channel_config *cfg);
	int (*load_channel_cal)(uint8_t ch, struct vc_channel_config *cfg);
	int (*erase_all)(void);
};

#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
extern const struct vc_storage_backend vc_settings_storage;
#endif

#endif

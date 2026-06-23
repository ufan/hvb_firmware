/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_H
#define VOLTAGE_CONTROL_VC_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct vc_runtime_config_snapshot;

/* Driver API vtable for a voltage-control channel (DAC output + ADC measurement). */
struct vc_channel_api {
	int (*set_output)(const struct device *dev, uint16_t code);       /* Write raw DAC code */
	int (*set_enable)(const struct device *dev, bool enable);         /* Enable/disable HV output */
	int (*apply_config)(const struct device *dev,                     /* Apply full runtime config */
			    const struct vc_runtime_config_snapshot *cfg);
	int (*start)(const struct device *dev);                           /* Start periodic work loop */
	int (*stop)(const struct device *dev);                            /* Cancel periodic work */
	int (*notify_config_changed)(const struct device *dev, uint32_t version); /* Wake worker on config change */
	int (*measure_voltage)(const struct device *dev, int32_t *value); /* Read raw ADC voltage */
	int (*measure_current)(const struct device *dev, int32_t *value); /* Read raw ADC current */
	uint16_t (*get_capabilities)(const struct device *dev);           /* Return CH_CAP_* bitmask */
};

#endif

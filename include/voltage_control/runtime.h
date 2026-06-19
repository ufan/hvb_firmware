/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_RUNTIME_H
#define VOLTAGE_CONTROL_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#define VC_MEAS_PRESENT_VOLTAGE         0x0001
#define VC_MEAS_PRESENT_CURRENT         0x0002
#define VC_MEAS_PRESENT_PROVIDER_STATUS 0x0004

#define VC_PROVIDER_STATUS_READY        0x0001
#define VC_PROVIDER_STATUS_APPLY_FAILED 0x0002
#define VC_PROVIDER_STATUS_SAMPLE_ERROR 0x0004
#define VC_PROVIDER_STATUS_INTERLOCK    0x0008

struct vc_runtime_config_snapshot {
	uint8_t channel;
	uint32_t version;
	uint16_t capability_flags;

	bool output_enable;
	uint16_t raw_output_drive;

	bool calibration_mode;
	bool calibration_output_enable;
	uint16_t calibration_raw_output_drive;

	bool force_safe_state;
};

struct vc_measurement_snapshot {
	uint8_t channel;
	uint32_t generation;
	uint32_t timestamp_ms;

	uint16_t present_mask;
	int32_t raw_voltage;
	int32_t raw_current;
	uint16_t provider_status;
	uint16_t provider_fault_cause;
};

#include "voltage_control/domain.h"

struct vc_runtime;
struct domain;

/*
 * The runtime borrows domain. The caller owns domain and must keep it alive
 * until after vc_runtime_destroy() returns. Destroying the runtime frees only
 * the runtime object, not the borrowed domain.
 */
struct vc_runtime *vc_runtime_create(struct domain *domain);
void vc_runtime_destroy(struct vc_runtime *runtime);
enum vc_status vc_runtime_submit_measurement(
	struct vc_runtime *runtime,
	const struct vc_measurement_snapshot *meas);
enum vc_status vc_runtime_get_channel_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_runtime_config_snapshot *cfg);

#endif

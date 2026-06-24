/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CONTROLLER_H
#define VOLTAGE_CONTROL_VC_CONTROLLER_H

#include "voltage_control/vc_channel_state.h"
#include "voltage_control/vc_storage.h"

struct vc_controller {
	struct vc_channel channels[VC_MAX_CHANNELS];
	size_t channel_count;
	enum vc_operating_mode operating_mode;
	uint8_t cal_unlock_step;
	bool cal_unlocked;
	const struct vc_storage_backend *storage;
	struct vc_system_config sys_cfg;
};

struct vc_controller *vc_controller_init_static(
	const struct vc_channel_entry *entries, size_t count);

enum vc_status vc_controller_set_operating_mode(
	struct vc_controller *ctrl, enum vc_operating_mode mode);
enum vc_operating_mode vc_controller_get_operating_mode(
	const struct vc_controller *ctrl);
enum vc_status vc_controller_calibration_unlock(
	struct vc_controller *ctrl, uint16_t value);

enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value);
enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action);
enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd);

void vc_controller_consume_voltage(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_voltage);
void vc_controller_consume_current(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_current);
void vc_controller_consume_fault(
	struct vc_controller *ctrl, uint8_t ch, uint16_t fault_cause);

void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms);

enum vc_status vc_controller_system_param_action(
	struct vc_controller *ctrl, enum vc_param_action action);
enum vc_status vc_controller_channel_param_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_param_action action);

enum vc_status vc_controller_get_system_config(
	const struct vc_controller *ctrl, struct vc_system_config *cfg);
enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg);
void vc_controller_get_system_snapshot(
	const struct vc_controller *ctrl, struct vc_system_snapshot *snap);
enum vc_status vc_controller_get_channel_snapshot(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_snapshot *snap);
enum vc_status vc_controller_get_channel_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_config *cfg);

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend);

size_t vc_controller_channel_count(const struct vc_controller *ctrl);
uint16_t vc_controller_channel_capabilities(
	const struct vc_controller *ctrl, uint8_t ch);

#endif

/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H
#define VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/device.h>

struct vc_measurement_buffer_entry {
	int32_t raw_voltage;
	uint32_t voltage_timestamp_ms;
	int32_t raw_current;
	uint32_t current_timestamp_ms;
};

struct vc_channel_table_entry {
	const struct device *dev;
	uint8_t index;
	uint16_t capabilities;
	struct vc_measurement_buffer_entry *meas;
};

struct vc_controller;

void vc_channel_table_init(struct vc_controller *ctrl);
struct vc_controller *vc_channel_table_get_controller(void);

int vc_channel_table_set_output(uint8_t ch, uint16_t code);
int vc_channel_table_set_enable(uint8_t ch, bool enable);
int vc_channel_table_start_sampling(uint8_t ch);
int vc_channel_table_stop_sampling(uint8_t ch);

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch);
size_t vc_channel_table_count(void);
uint16_t vc_channel_table_capabilities(uint8_t ch);

extern struct vc_channel_table_entry vc_channel_table[];

#endif

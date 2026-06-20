/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_MODBUS_ADAPTER_H
#define VOLTAGE_CONTROL_MODBUS_ADAPTER_H

#include <stdint.h>

struct vc_runtime;
struct vc_mb_adapter;

enum vc_mb_result {
	VC_MB_OK = 0,
	VC_MB_ILLEGAL_FUNCTION = 1,
	VC_MB_ILLEGAL_ADDRESS = 2,
	VC_MB_ILLEGAL_VALUE = 3,
	VC_MB_DEVICE_FAILURE = 4,
};

struct vc_mb_adapter *vc_mb_adapter_create(struct vc_runtime *runtime);
enum vc_mb_result vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr,
				 uint16_t *reg);
enum vc_mb_result vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t *reg);
enum vc_mb_result vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t val);

#endif

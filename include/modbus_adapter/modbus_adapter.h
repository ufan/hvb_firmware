/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MODBUS_ADAPTER_MODBUS_ADAPTER_H
#define MODBUS_ADAPTER_MODBUS_ADAPTER_H

#include <stdint.h>

struct vc_mb_adapter;

enum vc_mb_result {
	VC_MB_OK = 0,
	VC_MB_ILLEGAL_FUNCTION = 1,
	VC_MB_ILLEGAL_ADDRESS = 2,
	VC_MB_ILLEGAL_VALUE = 3,
	VC_MB_DEVICE_FAILURE = 4,
};

enum vc_baud_rate_code {
	VC_BAUD_RATE_115200 = 0,
	VC_BAUD_RATE_9600 = 1,
};

struct mb_adapter_config {
	uint16_t slave_address;
	uint16_t baud_rate_code;
};

int modbus_adapter_init(void);

struct vc_mb_adapter *vc_mb_adapter_create(void);
enum vc_mb_result vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr,
				 uint16_t *reg);
enum vc_mb_result vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t *reg);
enum vc_mb_result vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr,
				   uint16_t val);

#endif

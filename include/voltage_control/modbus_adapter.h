/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_MODBUS_ADAPTER_H
#define VOLTAGE_CONTROL_MODBUS_ADAPTER_H

#include <stdint.h>

struct domain;
struct vc_mb_adapter;

struct vc_mb_adapter *vc_mb_adapter_create(struct domain *domain);
int vc_mb_input_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg);
int vc_mb_holding_rd(struct vc_mb_adapter *a, uint16_t addr, uint16_t *reg);
int vc_mb_holding_wr(struct vc_mb_adapter *a, uint16_t addr, uint16_t val);

#endif

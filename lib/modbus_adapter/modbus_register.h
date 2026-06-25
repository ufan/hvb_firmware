/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register-level read/write interface for the VC domain.
 * Maps protocol register offsets (from regmap/vc_regs.h) to domain
 * queries and commands. Adapters call these instead of accessing
 * domain snapshots or config structs directly.
 */

#ifndef MODBUS_ADAPTER_MODBUS_REGISTER_H
#define MODBUS_ADAPTER_MODBUS_REGISTER_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include "voltage_control/vc_types.h"

struct vc_ctx;

enum vc_status vc_reg_read_sys_input(struct vc_ctx *ctx, uint16_t off,
				     uint16_t *reg);
enum vc_status vc_reg_read_sys_holding(struct vc_ctx *ctx, uint16_t off,
				       uint16_t *reg);
enum vc_status vc_reg_write_sys_holding(struct vc_ctx *ctx, uint16_t off,
					uint16_t val, k_timeout_t timeout);
enum vc_status vc_reg_read_ch_input(struct vc_ctx *ctx, uint8_t ch,
				    uint16_t off, uint16_t *reg);
enum vc_status vc_reg_read_ch_holding(struct vc_ctx *ctx, uint8_t ch,
				      uint16_t off, uint16_t *reg);
enum vc_status vc_reg_write_ch_holding(struct vc_ctx *ctx, uint8_t ch,
				       uint16_t off, uint16_t val,
				       k_timeout_t timeout);
enum vc_status vc_reg_write_ext(struct vc_ctx *ctx, uint16_t off,
				uint16_t val, k_timeout_t timeout);

#endif

/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register-level read/write interface for the VC domain.
 * Read functions draw from the central reg_store; write functions
 * dispatch to the VC domain and write back on success.
 */

#ifndef MODBUS_ADAPTER_MODBUS_REGISTER_H
#define MODBUS_ADAPTER_MODBUS_REGISTER_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include "voltage_control/vc_types.h"

struct vc_ctx;

/* Read functions — no ctx needed, data comes from reg_store. */
enum vc_status vc_reg_read_sys_input(uint16_t off, uint16_t *reg);
enum vc_status vc_reg_read_sys_holding(uint16_t off, uint16_t *reg);
enum vc_status vc_reg_read_ch_input(uint8_t ch, uint16_t off, uint16_t *reg);
enum vc_status vc_reg_read_ch_holding(uint8_t ch, uint16_t off, uint16_t *reg);

/* Write functions — ctx needed for domain dispatch. */
enum vc_status vc_reg_write_sys_holding(struct vc_ctx *ctx, uint16_t off,
					uint16_t val, k_timeout_t timeout);
enum vc_status vc_reg_write_ch_holding(struct vc_ctx *ctx, uint8_t ch,
					uint16_t off, uint16_t val,
					k_timeout_t timeout);
enum vc_status vc_reg_write_ext(struct vc_ctx *ctx, uint16_t off,
				uint16_t val, k_timeout_t timeout);

#endif

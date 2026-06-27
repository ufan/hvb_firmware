/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Central register store — a flat, ISR-safe backing store for all
 * protocol register values.  Modules publish into the store; Modbus
 * and other adapters read from it.
 *
 * Two independent banks mirror the Modbus register model:
 *   input   — FC04 read-only registers (status, measurements)
 *   holding — FC03/FC06 configuration registers
 *
 * Layout follows regmap/vc_regs.h:
 *   [0..39]    system block
 *   [40..679]  per-channel blocks  (16 × 40)
 *   [680..759] extension block     (80 registers)
 */

#ifndef REG_STORE_REG_STORE_H
#define REG_STORE_REG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define REG_STORE_SIZE 760U  /* sys(40) + 16ch×40(640) + ext(80) */

/*
 * Write a single register value.  No-op if addr is out of range.
 * Safe to call from any thread or ISR context.
 */
void reg_store_write_input(uint16_t addr, uint16_t val);
void reg_store_write_holding(uint16_t addr, uint16_t val);

/*
 * Read a single register value into *out.
 * Returns true on success, false if addr is out of range or out is NULL.
 * Safe to call from any thread or ISR context.
 */
bool reg_store_read_input(uint16_t addr, uint16_t *out);
bool reg_store_read_holding(uint16_t addr, uint16_t *out);

#endif /* REG_STORE_REG_STORE_H */

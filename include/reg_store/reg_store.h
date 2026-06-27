/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Central register store — an ISR-safe backing store for all protocol
 * register values.  Modules publish into the store; Modbus and other
 * adapters read from it.
 *
 * Two independent banks mirror the Modbus register model:
 *   input   — FC04 read-only registers (status, measurements)
 *   holding — FC03/FC06 configuration registers
 *
 * All functions take WIRE addresses as defined in reg_map.h.  The store
 * maps these to a compact in-RAM layout sized to the actual channel count
 * from DTS; wire addresses for unconfigured channel slots are silently
 * rejected (write is a no-op, read returns false).
 */

#ifndef REG_STORE_REG_STORE_H
#define REG_STORE_REG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "reg_store/reg_map.h"

/*
 * Write a single register value by wire address.
 * No-op if the address is out of range or maps to an unconfigured channel slot.
 * Safe to call from any thread or ISR context.
 */
void reg_store_write_input(uint16_t addr, uint16_t val);
void reg_store_write_holding(uint16_t addr, uint16_t val);

/*
 * Read a single register value by wire address into *out.
 * Returns true on success, false if the address is out of range,
 * maps to an unconfigured channel slot, or out is NULL.
 * Safe to call from any thread or ISR context.
 */
bool reg_store_read_input(uint16_t addr, uint16_t *out);
bool reg_store_read_holding(uint16_t addr, uint16_t *out);

#endif /* REG_STORE_REG_STORE_H */

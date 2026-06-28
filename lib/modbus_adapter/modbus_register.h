/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modbus register view over the protocol-neutral Register Catalog.
 * Writes are validated and committed by the owning module.
 */

#ifndef MODBUS_ADAPTER_MODBUS_REGISTER_H
#define MODBUS_ADAPTER_MODBUS_REGISTER_H

#include <stdint.h>
#include <zephyr/kernel.h>
#include "reg_store/reg_catalog.h"
#include "voltage_control/vc_types.h"

/* Register View accessors over the protocol-neutral Register Catalog. */
enum reg_status vc_reg_read_sys_input(uint16_t off, uint16_t *reg);
enum reg_status vc_reg_read_sys_holding(uint16_t off, uint16_t *reg);
enum reg_status vc_reg_read_ch_input(uint8_t ch, uint16_t off, uint16_t *reg);
enum reg_status vc_reg_read_ch_holding(uint8_t ch, uint16_t off, uint16_t *reg);

enum reg_status vc_reg_write_sys_holding(uint16_t off, uint16_t val,
					k_timeout_t timeout);
enum reg_status vc_reg_write_ch_holding(uint8_t ch, uint16_t off, uint16_t val,
				       k_timeout_t timeout);
enum reg_status vc_reg_write_ext(uint16_t off, uint16_t val,
				k_timeout_t timeout);

#endif

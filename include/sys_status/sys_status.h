/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SYS_STATUS_SYS_STATUS_H
#define SYS_STATUS_SYS_STATUS_H

#include <stdint.h>

struct sys_status_snapshot {
	int16_t board_temperature;
	uint16_t board_humidity;
	uint32_t uptime;
	uint16_t fw_version_high;
	uint16_t fw_version_low;
};

struct sys_status_snapshot sys_status_get(void);

#include "regmap/vc_regs.h"

static inline int sys_status_read_input_reg(uint16_t off, uint16_t *reg)
{
	struct sys_status_snapshot ss = sys_status_get();

	switch (off) {
	case SYS_BOARD_TEMPERATURE: *reg = (uint16_t)ss.board_temperature; return 0;
	case SYS_BOARD_HUMIDITY:    *reg = ss.board_humidity; return 0;
	case SYS_UPTIME_HI:        *reg = (uint16_t)(ss.uptime >> 16); return 0;
	case SYS_UPTIME_LO:        *reg = (uint16_t)(ss.uptime & 0xFFFF); return 0;
	case SYS_FW_VERSION_HI:    *reg = ss.fw_version_high; return 0;
	case SYS_FW_VERSION_LO:    *reg = ss.fw_version_low; return 0;
	default: return -1;
	}
}

#endif

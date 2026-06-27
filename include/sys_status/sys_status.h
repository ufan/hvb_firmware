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

#endif

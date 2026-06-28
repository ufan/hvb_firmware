/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SYS_STATUS_SYS_STATUS_H
#define SYS_STATUS_SYS_STATUS_H

#include <stdint.h>

int sys_status_request_reset(void);
void sys_status_platform_reset(void);

#endif

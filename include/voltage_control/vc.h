/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified voltage-control API.
 *
 * vc_init() constructs the singleton from DTS-composed channel data.
 * Frontends access state and commands through the Register Catalog.
 */

#ifndef VOLTAGE_CONTROL_VC_H
#define VOLTAGE_CONTROL_VC_H

#include "voltage_control/vc_runtime.h"

/* ------------------------------------------------------------------ */
/* Opaque context                                                      */
/* ------------------------------------------------------------------ */

struct vc_ctx;

/* Create the singleton vc_ctx from DTS channel data; returns NULL on failure. */
struct vc_ctx *vc_init(void);
/* Stop the runtime worker and release the singleton context. */
void vc_destroy(struct vc_ctx *ctx);
/* Start channel hw sampling; call once after vc_init. */
enum vc_status vc_ctx_start(struct vc_ctx *ctx);

#endif /* VOLTAGE_CONTROL_VC_H */

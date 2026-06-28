/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc.h"

struct vc_ctx {
	struct vc_runtime *runtime;
};

static struct vc_ctx g_ctx;

static struct vc_ctx *init_from_runtime(struct vc_runtime *rt)
{
	if (rt == NULL) {
		return NULL;
	}
	g_ctx.runtime = rt;
	return &g_ctx;
}

struct vc_ctx *vc_init(void)
{
	return init_from_runtime(vc_runtime_create_static());
}

void vc_destroy(struct vc_ctx *ctx)
{
	if (ctx == NULL) {
		return;
	}
	vc_runtime_destroy(ctx->runtime);
	ctx->runtime = NULL;
}

enum vc_status vc_ctx_start(struct vc_ctx *ctx)
{
	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	return vc_runtime_start_sampling(ctx->runtime);
}

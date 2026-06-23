/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc.h"
#include "voltage_control/provider_bus.h"

extern const struct vc_channel_entry vc_domain_channels[];
extern const size_t vc_domain_channel_count;

struct vc_ctx {
	struct vc_runtime *runtime;
};

static struct vc_ctx g_ctx;

struct vc_ctx *vc_init(void)
{
	struct vc_runtime *rt;

	rt = vc_domain_runtime_create_static(vc_domain_channels,
					     vc_domain_channel_count);
	if (rt == NULL) {
		return NULL;
	}

	g_ctx.runtime = rt;
	return &g_ctx;
}

enum vc_status vc_ctx_start(struct vc_ctx *ctx)
{
	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	return vc_provider_bus_start_all();
}

/* Translate a vc_cal_command into a vc_runtime_command and submit to the runtime. */
static enum vc_status dispatch_calibration(struct vc_runtime *rt,
					   const struct vc_cal_command *cal,
					   k_timeout_t timeout)
{
	struct vc_runtime_command cmd = { .channel = cal->channel };

	switch (cal->action) {
	case VC_CAL_UNLOCK:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_UNLOCK;
		cmd.payload.calibration_unlock_value = cal->value;
		break;
	case VC_CAL_SET_OUTPUT_ENABLE:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE;
		cmd.payload.calibration_output_enable = cal->enable;
		break;
	case VC_CAL_SET_RAW_DAC:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_RAW_DAC;
		cmd.payload.calibration_raw_dac = cal->value;
		break;
	case VC_CAL_SAMPLE:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_SAMPLE;
		break;
	case VC_CAL_COMMIT:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_COMMIT;
		break;
	case VC_CAL_SET_MAX_RAW_DAC:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC;
		cmd.payload.calibration_max_raw_dac = cal->value;
		break;
	default:
		return VC_ERR_INVALID_COMMAND;
	}

	return vc_runtime_submit_command(rt, &cmd, timeout);
}

enum vc_status vc_dispatch(struct vc_ctx *ctx, struct vc_cmd cmd,
			   k_timeout_t timeout)
{
	struct vc_runtime_command rtcmd = {0};

	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	switch (cmd.type) {
	case VC_CMD_SET_OPERATING_MODE:
		return vc_runtime_set_operating_mode(ctx->runtime,
						     cmd.operating_mode,
						     timeout);
	case VC_CMD_OUTPUT_ACTION:
		rtcmd.type = VC_RUNTIME_CMD_OUTPUT_ACTION;
		rtcmd.channel = cmd.channel;
		rtcmd.payload.output_action = cmd.output_action;
		return vc_runtime_submit_command(ctx->runtime, &rtcmd, timeout);

	case VC_CMD_FAULT_COMMAND:
		rtcmd.type = VC_RUNTIME_CMD_FAULT_COMMAND;
		rtcmd.channel = cmd.channel;
		rtcmd.payload.fault_command = cmd.fault_command;
		return vc_runtime_submit_command(ctx->runtime, &rtcmd, timeout);

	case VC_CMD_SET_SYSTEM_FIELD:
		return vc_runtime_set_system_field(ctx->runtime,
						   cmd.field_write.field,
						   cmd.field_write.value,
						   timeout);
	case VC_CMD_SET_CHANNEL_FIELD:
		return vc_runtime_set_channel_field(ctx->runtime,
						    cmd.channel,
						    cmd.field_write.field,
						    cmd.field_write.value,
						    timeout);
	case VC_CMD_CALIBRATION:
		return dispatch_calibration(ctx->runtime, &cmd.cal, timeout);

	case VC_CMD_SYSTEM_PARAM_ACTION:
		rtcmd.type = VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION;
		rtcmd.payload.param_action = cmd.param_action;
		return vc_runtime_submit_command(ctx->runtime, &rtcmd, timeout);

	case VC_CMD_CHANNEL_PARAM_ACTION:
		rtcmd.type = VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION;
		rtcmd.channel = cmd.channel;
		rtcmd.payload.param_action = cmd.param_action;
		return vc_runtime_submit_command(ctx->runtime, &rtcmd, timeout);

	case VC_CMD_SUBMIT_MEASUREMENT:
		return vc_runtime_submit_measurement(ctx->runtime,
						     &cmd.measurement);
	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

enum vc_status vc_query(struct vc_ctx *ctx, struct vc_query q)
{
	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	switch (q.type) {
	case VC_QUERY_SYSTEM_SNAPSHOT:
		return vc_runtime_get_published_system_snapshot(
			ctx->runtime, q.out.system_snapshot);

	case VC_QUERY_CHANNEL_SNAPSHOT:
		return vc_runtime_get_published_channel_snapshot(
			ctx->runtime, q.channel, q.out.channel_snapshot);

	case VC_QUERY_SYSTEM_CONFIG:
		return vc_runtime_get_published_system_config(
			ctx->runtime, q.out.system_config);

	case VC_QUERY_CHANNEL_CONFIG:
		return vc_runtime_get_published_channel_config(
			ctx->runtime, q.channel, q.out.channel_config);

	case VC_QUERY_RUNTIME_CONFIG:
		return vc_runtime_get_channel_config(
			ctx->runtime, q.channel, q.out.runtime_config);

	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

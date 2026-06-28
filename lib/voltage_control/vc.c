/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc.h"
#include "voltage_control/vc_controller.h"
#include "reg_store/reg_catalog.h"
#include "reg_store/reg_schema.h"

#include <string.h>

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
	case VC_CAL_EXIT:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_EXIT;
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
	case VC_CMD_SET_CHANNEL_CAL_FIELD:
		return vc_runtime_set_channel_cal_field(ctx->runtime,
							cmd.channel,
							cmd.cal_field_write.field,
							cmd.cal_field_write.value,
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

	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

enum vc_status vc_query(struct vc_ctx *ctx, struct vc_query_msg q)
{
	union reg_value value;
	enum reg_status status;

	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
#pragma push_macro("READ_REG")
#undef READ_REG
#define READ_REG(id_, member_, union_member_) do { \
	status = reg_read((id_), &value); \
	if (status == REG_NOT_FOUND || status == REG_UNSUPPORTED) { \
		return VC_ERR_UNSUPPORTED_CHANNEL; \
	} \
	if (status != REG_OK) { \
		return VC_ERR_INVALID_VALUE; \
	} \
	(member_) = value.union_member_; \
} while (false)

	switch (q.type) {
	case VC_QUERY_SYSTEM_SNAPSHOT: {
		struct vc_system_snapshot *s = q.out.system_snapshot;
		if (s == NULL) return VC_ERR_INVALID_VALUE;
		memset(s, 0, sizeof(*s));
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_PROTOCOL_MAJOR), s->protocol_major, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_PROTOCOL_MINOR), s->protocol_minor, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_VARIANT_ID), s->variant_id, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_CAPABILITY_FLAGS), s->system_capability_flags, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_SUPPORTED_CHANNELS), s->supported_channel_count, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_ACTIVE_CHANNEL_MASK), s->active_channel_mask, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_ACTIVE_OPERATING_MODE), s->active_operating_mode, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_STATUS), s->system_status, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_FAULT_CAUSE), s->system_fault_cause, u16);
		return VC_OK;
	}
	case VC_QUERY_CHANNEL_SNAPSHOT: {
		struct vc_channel_snapshot *s = q.out.channel_snapshot;
		uint8_t ch = q.channel;
		if (s == NULL) return VC_ERR_INVALID_VALUE;
		memset(s, 0, sizeof(*s));
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_VOLTAGE), s->measured_voltage, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_CURRENT), s->measured_current, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_OPER_TARGET_VOLTAGE), s->operational_target_voltage, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_STATUS_BITS), s->status_bits, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_ACTIVE_FAULT_CAUSE), s->active_fault_cause, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_FAULT_HISTORY_CAUSE), s->fault_history_cause, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_LAST_PROT_OUT_ACTION), s->last_protection_output_action, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_RETRY_COUNT), s->auto_retry_count, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_COOLDOWN_REMAINING), s->auto_cooldown_remaining, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_LAST_FAULT_TIMESTAMP), s->last_fault_timestamp, u32);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CAPABILITY_FLAGS), s->channel_capability_flags, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAW_ADC_VOLTAGE), s->raw_adc_voltage, s32);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAW_ADC_CURRENT), s->raw_adc_current, s32);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CAL_DAC_CODE), s->raw_dac_readback, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CAL_OUTPUT_ENABLE), s->cal_output_enabled, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT), s->cal_max_raw_dac_limit, u16);
		return VC_OK;
	}
	case VC_QUERY_SYSTEM_CONFIG: {
		struct vc_system_config *c = q.out.system_config;
		if (c == NULL) return VC_ERR_INVALID_VALUE;
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_OPERATING_MODE), c->operating_mode, u16);
		READ_REG(REG_SYS_ID(REG_SYS_FIELD_STARTUP_CHANNEL_POLICY), c->startup_channel_policy, u16);
		return VC_OK;
	}
	case VC_QUERY_CHANNEL_CONFIG: {
		struct vc_channel_config *c = q.out.channel_config;
		uint8_t ch = q.channel;
		if (c == NULL) return VC_ERR_INVALID_VALUE;
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CFG_TARGET_VOLTAGE), c->configured_target_voltage, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAMP_UP_STEP), c->ramp_up_step, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAMP_UP_INTERVAL), c->ramp_up_interval, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAMP_DOWN_STEP), c->ramp_down_step, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RAMP_DOWN_INTERVAL), c->ramp_down_interval, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_RECOVERY_POLICY_MODE), c->recovery_policy_mode, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_RETRY_DELAY), c->auto_retry_delay, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_RETRY_MAX_COUNT), c->auto_retry_max_count, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_RETRY_WINDOW), c->auto_retry_window, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CURRENT_SAFE_BAND_PCT), c->current_safe_band_pct, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CURRENT_PROTECTION_MODE), c->current_protection_mode, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CURRENT_PROT_OUT_ACTION), c->current_protection_output_action, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_CURRENT_LIMIT_THRESHOLD), c->current_limit_threshold, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_AUTO_DERATE_STEP), c->auto_derate_step, u16);
		return VC_OK;
	}
	case VC_QUERY_CHANNEL_CAL_CONFIG: {
		struct vc_channel_cal_config *c = q.out.channel_cal_config;
		uint8_t ch = q.channel;
		if (c == NULL) return VC_ERR_INVALID_VALUE;
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_OUTPUT_CAL_K), c->output_calib_k, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_OUTPUT_CAL_B), c->output_calib_b, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_V_CAL_K), c->measured_voltage_calib_k, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_V_CAL_B), c->measured_voltage_calib_b, s16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_I_CAL_K), c->measured_current_calib_k, u16);
		READ_REG(REG_VC_ID(ch, REG_VC_FIELD_MEASURED_I_CAL_B), c->measured_current_calib_b, s16);
		return VC_OK;
	}

	default:
		return VC_ERR_INVALID_COMMAND;
	}

#undef READ_REG
#pragma pop_macro("READ_REG")
}

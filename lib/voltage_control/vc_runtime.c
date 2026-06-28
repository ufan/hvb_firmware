/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#include "voltage_control/vc_runtime.h"
#include "voltage_control/vc_controller.h"
#include "voltage_control/vc_storage.h"

#include "reg_store/reg_catalog.h"
#include "reg_store/reg_map.h"
#include "reg_store/reg_schema.h"

#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
#include <zephyr/settings/settings.h>
#endif

K_KERNEL_STACK_DEFINE(vc_runtime_stack, CONFIG_VC_RUNTIME_THREAD_STACK_SIZE);

struct vc_runtime_work_item {
	struct vc_runtime_command command;
};

struct vc_runtime {
	struct vc_controller *ctrl;
	struct k_mutex lock;
	struct k_msgq command_queue;
	struct k_sem wake;
	struct k_thread thread;
	bool stop_requested;
	char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH *
			    sizeof(struct vc_runtime_work_item)];
};

static struct vc_runtime *catalog_runtime;

static enum reg_status vc_status_to_reg(enum vc_status status)
{
	switch (status) {
	case VC_OK:
		return REG_OK;
	case VC_ERR_INVALID_VALUE:
	case VC_ERR_INVALID_COMMAND:
	case VC_ERR_UNSAFE_STATE:
		return REG_INVALID_VALUE;
	case VC_ERR_UNSUPPORTED_CHANNEL:
	case VC_ERR_UNSUPPORTED_CAPABILITY:
		return REG_UNSUPPORTED;
	case VC_ERR_STORAGE:
		return REG_IO_ERROR;
	default:
		return REG_BUSY;
	}
}

static bool vc_catalog_supported(uint16_t field, uint16_t caps)
{
	switch (field) {
	case REG_VC_FIELD_MEASURED_VOLTAGE:
	case REG_VC_FIELD_RAW_ADC_VOLTAGE:
	case REG_VC_FIELD_MEASURED_V_CAL_K:
	case REG_VC_FIELD_MEASURED_V_CAL_B:
		return (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0U;
	case REG_VC_FIELD_MEASURED_CURRENT:
	case REG_VC_FIELD_RAW_ADC_CURRENT:
	case REG_VC_FIELD_MEASURED_I_CAL_K:
	case REG_VC_FIELD_MEASURED_I_CAL_B:
	case REG_VC_FIELD_CURRENT_PROTECTION_MODE:
	case REG_VC_FIELD_CURRENT_PROT_OUT_ACTION:
	case REG_VC_FIELD_CURRENT_LIMIT_THRESHOLD:
		return (caps & CH_CAP_CURRENT_MEASUREMENT) != 0U;
	case REG_VC_FIELD_CFG_TARGET_VOLTAGE:
	case REG_VC_FIELD_RAMP_UP_STEP:
	case REG_VC_FIELD_RAMP_UP_INTERVAL:
	case REG_VC_FIELD_RAMP_DOWN_STEP:
	case REG_VC_FIELD_RAMP_DOWN_INTERVAL:
	case REG_VC_FIELD_OUTPUT_CAL_K:
	case REG_VC_FIELD_OUTPUT_CAL_B:
	case REG_VC_FIELD_CAL_OUTPUT_ENABLE:
	case REG_VC_FIELD_CAL_DAC_CODE:
	case REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT:
		return (caps & CH_CAP_RAW_OUTPUT_DRIVE) != 0U;
	case REG_VC_FIELD_AUTO_DERATE_STEP:
		return (caps & (CH_CAP_RAW_OUTPUT_DRIVE |
				CH_CAP_VOLTAGE_MEASUREMENT)) ==
		       (CH_CAP_RAW_OUTPUT_DRIVE | CH_CAP_VOLTAGE_MEASUREMENT);
	case REG_VC_FIELD_CAL_SAMPLE_CMD:
		return (caps & (CH_CAP_VOLTAGE_MEASUREMENT |
				CH_CAP_CURRENT_MEASUREMENT)) != 0U;
	case REG_VC_FIELD_CAL_COMMIT_CMD:
		return (caps & (CH_CAP_RAW_OUTPUT_DRIVE |
				CH_CAP_VOLTAGE_MEASUREMENT |
				CH_CAP_CURRENT_MEASUREMENT)) != 0U;
	default:
		return true;
	}
}

static enum reg_status vc_catalog_read(const struct reg_descriptor *desc,
				       union reg_value *value)
{
	struct vc_runtime *runtime = catalog_runtime;
	uint16_t field = (uint16_t)REG_ID_FIELD(desc->id);
	uint8_t channel = (uint8_t)REG_ID_INSTANCE(desc->id);
	struct vc_controller *ctrl;
	struct vc_channel *ch = NULL;

	if (runtime == NULL || value == NULL) {
		return REG_BUSY;
	}

	ctrl = runtime->ctrl;
	if (REG_ID_MODULE(desc->id) == REG_MODULE_VOLTAGE_CONTROL) {
		if (channel >= ctrl->channel_count) {
			return REG_UNSUPPORTED;
		}
		ch = &ctrl->channels[channel];
		if (!vc_catalog_supported(field, ch->capabilities)) {
			return REG_UNSUPPORTED;
		}
	}

	k_mutex_lock(&runtime->lock, K_FOREVER);
	memset(value, 0, sizeof(*value));

	if (REG_ID_MODULE(desc->id) == REG_MODULE_SYSTEM) {
		switch (field) {
		case REG_SYS_FIELD_PROTOCOL_MAJOR: value->u16 = VC_PROTOCOL_MAJOR; break;
		case REG_SYS_FIELD_PROTOCOL_MINOR: value->u16 = VC_PROTOCOL_MINOR; break;
		case REG_SYS_FIELD_VARIANT_ID: value->u16 = 1U; break;
		case REG_SYS_FIELD_CAPABILITY_FLAGS:
			value->u16 = SYS_CAP_AUTOMATIC_MODE | SYS_CAP_CALIBRATION_MODE;
			if (IS_ENABLED(CONFIG_SYS_STATUS)) {
				value->u16 |= SYS_CAP_ENV_SENSOR;
			}
			break;
		case REG_SYS_FIELD_SUPPORTED_CHANNELS:
			value->u16 = (uint16_t)ctrl->channel_count;
			break;
		case REG_SYS_FIELD_ACTIVE_CHANNEL_MASK:
			value->u16 = (uint16_t)((1U << ctrl->channel_count) - 1U);
			break;
		case REG_SYS_FIELD_ACTIVE_OPERATING_MODE:
			value->u16 = (uint16_t)ctrl->operating_mode;
			break;
		case REG_SYS_FIELD_STATUS: value->u16 = 0U; break;
		case REG_SYS_FIELD_FAULT_CAUSE: value->u16 = 0U; break;
		case REG_SYS_FIELD_OPERATING_MODE:
			value->u16 = (uint16_t)ctrl->sys_cfg.operating_mode;
			break;
		case REG_SYS_FIELD_STARTUP_CHANNEL_POLICY:
			value->u16 = ctrl->sys_cfg.startup_channel_policy;
			break;
		default:
			k_mutex_unlock(&runtime->lock);
			return REG_NOT_FOUND;
		}
		k_mutex_unlock(&runtime->lock);
		return REG_OK;
	}

	switch (field) {
	case REG_VC_FIELD_STATUS_BITS: value->u16 = ch->status_bits; break;
	case REG_VC_FIELD_ACTIVE_FAULT_CAUSE: value->u16 = ch->active_fault_cause; break;
	case REG_VC_FIELD_FAULT_HISTORY_CAUSE: value->u16 = ch->fault_history_cause; break;
	case REG_VC_FIELD_LAST_PROT_OUT_ACTION:
		value->u16 = (uint16_t)ch->last_protection_output_action; break;
	case REG_VC_FIELD_AUTO_RETRY_COUNT: value->u16 = 0U; break;
	case REG_VC_FIELD_AUTO_COOLDOWN_REMAINING:
		value->u16 = (uint16_t)(ch->cooldown_remaining_ms / 1000U); break;
	case REG_VC_FIELD_LAST_FAULT_TIMESTAMP: value->u32 = ch->last_fault_timestamp; break;
	case REG_VC_FIELD_OPER_TARGET_VOLTAGE:
		value->s16 = ch->operational_target_voltage; break;
	case REG_VC_FIELD_CAPABILITY_FLAGS: value->u16 = ch->capabilities; break;
	case REG_VC_FIELD_MEASURED_VOLTAGE: value->s16 = ch->measured_voltage; break;
	case REG_VC_FIELD_MEASURED_CURRENT: value->s16 = ch->measured_current; break;
	case REG_VC_FIELD_RAW_ADC_VOLTAGE:
	case REG_VC_FIELD_RAW_ADC_CURRENT:
		if (ch->meas != NULL) {
			int32_t raw_voltage, raw_current;
			uint32_t voltage_ts, current_ts;

			vc_channel_buffer_read(ch->meas, &raw_voltage, &voltage_ts,
					       &raw_current, &current_ts);
			ARG_UNUSED(voltage_ts);
			ARG_UNUSED(current_ts);
			if (field == REG_VC_FIELD_RAW_ADC_VOLTAGE) {
				value->s32 = raw_voltage;
			} else {
				value->s32 = raw_current;
			}
		} else if (field == REG_VC_FIELD_RAW_ADC_VOLTAGE) {
			value->s32 = ch->raw_adc_voltage;
		} else {
			value->s32 = ch->raw_adc_current;
		}
		break;
	case REG_VC_FIELD_CFG_TARGET_VOLTAGE:
		value->s16 = ch->config.configured_target_voltage; break;
	case REG_VC_FIELD_RAMP_UP_STEP: value->u16 = ch->config.ramp_up_step; break;
	case REG_VC_FIELD_RAMP_UP_INTERVAL: value->u16 = ch->config.ramp_up_interval; break;
	case REG_VC_FIELD_RAMP_DOWN_STEP: value->u16 = ch->config.ramp_down_step; break;
	case REG_VC_FIELD_RAMP_DOWN_INTERVAL: value->u16 = ch->config.ramp_down_interval; break;
	case REG_VC_FIELD_RECOVERY_POLICY_MODE:
		value->u16 = (uint16_t)ch->config.recovery_policy_mode; break;
	case REG_VC_FIELD_AUTO_RETRY_DELAY: value->u16 = ch->config.auto_retry_delay; break;
	case REG_VC_FIELD_AUTO_RETRY_MAX_COUNT:
		value->u16 = ch->config.auto_retry_max_count; break;
	case REG_VC_FIELD_AUTO_RETRY_WINDOW: value->u16 = ch->config.auto_retry_window; break;
	case REG_VC_FIELD_CURRENT_SAFE_BAND_PCT:
		value->u16 = ch->config.current_safe_band_pct; break;
	case REG_VC_FIELD_CURRENT_PROTECTION_MODE:
		value->u16 = (uint16_t)ch->config.current_protection_mode; break;
	case REG_VC_FIELD_CURRENT_PROT_OUT_ACTION:
		value->u16 = (uint16_t)ch->config.current_protection_output_action; break;
	case REG_VC_FIELD_CURRENT_LIMIT_THRESHOLD:
		value->s16 = ch->config.current_limit_threshold; break;
	case REG_VC_FIELD_AUTO_DERATE_STEP: value->u16 = ch->config.auto_derate_step; break;
	case REG_VC_FIELD_OUTPUT_CAL_K: value->u16 = ch->cal_config.output_calib_k; break;
	case REG_VC_FIELD_OUTPUT_CAL_B: value->s16 = ch->cal_config.output_calib_b; break;
	case REG_VC_FIELD_MEASURED_V_CAL_K:
		value->u16 = ch->cal_config.measured_voltage_calib_k; break;
	case REG_VC_FIELD_MEASURED_V_CAL_B:
		value->s16 = ch->cal_config.measured_voltage_calib_b; break;
	case REG_VC_FIELD_MEASURED_I_CAL_K:
		value->u16 = ch->cal_config.measured_current_calib_k; break;
	case REG_VC_FIELD_MEASURED_I_CAL_B:
		value->s16 = ch->cal_config.measured_current_calib_b; break;
	case REG_VC_FIELD_CAL_OUTPUT_ENABLE: value->u16 = ch->cal_output_enabled; break;
	case REG_VC_FIELD_CAL_DAC_CODE: value->u16 = ch->raw_dac_readback; break;
	case REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT:
		value->u16 = ch->cal_max_raw_dac_limit; break;
	default:
		k_mutex_unlock(&runtime->lock);
		return REG_WRITE_ONLY;
	}

	k_mutex_unlock(&runtime->lock);
	return REG_OK;
}

static bool vc_catalog_field(uint16_t field, enum vc_config_field *out)
{
	switch (field) {
	case REG_VC_FIELD_CFG_TARGET_VOLTAGE: *out = VC_FIELD_CONFIGURED_TARGET_VOLTAGE; break;
	case REG_VC_FIELD_RAMP_UP_STEP: *out = VC_FIELD_RAMP_UP_STEP; break;
	case REG_VC_FIELD_RAMP_UP_INTERVAL: *out = VC_FIELD_RAMP_UP_INTERVAL; break;
	case REG_VC_FIELD_RAMP_DOWN_STEP: *out = VC_FIELD_RAMP_DOWN_STEP; break;
	case REG_VC_FIELD_RAMP_DOWN_INTERVAL: *out = VC_FIELD_RAMP_DOWN_INTERVAL; break;
	case REG_VC_FIELD_RECOVERY_POLICY_MODE: *out = VC_FIELD_RECOVERY_POLICY_MODE; break;
	case REG_VC_FIELD_AUTO_RETRY_DELAY: *out = VC_FIELD_AUTO_RETRY_DELAY; break;
	case REG_VC_FIELD_AUTO_RETRY_MAX_COUNT: *out = VC_FIELD_AUTO_RETRY_MAX_COUNT; break;
	case REG_VC_FIELD_AUTO_RETRY_WINDOW: *out = VC_FIELD_AUTO_RETRY_WINDOW; break;
	case REG_VC_FIELD_CURRENT_SAFE_BAND_PCT: *out = VC_FIELD_CURRENT_SAFE_BAND_PCT; break;
	case REG_VC_FIELD_CURRENT_PROTECTION_MODE: *out = VC_FIELD_CURRENT_PROTECTION_MODE; break;
	case REG_VC_FIELD_CURRENT_PROT_OUT_ACTION: *out = VC_FIELD_CURRENT_PROT_OUT_ACTION; break;
	case REG_VC_FIELD_CURRENT_LIMIT_THRESHOLD: *out = VC_FIELD_CURRENT_LIMIT_THRESHOLD; break;
	case REG_VC_FIELD_AUTO_DERATE_STEP: *out = VC_FIELD_AUTO_DERATE_STEP; break;
	default: return false;
	}
	return true;
}

static bool vc_catalog_cal_field(uint16_t field, enum vc_cal_field *out)
{
	switch (field) {
	case REG_VC_FIELD_OUTPUT_CAL_K: *out = VC_CAL_FIELD_OUTPUT_K; break;
	case REG_VC_FIELD_OUTPUT_CAL_B: *out = VC_CAL_FIELD_OUTPUT_B; break;
	case REG_VC_FIELD_MEASURED_V_CAL_K: *out = VC_CAL_FIELD_MEASURED_V_K; break;
	case REG_VC_FIELD_MEASURED_V_CAL_B: *out = VC_CAL_FIELD_MEASURED_V_B; break;
	case REG_VC_FIELD_MEASURED_I_CAL_K: *out = VC_CAL_FIELD_MEASURED_I_K; break;
	case REG_VC_FIELD_MEASURED_I_CAL_B: *out = VC_CAL_FIELD_MEASURED_I_B; break;
	default: return false;
	}
	return true;
}

static enum reg_status vc_catalog_write(const struct reg_descriptor *desc,
					union reg_value value,
					k_timeout_t timeout)
{
	struct vc_runtime *runtime = catalog_runtime;
	uint16_t field = (uint16_t)REG_ID_FIELD(desc->id);
	uint8_t channel = (uint8_t)REG_ID_INSTANCE(desc->id);
	struct vc_runtime_command cmd = { .channel = channel };
	enum vc_config_field config_field;
	enum vc_cal_field cal_field;

	if (runtime == NULL) {
		return REG_BUSY;
	}
	if (REG_ID_MODULE(desc->id) == REG_MODULE_VOLTAGE_CONTROL) {
		if (channel >= runtime->ctrl->channel_count ||
		    !vc_catalog_supported(field,
			 runtime->ctrl->channels[channel].capabilities)) {
			return REG_UNSUPPORTED;
		}
	}
	if (REG_ID_MODULE(desc->id) == REG_MODULE_SYSTEM) {
		if (field == REG_SYS_FIELD_OPERATING_MODE) {
			return vc_status_to_reg(vc_runtime_set_operating_mode(
				runtime, (enum vc_operating_mode)value.u16, timeout));
		}
		if (field == REG_SYS_FIELD_STARTUP_CHANNEL_POLICY) {
			return vc_status_to_reg(vc_runtime_set_system_field(
				runtime, VC_FIELD_STARTUP_CHANNEL_POLICY, value.u16, timeout));
		}
		if (field == REG_SYS_FIELD_PARAM_ACTION) {
			struct vc_runtime_command cmd = {
				.type = VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
				.payload.param_action = (enum vc_param_action)value.u16,
			};
			return vc_status_to_reg(
				vc_runtime_submit_command(runtime, &cmd, timeout));
		}
		return REG_UNSUPPORTED;
	}

	if (vc_catalog_field(field, &config_field)) {
		return vc_status_to_reg(vc_runtime_set_channel_field(
			runtime, channel, config_field, value.u16, timeout));
	}
	if (vc_catalog_cal_field(field, &cal_field)) {
		return vc_status_to_reg(vc_runtime_set_channel_cal_field(
			runtime, channel, cal_field, value.u16, timeout));
	}

	switch (field) {
	case REG_VC_FIELD_OUTPUT_ACTION:
		cmd.type = VC_RUNTIME_CMD_OUTPUT_ACTION;
		cmd.payload.output_action = (enum vc_output_action)value.u16;
		break;
	case REG_VC_FIELD_FAULT_CMD:
		cmd.type = VC_RUNTIME_CMD_FAULT_COMMAND;
		cmd.payload.fault_command = (enum vc_channel_fault_command)value.u16;
		break;
	case REG_VC_FIELD_PARAM_ACTION:
		cmd.type = VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION;
		cmd.payload.param_action = (enum vc_param_action)value.u16;
		break;
	case REG_VC_FIELD_CAL_OUTPUT_ENABLE:
		if (value.u16 > 1U) {
			return REG_INVALID_VALUE;
		}
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE;
		cmd.payload.calibration_output_enable = value.u16 != 0U;
		break;
	case REG_VC_FIELD_CAL_DAC_CODE:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_RAW_DAC;
		cmd.payload.calibration_raw_dac = value.u16;
		break;
	case REG_VC_FIELD_CAL_SAMPLE_CMD:
		if (value.u16 == CAL_COMMAND_NONE) {
			return REG_OK;
		}
		if (value.u16 != CAL_COMMAND_EXECUTE) {
			return REG_INVALID_VALUE;
		}
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_SAMPLE;
		break;
	case REG_VC_FIELD_CAL_COMMIT_CMD:
		if (value.u16 == CAL_COMMAND_NONE) {
			return REG_OK;
		}
		if (value.u16 != CAL_COMMAND_EXECUTE) {
			return REG_INVALID_VALUE;
		}
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_COMMIT;
		break;
	case REG_VC_FIELD_CAL_MAX_RAW_DAC_LIMIT:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC;
		cmd.payload.calibration_max_raw_dac = value.u16;
		break;
	default:
		return REG_UNSUPPORTED;
	}

	return vc_status_to_reg(vc_runtime_submit_command(runtime, &cmd, timeout));
}

static const struct reg_owner vc_catalog_owner = {
	.read = vc_catalog_read,
	.write = vc_catalog_write,
};

#define VC_DESC_INIT(ch, field_, type_, access_, category_) { \
	.id = REG_VC_ID((ch), REG_VC_FIELD_##field_), \
	.type = REG_##type_, .access = REG_##access_, \
	.category = REG_##category_, .value = NULL, .owner = &vc_catalog_owner, \
}

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)
#define VC_REGS_PER_CHANNEL 41

#define VC_NODE_DESCRIPTOR(node_id, field_, type_, access_, category_) \
	VC_DESC_INIT(DT_REG_ADDR(node_id), field_, type_, access_, category_),
#define VC_REG16(field_, semantic_field_, type_, access_, category_, bank_, offset_) \
	DT_FOREACH_CHILD_STATUS_OKAY_VARGS(VC_CONTROLLER_NODE, VC_NODE_DESCRIPTOR, \
					   field_, type_, access_, category_)
#define VC_REG32 VC_REG16
const STRUCT_SECTION_ITERABLE_ARRAY(reg_descriptor, vc_catalog_channel_regs,
	DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE) * VC_REGS_PER_CHANNEL) = {
#include "reg_store/vc_regs.def"
};
#undef VC_REG16
#undef VC_REG32
#undef VC_NODE_DESCRIPTOR

#define SYS_DESC_INIT(field_, type_, access_, category_) { \
	.id = REG_SYS_ID(REG_SYS_FIELD_##field_), \
	.type = REG_##type_, .access = REG_##access_, \
	.category = REG_##category_, .value = NULL, .owner = &vc_catalog_owner, \
}

const STRUCT_SECTION_ITERABLE_ARRAY(reg_descriptor, vc_catalog_system_regs, 12) = {
	SYS_DESC_INIT(PROTOCOL_MAJOR, U16, RO, FIXED),
	SYS_DESC_INIT(PROTOCOL_MINOR, U16, RO, FIXED),
	SYS_DESC_INIT(VARIANT_ID, U16, RO, FIXED),
	SYS_DESC_INIT(CAPABILITY_FLAGS, U16, RO, FIXED),
	SYS_DESC_INIT(SUPPORTED_CHANNELS, U16, RO, FIXED),
	SYS_DESC_INIT(ACTIVE_CHANNEL_MASK, U16, RO, FIXED),
	SYS_DESC_INIT(ACTIVE_OPERATING_MODE, ENUM, RO, RUNTIME_STATE),
	SYS_DESC_INIT(STATUS, U16, RO, RUNTIME_STATE),
	SYS_DESC_INIT(FAULT_CAUSE, U16, RO, RUNTIME_STATE),
	SYS_DESC_INIT(OPERATING_MODE, ENUM, RW, CONFIG),
	SYS_DESC_INIT(STARTUP_CHANNEL_POLICY, U16, RW, CONFIG),
	SYS_DESC_INIT(PARAM_ACTION, U16, WO, COMMAND),
};

static enum vc_status vc_runtime_dispatch_command(struct vc_runtime *runtime,
						  const struct vc_runtime_command *cmd)
{
	struct vc_controller *ctrl = runtime->ctrl;

	switch (cmd->type) {
	case VC_RUNTIME_CMD_SET_OPERATING_MODE:
		return vc_controller_set_operating_mode(ctrl,
							cmd->payload.operating_mode);
	case VC_RUNTIME_CMD_OUTPUT_ACTION:
		return vc_controller_channel_output_action(ctrl, cmd->channel,
							   cmd->payload.output_action);
	case VC_RUNTIME_CMD_FAULT_COMMAND:
		return vc_controller_channel_fault_command(ctrl, cmd->channel,
							   cmd->payload.fault_command);
	case VC_RUNTIME_CMD_CALIBRATION_UNLOCK:
		return vc_controller_calibration_unlock(ctrl,
						       cmd->payload.calibration_unlock_value);
	case VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE:
		return vc_controller_channel_cal_output_enable(ctrl, cmd->channel,
							      cmd->payload.calibration_output_enable);
	case VC_RUNTIME_CMD_CALIBRATION_RAW_DAC:
		return vc_controller_channel_cal_raw_dac(ctrl, cmd->channel,
							 cmd->payload.calibration_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_SAMPLE:
		return vc_controller_channel_cal_sample(ctrl, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_COMMIT:
		return vc_controller_channel_cal_commit(ctrl, cmd->channel);
	case VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC:
		return vc_controller_channel_cal_max_raw_dac(ctrl, cmd->channel,
							    cmd->payload.calibration_max_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_EXIT:
		return vc_controller_cal_exit(ctrl);
	case VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION:
		return vc_controller_system_param_action(ctrl,
							 cmd->payload.param_action);
	case VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION:
		return vc_controller_channel_param_action(ctrl, cmd->channel,
							  cmd->payload.param_action);
	case VC_RUNTIME_CMD_SET_SYSTEM_FIELD:
		return vc_controller_set_system_field(ctrl,
						     cmd->payload.field_write.field,
						     cmd->payload.field_write.value);
	case VC_RUNTIME_CMD_SET_CHANNEL_FIELD:
		return vc_controller_channel_set_field(ctrl, cmd->channel,
						      cmd->payload.field_write.field,
						      cmd->payload.field_write.value);
	case VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD:
		return vc_controller_channel_set_cal_field(ctrl, cmd->channel,
							   cmd->payload.cal_field_write.field,
							   cmd->payload.cal_field_write.value);
	default:
		return VC_ERR_INVALID_COMMAND;
	}
}

static void runtime_wake(void *user_data)
{
	struct vc_runtime *runtime = user_data;

	k_sem_give(&runtime->wake);
}

static void vc_runtime_worker(void *p1, void *p2, void *p3)
{
	struct vc_runtime *runtime = p1;
	struct vc_runtime_work_item work;
	uint32_t last_tick = k_uptime_get_32();

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!runtime->stop_requested) {
		k_sem_take(&runtime->wake,
			   K_MSEC(CONFIG_VC_RUNTIME_TICK_INTERVAL_MS));

		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;

			k_mutex_lock(&runtime->lock, K_FOREVER);
			result = vc_runtime_dispatch_command(runtime, &work.command);
			k_mutex_unlock(&runtime->lock);

			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		uint32_t now = k_uptime_get_32();
		uint32_t dt_ms = now - last_tick;

		last_tick = now;

		k_mutex_lock(&runtime->lock, K_FOREVER);
		vc_controller_tick(runtime->ctrl, dt_ms);
		k_mutex_unlock(&runtime->lock);

	}
}

static void vc_runtime_auto_load(struct vc_runtime *runtime)
{
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
	struct vc_controller *ctrl = runtime->ctrl;

	vc_controller_set_storage_backend(ctrl, &vc_settings_storage);
	settings_subsys_init();

	/* Phase 1: read system config for startup_channel_policy; do not apply yet
	 * (operating mode side effects — output enable — must fire after channel
	 * configs are populated with correct targets). */
	struct vc_system_config sys_cfg;

	vc_controller_get_system_config(ctrl, &sys_cfg);
	(void)ctrl->storage->load_system_config(&sys_cfg);

	/* Phase 2: load channel op-config per startup policy; cal always from NVS.
	 * FACTORY_RESET op-action already loads cal from NVS internally. */
	size_t count = vc_controller_channel_count(ctrl);
	enum vc_param_action op_action = sys_cfg.startup_channel_policy
					     ? VC_PARAM_ACTION_FACTORY_RESET
					     : VC_PARAM_ACTION_LOAD;

	for (uint8_t ch = 0; ch < count; ch++) {
		(void)vc_controller_channel_param_action(ctrl, ch, op_action);
	}

	/* Phase 3: apply system config — AUTO mode now auto-enables channels
	 * whose targets were populated in phase 2. */
	(void)vc_controller_set_system_config(ctrl, &sys_cfg);
#else
	ARG_UNUSED(runtime);
#endif
}

struct vc_runtime *vc_runtime_create_static(void)
{
	static struct vc_runtime runtime;
	struct vc_controller *ctrl;

	memset(&runtime, 0, sizeof(runtime));
	runtime.stop_requested = false;
	k_mutex_init(&runtime.lock);
	k_sem_init(&runtime.wake, 0, 1);

	ctrl = vc_controller_init(runtime_wake, &runtime);
	if (ctrl == NULL) {
		return NULL;
	}

	runtime.ctrl = ctrl;
	catalog_runtime = &runtime;

	vc_runtime_auto_load(&runtime);
	k_msgq_init(&runtime.command_queue, runtime.command_buffer,
		     sizeof(struct vc_runtime_work_item),
		     CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH);

	(void)k_thread_create(&runtime.thread, vc_runtime_stack,
			       K_KERNEL_STACK_SIZEOF(vc_runtime_stack),
			       vc_runtime_worker, &runtime, NULL, NULL,
			       CONFIG_VC_RUNTIME_THREAD_PRIORITY, 0, K_NO_WAIT);

	return &runtime;
}

void vc_runtime_destroy(struct vc_runtime *runtime)
{
	if (runtime == NULL) {
		return;
	}

	runtime->stop_requested = true;
	k_sem_give(&runtime->wake);
	(void)k_thread_join(&runtime->thread, K_FOREVER);
}

enum vc_status vc_runtime_submit_command(struct vc_runtime *runtime,
					 const struct vc_runtime_command *cmd,
					 k_timeout_t timeout)
{
	struct vc_runtime_work_item work;
	struct k_sem result_sem;
	enum vc_status result = VC_ERR_UNSAFE_STATE;

	if (runtime == NULL || cmd == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	work.command = *cmd;
	work.command.result = &result;
	work.command.result_sem = &result_sem;
	k_sem_init(&result_sem, 0, 1);

	if (k_msgq_put(&runtime->command_queue, &work, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}
	k_sem_give(&runtime->wake);

	/* Once queued, the worker owns pointers into this stack frame. Waiting
	 * forever is required to keep them valid; timeout only bounds admission. */
	(void)k_sem_take(&result_sem, K_FOREVER);

	return result;
}

enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
					     enum vc_operating_mode mode,
					     k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_OPERATING_MODE,
		.payload.operating_mode = mode,
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_set_system_field(struct vc_runtime *runtime,
					   enum vc_config_field field,
					   uint16_t value,
					   k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
		.payload.field_write = { .field = field, .value = value },
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_set_channel_field(struct vc_runtime *runtime,
					    uint8_t channel,
					    enum vc_config_field field,
					    uint16_t value,
					    k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
		.channel = channel,
		.payload.field_write = { .field = field, .value = value },
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

enum vc_status vc_runtime_start_sampling(struct vc_runtime *runtime)
{
	if (runtime == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	return vc_controller_start_sampling(runtime->ctrl);
}

enum vc_status vc_runtime_set_channel_cal_field(struct vc_runtime *runtime,
						uint8_t channel,
						enum vc_cal_field field,
						uint16_t value,
						k_timeout_t timeout)
{
	struct vc_runtime_command cmd = {
		.type = VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
		.channel = channel,
		.payload.cal_field_write = { .field = field, .value = value },
	};

	return vc_runtime_submit_command(runtime, &cmd, timeout);
}

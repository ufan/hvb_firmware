/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/domain.h"
#include "voltage_control/variant.h"
#include <stdlib.h>
#include <string.h>

struct vc_domain {
	const struct vc_variant_profile *variant;
	enum vc_operating_mode operating_mode;
	uint32_t uptime_seconds;
	struct vc_system_config sys_cfg;
	struct vc_channel_config channels[VC_MAX_CHANNELS];
	struct vc_channel_snapshot snapshots[VC_MAX_CHANNELS];
};

static bool channel_valid(const struct vc_domain *domain, uint8_t channel)
{
	return channel < domain->variant->num_channels;
}

static bool is_valid_operating_mode(enum vc_operating_mode mode)
{
	return mode == VC_OPERATING_MODE_NORMAL ||
	       mode == VC_OPERATING_MODE_AUTOMATIC;
}

static bool is_valid_recovery_policy(enum vc_recovery_policy_mode mode)
{
	return mode <= VC_RECOVERY_NEVER_RETRY;
}

static bool is_valid_protection_mode(enum vc_protection_mode mode)
{
	return mode <= VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
}

static bool is_valid_protection_output_action(enum vc_output_action action,
					      bool is_voltage)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		return true;
	case VC_OUTPUT_ACTION_CLAMP:
		return is_voltage;
	default:
		return false;
	}
}

static bool is_valid_host_output_action(enum vc_output_action action)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_ENABLE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		return true;
	default:
		return false;
	}
}

static bool is_valid_channel_fault_command(enum vc_channel_fault_command cmd)
{
	return cmd >= VC_CHANNEL_FAULT_COMMAND_NONE &&
	       cmd <= VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY;
}

static void __unused channel_config_defaults(struct vc_channel_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->output_calib_k = 10000;
	cfg->measured_voltage_calib_k = 10000;
	cfg->measured_current_calib_k = 10000;
}

static void __unused system_config_defaults(struct vc_system_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->operating_mode = VC_OPERATING_MODE_NORMAL;
	cfg->slave_address = 1;
	cfg->baud_rate_code = VC_BAUD_RATE_115200;
	cfg->recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH;
	cfg->voltage_safe_band_pct = 10;
	cfg->current_safe_band_pct = 10;
}

struct vc_domain *vc_domain_create(const struct vc_variant_profile *variant)
{
	struct vc_domain *domain;

	if (!variant) {
		return NULL;
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		return NULL;
	}

	domain->variant = variant;
	domain->operating_mode = variant->default_system_config.operating_mode;

	memcpy(&domain->sys_cfg, &variant->default_system_config,
	       sizeof(domain->sys_cfg));

	for (int i = 0; i < variant->num_channels && i < VC_MAX_CHANNELS; i++) {
		memcpy(&domain->channels[i], &variant->default_channel_config,
		       sizeof(domain->channels[i]));
	}

	return domain;
}

enum vc_operating_mode vc_domain_get_operating_mode(const struct vc_domain *domain)
{
	return domain->operating_mode;
}

enum vc_status vc_domain_set_operating_mode(struct vc_domain *domain,
					    enum vc_operating_mode mode)
{
	if (!is_valid_operating_mode(mode)) {
		return VC_ERR_INVALID_VALUE;
	}

	domain->operating_mode = mode;
	domain->sys_cfg.operating_mode = mode;
	return VC_OK;
}

enum vc_status vc_domain_get_system_config(const struct vc_domain *domain,
					   struct vc_system_config *cfg)
{
	memcpy(cfg, &domain->sys_cfg, sizeof(*cfg));
	cfg->operating_mode = domain->operating_mode;
	return VC_OK;
}

enum vc_status vc_domain_set_system_config(struct vc_domain *domain,
					   const struct vc_system_config *cfg)
{
	if (!is_valid_operating_mode(cfg->operating_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->slave_address > 247) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->baud_rate_code > VC_BAUD_RATE_9600) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_recovery_policy(cfg->recovery_policy_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}

	memcpy(&domain->sys_cfg, cfg, sizeof(*cfg));
	domain->operating_mode = cfg->operating_mode;
	return VC_OK;
}

enum vc_status vc_domain_get_channel_config(const struct vc_domain *domain,
					    uint8_t channel,
					    struct vc_channel_config *cfg)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	memcpy(cfg, &domain->channels[channel], sizeof(*cfg));
	return VC_OK;
}

enum vc_status vc_domain_set_channel_config(struct vc_domain *domain,
					    uint8_t channel,
					    const struct vc_channel_config *cfg)
{
	int16_t max_v, min_v;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	max_v = domain->variant->max_voltage_raw;
	min_v = domain->variant->min_voltage_raw;

	if (cfg->configured_target_voltage > max_v ||
	    cfg->configured_target_voltage < min_v) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_limit_threshold > max_v ||
	    cfg->voltage_limit_threshold < min_v) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_limit_threshold < 0) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->voltage_protection_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_output_action(cfg->voltage_protection_output_action,
					       true)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->current_protection_mode)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_output_action(cfg->current_protection_output_action,
					       false)) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->save_target_policy > 1) {
		return VC_ERR_INVALID_VALUE;
	}

	memcpy(&domain->channels[channel], cfg, sizeof(*cfg));
	return VC_OK;
}

enum vc_status vc_domain_get_system_snapshot(const struct vc_domain *domain,
					     struct vc_system_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->protocol_major = 2;
	snap->protocol_minor = 0;
	snap->variant_id = domain->variant->variant_id;
	snap->system_capability_flags = domain->variant->system_capability_flags;
	snap->supported_channel_count = domain->variant->num_channels;
	snap->active_channel_mask = domain->variant->channel_mask;
	snap->uptime = domain->uptime_seconds;
	snap->active_operating_mode = domain->operating_mode;
	return VC_OK;
}

enum vc_status vc_domain_get_channel_snapshot(const struct vc_domain *domain,
					      uint8_t channel,
					      struct vc_channel_snapshot *snap)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	memcpy(snap, &domain->snapshots[channel], sizeof(*snap));
	snap->channel_capability_flags = domain->variant->channel_capability_flags;
	return VC_OK;
}

enum vc_status vc_domain_channel_output_action(struct vc_domain *domain,
					       uint8_t channel,
					       enum vc_output_action action)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!is_valid_host_output_action(action)) {
		return VC_ERR_INVALID_COMMAND;
	}
	return VC_OK;
}

enum vc_status vc_domain_channel_fault_command(struct vc_domain *domain,
					       uint8_t channel,
					       enum vc_channel_fault_command cmd)
{
	struct vc_channel_snapshot *snap;

	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (!is_valid_channel_fault_command(cmd)) {
		return VC_ERR_INVALID_COMMAND;
	}

	snap = &domain->snapshots[channel];

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (snap->active_fault_cause != 0) {
			return VC_ERR_UNSAFE_STATE;
		}
		break;
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY:
		snap->fault_history_cause = 0;
		snap->auto_retry_count = 0;
		break;
	default:
		break;
	}

	return VC_OK;
}

enum vc_status vc_domain_system_param_action(struct vc_domain *domain,
					     enum vc_param_action action)
{
	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
	case VC_PARAM_ACTION_LOAD:
	case VC_PARAM_ACTION_FACTORY_RESET:
		return VC_ERR_STORAGE;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
		return VC_ERR_STORAGE;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

enum vc_status vc_domain_channel_param_action(struct vc_domain *domain,
					      uint8_t channel,
					      enum vc_param_action action)
{
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
	case VC_PARAM_ACTION_LOAD:
	case VC_PARAM_ACTION_FACTORY_RESET:
		return VC_ERR_STORAGE;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
		return VC_ERR_STORAGE;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

bool vc_domain_is_channel_supported(const struct vc_domain *domain,
				    uint8_t channel)
{
	return channel_valid(domain, channel);
}

uint16_t vc_domain_get_supported_channel_count(const struct vc_domain *domain)
{
	return domain->variant->num_channels;
}

uint16_t vc_domain_get_active_channel_mask(const struct vc_domain *domain)
{
	return domain->variant->channel_mask;
}

uint16_t vc_domain_get_variant_id(const struct vc_domain *domain)
{
	return domain->variant->variant_id;
}

void vc_domain_set_uptime(struct vc_domain *domain, uint32_t seconds)
{
	domain->uptime_seconds = seconds;
}

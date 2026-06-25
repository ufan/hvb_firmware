/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "shell_adapter/shell_adapter.h"
#include "voltage_control/vc.h"
#include <dt-bindings/voltage_control/capabilities.h>
#include "regmap/vc_regs.h"

#define SHELL_CMD_TIMEOUT K_SECONDS(1)

static struct vc_ctx *ctx;

void vc_shell_init(struct vc_ctx *c)
{
	ctx = c;
}

#define CTX_CHECK(sh) do { \
	if (!ctx) { \
		shell_error(sh, "vc not initialized"); \
		return -ENODEV; \
	} \
} while (0)

/* ------------------------------------------------------------------ */
/* String conversion helpers                                           */
/* ------------------------------------------------------------------ */

static const char *mode_str(enum vc_operating_mode m)
{
	switch (m) {
	case VC_OPERATING_MODE_NORMAL:      return "NORMAL";
	case VC_OPERATING_MODE_AUTOMATIC:   return "AUTO";
	case VC_OPERATING_MODE_CALIBRATION: return "CAL";
	default:                            return "?";
	}
}

static const char *recovery_str(enum vc_recovery_policy_mode m)
{
	switch (m) {
	case VC_RECOVERY_MANUAL_LATCH:       return "MANUAL";
	case VC_RECOVERY_AUTO_RETRY:         return "AUTO_RETRY";
	case VC_RECOVERY_AUTO_DERATE_RETRY:  return "DERATE_RETRY";
	case VC_RECOVERY_NEVER_RETRY:        return "NEVER";
	default:                             return "?";
	}
}

static const char *prot_str(enum vc_protection_mode m)
{
	switch (m) {
	case VC_PROTECTION_MODE_DISABLED:            return "OFF";
	case VC_PROTECTION_MODE_FLAG_ONLY:           return "FLAG";
	case VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION: return "APPLY";
	default:                                     return "?";
	}
}

static const char *action_str(enum vc_output_action a)
{
	switch (a) {
	case VC_OUTPUT_ACTION_NONE:              return "NONE";
	case VC_OUTPUT_ACTION_ENABLE:            return "ENABLE";
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:  return "GRACEFUL";
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE: return "IMMEDIATE";
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:  return "FORCE_ZERO";
	default:                                 return "?";
	}
}

static void fault_str(uint16_t cause, char *buf, size_t len)
{
	if (cause == 0) {
		strncpy(buf, "none", len);
		return;
	}
	buf[0] = '\0';
	if (cause & VC_FAULT_CURRENT)       strncat(buf, "I,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_MEASUREMENT)   strncat(buf, "MEAS,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_HARDWARE)      strncat(buf, "HW,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_INTERLOCK)     strncat(buf, "LOCK,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_RETRY_EXHAUST) strncat(buf, "RETRY,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_CFG_INVALID)   strncat(buf, "CFG,", len - strlen(buf) - 1);
	if (cause & VC_FAULT_STALE)         strncat(buf, "STALE,", len - strlen(buf) - 1);
	size_t l = strlen(buf);
	if (l > 0 && buf[l - 1] == ',') {
		buf[l - 1] = '\0';
	}
}

static void caps_str(uint16_t caps, char *buf, size_t len)
{
	buf[0] = '\0';
	if (caps & CH_CAP_OUTPUT_ENABLE)       strncat(buf, "O,", len - strlen(buf) - 1);
	if (caps & CH_CAP_RAW_OUTPUT_DRIVE)    strncat(buf, "R,", len - strlen(buf) - 1);
	if (caps & CH_CAP_VOLTAGE_MEASUREMENT) strncat(buf, "V,", len - strlen(buf) - 1);
	if (caps & CH_CAP_CURRENT_MEASUREMENT) strncat(buf, "I,", len - strlen(buf) - 1);
	if (caps & CH_CAP_HARDWARE_STATUS)     strncat(buf, "H,", len - strlen(buf) - 1);
	size_t l = strlen(buf);
	if (l > 0 && buf[l - 1] == ',') {
		buf[l - 1] = '\0';
	}
}

/* ------------------------------------------------------------------ */
/* String parse helpers                                                */
/* ------------------------------------------------------------------ */

static int parse_mode(const char *s, enum vc_operating_mode *out)
{
	if (strcmp(s, "normal") == 0) { *out = VC_OPERATING_MODE_NORMAL; return 0; }
	if (strcmp(s, "auto") == 0)   { *out = VC_OPERATING_MODE_AUTOMATIC; return 0; }
	if (strcmp(s, "cal") == 0)    { *out = VC_OPERATING_MODE_CALIBRATION; return 0; }
	return -EINVAL;
}

static int parse_param(const char *s, enum vc_param_action *out)
{
	if (strcmp(s, "save") == 0)  { *out = VC_PARAM_ACTION_SAVE; return 0; }
	if (strcmp(s, "load") == 0)  { *out = VC_PARAM_ACTION_LOAD; return 0; }
	if (strcmp(s, "reset") == 0) { *out = VC_PARAM_ACTION_FACTORY_RESET; return 0; }
	return -EINVAL;
}

static int parse_fault_cmd(const char *s, enum vc_channel_fault_command *out)
{
	if (strcmp(s, "clear") == 0)         { *out = VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE; return 0; }
	if (strcmp(s, "clear-history") == 0) { *out = VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY; return 0; }
	return -EINVAL;
}

static int parse_channel(const struct shell *sh, const char *s, uint8_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 10);

	if (*end != '\0') {
		shell_error(sh, "invalid channel: %s", s);
		return -EINVAL;
	}

	struct vc_system_snapshot sys;

	vc_query(ctx, vc_q_system_snapshot(&sys));
	if (v >= sys.supported_channel_count) {
		shell_error(sh, "channel %lu out of range (0..%d)",
			    v, sys.supported_channel_count - 1);
		return -EINVAL;
	}
	*out = (uint8_t)v;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Field name tables                                                   */
/* ------------------------------------------------------------------ */

struct field_entry {
	const char *name;
	enum vc_config_field field;
};

static const struct field_entry sys_fields[] = {
	{"mode",         VC_FIELD_OPERATING_MODE},
	{"recovery",     VC_FIELD_RECOVERY_POLICY_MODE},
	{"retry_delay",  VC_FIELD_AUTO_RETRY_DELAY},
	{"retry_max",    VC_FIELD_AUTO_RETRY_MAX_COUNT},
	{"retry_window", VC_FIELD_AUTO_RETRY_WINDOW},
	{"safe_band",    VC_FIELD_CURRENT_SAFE_BAND_PCT},
};

static const struct field_entry ch_fields[] = {
	{"target",       VC_FIELD_CONFIGURED_TARGET_VOLTAGE},
	{"ramp_up_step", VC_FIELD_RAMP_UP_STEP},
	{"ramp_up_int",  VC_FIELD_RAMP_UP_INTERVAL},
	{"ramp_dn_step", VC_FIELD_RAMP_DOWN_STEP},
	{"ramp_dn_int",  VC_FIELD_RAMP_DOWN_INTERVAL},
	{"prot_mode",    VC_FIELD_CURRENT_PROTECTION_MODE},
	{"prot_action",  VC_FIELD_CURRENT_PROT_OUT_ACTION},
	{"i_limit",      VC_FIELD_CURRENT_LIMIT_THRESHOLD},
	{"derate_step",  VC_FIELD_AUTO_DERATE_STEP},
	{"save_policy",  VC_FIELD_SAVE_TARGET_POLICY},
	{"out_cal_k",    VC_FIELD_OUTPUT_CAL_K},
	{"out_cal_b",    VC_FIELD_OUTPUT_CAL_B},
	{"v_cal_k",      VC_FIELD_MEASURED_V_CAL_K},
	{"v_cal_b",      VC_FIELD_MEASURED_V_CAL_B},
	{"i_cal_k",      VC_FIELD_MEASURED_I_CAL_K},
	{"i_cal_b",      VC_FIELD_MEASURED_I_CAL_B},
};

static int lookup_field(const struct shell *sh, const char *name,
			const struct field_entry *table, size_t count,
			enum vc_config_field *out)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(name, table[i].name) == 0) {
			*out = table[i].field;
			return 0;
		}
	}
	shell_error(sh, "unknown field: %s", name);
	shell_fprintf(sh, SHELL_NORMAL, "available:");
	for (size_t i = 0; i < count; i++) {
		shell_fprintf(sh, SHELL_NORMAL, " %s", table[i].name);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Dispatch helper                                                     */
/* ------------------------------------------------------------------ */

static int dispatch(const struct shell *sh, struct vc_cmd cmd)
{
	enum vc_status st = vc_dispatch(ctx, cmd, SHELL_CMD_TIMEOUT);

	if (st != VC_OK) {
		shell_error(sh, "error: %d", st);
		return -EIO;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Output formatting                                                   */
/* ------------------------------------------------------------------ */

static void print_sys_oneliner(const struct shell *sh,
			       const struct vc_system_snapshot *s)
{
	char fbuf[48];

	fault_str(s->system_fault_cause, fbuf, sizeof(fbuf));
	shell_print(sh, "SYS: mode=%-6s channels=%d active=0x%04x fault=%s",
		    mode_str(s->active_operating_mode),
		    s->supported_channel_count,
		    s->active_channel_mask, fbuf);
}

static void print_ch_oneliner(const struct shell *sh, uint8_t ch,
			      const struct vc_channel_snapshot *s)
{
	char fbuf[48];

	fault_str(s->active_fault_cause, fbuf, sizeof(fbuf));

	bool has_v = s->channel_capability_flags & CH_CAP_VOLTAGE_MEASUREMENT;
	bool has_i = s->channel_capability_flags & CH_CAP_CURRENT_MEASUREMENT;

	shell_fprintf(sh, SHELL_NORMAL, "CH%d:", ch);
	if (has_v) {
		shell_fprintf(sh, SHELL_NORMAL, " V=%d", s->measured_voltage);
	}
	if (has_i) {
		shell_fprintf(sh, SHELL_NORMAL, " I=%d", s->measured_current);
	}
	shell_fprintf(sh, SHELL_NORMAL, " target=%d fault=%s\n",
		      s->operational_target_voltage, fbuf);
}

static void print_ch_config(const struct shell *sh, uint8_t ch,
			    const struct vc_channel_config *c)
{
	shell_print(sh, "Channel %d Configuration", ch);
	shell_print(sh, "  target:       %d", c->configured_target_voltage);
	shell_print(sh, "  ramp_up:      step=%d interval=%d",
		    c->ramp_up_step, c->ramp_up_interval);
	shell_print(sh, "  ramp_down:    step=%d interval=%d",
		    c->ramp_down_step, c->ramp_down_interval);
	shell_print(sh, "  protection:   mode=%s action=%s limit=%d",
		    prot_str(c->current_protection_mode),
		    action_str(c->current_protection_output_action),
		    c->current_limit_threshold);
	shell_print(sh, "  derate_step:  %d", c->auto_derate_step);
	shell_print(sh, "  save_policy:  %d", c->save_target_policy);
	shell_print(sh, "  out_cal:      k=%d b=%d",
		    c->output_calib_k, c->output_calib_b);
	shell_print(sh, "  v_cal:        k=%d b=%d",
		    c->measured_voltage_calib_k, c->measured_voltage_calib_b);
	shell_print(sh, "  i_cal:        k=%d b=%d",
		    c->measured_current_calib_k, c->measured_current_calib_b);
}

static void print_sys_config(const struct shell *sh,
			     const struct vc_system_config *c)
{
	shell_print(sh, "System Configuration");
	shell_print(sh, "  mode:         %s", mode_str(c->operating_mode));
	shell_print(sh, "  recovery:     %s", recovery_str(c->recovery_policy_mode));
	shell_print(sh, "  retry:        delay=%d max=%d window=%d",
		    c->auto_retry_delay, c->auto_retry_max_count,
		    c->auto_retry_window);
	shell_print(sh, "  safe_band:    %d%%", c->current_safe_band_pct);
}

/* ------------------------------------------------------------------ */
/* Top-level commands                                                  */
/* ------------------------------------------------------------------ */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	struct vc_system_snapshot sys;

	vc_query(ctx, vc_q_system_snapshot(&sys));
	print_sys_oneliner(sh, &sys);

	for (uint8_t i = 0; i < sys.supported_channel_count; i++) {
		struct vc_channel_snapshot snap;

		vc_query(ctx, vc_q_channel_snapshot(i, &snap));
		print_ch_oneliner(sh, i, &snap);
	}
	return 0;
}

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	enum vc_operating_mode m;

	if (parse_mode(argv[1], &m) < 0) {
		shell_error(sh, "usage: vc mode <normal|auto|cal>");
		return -EINVAL;
	}
	return dispatch(sh, vc_cmd_set_mode(m));
}

static int cmd_param(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	enum vc_param_action action;

	if (parse_param(argv[1], &action) < 0) {
		shell_error(sh, "usage: vc param <save|load|reset>");
		return -EINVAL;
	}

	int ret = dispatch(sh, vc_cmd_sys_param(action));

	if (ret) {
		return ret;
	}

	struct vc_system_snapshot sys;

	vc_query(ctx, vc_q_system_snapshot(&sys));
	for (uint8_t i = 0; i < sys.supported_channel_count; i++) {
		ret = dispatch(sh, vc_cmd_ch_param(i, action));
		if (ret) {
			return ret;
		}
	}

	shell_print(sh, "OK");
	return 0;
}

/* ------------------------------------------------------------------ */
/* System subcommands                                                  */
/* ------------------------------------------------------------------ */

static int cmd_sys_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	struct vc_system_snapshot sys;

	vc_query(ctx, vc_q_system_snapshot(&sys));

	char fbuf[48];

	fault_str(sys.system_fault_cause, fbuf, sizeof(fbuf));
	shell_print(sh, "Protocol:    %d.%d", sys.protocol_major, sys.protocol_minor);
	shell_print(sh, "Variant:     %d", sys.variant_id);
	shell_print(sh, "Caps:        0x%04x", sys.system_capability_flags);
	shell_print(sh, "Channels:    %d", sys.supported_channel_count);
	shell_print(sh, "Active:      0x%04x", sys.active_channel_mask);
	shell_print(sh, "Mode:        %s", mode_str(sys.active_operating_mode));
	shell_print(sh, "Status:      0x%04x", sys.system_status);
	shell_print(sh, "Fault:       %s", fbuf);
	return 0;
}

static int cmd_sys_config(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	struct vc_system_config cfg;

	vc_query(ctx, vc_q_system_config(&cfg));
	print_sys_config(sh, &cfg);
	return 0;
}

static int cmd_sys_param(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	enum vc_param_action action;

	if (parse_param(argv[1], &action) < 0) {
		shell_error(sh, "usage: vc sys param <save|load|reset>");
		return -EINVAL;
	}
	int ret = dispatch(sh, vc_cmd_sys_param(action));

	if (ret == 0) {
		shell_print(sh, "OK");
	}
	return ret;
}

static int cmd_sys_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	enum vc_config_field field;

	if (lookup_field(sh, argv[1], sys_fields, ARRAY_SIZE(sys_fields),
			 &field) < 0) {
		return -EINVAL;
	}
	uint16_t value = (uint16_t)strtoul(argv[2], NULL, 0);

	return dispatch(sh, vc_cmd_sys_field(field, value));
}

/* ------------------------------------------------------------------ */
/* Channel subcommand dispatcher                                       */
/* ------------------------------------------------------------------ */

static int cmd_ch_status(const struct shell *sh, uint8_t ch)
{
	struct vc_channel_snapshot snap;

	vc_query(ctx, vc_q_channel_snapshot(ch, &snap));

	char fbuf[48], cbuf[24];

	fault_str(snap.active_fault_cause, fbuf, sizeof(fbuf));
	caps_str(snap.channel_capability_flags, cbuf, sizeof(cbuf));

	bool has_v = snap.channel_capability_flags & CH_CAP_VOLTAGE_MEASUREMENT;
	bool has_i = snap.channel_capability_flags & CH_CAP_CURRENT_MEASUREMENT;
	bool enabled = snap.status_bits & 0x0002;

	shell_print(sh, "Channel %d Status", ch);
	shell_print(sh, "  caps:         [%s]", cbuf);
	shell_print(sh, "  enabled:      %s", enabled ? "yes" : "no");
	shell_print(sh, "  target:       %d", snap.operational_target_voltage);
	if (has_v) {
		shell_print(sh, "  voltage:      %d", snap.measured_voltage);
	}
	if (has_i) {
		shell_print(sh, "  current:      %d", snap.measured_current);
	}
	shell_print(sh, "  status_bits:  0x%04x", snap.status_bits);
	shell_print(sh, "  fault:        %s", fbuf);
	fault_str(snap.fault_history_cause, fbuf, sizeof(fbuf));
	shell_print(sh, "  fault_hist:   %s", fbuf);
	return 0;
}

static int cmd_ch(const struct shell *sh, size_t argc, char **argv)
{
	CTX_CHECK(sh);

	if (argc < 3) {
		shell_error(sh, "usage: vc ch <n> <status|config|enable|disable|target|set|fault|param>");
		return -EINVAL;
	}

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}

	const char *sub = argv[2];

	if (strcmp(sub, "status") == 0) {
		return cmd_ch_status(sh, ch);
	}

	if (strcmp(sub, "config") == 0) {
		struct vc_channel_config cfg;

		vc_query(ctx, vc_q_channel_config(ch, &cfg));
		print_ch_config(sh, ch, &cfg);
		return 0;
	}

	if (strcmp(sub, "enable") == 0) {
		return dispatch(sh, vc_cmd_output(ch, VC_OUTPUT_ACTION_ENABLE));
	}

	if (strcmp(sub, "disable") == 0) {
		enum vc_output_action action = VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;

		if (argc > 3 && strcmp(argv[3], "graceful") == 0) {
			action = VC_OUTPUT_ACTION_DISABLE_GRACEFUL;
		}
		return dispatch(sh, vc_cmd_output(ch, action));
	}

	if (strcmp(sub, "target") == 0) {
		if (argc < 4) {
			shell_error(sh, "usage: vc ch %d target <voltage>", ch);
			return -EINVAL;
		}
		uint16_t v = (uint16_t)strtoul(argv[3], NULL, 0);

		return dispatch(sh,
				vc_cmd_ch_field(ch,
						VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
						v));
	}

	if (strcmp(sub, "set") == 0) {
		if (argc < 5) {
			shell_error(sh, "usage: vc ch %d set <field> <value>", ch);
			return -EINVAL;
		}
		enum vc_config_field field;

		if (lookup_field(sh, argv[3], ch_fields,
				 ARRAY_SIZE(ch_fields), &field) < 0) {
			return -EINVAL;
		}
		uint16_t value = (uint16_t)strtoul(argv[4], NULL, 0);

		return dispatch(sh, vc_cmd_ch_field(ch, field, value));
	}

	if (strcmp(sub, "fault") == 0) {
		if (argc < 4) {
			shell_error(sh, "usage: vc ch %d fault <clear|clear-history>", ch);
			return -EINVAL;
		}
		enum vc_channel_fault_command cmd;

		if (parse_fault_cmd(argv[3], &cmd) < 0) {
			shell_error(sh, "expected: clear, clear-history");
			return -EINVAL;
		}
		return dispatch(sh, vc_cmd_fault(ch, cmd));
	}

	if (strcmp(sub, "param") == 0) {
		if (argc < 4) {
			shell_error(sh, "usage: vc ch %d param <save|load|reset>", ch);
			return -EINVAL;
		}
		enum vc_param_action action;

		if (parse_param(argv[3], &action) < 0) {
			shell_error(sh, "expected: save, load, reset");
			return -EINVAL;
		}
		int ret = dispatch(sh, vc_cmd_ch_param(ch, action));

		if (ret == 0) {
			shell_print(sh, "OK");
		}
		return ret;
	}

	shell_error(sh, "unknown: vc ch %d %s", ch, sub);
	shell_print(sh, "subcommands: status config enable disable target set fault param");
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Calibration subcommands                                             */
/* ------------------------------------------------------------------ */

static int cmd_cal_unlock(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	CTX_CHECK(sh);

	int ret = dispatch(sh, vc_cmd_cal_unlock(CAL_UNLOCK_STEP1));

	if (ret) {
		return ret;
	}
	ret = dispatch(sh, vc_cmd_cal_unlock(CAL_UNLOCK_STEP2));
	if (ret == 0) {
		shell_print(sh, "calibration unlocked");
	}
	return ret;
}

static int cmd_cal_output(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}

	bool enable;

	if (strcmp(argv[2], "on") == 0) {
		enable = true;
	} else if (strcmp(argv[2], "off") == 0) {
		enable = false;
	} else {
		shell_error(sh, "expected: on, off");
		return -EINVAL;
	}
	return dispatch(sh, vc_cmd_cal_output(ch, enable));
}

static int cmd_cal_dac(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}
	uint16_t code = (uint16_t)strtoul(argv[2], NULL, 0);

	return dispatch(sh, vc_cmd_cal_dac(ch, code));
}

static int cmd_cal_sample(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}
	return dispatch(sh, vc_cmd_cal_sample(ch));
}

static int cmd_cal_commit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}
	return dispatch(sh, vc_cmd_cal_commit(ch));
}

static int cmd_cal_max_dac(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	CTX_CHECK(sh);

	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) < 0) {
		return -EINVAL;
	}
	uint16_t limit = (uint16_t)strtoul(argv[2], NULL, 0);

	return dispatch(sh, vc_cmd_cal_max_dac(ch, limit));
}

/* ------------------------------------------------------------------ */
/* Watch mode                                                          */
/* ------------------------------------------------------------------ */

static bool watch_has_key(const struct shell *sh)
{
	uint8_t c;
	size_t cnt = 0;

	sh->iface->api->read(sh->iface, &c, 1, &cnt);
	return cnt > 0;
}

static int cmd_watch(const struct shell *sh, size_t argc, char **argv)
{
	CTX_CHECK(sh);

	struct vc_system_snapshot sys;

	vc_query(ctx, vc_q_system_snapshot(&sys));

	int8_t watch_ch = -1;
	int interval_ms = CONFIG_VC_SHELL_WATCH_DEFAULT_INTERVAL_MS;

	if (argc >= 2) {
		unsigned long v = strtoul(argv[1], NULL, 10);

		if (v < sys.supported_channel_count) {
			watch_ch = (int8_t)v;
			if (argc >= 3) {
				interval_ms = (int)strtoul(argv[2], NULL, 10);
			}
		} else {
			interval_ms = (int)v;
		}
	}

	if (interval_ms < 100) {
		interval_ms = 100;
	}

	shell_print(sh, "watching%s interval=%dms (press any key to stop)",
		    watch_ch >= 0 ? "" : " all", interval_ms);

	while (true) {
		if (watch_ch >= 0) {
			struct vc_channel_snapshot snap;

			vc_query(ctx, vc_q_channel_snapshot(watch_ch, &snap));
			print_ch_oneliner(sh, watch_ch, &snap);
		} else {
			for (uint8_t i = 0; i < sys.supported_channel_count; i++) {
				struct vc_channel_snapshot snap;

				vc_query(ctx, vc_q_channel_snapshot(i, &snap));
				if (snap.channel_capability_flags &
				    (CH_CAP_VOLTAGE_MEASUREMENT |
				     CH_CAP_CURRENT_MEASUREMENT)) {
					print_ch_oneliner(sh, i, &snap);
				}
			}
		}

		for (int i = 0; i < interval_ms / 50; i++) {
			k_msleep(50);
			if (watch_has_key(sh)) {
				shell_print(sh, "stopped");
				return 0;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Shell command registration                                          */
/* ------------------------------------------------------------------ */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_vc_cal,
	SHELL_CMD(unlock, NULL, "2-step calibration unlock", cmd_cal_unlock),
	SHELL_CMD_ARG(output, NULL, "Cal output <ch> <on|off>", cmd_cal_output, 3, 0),
	SHELL_CMD_ARG(dac, NULL, "Raw DAC write <ch> <code>", cmd_cal_dac, 3, 0),
	SHELL_CMD_ARG(sample, NULL, "Trigger ADC sample <ch>", cmd_cal_sample, 2, 0),
	SHELL_CMD_ARG(commit, NULL, "Commit calibration <ch>", cmd_cal_commit, 2, 0),
	SHELL_CMD_ARG(max_dac, NULL, "Set max DAC limit <ch> <limit>", cmd_cal_max_dac, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_vc_sys,
	SHELL_CMD(status, NULL, "Detailed system snapshot", cmd_sys_status),
	SHELL_CMD(config, NULL, "System configuration", cmd_sys_config),
	SHELL_CMD_ARG(param, NULL, "System param <save|load|reset>", cmd_sys_param, 2, 0),
	SHELL_CMD_ARG(set, NULL, "Set system field <field> <value>", cmd_sys_set, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_vc,
	SHELL_CMD(status, NULL, "System + channel snapshot", cmd_status),
	SHELL_CMD_ARG(mode, NULL, "Set mode <normal|auto|cal>", cmd_mode, 2, 0),
	SHELL_CMD_ARG(param, NULL, "All param <save|load|reset>", cmd_param, 2, 0),
	SHELL_CMD(sys, &sub_vc_sys, "System commands", NULL),
	SHELL_CMD_ARG(ch, NULL, "Channel <n> <subcmd> [args]", cmd_ch, 3, 2),
	SHELL_CMD(cal, &sub_vc_cal, "Calibration commands", NULL),
	SHELL_CMD_ARG(watch, NULL, "Monitor [ch] [interval_ms]", cmd_watch, 1, 2),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(vc, &sub_vc, "Voltage control", NULL);

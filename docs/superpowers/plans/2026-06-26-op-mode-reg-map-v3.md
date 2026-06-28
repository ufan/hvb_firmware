# Operating Mode + Register Map v3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement system operating-mode side effects, separate calibration config from operational config, move recovery policy to channel level, and bump the protocol to v3 — fixing five unimplemented features in one clean refactor.

**Architecture:** The change flows top-down through eight layers: register map → types + storage interface → channel → controller → runtime/vc → shell → modbus → tests. Each layer must compile before the next is touched. The `vc_types.h` + `vc_channel.h/c` changes are the largest because cal k/b fields leave `vc_channel_config` entirely and become `vc_channel_cal_config`.

**Tech Stack:** Zephyr RTOS, C11, native-sim for unit tests (`west build -t run`).

## Global Constraints

- All file paths are relative to repo root `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git`
- Do NOT work on `main` directly — create branch `feat/op-mode-v3` first
- Protocol major version must become **3**, minor **0**
- `SYS_STARTUP_CHANNEL_POLICY` holds offset **1** (not 7 from old notes)
- Recovery fields (`recovery_policy_mode`, `auto_retry_delay`, etc.) move **system→channel**
- Cal k/b coefficients move into new `vc_channel_cal_config`; `vc_channel_config` loses all 6 cal fields
- `calibration_mode` bool param removed from `vc_channel_set_config`, `vc_channel_set_field`, `vc_channel_output_action`
- `vc_channel_fault_command` drops its `sys_cfg` parameter
- `calibration_fields_changed()` deleted entirely
- `save_target_policy` field and `CH_SAVE_TARGET_POLICY` register deleted entirely
- AUTO mode invariant: entering AUTO enables all non-faulted channels with `configured_target_voltage != 0`; AUTO→NORMAL gracefully disables all outputs
- Cal commit → saves cal to NVS via `storage->save_channel_cal`
- FACTORY_RESET: reset operational to hardcoded defaults, then load cal from NVS (or keep defaults k=10000/b=0 if no NVS cal)
- `vc_runtime_auto_load` becomes three-phase: read sys config → load channels → apply sys config
- Keep `raw_dac_readback`, `cal_output_enabled`, `cal_max_raw_dac_limit` in `vc_channel_snapshot` (needed for FC03 cal-session readback); remove only `cal_sample_status`
- NVS schema changes; previously stored data silently ignored on size mismatch (existing behavior)

---

## File Map

| File | Change |
|------|--------|
| `include/regmap/vc_regs.h` | v3 offsets, bump version |
| `include/voltage_control/vc_types.h` | new structs/enums, updated config structs |
| `include/voltage_control/vc_storage.h` | cal storage signatures use `vc_channel_cal_config*` |
| `include/voltage_control/vc_channel.h` | drop `calibration_mode`/`sys_cfg` params; new cal API |
| `lib/voltage_control/vc_channel.c` | implement new cal API; fix all cal ref sites |
| `include/voltage_control/vc_controller.h` | new cal accessor/setter declarations |
| `lib/voltage_control/vc_controller.c` | mode side effects, factory-reset cal load, cal commit persist |
| `lib/voltage_control/vc_storage_settings.c` | direct `vc_channel_cal_config` storage, remove pack helpers |
| `include/voltage_control/vc_runtime.h` | new cmd type + accessor |
| `lib/voltage_control/vc_runtime.c` | three-phase auto_load, cal publish, new cmd dispatch |
| `include/voltage_control/vc.h` | new `VC_CMD_SET_CHANNEL_CAL_FIELD` + `VC_QUERY_CHANNEL_CAL_CONFIG` |
| `lib/voltage_control/vc.c` | route new command + query |
| `lib/shell_adapter/shell_adapter.c` | updated field tables, new `vc cal config/set` commands |
| `lib/modbus_adapter/modbus_register.c` | v3 register offsets and cal routing |
| `tests/voltage_control/domain/src/main.c` | fix compile errors from API changes |
| `tests/voltage_control/vc_channel_state/src/main.c` | fix compile errors from API changes |

---

## Task 1: Register Map v3

**Files:**
- Modify: `include/regmap/vc_regs.h`

**Interfaces:**
- Produces: all downstream layers use these offsets

- [ ] **Step 1: Replace `vc_regs.h` content with v3 layout**

Replace the entire file (keep copyright header) with:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Protocol major version 3 register-block layout.
 * Breaking changes from v2:
 *   - SYS_STARTUP_CHANNEL_POLICY added at holding offset 1
 *   - SYS_SLAVE_ADDRESS/BAUD_RATE_CODE shifted to 2/3
 *   - Recovery fields (SYS_RECOVERY_*) removed from system holding
 *   - CH_SAVE_TARGET_POLICY removed
 *   - CH_CAL_SAMPLE_STATUS (FC04 in:16) and CH_RAW_DAC_READBACK (FC04 in:17) removed
 *   - Recovery fields added to channel holding at 8-12
 *   - Cal config (k/b) moved to channel holding 20-25
 *   - Cal session commands moved to channel holding 30-34
 */

#ifndef REGMAP_VC_REGS_H
#define REGMAP_VC_REGS_H

#define SYS_BLOCK_BASE    0
#define CH_BLOCK_BASE(c)  (40 + (c) * 40)
#define CH_BLOCK_SIZE     40
#define EXT_BLOCK_BASE    200

#define VC_PROTOCOL_MAJOR             3
#define VC_PROTOCOL_MINOR             0

#include <dt-bindings/voltage_control/capabilities.h>

#define SYS_CAP_AUTOMATIC_MODE         0x0001
#define SYS_CAP_ENV_SENSOR             0x0002
#define SYS_CAP_CALIBRATION_MODE       0x0004

/* ------------------------------------------------------------------ */
/* System input block  (FC04, offsets 0..39)                           */
/* ------------------------------------------------------------------ */

#define SYS_PROTOCOL_MAJOR            0
#define SYS_PROTOCOL_MINOR            1
#define SYS_VARIANT_ID                2
#define SYS_CAPABILITY_FLAGS          3
#define SYS_SUPPORTED_CHANNELS        4
#define SYS_ACTIVE_CHANNEL_MASK       5
#define SYS_BOARD_TEMPERATURE         6
#define SYS_BOARD_HUMIDITY            7
#define SYS_UPTIME_HI                 8
#define SYS_UPTIME_LO                 9
#define SYS_FW_VERSION_HI             10
#define SYS_FW_VERSION_LO             11
#define SYS_ACTIVE_OPERATING_MODE     12
#define SYS_STATUS                    13
#define SYS_FAULT_CAUSE               14
/* 15..39 reserved */

/* ------------------------------------------------------------------ */
/* System holding block  (FC03 / FC06, offsets 0..39)                  */
/* ------------------------------------------------------------------ */

#define SYS_OPERATING_MODE            0
#define SYS_STARTUP_CHANNEL_POLICY    1   /* 0=load NVS op-config, 1=factory reset op-config */
#define SYS_SLAVE_ADDRESS             2
#define SYS_BAUD_RATE_CODE            3
/* 4..38 reserved */
#define SYS_PARAM_ACTION              39

/* ------------------------------------------------------------------ */
/* Channel input block  (FC04, per-channel offsets 0..39)              */
/* ------------------------------------------------------------------ */

#define CH_STATUS_BITS                0
#define CH_ACTIVE_FAULT_CAUSE         1
#define CH_FAULT_HISTORY_CAUSE        2
#define CH_LAST_PROT_OUT_ACTION       3
#define CH_AUTO_RETRY_COUNT           4
#define CH_AUTO_COOLDOWN_REMAINING    5
#define CH_LAST_FAULT_TIMESTAMP_HI    6
#define CH_LAST_FAULT_TIMESTAMP_LO    7
#define CH_OPER_TARGET_VOLTAGE        8
#define CH_CAPABILITY_FLAGS           9
#define CH_MEASURED_VOLTAGE           10
#define CH_MEASURED_CURRENT           11
#define CH_RAW_ADC_VOLTAGE_HI         12
#define CH_RAW_ADC_VOLTAGE_LO         13
#define CH_RAW_ADC_CURRENT_HI         14
#define CH_RAW_ADC_CURRENT_LO         15
/* 16..39 reserved  (CH_CAL_SAMPLE_STATUS and CH_RAW_DAC_READBACK removed) */

/* ------------------------------------------------------------------ */
/* Channel holding block  (FC03 / FC06, per-channel offsets 0..39)     */
/* ------------------------------------------------------------------ */

/* Commands */
#define CH_OUTPUT_ACTION              0
#define CH_FAULT_CMD                  1
#define CH_PARAM_ACTION               2

/* Operational config */
#define CH_CFG_TARGET_VOLTAGE         3
#define CH_RAMP_UP_STEP               4
#define CH_RAMP_UP_INTERVAL           5
#define CH_RAMP_DOWN_STEP             6
#define CH_RAMP_DOWN_INTERVAL         7
#define CH_RECOVERY_POLICY_MODE       8   /* moved from system block */
#define CH_AUTO_RETRY_DELAY           9   /* moved from system block */
#define CH_AUTO_RETRY_MAX_COUNT       10  /* moved from system block */
#define CH_AUTO_RETRY_WINDOW          11  /* moved from system block */
#define CH_CURRENT_SAFE_BAND_PCT      12  /* moved from system block */
#define CH_CURRENT_PROTECTION_MODE    13
#define CH_CURRENT_PROT_OUT_ACTION    14
#define CH_CURRENT_LIMIT_THRESHOLD    15
#define CH_AUTO_DERATE_STEP           16
/* 17..19 reserved  (CH_SAVE_TARGET_POLICY removed) */

/* Cal config — readable in any mode, writable in cal mode only */
#define CH_OUTPUT_CAL_K               20
#define CH_OUTPUT_CAL_B               21
#define CH_MEASURED_V_CAL_K           22
#define CH_MEASURED_V_CAL_B           23
#define CH_MEASURED_I_CAL_K           24
#define CH_MEASURED_I_CAL_B           25
/* 26..29 reserved */

/* Cal session commands — cal mode only */
#define CH_CAL_OUTPUT_ENABLE          30
#define CH_CAL_DAC_CODE               31
#define CH_CAL_SAMPLE_CMD             32
#define CH_CAL_COMMIT_CMD             33
#define CH_CAL_MAX_RAW_DAC_LIMIT      34
/* 35..39 reserved */

/* ------------------------------------------------------------------ */
/* Extension holding block  (FC03 / FC06, offsets 0..79)               */
/* ------------------------------------------------------------------ */

#define EXT_CAL_UNLOCK                0
#define EXT_CAL_UNLOCK_ABS            (EXT_BLOCK_BASE + EXT_CAL_UNLOCK)

#define CAL_UNLOCK_STEP1              0xCA1B
#define CAL_UNLOCK_STEP2              0xA11B
#define CAL_COMMAND_NONE              0
#define CAL_COMMAND_EXECUTE           1

#endif
```

- [ ] **Step 2: Verify build (expects compile errors in downstream files — that's OK for now)**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
west build 2>&1 | grep -c "error:" || true
```

Expected: some errors (downstream uses `SYS_RECOVERY_POLICY_MODE`, `CH_SAVE_TARGET_POLICY`, old offsets). The register map itself is valid.

- [ ] **Step 3: Commit**

```bash
git add include/regmap/vc_regs.h
git commit -m "feat(regmap): bump to protocol v3, reorganise holding offsets"
```

---

## Task 2: Type System + Storage Interface

**Files:**
- Modify: `include/voltage_control/vc_types.h`
- Modify: `include/voltage_control/vc_storage.h`

**Interfaces:**
- Produces: `struct vc_channel_cal_config`, `enum vc_cal_field`, updated `vc_system_config`, updated `vc_channel_config`, updated `vc_channel_snapshot`, `VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD`

- [ ] **Step 1: Replace `vc_types.h` with v3 types**

Replace the entire file body (keep copyright header) with:

```c
#ifndef VOLTAGE_CONTROL_VC_TYPES_H
#define VOLTAGE_CONTROL_VC_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#define VC_MAX_CHANNELS DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))

#define VC_FAULT_CURRENT       0x0002
#define VC_FAULT_MEASUREMENT   0x0004
#define VC_FAULT_HARDWARE      0x0008
#define VC_FAULT_INTERLOCK     0x0010
#define VC_FAULT_RETRY_EXHAUST 0x0020
#define VC_FAULT_CFG_INVALID   0x0040
#define VC_FAULT_STALE         0x0080

enum vc_status {
	VC_OK = 0,
	VC_ERR_UNSUPPORTED_CHANNEL = -1,
	VC_ERR_INVALID_VALUE = -2,
	VC_ERR_INVALID_COMMAND = -3,
	VC_ERR_UNSAFE_STATE = -4,
	VC_ERR_STORAGE = -5,
	VC_ERR_UNSUPPORTED_CAPABILITY = -6,
};

enum vc_operating_mode {
	VC_OPERATING_MODE_NORMAL = 0,
	VC_OPERATING_MODE_AUTOMATIC = 1,
	VC_OPERATING_MODE_CALIBRATION = 2,
};

enum vc_cal_sample_status {
	VC_CAL_SAMPLE_NONE = 0,
	VC_CAL_SAMPLE_VALID = 1,
	VC_CAL_SAMPLE_BUSY = 2,
	VC_CAL_SAMPLE_ERROR = 3,
};

enum vc_output_action {
	VC_OUTPUT_ACTION_NONE = 0,
	VC_OUTPUT_ACTION_ENABLE = 1,
	VC_OUTPUT_ACTION_DISABLE_GRACEFUL = 2,
	VC_OUTPUT_ACTION_DISABLE_IMMEDIATE = 3,
	VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO = 4,
};

enum vc_channel_fault_command {
	VC_CHANNEL_FAULT_COMMAND_NONE = 0,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE = 1,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY = 2,
};

enum vc_protection_mode {
	VC_PROTECTION_MODE_DISABLED = 0,
	VC_PROTECTION_MODE_FLAG_ONLY = 1,
	VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION = 2,
};

enum vc_recovery_policy_mode {
	VC_RECOVERY_MANUAL_LATCH = 0,
	VC_RECOVERY_AUTO_RETRY = 1,
	VC_RECOVERY_AUTO_DERATE_RETRY = 2,
	VC_RECOVERY_NEVER_RETRY = 3,
};

enum vc_param_action {
	VC_PARAM_ACTION_NONE = 0,
	VC_PARAM_ACTION_SAVE = 1,
	VC_PARAM_ACTION_LOAD = 2,
	VC_PARAM_ACTION_FACTORY_RESET = 3,
	VC_PARAM_ACTION_SOFTWARE_RESET = 255,
};

/* System config — operating mode + startup policy only; recovery moved to channel */
struct vc_system_config {
	enum vc_operating_mode operating_mode;
	uint16_t startup_channel_policy;  /* 0=load NVS op-config, 1=factory reset op-config */
};

/* Channel operational config — no cal coefficients (moved to vc_channel_cal_config) */
struct vc_channel_config {
	int16_t configured_target_voltage;
	uint16_t ramp_up_step;
	uint16_t ramp_up_interval;
	uint16_t ramp_down_step;
	uint16_t ramp_down_interval;
	enum vc_recovery_policy_mode recovery_policy_mode;
	uint16_t auto_retry_delay;
	uint16_t auto_retry_max_count;
	uint16_t auto_retry_window;
	uint16_t current_safe_band_pct;
	enum vc_protection_mode current_protection_mode;
	enum vc_output_action current_protection_output_action;
	int16_t current_limit_threshold;
	uint16_t auto_derate_step;
};

/* Calibration coefficients — separate from operational config */
struct vc_channel_cal_config {
	uint16_t output_calib_k;
	int16_t  output_calib_b;
	uint16_t measured_voltage_calib_k;
	int16_t  measured_voltage_calib_b;
	uint16_t measured_current_calib_k;
	int16_t  measured_current_calib_b;
};

/* Cal field selector for SET_CHANNEL_CAL_FIELD command */
enum vc_cal_field {
	VC_CAL_FIELD_OUTPUT_K,
	VC_CAL_FIELD_OUTPUT_B,
	VC_CAL_FIELD_MEASURED_V_K,
	VC_CAL_FIELD_MEASURED_V_B,
	VC_CAL_FIELD_MEASURED_I_K,
	VC_CAL_FIELD_MEASURED_I_B,
};

struct vc_channel_snapshot {
	int16_t measured_voltage;
	int16_t measured_current;
	int16_t operational_target_voltage;
	uint16_t status_bits;
	uint16_t active_fault_cause;
	uint16_t fault_history_cause;
	enum vc_output_action last_protection_output_action;
	uint16_t auto_retry_count;
	uint16_t auto_cooldown_remaining;
	uint32_t last_fault_timestamp;
	uint16_t channel_capability_flags;
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;
	/* cal session state — used for FC03 holding readback, not FC04 input */
	uint16_t raw_dac_readback;
	uint16_t cal_output_enabled;
	uint16_t cal_max_raw_dac_limit;
	/* cal_sample_status removed — register CH_CAL_SAMPLE_STATUS deleted in v3 */
};

struct vc_system_snapshot {
	uint16_t protocol_major;
	uint16_t protocol_minor;
	uint16_t variant_id;
	uint16_t system_capability_flags;
	uint16_t supported_channel_count;
	uint16_t active_channel_mask;
	enum vc_operating_mode active_operating_mode;
	uint16_t system_status;
	uint16_t system_fault_cause;
};

enum vc_config_field {
	/* System fields */
	VC_FIELD_OPERATING_MODE,
	VC_FIELD_STARTUP_CHANNEL_POLICY,

	/* Channel fields (including recovery, moved from system) */
	VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
	VC_FIELD_RAMP_UP_STEP,
	VC_FIELD_RAMP_UP_INTERVAL,
	VC_FIELD_RAMP_DOWN_STEP,
	VC_FIELD_RAMP_DOWN_INTERVAL,
	VC_FIELD_RECOVERY_POLICY_MODE,
	VC_FIELD_AUTO_RETRY_DELAY,
	VC_FIELD_AUTO_RETRY_MAX_COUNT,
	VC_FIELD_AUTO_RETRY_WINDOW,
	VC_FIELD_CURRENT_SAFE_BAND_PCT,
	VC_FIELD_CURRENT_PROTECTION_MODE,
	VC_FIELD_CURRENT_PROT_OUT_ACTION,
	VC_FIELD_CURRENT_LIMIT_THRESHOLD,
	VC_FIELD_AUTO_DERATE_STEP,
	/* VC_FIELD_SAVE_TARGET_POLICY removed */
	/* VC_FIELD_OUTPUT_CAL_K/B, MEASURED_V/I_CAL_K/B removed — use vc_cal_field */
};

struct vc_field_write {
	enum vc_config_field field;
	uint16_t value;
};

#endif
```

- [ ] **Step 2: Update `vc_storage.h` — cal function signatures**

Replace the `vc_storage_backend` struct in `include/voltage_control/vc_storage.h`:

```c
struct vc_storage_backend {
	int (*save_system_config)(const struct vc_system_config *cfg);
	int (*load_system_config)(struct vc_system_config *cfg);
	int (*save_channel_config)(uint8_t ch, const struct vc_channel_config *cfg);
	int (*load_channel_config)(uint8_t ch, struct vc_channel_config *cfg);
	int (*save_channel_cal)(uint8_t ch, const struct vc_channel_cal_config *cal);
	int (*load_channel_cal)(uint8_t ch, struct vc_channel_cal_config *cal);
	int (*erase_all)(void);
};
```

- [ ] **Step 3: Commit (will cause downstream compile errors — fixed in Tasks 3-6)**

```bash
git add include/voltage_control/vc_types.h include/voltage_control/vc_storage.h
git commit -m "feat(types): v3 type system — separate cal config, move recovery to channel"
```

---

## Task 3: Channel Layer

**Files:**
- Modify: `include/voltage_control/vc_channel.h`
- Modify: `lib/voltage_control/vc_channel.c`

**Interfaces:**
- Consumes: `vc_channel_cal_config`, `vc_cal_field` from Task 2
- Produces:
  - `vc_channel_set_config(ch, cfg)` — no `calibration_mode` param
  - `vc_channel_set_field(ch, field, value)` — no `calibration_mode` param
  - `vc_channel_output_action(ch, action)` — no `calibration_mode` param
  - `vc_channel_fault_command(ch, cmd)` — no `sys_cfg` param
  - `vc_channel_get_cal_config(ch, cal)` → `enum vc_status`
  - `vc_channel_set_cal_field(ch, field, value)` → `enum vc_status`
  - `vc_channel_load_cal(ch, cal)` → void (init-time bypass, no mode guard)

- [ ] **Step 1: Update `vc_channel.h`**

Replace the function declarations section (lines 70–116) with:

```c
void vc_channel_init(struct vc_channel *ch,
		     const struct device *dev,
		     uint8_t index, uint16_t caps,
		     struct vc_channel_buffer *meas,
		     vc_wake_fn_t wake_fn, void *wake_user_data);

void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
		    const struct vc_system_config *sys_cfg);

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg);
enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg);
enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action);
enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd);
enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value);

/* Calibration config API */
enum vc_status vc_channel_get_cal_config(const struct vc_channel *ch,
					  struct vc_channel_cal_config *cal);
enum vc_status vc_channel_set_cal_field(struct vc_channel *ch,
					 enum vc_cal_field field, uint16_t value);
void vc_channel_load_cal(struct vc_channel *ch,
			  const struct vc_channel_cal_config *cal);

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage);
void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current);
void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause);

void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg);

void vc_channel_get_snapshot(const struct vc_channel *ch,
			     struct vc_channel_snapshot *snap);

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable);
enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code);
enum vc_status vc_channel_cal_sample(struct vc_channel *ch);
enum vc_status vc_channel_cal_commit(struct vc_channel *ch);
enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit);

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering);

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch);
```

Also update the `vc_channel` struct in `vc_channel.h` — add `cal_config` field (after `config`):

```c
struct vc_channel {
	struct smf_ctx smf;

	const struct device *dev;
	struct vc_channel_buffer *meas;
	vc_wake_fn_t wake_fn;
	void *wake_user_data;

	uint8_t index;
	uint16_t capabilities;

	struct vc_channel_config config;
	struct vc_channel_cal_config cal_config;  /* separate from operational config */

	bool output_enabled;
	bool ramping;
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	int16_t operational_target_voltage;

	int16_t measured_voltage;
	int16_t measured_current;

	uint16_t active_fault_cause;
	uint16_t fault_history_cause;
	uint16_t status_bits;
	uint32_t last_fault_timestamp;
	enum vc_output_action last_protection_output_action;

	uint16_t cal_max_raw_dac_limit;
	uint16_t raw_dac_readback;
	uint16_t cal_output_enabled;
	enum vc_cal_sample_status cal_sample_status;
	int32_t raw_adc_voltage;
	int32_t raw_adc_current;

	uint32_t uptime_ref;

	uint32_t last_consumed_voltage_ts;
	uint32_t last_consumed_current_ts;
};
```

- [ ] **Step 2: Rewrite `vc_channel.c`**

Apply the following changes to `lib/voltage_control/vc_channel.c`:

**2a. Replace `default_channel_config()`** — remove cal k defaults:

```c
static struct vc_channel_config default_channel_config(void)
{
	return (struct vc_channel_config){
		.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
	};
}

static struct vc_channel_cal_config default_cal_config(void)
{
	return (struct vc_channel_cal_config){
		.output_calib_k = 10000,
		.measured_voltage_calib_k = 10000,
		.measured_current_calib_k = 10000,
	};
}
```

**2b. Update `raw_drive_from_target()`** — use `cal_config` not `config`:

```c
static uint16_t raw_drive_from_target(const struct vc_channel *ch, int32_t target)
{
	int64_t raw = ((int64_t)target * ch->cal_config.output_calib_k) / 10000 +
		      ch->cal_config.output_calib_b;

	if (raw <= 0) {
		return 0;
	}
	if (raw > UINT16_MAX) {
		return UINT16_MAX;
	}
	return (uint16_t)raw;
}
```

**2c. Update `apply_hw()`** — change call:

```c
code = raw_drive_from_target(ch, ch->operational_target_voltage);
```

(was `raw_drive_from_target(&ch->config, ch->operational_target_voltage)`)

**2d. Delete `calibration_fields_changed()`** — remove the entire static function.

**2e. Update `validate_capability_config()`** — remove save_target_policy and cal field checks:

```c
static enum vc_status validate_capability_config(
	const struct vc_channel *ch,
	const struct vc_channel_config *old_cfg,
	const struct vc_channel_config *new_cfg)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		if (new_cfg->configured_target_voltage != 0 ||
		    new_cfg->ramp_up_step != old_cfg->ramp_up_step ||
		    new_cfg->ramp_up_interval != old_cfg->ramp_up_interval ||
		    new_cfg->ramp_down_step != old_cfg->ramp_down_step ||
		    new_cfg->ramp_down_interval != old_cfg->ramp_down_interval) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_CURRENT_MEASUREMENT)) {
		if (new_cfg->current_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->current_protection_output_action != old_cfg->current_protection_output_action ||
		    new_cfg->current_limit_threshold != old_cfg->current_limit_threshold) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE) ||
	    !channel_has_cap(ch, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->auto_derate_step != old_cfg->auto_derate_step) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	return VC_OK;
}
```

**2f. Update `is_safe_to_clear_active()`** — read safe_band from channel config (drop sys_cfg param):

```c
static bool is_safe_to_clear_active(const struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;
	int32_t safe_limit;

	if (ch->active_fault_cause & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)cfg->current_safe_band_pct) / 100;
		if (ch->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}
```

**2g. Update `vc_channel_init()`** — initialize `cal_config`:

```c
void vc_channel_init(struct vc_channel *ch, ...)
{
	memset(ch, 0, sizeof(*ch));
	/* ... existing assignments ... */
	ch->config = default_channel_config();
	ch->cal_config = default_cal_config();
	/* ... rest unchanged ... */
}
```

**2h. Update `vc_channel_set_config()`** — drop `calibration_mode` param, remove cal guard:

```c
enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg)
{
	enum vc_status st;

	st = validate_capability_config(ch, &ch->config, cfg);
	if (st != VC_OK) {
		return st;
	}
	if (cfg->configured_target_voltage > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->configured_target_voltage < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->current_limit_threshold < 0) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!is_valid_protection_mode(cfg->current_protection_mode) ||
	    !is_valid_protection_output_action(cfg->current_protection_output_action)) {
		return VC_ERR_INVALID_VALUE;
	}

	ch->config = *cfg;
	tick_current_protection(ch);
	apply_hw(ch);
	update_status_bits(ch);
	return VC_OK;
}
```

**2i. Update `vc_channel_set_field()`** — drop `calibration_mode` param, remove cal cases, add recovery cases:

```c
enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value)
{
	struct vc_channel_config cfg;
	enum vc_status st;

	st = vc_channel_get_config(ch, &cfg);
	if (st != VC_OK) {
		return st;
	}

	switch (field) {
	case VC_FIELD_CONFIGURED_TARGET_VOLTAGE:
		cfg.configured_target_voltage = (int16_t)value;
		break;
	case VC_FIELD_RAMP_UP_STEP:
		cfg.ramp_up_step = value;
		break;
	case VC_FIELD_RAMP_UP_INTERVAL:
		cfg.ramp_up_interval = value;
		break;
	case VC_FIELD_RAMP_DOWN_STEP:
		cfg.ramp_down_step = value;
		break;
	case VC_FIELD_RAMP_DOWN_INTERVAL:
		cfg.ramp_down_interval = value;
		break;
	case VC_FIELD_RECOVERY_POLICY_MODE:
		cfg.recovery_policy_mode = (enum vc_recovery_policy_mode)value;
		break;
	case VC_FIELD_AUTO_RETRY_DELAY:
		cfg.auto_retry_delay = value;
		break;
	case VC_FIELD_AUTO_RETRY_MAX_COUNT:
		cfg.auto_retry_max_count = value;
		break;
	case VC_FIELD_AUTO_RETRY_WINDOW:
		cfg.auto_retry_window = value;
		break;
	case VC_FIELD_CURRENT_SAFE_BAND_PCT:
		cfg.current_safe_band_pct = value;
		break;
	case VC_FIELD_CURRENT_PROTECTION_MODE:
		cfg.current_protection_mode = (enum vc_protection_mode)value;
		break;
	case VC_FIELD_CURRENT_PROT_OUT_ACTION:
		cfg.current_protection_output_action = (enum vc_output_action)value;
		break;
	case VC_FIELD_CURRENT_LIMIT_THRESHOLD:
		cfg.current_limit_threshold = (int16_t)value;
		break;
	case VC_FIELD_AUTO_DERATE_STEP:
		cfg.auto_derate_step = value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_channel_set_config(ch, &cfg);
}
```

**2j. Update `vc_channel_output_action()`** — drop `calibration_mode` param:

```c
enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action)
{
	if (!is_valid_host_output_action(action)) {
		return VC_ERR_INVALID_COMMAND;
	}

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (ch->active_fault_cause != 0) {
			return VC_ERR_UNSAFE_STATE;
		}
		ch->output_enabled = true;
		ch->ramping = true;
		set_smf_state(ch, VC_CHANNEL_SMF_RAMPING);
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->operational_target_voltage = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->raw_dac_readback = 0;
		ch->operational_target_voltage = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	default:
		return VC_OK;
	}
	apply_hw(ch);
	update_status_bits(ch);
	return VC_OK;
}
```

**2k. Update `vc_channel_fault_command()`** — drop `sys_cfg` param:

```c
enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd)
{
	if (!is_valid_channel_fault_command(cmd)) {
		return VC_ERR_INVALID_COMMAND;
	}

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (ch->active_fault_cause == 0) {
			break;
		}
		if (!is_safe_to_clear_active(ch)) {
			return VC_ERR_UNSAFE_STATE;
		}
		ch->active_fault_cause = 0;
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
		break;
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY:
		ch->fault_history_cause = 0;
		break;
	default:
		break;
	}
	update_status_bits(ch);
	return VC_OK;
}
```

**2l. Update `vc_channel_consume_voltage()`** — use `cal_config`:

```c
void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage)
{
	ch->raw_adc_voltage = raw_voltage;
	ch->measured_voltage = clamp_int16(
		((int64_t)raw_voltage * ch->cal_config.measured_voltage_calib_k) /
		10000 + ch->cal_config.measured_voltage_calib_b);

	update_status_bits(ch);
}
```

**2m. Update `vc_channel_consume_current()`** — use `cal_config`:

```c
void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current)
{
	ch->raw_adc_current = raw_current;
	ch->measured_current = clamp_int16(
		((int64_t)raw_current * ch->cal_config.measured_current_calib_k) /
		10000 + ch->cal_config.measured_current_calib_b);

	if (!ch->ramping) {
		tick_current_protection(ch);
	}
	update_status_bits(ch);
}
```

**2n. Update `vc_channel_get_snapshot()`** — remove `cal_sample_status`, keep the rest:

```c
void vc_channel_get_snapshot(const struct vc_channel *ch,
			     struct vc_channel_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->measured_voltage = ch->measured_voltage;
	snap->measured_current = ch->measured_current;
	snap->operational_target_voltage = ch->operational_target_voltage;
	snap->status_bits = ch->status_bits;
	snap->active_fault_cause = ch->active_fault_cause;
	snap->fault_history_cause = ch->fault_history_cause;
	snap->last_protection_output_action = ch->last_protection_output_action;
	snap->auto_retry_count = 0;
	snap->auto_cooldown_remaining = (uint16_t)(ch->cooldown_remaining_ms / 1000);
	snap->last_fault_timestamp = ch->last_fault_timestamp;
	snap->channel_capability_flags = ch->capabilities;
	snap->raw_adc_voltage = ch->raw_adc_voltage;
	snap->raw_adc_current = ch->raw_adc_current;
	snap->raw_dac_readback = ch->raw_dac_readback;
	snap->cal_output_enabled = ch->cal_output_enabled;
	snap->cal_max_raw_dac_limit = ch->cal_max_raw_dac_limit;
}
```

**2o. Add new cal API functions at end of file:**

```c
enum vc_status vc_channel_get_cal_config(const struct vc_channel *ch,
					  struct vc_channel_cal_config *cal)
{
	*cal = ch->cal_config;
	return VC_OK;
}

enum vc_status vc_channel_set_cal_field(struct vc_channel *ch,
					 enum vc_cal_field field, uint16_t value)
{
	switch (field) {
	case VC_CAL_FIELD_OUTPUT_K:
		ch->cal_config.output_calib_k = value;
		break;
	case VC_CAL_FIELD_OUTPUT_B:
		ch->cal_config.output_calib_b = (int16_t)value;
		break;
	case VC_CAL_FIELD_MEASURED_V_K:
		ch->cal_config.measured_voltage_calib_k = value;
		break;
	case VC_CAL_FIELD_MEASURED_V_B:
		ch->cal_config.measured_voltage_calib_b = (int16_t)value;
		break;
	case VC_CAL_FIELD_MEASURED_I_K:
		ch->cal_config.measured_current_calib_k = value;
		break;
	case VC_CAL_FIELD_MEASURED_I_B:
		ch->cal_config.measured_current_calib_b = (int16_t)value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}
	apply_hw(ch);
	return VC_OK;
}

void vc_channel_load_cal(struct vc_channel *ch,
			  const struct vc_channel_cal_config *cal)
{
	ch->cal_config = *cal;
	apply_hw(ch);
}
```

- [ ] **Step 3: Compile check (channel layer only — will still have errors in controller)**

Look for errors specifically in `vc_channel.c` and `vc_channel.h`:

```bash
west build 2>&1 | grep "vc_channel" | grep "error:" | head -20
```

Expected: no errors in `vc_channel.c` or `vc_channel.h`.

- [ ] **Step 4: Commit**

```bash
git add include/voltage_control/vc_channel.h lib/voltage_control/vc_channel.c
git commit -m "feat(channel): separate cal config, remove calibration_mode params"
```

---

## Task 4: Controller Layer + Storage Settings

**Files:**
- Modify: `include/voltage_control/vc_controller.h`
- Modify: `lib/voltage_control/vc_controller.c`
- Modify: `lib/voltage_control/vc_storage_settings.c`

**Interfaces:**
- Consumes: Task 3 channel API (no `calibration_mode` params)
- Produces:
  - `vc_controller_channel_set_cal_field(ctrl, ch, field, value)` → `enum vc_status`
  - `vc_controller_get_channel_cal_config(ctrl, ch, cal)` → `enum vc_status`
  - `vc_controller_set_operating_mode()` — AUTO side effects
  - `vc_controller_channel_set_field()` — auto-enable target voltage in AUTO mode
  - `vc_controller_channel_param_action(LOAD)` — loads cal separately
  - `vc_controller_channel_param_action(FACTORY_RESET)` — reset op-config, load NVS cal
  - `vc_controller_channel_cal_commit()` — persists to NVS

- [ ] **Step 1: Update `vc_controller.h` — add new declarations**

Add after `vc_controller_get_channel_config` declaration:

```c
enum vc_status vc_controller_get_channel_cal_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_cal_config *cal);

enum vc_status vc_controller_channel_set_cal_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_cal_field field, uint16_t value);
```

Also update `vc_controller_set_system_field` — its body handles `VC_FIELD_STARTUP_CHANNEL_POLICY`, replacing the old recovery fields (handled in Task 4 Step 3).

- [ ] **Step 2: Update `vc_controller.c` — `default_system_config()`**

```c
static struct vc_system_config default_system_config(void)
{
	return (struct vc_system_config){
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.startup_channel_policy = 0,
	};
}
```

- [ ] **Step 3: Update `vc_controller_set_operating_mode()` — add AUTO side effects**

After the existing unlock guards and before `ctrl->operating_mode = mode;`, insert:

```c
/* AUTO → NORMAL: gracefully disable all outputs */
if (mode == VC_OPERATING_MODE_NORMAL &&
    ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC) {
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		(void)vc_channel_output_action(&ctrl->channels[i],
					       VC_OUTPUT_ACTION_DISABLE_GRACEFUL);
	}
}
/* → AUTOMATIC: enable all non-faulted channels with non-zero configured target */
if (mode == VC_OPERATING_MODE_AUTOMATIC) {
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		if (ctrl->channels[i].config.configured_target_voltage != 0 &&
		    ctrl->channels[i].active_fault_cause == 0) {
			(void)vc_channel_output_action(&ctrl->channels[i],
						       VC_OUTPUT_ACTION_ENABLE);
		}
	}
}
```

Also fix the existing AUTO→NORMAL block (currently only resets cooldown) — remove the cooldown reset and rely solely on the new graceful disable loop above:

```c
/* Remove: the old cooldown reset for AUTO→NORMAL transition */
```

- [ ] **Step 4: Update `vc_controller_channel_set_field()` — auto-enable on target write in AUTO**

After the `return vc_channel_set_field(...)` call, restructure to:

```c
enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	enum vc_status st = vc_channel_set_field(&ctrl->channels[ch], field, value);

	if (st == VC_OK &&
	    field == VC_FIELD_CONFIGURED_TARGET_VOLTAGE &&
	    ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    (int16_t)value != 0 &&
	    ctrl->channels[ch].active_fault_cause == 0) {
		(void)vc_channel_output_action(&ctrl->channels[ch],
					       VC_OUTPUT_ACTION_ENABLE);
	}
	return st;
}
```

- [ ] **Step 5: Update `vc_controller_channel_fault_command()` — drop `sys_cfg` arg**

```c
enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_fault_command(&ctrl->channels[ch], cmd);
}
```

- [ ] **Step 6: Update `vc_controller_set_system_field()` — swap recovery → startup_policy**

```c
enum vc_status vc_controller_set_system_field(
	struct vc_controller *ctrl, enum vc_config_field field, uint16_t value)
{
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);

	switch (field) {
	case VC_FIELD_OPERATING_MODE:
		return vc_controller_set_operating_mode(ctrl,
						       (enum vc_operating_mode)value);
	case VC_FIELD_STARTUP_CHANNEL_POLICY:
		cfg.startup_channel_policy = value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_controller_set_system_config(ctrl, &cfg);
}
```

- [ ] **Step 7: Update `vc_controller_set_system_config()` — remove `current_safe_band_pct` guard**

Remove the `current_safe_band_pct > 50` guard (that field is no longer in `vc_system_config`).

Replace the entire function with:

```c
enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg)
{
	enum vc_operating_mode old_cfg_mode = ctrl->sys_cfg.operating_mode;

	ctrl->sys_cfg = *cfg;
	if (cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->sys_cfg.operating_mode = old_cfg_mode;
	}

	if (cfg->operating_mode != ctrl->operating_mode) {
		return vc_controller_set_operating_mode(ctrl, cfg->operating_mode);
	}
	return VC_OK;
}
```

- [ ] **Step 8: Update `vc_controller_channel_param_action(LOAD)` — load cal separately**

Replace the `VC_PARAM_ACTION_LOAD` case:

```c
case VC_PARAM_ACTION_LOAD: {
	struct vc_channel_config cfg;

	if (ctrl->storage == NULL ||
	    ctrl->storage->load_channel_config == NULL) {
		return VC_ERR_STORAGE;
	}
	vc_channel_get_config(&ctrl->channels[ch], &cfg);
	if (ctrl->storage->load_channel_config(ch, &cfg) < 0) {
		return VC_ERR_STORAGE;
	}
	enum vc_status st = vc_channel_set_config(&ctrl->channels[ch], &cfg);
	if (st != VC_OK) {
		return st;
	}
	/* Load cal from NVS; -ENOENT is OK — channel keeps cal defaults */
	if (ctrl->storage->load_channel_cal != NULL) {
		struct vc_channel_cal_config cal;
		vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
		if (ctrl->storage->load_channel_cal(ch, &cal) == 0) {
			vc_channel_load_cal(&ctrl->channels[ch], &cal);
		}
	}
	return VC_OK;
}
```

- [ ] **Step 9: Update `vc_controller_channel_param_action(FACTORY_RESET)` — reset op, load NVS cal**

Replace the `VC_PARAM_ACTION_FACTORY_RESET` case:

```c
case VC_PARAM_ACTION_FACTORY_RESET: {
	struct vc_channel_config defaults = {
		.current_limit_threshold = 32767,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
	};
	enum vc_status st = vc_channel_set_config(&ctrl->channels[ch], &defaults);
	if (st != VC_OK) {
		return st;
	}
	/* Load cal from NVS; -ENOENT keeps default k=10000/b=0 */
	if (ctrl->storage != NULL && ctrl->storage->load_channel_cal != NULL) {
		struct vc_channel_cal_config cal;
		vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
		if (ctrl->storage->load_channel_cal(ch, &cal) == 0) {
			vc_channel_load_cal(&ctrl->channels[ch], &cal);
		}
	}
	return VC_OK;
}
```

- [ ] **Step 10: Update `vc_controller_channel_cal_commit()` — persist to NVS**

Replace the existing implementation:

```c
enum vc_status vc_controller_channel_cal_commit(
	struct vc_controller *ctrl, uint8_t ch)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	enum vc_status ret = vc_channel_cal_commit(&ctrl->channels[ch]);
	if (ret != VC_OK) {
		return ret;
	}
	if (ctrl->storage != NULL && ctrl->storage->save_channel_cal != NULL) {
		struct vc_channel_cal_config cal;
		vc_channel_get_cal_config(&ctrl->channels[ch], &cal);
		if (ctrl->storage->save_channel_cal(ch, &cal) < 0) {
			return VC_ERR_STORAGE;
		}
	}
	return VC_OK;
}
```

- [ ] **Step 11: Add new cal accessor/setter functions to `vc_controller.c`**

```c
enum vc_status vc_controller_get_channel_cal_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_cal_config *cal)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_get_cal_config(&ctrl->channels[ch], cal);
}

enum vc_status vc_controller_channel_set_cal_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_cal_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_channel_set_cal_field(&ctrl->channels[ch], field, value);
}
```

- [ ] **Step 12: Update `vc_storage_settings.c` — simplify to direct struct storage**

Replace the entire file body (keep copyright header) with the simplified version that uses `vc_channel_config` and `vc_channel_cal_config` directly (no intermediate packing structs):

```c
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "voltage_control/vc_storage.h"

struct settings_read_ctx {
	void *dst;
	size_t len;
	bool found;
};

static int settings_direct_loader(const char *name, size_t len,
				  settings_read_cb read_cb, void *cb_arg,
				  void *param)
{
	struct settings_read_ctx *ctx = param;

	if (len != ctx->len) {
		return -EINVAL;
	}

	if (read_cb(cb_arg, ctx->dst, len) < 0) {
		return -EIO;
	}

	ctx->found = true;
	return 0;
}

static int settings_load_key(const char *key, void *dst, size_t len)
{
	struct settings_read_ctx ctx = {
		.dst = dst,
		.len = len,
		.found = false,
	};
	int rc = settings_load_subtree_direct(key, settings_direct_loader, &ctx);

	if (rc < 0) {
		return rc;
	}
	return ctx.found ? 0 : -ENOENT;
}

static int settings_save_sys(const struct vc_system_config *cfg)
{
	return settings_save_one("vc/sys", cfg, sizeof(*cfg));
}

static int settings_load_sys(struct vc_system_config *cfg)
{
	return settings_load_key("vc/sys", cfg, sizeof(*cfg));
}

static int settings_save_ch_cfg(uint8_t ch, const struct vc_channel_config *cfg)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	return settings_save_one(key, cfg, sizeof(*cfg));
}

static int settings_load_ch_cfg(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	return settings_load_key(key, cfg, sizeof(*cfg));
}

static int settings_save_ch_cal(uint8_t ch, const struct vc_channel_cal_config *cal)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	return settings_save_one(key, cal, sizeof(*cal));
}

static int settings_load_ch_cal(uint8_t ch, struct vc_channel_cal_config *cal)
{
	char key[16];

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	return settings_load_key(key, cal, sizeof(*cal));
}

static int settings_erase(void)
{
	(void)settings_delete("vc/sys");
	for (uint8_t ch = 0; ch < VC_MAX_CHANNELS; ch++) {
		char key[16];

		snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
		(void)settings_delete(key);
		snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
		(void)settings_delete(key);
	}
	return 0;
}

const struct vc_storage_backend vc_settings_storage = {
	.save_system_config = settings_save_sys,
	.load_system_config = settings_load_sys,
	.save_channel_config = settings_save_ch_cfg,
	.load_channel_config = settings_load_ch_cfg,
	.save_channel_cal = settings_save_ch_cal,
	.load_channel_cal = settings_load_ch_cal,
	.erase_all = settings_erase,
};
```

- [ ] **Step 13: Compile check (controller + storage)**

```bash
west build 2>&1 | grep -E "vc_controller|vc_storage" | grep "error:" | head -20
```

Expected: no errors in these files.

- [ ] **Step 14: Commit**

```bash
git add include/voltage_control/vc_controller.h lib/voltage_control/vc_controller.c \
        lib/voltage_control/vc_storage_settings.c
git commit -m "feat(controller): AUTO side effects, per-channel recovery, cal commit persists"
```

---

## Task 5: Runtime Layer

**Files:**
- Modify: `include/voltage_control/vc_runtime.h`
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `include/voltage_control/vc.h`
- Modify: `lib/voltage_control/vc.c`

**Interfaces:**
- Consumes: `vc_controller_channel_set_cal_field`, `vc_controller_get_channel_cal_config` from Task 4
- Produces:
  - `vc_runtime_get_published_channel_cal_config(runtime, ch, cal)` → `enum vc_status`
  - `VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD` command type
  - `VC_QUERY_CHANNEL_CAL_CONFIG` query type
  - `vc_cmd_cal_set_field(ch, field, value)` command builder
  - `vc_q_channel_cal_config(ch, out)` query builder

- [ ] **Step 1: Update `vc_runtime.h`**

Add `VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD` to the enum (after `VC_RUNTIME_CMD_SET_CHANNEL_FIELD`):

```c
VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
```

Add `cal_field_write` to the payload union in `vc_runtime_command`:

```c
struct { enum vc_cal_field field; uint16_t value; } cal_field_write;
```

Add new accessor declaration:

```c
enum vc_status vc_runtime_get_published_channel_cal_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_cal_config *cal);
```

- [ ] **Step 2: Update `vc_runtime.c` — add `cal_configs` to published snapshot**

In the `vc_published_snapshot` struct, add:

```c
struct vc_channel_cal_config cal_configs[VC_MAX_CHANNELS];
```

- [ ] **Step 3: Update `vc_runtime_publish_snapshot()` — publish cal configs**

Inside the per-channel loop, after publishing the channel config, add:

```c
vc_controller_get_channel_cal_config(ctrl, ch,
				     &runtime->published.cal_configs[ch]);
```

- [ ] **Step 4: Update `vc_runtime_auto_load()` — three-phase init**

Replace the existing `vc_runtime_auto_load()` with:

```c
static void vc_runtime_auto_load(struct vc_runtime *runtime)
{
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
	struct vc_controller *ctrl = runtime->ctrl;

	vc_controller_set_storage_backend(ctrl, &vc_settings_storage);
	settings_subsys_init();

	/* Phase 1: read system config for startup_channel_policy; do not apply yet
	 * (operating mode side effects fire after channel configs are populated). */
	struct vc_system_config sys_cfg;
	vc_controller_get_system_config(ctrl, &sys_cfg);
	(void)ctrl->storage->load_system_config(&sys_cfg);

	/* Phase 2: load channel op-config per startup policy + always load NVS cal.
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
```

- [ ] **Step 5: Add `VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD` dispatch case**

In `vc_runtime_dispatch_command()`, add before the `default:` case:

```c
case VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD:
	return vc_controller_channel_set_cal_field(ctrl, cmd->channel,
						   cmd->payload.cal_field_write.field,
						   cmd->payload.cal_field_write.value);
```

- [ ] **Step 6: Add `vc_runtime_get_published_channel_cal_config()` accessor**

```c
enum vc_status vc_runtime_get_published_channel_cal_config(
	struct vc_runtime *runtime,
	uint8_t channel,
	struct vc_channel_cal_config *cal)
{
	if (runtime == NULL || cal == NULL || channel >= VC_MAX_CHANNELS) {
		return VC_ERR_INVALID_VALUE;
	}
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	*cal = runtime->published.cal_configs[channel];
	k_mutex_unlock(&runtime->snapshot_lock);
	return VC_OK;
}
```

- [ ] **Step 7: Update `vc.h` — add cal field command and cal config query**

Add to `vc_cmd_type` enum (after `VC_CMD_CHANNEL_PARAM_ACTION`):

```c
VC_CMD_SET_CHANNEL_CAL_FIELD,
```

Add `cal_field_write` to the `vc_cmd` union (alongside `field_write`):

```c
struct { enum vc_cal_field field; uint16_t value; } cal_field_write;
```

Add to `vc_query_type` enum (after `VC_QUERY_CHANNEL_CONFIG`):

```c
VC_QUERY_CHANNEL_CAL_CONFIG,
```

Add to `vc_query_msg` union:

```c
struct vc_channel_cal_config *channel_cal_config;
```

Add command builder:

```c
static inline struct vc_cmd vc_cmd_cal_set_field(uint8_t ch,
						  enum vc_cal_field field,
						  uint16_t value)
{
	return (struct vc_cmd){
		.type = VC_CMD_SET_CHANNEL_CAL_FIELD,
		.channel = ch,
		.cal_field_write = { .field = field, .value = value },
	};
}
```

Add query builder:

```c
static inline struct vc_query_msg vc_q_channel_cal_config(
	uint8_t ch, struct vc_channel_cal_config *out)
{
	return (struct vc_query_msg){
		.type = VC_QUERY_CHANNEL_CAL_CONFIG,
		.channel = ch,
		.out.channel_cal_config = out,
	};
}
```

- [ ] **Step 8: Update `vc.c` — route new command and query**

In `vc_dispatch()`, add case before `default:`:

```c
case VC_CMD_SET_CHANNEL_CAL_FIELD: {
	struct vc_runtime_command rtcmd2 = {
		.type = VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
		.channel = cmd.channel,
		.payload.cal_field_write = {
			.field = cmd.cal_field_write.field,
			.value = cmd.cal_field_write.value,
		},
	};
	return vc_runtime_submit_command(ctx->runtime, &rtcmd2, timeout);
}
```

In `vc_query()`, add case before `default:`:

```c
case VC_QUERY_CHANNEL_CAL_CONFIG:
	return vc_runtime_get_published_channel_cal_config(
		ctx->runtime, q.channel, q.out.channel_cal_config);
```

- [ ] **Step 9: Compile check**

```bash
west build 2>&1 | grep "error:" | head -20
```

Expected: errors only in `shell_adapter.c`, `modbus_register.c`, and test files (not yet updated).

- [ ] **Step 10: Commit**

```bash
git add include/voltage_control/vc_runtime.h lib/voltage_control/vc_runtime.c \
        include/voltage_control/vc.h lib/voltage_control/vc.c
git commit -m "feat(runtime): three-phase auto_load, cal config publish, SET_CHANNEL_CAL_FIELD cmd"
```

---

## Task 6: Shell Adapter

**Files:**
- Modify: `lib/shell_adapter/shell_adapter.c`

**Interfaces:**
- Consumes: `vc_cmd_cal_set_field`, `vc_q_channel_cal_config` from Task 5

- [ ] **Step 1: Update `sys_fields[]` — replace recovery entries with startup_policy**

```c
static const struct field_entry sys_fields[] = {
	{"mode",           VC_FIELD_OPERATING_MODE},
	{"startup_policy", VC_FIELD_STARTUP_CHANNEL_POLICY},
};
```

- [ ] **Step 2: Update `ch_fields[]` — add recovery fields, remove save_policy and cal k/b**

```c
static const struct field_entry ch_fields[] = {
	{"target",       VC_FIELD_CONFIGURED_TARGET_VOLTAGE},
	{"ramp_up_step", VC_FIELD_RAMP_UP_STEP},
	{"ramp_up_int",  VC_FIELD_RAMP_UP_INTERVAL},
	{"ramp_dn_step", VC_FIELD_RAMP_DOWN_STEP},
	{"ramp_dn_int",  VC_FIELD_RAMP_DOWN_INTERVAL},
	{"recovery",     VC_FIELD_RECOVERY_POLICY_MODE},
	{"retry_delay",  VC_FIELD_AUTO_RETRY_DELAY},
	{"retry_max",    VC_FIELD_AUTO_RETRY_MAX_COUNT},
	{"retry_window", VC_FIELD_AUTO_RETRY_WINDOW},
	{"safe_band",    VC_FIELD_CURRENT_SAFE_BAND_PCT},
	{"prot_mode",    VC_FIELD_CURRENT_PROTECTION_MODE},
	{"prot_action",  VC_FIELD_CURRENT_PROT_OUT_ACTION},
	{"i_limit",      VC_FIELD_CURRENT_LIMIT_THRESHOLD},
	{"derate_step",  VC_FIELD_AUTO_DERATE_STEP},
};
```

- [ ] **Step 3: Add `cal_fields[]` table**

```c
struct cal_field_entry {
	const char *name;
	enum vc_cal_field field;
};

static const struct cal_field_entry cal_fields[] = {
	{"out_cal_k", VC_CAL_FIELD_OUTPUT_K},
	{"out_cal_b", VC_CAL_FIELD_OUTPUT_B},
	{"v_cal_k",   VC_CAL_FIELD_MEASURED_V_K},
	{"v_cal_b",   VC_CAL_FIELD_MEASURED_V_B},
	{"i_cal_k",   VC_CAL_FIELD_MEASURED_I_K},
	{"i_cal_b",   VC_CAL_FIELD_MEASURED_I_B},
};

static int lookup_cal_field(const struct shell *sh, const char *name,
			     enum vc_cal_field *out)
{
	for (size_t i = 0; i < ARRAY_SIZE(cal_fields); i++) {
		if (strcmp(name, cal_fields[i].name) == 0) {
			*out = cal_fields[i].field;
			return 0;
		}
	}
	shell_error(sh, "unknown cal field: %s", name);
	shell_fprintf(sh, SHELL_NORMAL, "available:");
	for (size_t i = 0; i < ARRAY_SIZE(cal_fields); i++) {
		shell_fprintf(sh, SHELL_NORMAL, " %s", cal_fields[i].name);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return -EINVAL;
}
```

- [ ] **Step 4: Update `print_ch_config()` — remove cal and save_policy fields**

```c
static void print_ch_config(const struct shell *sh, uint8_t ch,
			    const struct vc_channel_config *c)
{
	shell_print(sh, "Channel %d Configuration", ch);
	shell_print(sh, "  target:       %d", c->configured_target_voltage);
	shell_print(sh, "  ramp_up:      step=%d interval=%d",
		    c->ramp_up_step, c->ramp_up_interval);
	shell_print(sh, "  ramp_down:    step=%d interval=%d",
		    c->ramp_down_step, c->ramp_down_interval);
	shell_print(sh, "  recovery:     %s", recovery_str(c->recovery_policy_mode));
	shell_print(sh, "  retry:        delay=%d max=%d window=%d",
		    c->auto_retry_delay, c->auto_retry_max_count,
		    c->auto_retry_window);
	shell_print(sh, "  safe_band:    %d%%", c->current_safe_band_pct);
	shell_print(sh, "  protection:   mode=%s action=%s limit=%d",
		    prot_str(c->current_protection_mode),
		    action_str(c->current_protection_output_action),
		    c->current_limit_threshold);
	shell_print(sh, "  derate_step:  %d", c->auto_derate_step);
}
```

- [ ] **Step 5: Update `print_sys_config()` — remove recovery, add startup_policy**

```c
static void print_sys_config(const struct shell *sh,
			     const struct vc_system_config *c)
{
	shell_print(sh, "System Configuration");
	shell_print(sh, "  mode:           %s", mode_str(c->operating_mode));
	shell_print(sh, "  startup_policy: %d", c->startup_channel_policy);
}
```

- [ ] **Step 6: Add `print_cal_config()` helper**

```c
static void print_cal_config(const struct shell *sh, uint8_t ch,
			     const struct vc_channel_cal_config *c)
{
	shell_print(sh, "Channel %d Calibration Config", ch);
	shell_print(sh, "  out_cal:  k=%d b=%d", c->output_calib_k, c->output_calib_b);
	shell_print(sh, "  v_cal:    k=%d b=%d",
		    c->measured_voltage_calib_k, c->measured_voltage_calib_b);
	shell_print(sh, "  i_cal:    k=%d b=%d",
		    c->measured_current_calib_k, c->measured_current_calib_b);
}
```

- [ ] **Step 7: Add `cmd_cal_config` and `cmd_cal_set` handlers**

Add before the `SHELL_STATIC_SUBCMD_SET_CREATE` for the cal subcommands:

```c
static int cmd_cal_config(const struct shell *sh, size_t argc, char **argv)
{
	CTX_CHECK(sh);
	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) != 0) {
		return -EINVAL;
	}

	struct vc_channel_cal_config cal;

	vc_query(ctx, vc_q_channel_cal_config(ch, &cal));
	print_cal_config(sh, ch, &cal);
	return 0;
}

static int cmd_cal_set(const struct shell *sh, size_t argc, char **argv)
{
	CTX_CHECK(sh);
	uint8_t ch;

	if (parse_channel(sh, argv[1], &ch) != 0) {
		return -EINVAL;
	}

	enum vc_cal_field field;

	if (lookup_cal_field(sh, argv[2], &field) != 0) {
		return -EINVAL;
	}

	char *end;
	long val = strtol(argv[3], &end, 10);

	if (*end != '\0') {
		shell_error(sh, "invalid value: %s", argv[3]);
		return -EINVAL;
	}

	return dispatch(sh, vc_cmd_cal_set_field(ch, field, (uint16_t)(int16_t)val));
}
```

- [ ] **Step 8: Register the new subcommands in the `vc cal` subcommand set**

Find the existing `SHELL_STATIC_SUBCMD_SET_CREATE` for the `cal` subcommand and add:

```c
SHELL_CMD_ARG(config, NULL, "<ch>", cmd_cal_config, 2, 0),
SHELL_CMD_ARG(set, NULL, "<ch> <field> <value>", cmd_cal_set, 4, 0),
```

- [ ] **Step 9: Compile check**

```bash
west build 2>&1 | grep "shell_adapter" | grep "error:" | head -20
```

Expected: no errors in `shell_adapter.c`.

- [ ] **Step 10: Commit**

```bash
git add lib/shell_adapter/shell_adapter.c
git commit -m "feat(shell): new vc cal config/set commands, recovery fields on channel"
```

---

## Task 7: Modbus Adapter

**Files:**
- Modify: `lib/modbus_adapter/modbus_register.c`

**Interfaces:**
- Consumes: v3 register offsets (Task 1), `vc_runtime_get_published_channel_cal_config` (Task 5)

- [ ] **Step 1: Update `is_ch_cal_input_reg()` — remove deleted FC04 registers**

```c
static bool is_ch_cal_input_reg(uint16_t off)
{
	return off >= CH_RAW_ADC_VOLTAGE_HI && off <= CH_RAW_ADC_CURRENT_LO;
}
```

- [ ] **Step 2: Update `is_ch_cal_holding_reg()` — new cal session range (30-34)**

```c
static bool is_ch_cal_holding_reg(uint16_t off)
{
	return off >= CH_CAL_OUTPUT_ENABLE && off <= CH_CAL_MAX_RAW_DAC_LIMIT;
}
```

- [ ] **Step 3: Update `is_ch_calibration_coefficient_reg()` — new cal config range (20-25)**

```c
static bool is_ch_calibration_coefficient_reg(uint16_t off)
{
	return off >= CH_OUTPUT_CAL_K && off <= CH_MEASURED_I_CAL_B;
}
```

- [ ] **Step 4: Update `ch_input_supported()` — remove deleted FC04 registers**

Remove the cases for `CH_CAL_SAMPLE_STATUS` and `CH_RAW_DAC_READBACK`:

```c
static bool ch_input_supported(uint16_t caps, uint16_t off)
{
	switch (off) {
	case CH_STATUS_BITS:
	case CH_ACTIVE_FAULT_CAUSE:
	case CH_FAULT_HISTORY_CAUSE:
	case CH_LAST_PROT_OUT_ACTION:
	case CH_AUTO_RETRY_COUNT:
	case CH_AUTO_COOLDOWN_REMAINING:
	case CH_LAST_FAULT_TIMESTAMP_HI:
	case CH_LAST_FAULT_TIMESTAMP_LO:
	case CH_OPER_TARGET_VOLTAGE:
	case CH_CAPABILITY_FLAGS:
		return true;
	case CH_MEASURED_VOLTAGE:
	case CH_RAW_ADC_VOLTAGE_HI:
	case CH_RAW_ADC_VOLTAGE_LO:
		return caps_all(caps, CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_MEASURED_CURRENT:
	case CH_RAW_ADC_CURRENT_HI:
	case CH_RAW_ADC_CURRENT_LO:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	default:
		return off < CH_BLOCK_SIZE;
	}
}
```

- [ ] **Step 5: Update `ch_holding_supported()` — v3 layout**

```c
static bool ch_holding_supported(uint16_t caps, uint16_t off)
{
	switch (off) {
	case CH_OUTPUT_ACTION:
	case CH_FAULT_CMD:
	case CH_PARAM_ACTION:
		return true;
	case CH_CFG_TARGET_VOLTAGE:
	case CH_RAMP_UP_STEP:
	case CH_RAMP_UP_INTERVAL:
	case CH_RAMP_DOWN_STEP:
	case CH_RAMP_DOWN_INTERVAL:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
	case CH_RECOVERY_POLICY_MODE:
	case CH_AUTO_RETRY_DELAY:
	case CH_AUTO_RETRY_MAX_COUNT:
	case CH_AUTO_RETRY_WINDOW:
	case CH_CURRENT_SAFE_BAND_PCT:
		return true;
	case CH_CURRENT_PROTECTION_MODE:
	case CH_CURRENT_PROT_OUT_ACTION:
	case CH_CURRENT_LIMIT_THRESHOLD:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	case CH_AUTO_DERATE_STEP:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE |
					     CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_OUTPUT_CAL_K:
	case CH_OUTPUT_CAL_B:
	case CH_CAL_OUTPUT_ENABLE:
	case CH_CAL_DAC_CODE:
	case CH_CAL_MAX_RAW_DAC_LIMIT:
		return caps_all(caps, CH_CAP_RAW_OUTPUT_DRIVE);
	case CH_MEASURED_V_CAL_K:
	case CH_MEASURED_V_CAL_B:
		return caps_all(caps, CH_CAP_VOLTAGE_MEASUREMENT);
	case CH_MEASURED_I_CAL_K:
	case CH_MEASURED_I_CAL_B:
		return caps_all(caps, CH_CAP_CURRENT_MEASUREMENT);
	case CH_CAL_SAMPLE_CMD:
		return caps_any(caps, CH_CAP_VOLTAGE_MEASUREMENT |
					     CH_CAP_CURRENT_MEASUREMENT);
	case CH_CAL_COMMIT_CMD:
		return caps_any(caps, CH_CAP_RAW_OUTPUT_DRIVE |
					     CH_CAP_VOLTAGE_MEASUREMENT |
					     CH_CAP_CURRENT_MEASUREMENT);
	default:
		return off < CH_BLOCK_SIZE;
	}
}
```

- [ ] **Step 6: Update `sys_reg_to_field[]` — replace recovery with startup_policy**

```c
static const enum vc_config_field sys_reg_to_field[] = {
	[SYS_OPERATING_MODE]          = VC_FIELD_OPERATING_MODE,
	[SYS_STARTUP_CHANNEL_POLICY]  = VC_FIELD_STARTUP_CHANNEL_POLICY,
};
```

- [ ] **Step 7: Update `ch_reg_to_field[]` — add recovery at 8-12, remove save/cal entries**

```c
static const enum vc_config_field ch_reg_to_field[] = {
	[CH_CFG_TARGET_VOLTAGE]      = VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
	[CH_RAMP_UP_STEP]            = VC_FIELD_RAMP_UP_STEP,
	[CH_RAMP_UP_INTERVAL]        = VC_FIELD_RAMP_UP_INTERVAL,
	[CH_RAMP_DOWN_STEP]          = VC_FIELD_RAMP_DOWN_STEP,
	[CH_RAMP_DOWN_INTERVAL]      = VC_FIELD_RAMP_DOWN_INTERVAL,
	[CH_RECOVERY_POLICY_MODE]    = VC_FIELD_RECOVERY_POLICY_MODE,
	[CH_AUTO_RETRY_DELAY]        = VC_FIELD_AUTO_RETRY_DELAY,
	[CH_AUTO_RETRY_MAX_COUNT]    = VC_FIELD_AUTO_RETRY_MAX_COUNT,
	[CH_AUTO_RETRY_WINDOW]       = VC_FIELD_AUTO_RETRY_WINDOW,
	[CH_CURRENT_SAFE_BAND_PCT]   = VC_FIELD_CURRENT_SAFE_BAND_PCT,
	[CH_CURRENT_PROTECTION_MODE] = VC_FIELD_CURRENT_PROTECTION_MODE,
	[CH_CURRENT_PROT_OUT_ACTION] = VC_FIELD_CURRENT_PROT_OUT_ACTION,
	[CH_CURRENT_LIMIT_THRESHOLD] = VC_FIELD_CURRENT_LIMIT_THRESHOLD,
	[CH_AUTO_DERATE_STEP]        = VC_FIELD_AUTO_DERATE_STEP,
};
```

- [ ] **Step 8: Add `ch_reg_to_cal_field[]` mapping table**

```c
static const enum vc_cal_field ch_reg_to_cal_field[] = {
	[CH_OUTPUT_CAL_K]     = VC_CAL_FIELD_OUTPUT_K,
	[CH_OUTPUT_CAL_B]     = VC_CAL_FIELD_OUTPUT_B,
	[CH_MEASURED_V_CAL_K] = VC_CAL_FIELD_MEASURED_V_K,
	[CH_MEASURED_V_CAL_B] = VC_CAL_FIELD_MEASURED_V_B,
	[CH_MEASURED_I_CAL_K] = VC_CAL_FIELD_MEASURED_I_K,
	[CH_MEASURED_I_CAL_B] = VC_CAL_FIELD_MEASURED_I_B,
};
```

- [ ] **Step 9: Update `vc_reg_read_sys_holding()` — swap recovery reads for startup_policy**

```c
enum vc_status vc_reg_read_sys_holding(struct vc_ctx *ctx, uint16_t off,
				       uint16_t *reg)
{
	struct vc_system_config cfg;

	vc_query(ctx, vc_q_system_config(&cfg));

	switch (off) {
	case SYS_OPERATING_MODE:          *reg = cfg.operating_mode; break;
	case SYS_STARTUP_CHANNEL_POLICY:  *reg = cfg.startup_channel_policy; break;
	case SYS_PARAM_ACTION:            *reg = 0; break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}
```

- [ ] **Step 10: Update `vc_reg_read_ch_holding()` — v3 channel holding reads**

Find `vc_reg_read_ch_holding()` and update to serve recovery (8-12), cal config (20-25), and cal session (30-34). Add a new include for the cal config query at the top of the function:

```c
enum vc_status vc_reg_read_ch_holding(struct vc_ctx *ctx, uint8_t ch_idx,
				      uint16_t off, uint16_t *reg)
{
	struct vc_channel_config cfg;
	struct vc_channel_snapshot snap;
	struct vc_channel_cal_config cal;

	vc_query(ctx, vc_q_channel_config(ch_idx, &cfg));

	switch (off) {
	/* Operational config */
	case CH_CFG_TARGET_VOLTAGE:      *reg = (uint16_t)cfg.configured_target_voltage; break;
	case CH_RAMP_UP_STEP:            *reg = cfg.ramp_up_step; break;
	case CH_RAMP_UP_INTERVAL:        *reg = cfg.ramp_up_interval; break;
	case CH_RAMP_DOWN_STEP:          *reg = cfg.ramp_down_step; break;
	case CH_RAMP_DOWN_INTERVAL:      *reg = cfg.ramp_down_interval; break;
	/* Recovery (moved from system) */
	case CH_RECOVERY_POLICY_MODE:    *reg = cfg.recovery_policy_mode; break;
	case CH_AUTO_RETRY_DELAY:        *reg = cfg.auto_retry_delay; break;
	case CH_AUTO_RETRY_MAX_COUNT:    *reg = cfg.auto_retry_max_count; break;
	case CH_AUTO_RETRY_WINDOW:       *reg = cfg.auto_retry_window; break;
	case CH_CURRENT_SAFE_BAND_PCT:   *reg = cfg.current_safe_band_pct; break;
	/* Protection */
	case CH_CURRENT_PROTECTION_MODE: *reg = cfg.current_protection_mode; break;
	case CH_CURRENT_PROT_OUT_ACTION: *reg = cfg.current_protection_output_action; break;
	case CH_CURRENT_LIMIT_THRESHOLD: *reg = (uint16_t)cfg.current_limit_threshold; break;
	case CH_AUTO_DERATE_STEP:        *reg = cfg.auto_derate_step; break;
	/* Cal config (readable in any mode) */
	case CH_OUTPUT_CAL_K:
	case CH_OUTPUT_CAL_B:
	case CH_MEASURED_V_CAL_K:
	case CH_MEASURED_V_CAL_B:
	case CH_MEASURED_I_CAL_K:
	case CH_MEASURED_I_CAL_B:
		vc_query(ctx, vc_q_channel_cal_config(ch_idx, &cal));
		switch (off) {
		case CH_OUTPUT_CAL_K:     *reg = cal.output_calib_k; break;
		case CH_OUTPUT_CAL_B:     *reg = (uint16_t)cal.output_calib_b; break;
		case CH_MEASURED_V_CAL_K: *reg = cal.measured_voltage_calib_k; break;
		case CH_MEASURED_V_CAL_B: *reg = (uint16_t)cal.measured_voltage_calib_b; break;
		case CH_MEASURED_I_CAL_K: *reg = cal.measured_current_calib_k; break;
		case CH_MEASURED_I_CAL_B: *reg = (uint16_t)cal.measured_current_calib_b; break;
		}
		break;
	/* Cal session state (readable via FC03 holding) */
	case CH_CAL_OUTPUT_ENABLE:
	case CH_CAL_DAC_CODE:
	case CH_CAL_MAX_RAW_DAC_LIMIT:
		vc_query(ctx, vc_q_channel_snapshot(ch_idx, &snap));
		switch (off) {
		case CH_CAL_OUTPUT_ENABLE:    *reg = snap.cal_output_enabled; break;
		case CH_CAL_DAC_CODE:         *reg = snap.raw_dac_readback; break;
		case CH_CAL_MAX_RAW_DAC_LIMIT: *reg = snap.cal_max_raw_dac_limit; break;
		}
		break;
	case CH_CAL_SAMPLE_CMD:
	case CH_CAL_COMMIT_CMD:
		*reg = 0;
		break;
	case CH_PARAM_ACTION:
		*reg = 0;
		break;
	default:
		if (off < CH_BLOCK_SIZE) {
			*reg = 0;
		} else {
			return VC_ERR_INVALID_VALUE;
		}
		break;
	}
	return VC_OK;
}
```

- [ ] **Step 11: Update `vc_reg_write_ch_holding()` — route cal coefficient writes to SET_CHANNEL_CAL_FIELD**

Find `vc_reg_write_ch_holding()` and update the section that routes cal coefficient writes. Replace the existing cal-coefficient write path (which used `VC_CMD_SET_CHANNEL_FIELD` with `VC_FIELD_OUTPUT_CAL_K` etc.) with:

```c
/* Cal coefficient write — route via SET_CHANNEL_CAL_FIELD */
if (is_ch_calibration_coefficient_reg(off)) {
	enum vc_cal_field cal_field = ch_reg_to_cal_field[off];
	return vc_dispatch(ctx, vc_cmd_cal_set_field(ch_idx, cal_field, value), timeout);
}
```

Also update the sys holding write to handle `SYS_STARTUP_CHANNEL_POLICY` (using existing `sys_reg_to_field[]`).

- [ ] **Step 12: Compile check — full build**

```bash
west build 2>&1 | grep "error:" | head -30
```

Expected: no errors (all layers now updated).

- [ ] **Step 13: Commit**

```bash
git add lib/modbus_adapter/modbus_register.c
git commit -m "feat(modbus): update register handler for v3 layout"
```

---

## Task 8: Test Suite Updates

**Files:**
- Modify: `tests/voltage_control/domain/src/main.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

**Interfaces:**
- Consumes: all updated APIs from Tasks 2-7

- [ ] **Step 1: Find all `calibration_mode` references in domain tests**

```bash
grep -n "calibration_mode\|save_target_policy\|output_calib_k\|output_calib_b\|VC_FIELD_OUTPUT_CAL\|VC_FIELD_SAVE\|recovery_policy_mode.*sys\|current_safe_band_pct.*sys\|sys_cfg\." \
  tests/voltage_control/domain/src/main.c | head -50
```

- [ ] **Step 2: Fix domain test `test_calibration_coefficients_require_calibration_mode`**

This test verifies that cal writes are blocked outside cal mode. After the refactor, cal coefficient writes go through `vc_cmd_cal_set_field` / `VC_CMD_SET_CHANNEL_CAL_FIELD`, which is gated at the controller (requires CAL mode). The test should now use `vc_dispatch(ctx, vc_cmd_cal_set_field(0, VC_CAL_FIELD_OUTPUT_K, value), ...)` and verify it returns `VC_ERR_INVALID_COMMAND` outside CAL mode.

Update the test:

```c
ZTEST(vc_domain, test_calibration_coefficients_require_calibration_mode)
{
	struct vc_channel_cal_config baseline, after;

	vc_query(ctx, vc_q_channel_cal_config(0, &baseline));

	/* Write in NORMAL mode — should be rejected */
	zassert_equal(
		vc_dispatch(ctx, vc_cmd_cal_set_field(0, VC_CAL_FIELD_OUTPUT_K,
						      baseline.output_calib_k + 1),
			    SHELL_CMD_TIMEOUT),
		VC_ERR_INVALID_COMMAND);

	vc_query(ctx, vc_q_channel_cal_config(0, &after));
	zassert_equal(after.output_calib_k, baseline.output_calib_k);

	/* Write in CAL mode — should succeed */
	enter_calibration_mode(ctrl);
	zassert_equal(
		vc_dispatch(ctx, vc_cmd_cal_set_field(0, VC_CAL_FIELD_OUTPUT_K, 10001),
			    SHELL_CMD_TIMEOUT),
		VC_OK);

	vc_query(ctx, vc_q_channel_cal_config(0, &after));
	zassert_equal(after.output_calib_k, 10001);
}
```

- [ ] **Step 3: Fix domain test — system config assertions**

Any test that checks `cfg.recovery_policy_mode` or `cfg.current_safe_band_pct` from a `vc_system_config` must instead check the same fields from `vc_channel_config` (per-channel). Update accordingly:

```c
/* Before: checking sys config for recovery */
struct vc_system_config sys;
vc_query(ctx, vc_q_system_config(&sys));
zassert_equal(sys.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);

/* After: checking channel config for recovery */
struct vc_channel_config ch_cfg;
vc_query(ctx, vc_q_channel_config(0, &ch_cfg));
zassert_equal(ch_cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
zassert_equal(ch_cfg.current_safe_band_pct, 10);
```

- [ ] **Step 4: Fix domain tests — `output_calib_k` assertions in channel config**

Tests that check `cfg.output_calib_k` on a `vc_channel_config` must now check `vc_channel_cal_config`:

```c
/* Before */
struct vc_channel_config cfg;
vc_query(ctx, vc_q_channel_config(0, &cfg));
zassert_equal(cfg.output_calib_k, 10000);

/* After */
struct vc_channel_cal_config cal;
vc_query(ctx, vc_q_channel_cal_config(0, &cal));
zassert_equal(cal.output_calib_k, 10000);
```

- [ ] **Step 5: Fix domain tests — cal affect ramping behavior**

Tests that set `cfg.output_calib_k = 20000` before ramping now need to use `vc_cmd_cal_set_field` in cal mode. Find all such tests in domain tests and update:

```c
/* Before: cfg.output_calib_k = 20000; vc_channel_set_config(&ctrl->channels[0], &cfg, true); */
/* After: enter cal mode, set cal field, exit cal mode */
enter_calibration_mode(ctrl);
zassert_equal(vc_dispatch(ctx, vc_cmd_cal_set_field(0, VC_CAL_FIELD_OUTPUT_K, 20000),
			  SHELL_CMD_TIMEOUT), VC_OK);
(void)vc_dispatch(ctx, vc_cmd_set_mode(VC_OPERATING_MODE_NORMAL), SHELL_CMD_TIMEOUT);
```

- [ ] **Step 6: Fix `vc_channel_state` tests — drop `calibration_mode` param**

In `tests/voltage_control/vc_channel_state/src/main.c`, update all calls:

```c
/* Before */
vc_channel_set_config(&ch, &cfg, false);
vc_channel_set_config(&ch, &cfg, true);
vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, true);
vc_channel_set_field(&ch, field, value, false);
vc_channel_fault_command(&ch, cmd, &default_sys);

/* After */
vc_channel_set_config(&ch, &cfg);
vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);
vc_channel_set_field(&ch, field, value);
vc_channel_fault_command(&ch, cmd);
```

Note: The test at line 118 that verifies `vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, true)` returns `VC_ERR_INVALID_COMMAND` (cal mode rejection) no longer has meaning — output action no longer takes a `calibration_mode` param. Remove that test case or update it to test something meaningful (e.g. enable with active fault).

- [ ] **Step 7: Fix `vc_channel_state` tests — cal coefficient tests**

Replace `vc_channel_set_config(&ch, &cfg, true)` for setting `output_calib_k` with:

```c
vc_channel_set_cal_field(&ch, VC_CAL_FIELD_OUTPUT_K, 20000);
struct vc_channel_cal_config cal;
vc_channel_get_cal_config(&ch, &cal);
zassert_equal(cal.output_calib_k, 20000);
```

- [ ] **Step 8: Fix `vc_channel_state` tests — channel snapshot fields**

Remove any assertions on `snap.cal_sample_status` (removed from snapshot).

- [ ] **Step 9: Build and run domain tests**

```bash
west build -b native_sim tests/voltage_control/domain -- -DBOARD_FLASH_RUNNER= 2>&1 | tail -5
west build -t run 2>&1 | grep -E "PASS|FAIL|error"
```

Expected: all tests PASS.

- [ ] **Step 10: Build and run channel state tests**

```bash
west build -b native_sim tests/voltage_control/vc_channel_state -- -DBOARD_FLASH_RUNNER= 2>&1 | tail -5
west build -t run 2>&1 | grep -E "PASS|FAIL|error"
```

Expected: all tests PASS.

- [ ] **Step 11: Build and run modbus adapter tests**

```bash
west build -b native_sim tests/voltage_control/modbus_adapter -- -DBOARD_FLASH_RUNNER= 2>&1 | tail -5
west build -t run 2>&1 | grep -E "PASS|FAIL|error"
```

Expected: all tests PASS.

- [ ] **Step 12: Final full firmware build**

```bash
west build 2>&1 | grep -c "error:" || echo "0 errors"
```

Expected: 0 errors.

- [ ] **Step 13: Commit test fixes**

```bash
git add tests/voltage_control/domain/src/main.c \
        tests/voltage_control/vc_channel_state/src/main.c
git commit -m "test: update for v3 API — cal config separate, recovery per-channel"
```

---

## Verification Checklist

After all tasks complete, manually verify against the spec (using shell commands on target or native_sim):

```
# Protocol version check
vc status
# → protocol_major = 3

# AUTO mode side effects
vc ch 0 set target 1000
vc sys set mode auto
vc ch 0 status
# → output_enabled = 1

vc sys set mode normal
vc ch 0 status
# → output_enabled = 0

# startup_policy at sys level, not recovery
vc sys config
# → shows mode and startup_policy, NOT recovery/retry fields

# Recovery at channel level
vc ch 0 config
# → shows recovery, retry_*, safe_band fields

# Cal workflow
vc cal unlock  # or via modbus: write CAL_UNLOCK_STEP1 then STEP2
vc sys set mode cal
vc cal config 0          # shows cal coefficients
vc cal set 0 out_cal_k 9800   # new command
vc cal commit 0          # persists to NVS

# After reboot: cal survived
vc cal config 0
# → out_cal_k = 9800

# Factory reset: operational reset, cal preserved
vc ch 0 param reset
vc ch 0 config
# → target = 0 (factory default)
vc cal config 0
# → out_cal_k = 9800 (preserved)
```

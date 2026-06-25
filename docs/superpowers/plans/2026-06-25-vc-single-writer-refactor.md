# VC Single-Writer Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor voltage_control into a clean layered architecture where vc_channel owns its state machine and drives hardware directly, with zero locks (single-writer via runtime thread), centrally managed measurement buffers in iterable sections, and a single source of truth for channel identity from DTS.

**Architecture:** `vc.h` → `vc_runtime` (execution engine, single-writer thread) → `vc_controller` (thin wrapper: route commands, orchestrate collective mode changes) → `vc_channel` (owns state machine, calls hw directly via device pointer) → `vc_channel_hw_api` → concrete drivers. Measurement buffers are defined centrally via DTS iteration, placed in a Zephyr iterable section, and shared between drivers (write) and vc_channel (read). The runtime semaphore wake provides the memory barrier.

**Tech Stack:** Zephyr RTOS v3.7, SMF (state machine framework), `STRUCT_SECTION_ITERABLE`, DTS macros (`DT_FOREACH_CHILD_STATUS_OKAY`), ztest

**Test command:** `west twister -T tests/voltage_control/ --no-clean -p native_posix`

**Baseline:** 5 test suites, 142 test cases, all pass.

---

### Task 1: Extend `vc_channel_hw.h` — Merged API + Measurement Buffer

**Files:**
- Modify: `include/voltage_control/vc_channel_hw.h`

This task adds `get_capabilities` and `set_meas_callback` to the driver vtable, defines the shared `vc_meas_buffer` struct, and provides macros for accessing centrally defined measurement buffer entries.

- [ ] **Step 1: Write the new header**

Replace the contents of `include/voltage_control/vc_channel_hw.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_HW_H
#define VOLTAGE_CONTROL_VC_CHANNEL_HW_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

typedef void (*vc_meas_ready_cb_t)(uint8_t channel, void *user_data);

struct vc_channel_hw_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*start_sampling)(const struct device *dev);
	int (*stop_sampling)(const struct device *dev);
	uint16_t (*get_capabilities)(const struct device *dev);
	int (*set_meas_callback)(const struct device *dev,
				 vc_meas_ready_cb_t cb, void *user_data);
};

struct vc_meas_buffer {
	uint8_t channel_id;
	int32_t raw_voltage;
	uint32_t voltage_timestamp_ms;
	int32_t raw_current;
	uint32_t current_timestamp_ms;
};

#define VC_MEAS_BUFFER_NAME(node_id) \
	UTIL_CAT(_vc_meas_, DT_REG_ADDR(node_id))

#define VC_MEAS_BUFFER_EXTERN(node_id) \
	extern struct vc_meas_buffer VC_MEAS_BUFFER_NAME(node_id)

#define VC_MEAS_BUFFER_PTR(node_id) \
	(&VC_MEAS_BUFFER_NAME(node_id))

#endif
```

- [ ] **Step 2: Verify existing tests still compile**

Run: `west twister -T tests/voltage_control/ --no-clean -p native_posix`

Expected: All 5 suites pass (142 cases). The header change is additive — existing code only uses the old 4-function vtable.

- [ ] **Step 3: Commit**

```bash
git add include/voltage_control/vc_channel_hw.h
git commit -m "refactor(vc): extend vc_channel_hw_api with get_capabilities, set_meas_callback, and vc_meas_buffer"
```

---

### Task 2: Stub Driver — Full `vc_channel_hw_api` Vtable

**Files:**
- Modify: `tests/voltage_control/vc/src/vc_channel_stub.c`
- Modify: `tests/voltage_control/vc_controller/boards/native_posix.overlay` (add stub driver source)

The stub driver currently passes `NULL` as the API pointer. We need a real vtable so vc_channel can call `set_meas_callback` and `get_capabilities` during init.

- [ ] **Step 1: Rewrite the stub driver**

Replace `tests/voltage_control/vc/src/vc_channel_stub.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel_hw.h"

#define DT_DRV_COMPAT jianwei_vc_channel_stub

struct vc_stub_config {
	uint16_t capabilities;
};

struct vc_stub_data {
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	uint16_t last_output_code;
	bool last_enable;
};

static int stub_set_output(const struct device *dev, uint16_t code)
{
	struct vc_stub_data *data = dev->data;

	data->last_output_code = code;
	return 0;
}

static int stub_set_enable(const struct device *dev, bool enable)
{
	struct vc_stub_data *data = dev->data;

	data->last_enable = enable;
	return 0;
}

static int stub_start_sampling(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int stub_stop_sampling(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static uint16_t stub_get_capabilities(const struct device *dev)
{
	const struct vc_stub_config *cfg = dev->config;

	return cfg->capabilities;
}

static int stub_set_meas_callback(const struct device *dev,
				  vc_meas_ready_cb_t cb, void *user_data)
{
	struct vc_stub_data *data = dev->data;

	data->meas_cb = cb;
	data->meas_cb_user_data = user_data;
	return 0;
}

static const struct vc_channel_hw_api stub_hw_api = {
	.set_output = stub_set_output,
	.set_enable = stub_set_enable,
	.start_sampling = stub_start_sampling,
	.stop_sampling = stub_stop_sampling,
	.get_capabilities = stub_get_capabilities,
	.set_meas_callback = stub_set_meas_callback,
};

static int vc_channel_stub_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define VC_CHANNEL_STUB_DEFINE(inst)                                     \
	static const struct vc_stub_config vc_stub_config_##inst = {     \
		.capabilities = DT_INST_PROP(inst, capabilities),        \
	};                                                               \
	static struct vc_stub_data vc_stub_data_##inst;                  \
	DEVICE_DT_INST_DEFINE(inst, vc_channel_stub_init, NULL,          \
			      &vc_stub_data_##inst,                      \
			      &vc_stub_config_##inst,                    \
			      POST_KERNEL,                               \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,        \
			      &stub_hw_api);

DT_INST_FOREACH_STATUS_OKAY(VC_CHANNEL_STUB_DEFINE)
```

- [ ] **Step 2: Copy stub driver to vc_controller test**

The vc_controller test also uses `jianwei,vc-channel-stub` devices from its DTS overlay. It needs the same stub driver source.

Create a symlink or copy `tests/voltage_control/vc_controller/src/vc_channel_stub.c` with the same content.

```bash
cp tests/voltage_control/vc/src/vc_channel_stub.c tests/voltage_control/vc_controller/src/vc_channel_stub.c
```

- [ ] **Step 3: Run tests**

Run: `west twister -T tests/voltage_control/ --no-clean -p native_posix`

Expected: All 5 suites pass. The stub now provides a real API but the existing code doesn't call the new ops yet.

- [ ] **Step 4: Commit**

```bash
git add tests/voltage_control/vc/src/vc_channel_stub.c
git add tests/voltage_control/vc_controller/src/vc_channel_stub.c
git commit -m "refactor(vc): stub driver with full vc_channel_hw_api vtable"
```

---

### Task 3: Rewrite `vc_channel_state.h` — Single-Writer Struct

**Files:**
- Modify: `include/voltage_control/vc_channel_state.h`

Remove spinlock and pending command. Add device pointer, measurement buffer pointer, wake callback, and timestamp tracking. Change init signature.

- [ ] **Step 1: Write the new header**

Replace `include/voltage_control/vc_channel_state.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_STATE_H
#define VOLTAGE_CONTROL_VC_CHANNEL_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/smf.h>

#include "voltage_control/vc_types.h"
#include "voltage_control/vc_channel_hw.h"

typedef void (*vc_wake_fn_t)(void *user_data);

enum vc_channel_smf_state {
	VC_CHANNEL_SMF_DISABLED_SAFE,
	VC_CHANNEL_SMF_ENABLED_HOLDING,
	VC_CHANNEL_SMF_RAMPING,
	VC_CHANNEL_SMF_FAULT_LATCHED,
	VC_CHANNEL_SMF_RETRY_COOLDOWN,
	VC_CHANNEL_SMF_CALIBRATION_OUTPUT,
	VC_CHANNEL_SMF_COUNT,
};

struct vc_channel {
	const struct device *dev;
	struct vc_meas_buffer *meas;
	vc_wake_fn_t wake_fn;
	void *wake_user_data;

	struct smf_ctx smf;
	uint8_t index;
	uint16_t capabilities;

	struct vc_channel_config config;

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

void vc_channel_init(struct vc_channel *ch,
		     const struct device *dev,
		     uint8_t index, uint16_t caps,
		     struct vc_meas_buffer *meas,
		     vc_wake_fn_t wake_fn, void *wake_user_data);

void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
		    const struct vc_system_config *sys_cfg);

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode);
enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg);
enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode);
enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg);
enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value,
				    bool calibration_mode);

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

#endif
```

Key changes from old header:
- `vc_channel_init` takes `(dev, index, caps, meas, wake_fn, wake_user_data)` instead of `(index, caps)`
- `vc_channel_run` added — reads meas buffer, consumes, ticks ramp
- `vc_channel_cal_set_output_enable` takes only `(ch, enable)` — no `all_channels`/`count`
- Removed: `k_spinlock lock`, `struct vc_pending_command`, `vc_channel_has_pending_command`, `vc_channel_take_pending_command`

- [ ] **Step 2: Commit header (code won't compile yet — that's expected)**

```bash
git add include/voltage_control/vc_channel_state.h
git commit -m "refactor(vc): vc_channel_state.h — single-writer struct, device pointer, apply_hw"
```

---

### Task 4: Rewrite `vc_channel_state.c` — `apply_hw`, Remove Spinlock

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`

Replace `set_pending` with `apply_hw` (calls hw driver directly). Remove all spinlock usage. Add `vc_channel_run`. Add `vc_channel_meas_ready` (registered with hw driver). Simplify `vc_channel_cal_set_output_enable` to single-channel only.

- [ ] **Step 1: Write the new implementation**

Replace `lib/voltage_control/vc_channel_state.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel_state.h"
#include "regmap/vc_regs.h"
#include <string.h>

#define VC_DEFAULT_MAX_RAW_DAC      0xFFFF
#define VC_DEFAULT_MAX_VOLTAGE_RAW  20000
#define VC_DEFAULT_MIN_VOLTAGE_RAW  0
#define VC_DEFAULT_MAX_CURRENT_RAW  32767

static const struct smf_state vc_channel_states[VC_CHANNEL_SMF_COUNT] = {
	[VC_CHANNEL_SMF_DISABLED_SAFE]      = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_ENABLED_HOLDING]    = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RAMPING]            = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_FAULT_LATCHED]      = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_RETRY_COOLDOWN]     = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
	[VC_CHANNEL_SMF_CALIBRATION_OUTPUT] = SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL),
};

static struct vc_channel_config default_channel_config(void)
{
	return (struct vc_channel_config){
		.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
		.output_calib_k = 10000,
		.measured_voltage_calib_k = 10000,
		.measured_current_calib_k = 10000,
	};
}

static void set_smf_state(struct vc_channel *ch, enum vc_channel_smf_state state)
{
	smf_set_state(SMF_CTX(ch), &vc_channel_states[state]);
}

static bool channel_has_cap(const struct vc_channel *ch, uint16_t cap)
{
	return (ch->capabilities & cap) == cap;
}

static bool has_hard_safety_fault(const struct vc_channel *ch)
{
	return (ch->active_fault_cause & (VC_FAULT_HARDWARE | VC_FAULT_INTERLOCK)) != 0;
}

static int16_t clamp_int16(int64_t value)
{
	if (value > INT16_MAX) {
		return INT16_MAX;
	}
	if (value < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)value;
}

static uint16_t raw_drive_from_target(const struct vc_channel_config *cfg,
				      int32_t target)
{
	int64_t raw = ((int64_t)target * cfg->output_calib_k) / 10000 +
		      cfg->output_calib_b;

	if (raw <= 0) {
		return 0;
	}
	if (raw > UINT16_MAX) {
		return UINT16_MAX;
	}
	return (uint16_t)raw;
}

static void apply_hw(struct vc_channel *ch)
{
	if (ch->dev == NULL) {
		return;
	}
	const struct vc_channel_hw_api *api = ch->dev->api;

	if (api == NULL) {
		return;
	}

	bool enable;
	uint16_t code;

	if (ch->cal_output_enabled) {
		enable = true;
		code = ch->raw_dac_readback;
	} else if (ch->output_enabled) {
		enable = true;
		code = raw_drive_from_target(&ch->config,
					     ch->operational_target_voltage);
	} else {
		enable = false;
		code = 0;
	}

	if (api->set_output) {
		api->set_output(ch->dev, code);
	}
	if (api->set_enable) {
		api->set_enable(ch->dev, enable);
	}
}

static void update_status_bits(struct vc_channel *ch)
{
	enum vc_channel_smf_state state = vc_channel_get_smf_state(ch);
	uint16_t bits = 0;

	if (state == VC_CHANNEL_SMF_ENABLED_HOLDING ||
	    state == VC_CHANNEL_SMF_RAMPING) {
		bits |= 0x0001;
	} else if (ch->operational_target_voltage != 0 ||
		   ch->measured_voltage != 0) {
		bits |= 0x0001;
	}
	if (ch->output_enabled) {
		bits |= 0x0002;
	}
	if (state == VC_CHANNEL_SMF_RAMPING) {
		bits |= 0x0004;
	}
	if (state == VC_CHANNEL_SMF_FAULT_LATCHED ||
	    state == VC_CHANNEL_SMF_RETRY_COOLDOWN) {
		bits |= 0x0008;
	}
	if (ch->fault_history_cause != 0) {
		bits |= 0x0010;
	}
	if (state == VC_CHANNEL_SMF_RETRY_COOLDOWN) {
		bits |= 0x0020;
	}
	ch->status_bits = bits;
}

static bool is_valid_host_output_action(enum vc_output_action action)
{
	return action == VC_OUTPUT_ACTION_NONE ||
	       action == VC_OUTPUT_ACTION_ENABLE ||
	       action == VC_OUTPUT_ACTION_DISABLE_GRACEFUL ||
	       action == VC_OUTPUT_ACTION_DISABLE_IMMEDIATE;
}

static bool is_valid_channel_fault_command(enum vc_channel_fault_command cmd)
{
	return cmd >= VC_CHANNEL_FAULT_COMMAND_NONE &&
	       cmd <= VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY;
}

static bool is_valid_protection_mode(enum vc_protection_mode mode)
{
	return mode <= VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
}

static bool is_valid_protection_output_action(enum vc_output_action action)
{
	switch (action) {
	case VC_OUTPUT_ACTION_NONE:
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		return true;
	default:
		return false;
	}
}

static bool calibration_fields_changed(const struct vc_channel_config *old_cfg,
				       const struct vc_channel_config *new_cfg)
{
	return old_cfg->output_calib_k != new_cfg->output_calib_k ||
	       old_cfg->output_calib_b != new_cfg->output_calib_b ||
	       old_cfg->measured_voltage_calib_k != new_cfg->measured_voltage_calib_k ||
	       old_cfg->measured_voltage_calib_b != new_cfg->measured_voltage_calib_b ||
	       old_cfg->measured_current_calib_k != new_cfg->measured_current_calib_k ||
	       old_cfg->measured_current_calib_b != new_cfg->measured_current_calib_b;
}

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
		    new_cfg->ramp_down_interval != old_cfg->ramp_down_interval ||
		    new_cfg->save_target_policy != old_cfg->save_target_policy ||
		    new_cfg->output_calib_k != old_cfg->output_calib_k ||
		    new_cfg->output_calib_b != old_cfg->output_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_VOLTAGE_MEASUREMENT)) {
		if (new_cfg->measured_voltage_calib_k != old_cfg->measured_voltage_calib_k ||
		    new_cfg->measured_voltage_calib_b != old_cfg->measured_voltage_calib_b) {
			return VC_ERR_UNSUPPORTED_CAPABILITY;
		}
	}
	if (!channel_has_cap(ch, CH_CAP_CURRENT_MEASUREMENT)) {
		if (new_cfg->current_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->current_protection_output_action != old_cfg->current_protection_output_action ||
		    new_cfg->current_limit_threshold != old_cfg->current_limit_threshold ||
		    new_cfg->measured_current_calib_k != old_cfg->measured_current_calib_k ||
		    new_cfg->measured_current_calib_b != old_cfg->measured_current_calib_b) {
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

static void apply_protection_action(struct vc_channel *ch,
				    enum vc_output_action action)
{
	ch->last_protection_output_action = action;

	switch (action) {
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		ch->operational_target_voltage = 0;
		ch->output_enabled = false;
		break;
	case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
		ch->output_enabled = false;
		ch->ramping = false;
		ch->raw_dac_readback = 0;
		ch->operational_target_voltage = 0;
		break;
	case VC_OUTPUT_ACTION_DISABLE_GRACEFUL:
		ch->output_enabled = false;
		ch->operational_target_voltage = 0;
		break;
	default:
		break;
	}
	apply_hw(ch);
}

static void tick_current_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
		return;
	}
	if (ch->ramping) {
		return;
	}
	if (cfg->current_protection_mode == VC_PROTECTION_MODE_DISABLED) {
		return;
	}
	if (ch->measured_current <= cfg->current_limit_threshold) {
		return;
	}

	ch->fault_history_cause |= VC_FAULT_CURRENT;

	if (cfg->current_protection_mode != VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION) {
		return;
	}

	ch->active_fault_cause |= VC_FAULT_CURRENT;
	ch->last_fault_timestamp = ch->uptime_ref;
	apply_protection_action(ch, cfg->current_protection_output_action);
	set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
}

static void force_safe_state(struct vc_channel *ch)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->cal_output_enabled = 0;
	ch->raw_dac_readback = 0;
	ch->operational_target_voltage = 0;
	set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	apply_hw(ch);
}

static bool is_safe_to_clear_active(const struct vc_channel *ch,
				    const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int32_t safe_limit;

	if (ch->active_fault_cause & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)sys_cfg->current_safe_band_pct) / 100;
		if (ch->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}

/* ---- Measurement callback — registered with hw driver ---- */

static void vc_channel_meas_ready(uint8_t channel, void *user_data)
{
	struct vc_channel *ch = user_data;

	ARG_UNUSED(channel);
	if (ch->wake_fn) {
		ch->wake_fn(ch->wake_user_data);
	}
}

/* ---- Public API ---- */

void vc_channel_init(struct vc_channel *ch,
		     const struct device *dev,
		     uint8_t index, uint16_t caps,
		     struct vc_meas_buffer *meas,
		     vc_wake_fn_t wake_fn, void *wake_user_data)
{
	memset(ch, 0, sizeof(*ch));
	ch->dev = dev;
	ch->index = index;
	ch->capabilities = caps;
	ch->meas = meas;
	ch->wake_fn = wake_fn;
	ch->wake_user_data = wake_user_data;
	ch->config = default_channel_config();
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	smf_set_initial(SMF_CTX(ch), &vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);

	if (dev != NULL && dev->api != NULL) {
		const struct vc_channel_hw_api *api = dev->api;

		if (api->set_meas_callback) {
			api->set_meas_callback(dev, vc_channel_meas_ready, ch);
		}
	}
}

void vc_channel_run(struct vc_channel *ch, uint32_t dt_ms,
		    const struct vc_system_config *sys_cfg)
{
	if (ch->meas != NULL) {
		if (ch->meas->voltage_timestamp_ms != ch->last_consumed_voltage_ts) {
			vc_channel_consume_voltage(ch, ch->meas->raw_voltage);
			ch->last_consumed_voltage_ts = ch->meas->voltage_timestamp_ms;
		}
		if (ch->meas->current_timestamp_ms != ch->last_consumed_current_ts) {
			vc_channel_consume_current(ch, ch->meas->raw_current);
			ch->last_consumed_current_ts = ch->meas->current_timestamp_ms;
		}
	}

	vc_channel_tick_ramp(ch, dt_ms, sys_cfg);
}

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch)
{
	const struct smf_ctx *ctx = SMF_CTX((struct vc_channel *)ch);

	return (enum vc_channel_smf_state)(ctx->current - &vc_channel_states[0]);
}

enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg)
{
	*cfg = ch->config;
	return VC_OK;
}

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode)
{
	enum vc_status st;

	st = validate_capability_config(ch, &ch->config, cfg);
	if (st != VC_OK) {
		return st;
	}
	if (!calibration_mode && calibration_fields_changed(&ch->config, cfg)) {
		return VC_ERR_INVALID_COMMAND;
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
	if (cfg->save_target_policy > 1) {
		return VC_ERR_INVALID_VALUE;
	}

	ch->config = *cfg;
	if (!calibration_mode) {
		tick_current_protection(ch);
	}
	apply_hw(ch);
	update_status_bits(ch);
	return VC_OK;
}

enum vc_status vc_channel_set_field(struct vc_channel *ch,
				    enum vc_config_field field, uint16_t value,
				    bool calibration_mode)
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
	case VC_FIELD_SAVE_TARGET_POLICY:
		cfg.save_target_policy = value;
		break;
	case VC_FIELD_OUTPUT_CAL_K:
		cfg.output_calib_k = value;
		break;
	case VC_FIELD_OUTPUT_CAL_B:
		cfg.output_calib_b = (int16_t)value;
		break;
	case VC_FIELD_MEASURED_V_CAL_K:
		cfg.measured_voltage_calib_k = value;
		break;
	case VC_FIELD_MEASURED_V_CAL_B:
		cfg.measured_voltage_calib_b = (int16_t)value;
		break;
	case VC_FIELD_MEASURED_I_CAL_K:
		cfg.measured_current_calib_k = value;
		break;
	case VC_FIELD_MEASURED_I_CAL_B:
		cfg.measured_current_calib_b = (int16_t)value;
		break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_channel_set_config(ch, &cfg, calibration_mode);
}

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
	snap->cal_sample_status = ch->cal_sample_status;
	snap->raw_dac_readback = ch->raw_dac_readback;
	snap->cal_output_enabled = ch->cal_output_enabled;
	snap->cal_max_raw_dac_limit = ch->cal_max_raw_dac_limit;
}

enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode)
{
	if (!is_valid_host_output_action(action)) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (calibration_mode) {
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

enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg)
{
	if (!is_valid_channel_fault_command(cmd)) {
		return VC_ERR_INVALID_COMMAND;
	}

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (ch->active_fault_cause == 0) {
			break;
		}
		if (!is_safe_to_clear_active(ch, sys_cfg)) {
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

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage)
{
	ch->raw_adc_voltage = raw_voltage;
	ch->measured_voltage = clamp_int16(
		((int64_t)raw_voltage * ch->config.measured_voltage_calib_k) /
		10000 + ch->config.measured_voltage_calib_b);

	update_status_bits(ch);
}

void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current)
{
	ch->raw_adc_current = raw_current;
	ch->measured_current = clamp_int16(
		((int64_t)raw_current * ch->config.measured_current_calib_k) /
		10000 + ch->config.measured_current_calib_b);

	if (!ch->ramping) {
		tick_current_protection(ch);
	}
	update_status_bits(ch);
}

void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause)
{
	ch->active_fault_cause |= fault_cause;
	ch->fault_history_cause |= fault_cause;
	force_safe_state(ch);
	update_status_bits(ch);
}

void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int16_t target, current;
	uint16_t step, interval;
	uint32_t interval_ms;

	ARG_UNUSED(sys_cfg);

	if (!ch->output_enabled || ch->active_fault_cause != 0) {
		return;
	}

	target = cfg->configured_target_voltage;
	current = ch->operational_target_voltage;

	if (current == target) {
		if (ch->ramping) {
			ch->ramping = false;
			set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		}
		return;
	}

	ch->ramping = true;

	if (current < target) {
		step = cfg->ramp_up_step;
		interval = cfg->ramp_up_interval;
	} else {
		step = cfg->ramp_down_step;
		interval = cfg->ramp_down_interval;
	}

	if (step == 0 || interval == 0) {
		ch->operational_target_voltage = target;
		ch->ramping = false;
		set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		apply_hw(ch);
		update_status_bits(ch);
		return;
	}

	interval_ms = (uint32_t)interval * 100;
	ch->ramp_accum_ms += dt_ms;

	while (ch->ramp_accum_ms >= interval_ms && current != target) {
		ch->ramp_accum_ms -= interval_ms;
		if (current < target) {
			current += (int16_t)step;
			if (current > target) {
				current = target;
			}
		} else {
			current -= (int16_t)step;
			if (current < target) {
				current = target;
			}
		}
	}

	ch->operational_target_voltage = current;
	if (current == target) {
		ch->ramping = false;
		set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
	}
	apply_hw(ch);
	update_status_bits(ch);
}

/* ---- Calibration ---- */

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->ramp_accum_ms = 0;
	ch->cooldown_remaining_ms = 0;
	ch->operational_target_voltage = 0;
	ch->measured_voltage = 0;
	ch->measured_current = 0;
	ch->raw_dac_readback = 0;
	ch->cal_output_enabled = 0;
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	ch->raw_adc_voltage = 0;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_NONE;

	if (entering) {
		set_smf_state(ch, VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
	} else {
		set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	}
	apply_hw(ch);
	update_status_bits(ch);
}

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}

	if (enable) {
		if (has_hard_safety_fault(ch)) {
			return VC_ERR_UNSAFE_STATE;
		}
		ch->cal_output_enabled = 1;
	} else {
		ch->cal_output_enabled = 0;
		ch->raw_dac_readback = 0;
		ch->raw_adc_voltage = 0;
		ch->raw_adc_current = 0;
		ch->cal_sample_status = VC_CAL_SAMPLE_NONE;
	}
	apply_hw(ch);
	return VC_OK;
}

enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if (code > ch->cal_max_raw_dac_limit) {
		return VC_ERR_INVALID_VALUE;
	}
	if (code != 0 && has_hard_safety_fault(ch)) {
		return VC_ERR_UNSAFE_STATE;
	}
	if (code != 0 && !ch->cal_output_enabled) {
		return VC_ERR_UNSAFE_STATE;
	}
	ch->raw_dac_readback = code;
	apply_hw(ch);
	return VC_OK;
}

enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit)
{
	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	if (limit > VC_DEFAULT_MAX_RAW_DAC) {
		return VC_ERR_INVALID_VALUE;
	}
	if (limit < ch->raw_dac_readback) {
		return VC_ERR_UNSAFE_STATE;
	}
	ch->cal_max_raw_dac_limit = limit;
	return VC_OK;
}

enum vc_status vc_channel_cal_sample(struct vc_channel *ch)
{
	if ((ch->capabilities &
	     (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)) == 0) {
		return VC_ERR_UNSUPPORTED_CAPABILITY;
	}
	ch->raw_adc_voltage = ch->raw_dac_readback;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_VALID;
	return VC_OK;
}

enum vc_status vc_channel_cal_commit(struct vc_channel *ch)
{
	if (ch->cal_output_enabled || ch->raw_dac_readback != 0 ||
	    has_hard_safety_fault(ch)) {
		return VC_ERR_UNSAFE_STATE;
	}
	return VC_OK;
}
```

- [ ] **Step 2: Commit implementation (tests not updated yet — will fail)**

```bash
git add lib/voltage_control/vc_channel_state.c include/voltage_control/vc_channel_state.h
git commit -m "refactor(vc): vc_channel_state — single-writer, apply_hw replaces set_pending, remove spinlock"
```

---

### Task 5: Update `vc_channel_state` Tests

**Files:**
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`
- Modify: `tests/voltage_control/vc_channel_state/boards/native_posix.overlay` (add stub channel DTS nodes)
- Modify: `tests/voltage_control/vc_channel_state/CMakeLists.txt` (add stub driver source)
- Modify: `tests/voltage_control/vc_channel_state/prj.conf`

Tests must be updated for the new `vc_channel_init` signature (adds `dev, meas, wake_fn, wake_user_data`), removed `pending` field, and the new `vc_channel_cal_set_output_enable` signature (removed `all_channels, count`).

- [ ] **Step 1: Add DTS overlay for vc_channel_state tests**

Create `tests/voltage_control/vc_channel_state/boards/native_posix.overlay`:

```dts
#include <dt-bindings/voltage_control/capabilities.h>

/ {
	vc_controller: vc-controller {
		compatible = "jianwei,vc-controller";
		#address-cells = <1>;
		#size-cells = <0>;
		status = "okay";

		vc_ch0: channel@0 {
			compatible = "jianwei,vc-channel-stub";
			reg = <0>;
			label = "CH0";
			capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
					 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
		};

		vc_ch1: channel@1 {
			compatible = "jianwei,vc-channel-stub";
			reg = <1>;
			label = "CH1";
			capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
					 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
		};
	};
};
```

- [ ] **Step 2: Add stub driver source and update CMakeLists**

```bash
cp tests/voltage_control/vc/src/vc_channel_stub.c tests/voltage_control/vc_channel_state/src/vc_channel_stub.c
```

Update `tests/voltage_control/vc_channel_state/CMakeLists.txt` — no change needed (it already GLOBs `src/*.c`).

- [ ] **Step 3: Update prj.conf**

Replace `tests/voltage_control/vc_channel_state/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_TEST_RANDOM_GENERATOR=y
CONFIG_SMF=y
```

(Removed `CONFIG_SPIN_VALIDATE=n` — no longer needed since there are no spinlocks.)

- [ ] **Step 4: Write the updated tests**

Replace `tests/voltage_control/vc_channel_state/src/main.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "voltage_control/vc_channel_state.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

static struct vc_channel ch;
static const struct vc_system_config default_sys = {
	.current_safe_band_pct = 10,
};
static int wake_count;

static void test_wake_fn(void *user_data)
{
	ARG_UNUSED(user_data);
	wake_count++;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	wake_count = 0;
	vc_channel_init(&ch, NULL, 0, FULL_CAPS, NULL, test_wake_fn, &ch);
}

ZTEST_SUITE(vc_channel_state, NULL, NULL, before_each, NULL, NULL);

/* ---- Init + defaults ---- */

ZTEST(vc_channel_state, test_init_defaults)
{
	zassert_equal(ch.index, 0);
	zassert_equal(ch.capabilities, FULL_CAPS);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(ch.active_fault_cause, 0);
	zassert_equal(ch.measured_voltage, 0);
	zassert_equal(ch.measured_current, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_is_null(ch.dev);
	zassert_is_null(ch.meas);
}

ZTEST(vc_channel_state, test_init_with_device)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, test_wake_fn, &hw_ch);
	zassert_equal(hw_ch.dev, dev);
}

ZTEST(vc_channel_state, test_default_config)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_get_config(&ch, &cfg), VC_OK);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.output_calib_k, 10000);
	zassert_equal(cfg.output_calib_b, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 10000);
	zassert_equal(cfg.measured_current_calib_k, 10000);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.current_protection_mode, VC_PROTECTION_MODE_DISABLED);
}

ZTEST(vc_channel_state, test_snapshot_defaults)
{
	struct vc_channel_snapshot snap;

	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.measured_voltage, 0);
	zassert_equal(snap.measured_current, 0);
	zassert_equal(snap.active_fault_cause, 0);
	zassert_equal(snap.fault_history_cause, 0);
	zassert_equal(snap.status_bits, 0);
	zassert_equal(snap.channel_capability_flags, FULL_CAPS);
	zassert_equal(snap.cal_max_raw_dac_limit, 0xFFFF);
}

/* ---- Output action ---- */

ZTEST(vc_channel_state, test_output_action_enable)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false), VC_OK);
	zassert_true(ch.output_enabled);
	zassert_true(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RAMPING);
}

ZTEST(vc_channel_state, test_output_action_disable_immediate)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE, false),
		      VC_OK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_channel_state, test_output_action_rejected_in_calibration)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, true),
		      VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_output_action_rejected_with_active_fault)
{
	ch.active_fault_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(vc_channel_state, test_output_action_invalid_host_action)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO, false),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Fault command ---- */

ZTEST(vc_channel_state, test_fault_command_clear_history)
{
	ch.fault_history_cause = VC_FAULT_CURRENT;
	zassert_equal(vc_channel_fault_command(&ch, VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY,
					       &default_sys), VC_OK);
	zassert_equal(ch.fault_history_cause, 0);
}

ZTEST(vc_channel_state, test_fault_command_invalid)
{
	zassert_equal(vc_channel_fault_command(&ch, 3, &default_sys),
		      VC_ERR_INVALID_COMMAND);
}

/* ---- Consume voltage ---- */

ZTEST(vc_channel_state, test_consume_voltage_applies_calibration)
{
	vc_channel_consume_voltage(&ch, 1200);
	zassert_equal(ch.measured_voltage, 1200);
	zassert_equal(ch.raw_adc_voltage, 1200);
}

ZTEST(vc_channel_state, test_consume_voltage_with_calibration_gain)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.measured_voltage_calib_k = 20000;
	vc_channel_set_config(&ch, &cfg, true);

	vc_channel_consume_voltage(&ch, 100);
	zassert_equal(ch.measured_voltage, 200);
}

ZTEST(vc_channel_state, test_consume_voltage_clamps_to_int16)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.measured_voltage_calib_k = 65535;
	vc_channel_set_config(&ch, &cfg, true);

	vc_channel_consume_voltage(&ch, 20000);
	zassert_equal(ch.measured_voltage, INT16_MAX);
}

/* ---- Consume current ---- */

ZTEST(vc_channel_state, test_consume_current_applies_calibration)
{
	vc_channel_consume_current(&ch, 500);
	zassert_equal(ch.measured_current, 500);
	zassert_equal(ch.raw_adc_current, 500);
}

ZTEST(vc_channel_state, test_current_protection_triggers_fault)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	vc_channel_tick_ramp(&ch, 100, &default_sys);

	vc_channel_consume_current(&ch, 200);

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_false(ch.output_enabled);
}

/* ---- Consume fault ---- */

ZTEST(vc_channel_state, test_consume_fault_sets_hardware_fault)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);
	zassert_true(ch.fault_history_cause & VC_FAULT_HARDWARE);
	zassert_false(ch.output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

ZTEST(vc_channel_state, test_consume_fault_interlock)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_consume_fault(&ch, VC_FAULT_INTERLOCK);

	zassert_true(ch.active_fault_cause & VC_FAULT_INTERLOCK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
}

/* ---- Tick ramp ---- */

ZTEST(vc_channel_state, test_tick_ramp_instant)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
}

ZTEST(vc_channel_state, test_tick_ramp_gradual)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 100);
	zassert_true(ch.ramping);
}

ZTEST(vc_channel_state, test_tick_ramp_no_output_enabled)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	vc_channel_set_config(&ch, &cfg, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);

	zassert_equal(ch.operational_target_voltage, 0);
}

ZTEST(vc_channel_state, test_tick_ramp_at_target_stops_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 100;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_equal(ch.operational_target_voltage, 100);
	zassert_false(ch.ramping);
}

/* ---- Set config validation ---- */

ZTEST(vc_channel_state, test_set_config_target_voltage_range)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 20000;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_OK);

	cfg.configured_target_voltage = 20001;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_VALUE);

	cfg.configured_target_voltage = -1;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_set_config_calibration_blocked_outside_cal_mode)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.output_calib_k++;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_set_config_calibration_allowed_in_cal_mode)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.output_calib_k = 20000;
	cfg.output_calib_b = 5;
	zassert_equal(vc_channel_set_config(&ch, &cfg, true), VC_OK);
	vc_channel_get_config(&ch, &cfg);
	zassert_equal(cfg.output_calib_k, 20000);
	zassert_equal(cfg.output_calib_b, 5);
}

ZTEST(vc_channel_state, test_set_field_target_voltage)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					   5000, false), VC_OK);
	vc_channel_get_config(&ch, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_channel_state, test_set_field_rejects_system_field)
{
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_OPERATING_MODE, 0, false),
		      VC_ERR_INVALID_VALUE);
}

ZTEST(vc_channel_state, test_onoff_channel_rejects_output_drive_config)
{
	struct vc_channel onoff_ch;
	struct vc_channel_config cfg;

	vc_channel_init(&onoff_ch, NULL, 0, CH_CAP_OUTPUT_ENABLE, NULL, NULL, NULL);
	vc_channel_get_config(&onoff_ch, &cfg);
	cfg.configured_target_voltage = 100;
	zassert_equal(vc_channel_set_config(&onoff_ch, &cfg, false),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}

/* ---- Calibration ---- */

ZTEST(vc_channel_state, test_cal_set_output_enable)
{
	zassert_equal(vc_channel_cal_set_output_enable(&ch, true), VC_OK);
	zassert_equal(ch.cal_output_enabled, 1);
}

ZTEST(vc_channel_state, test_cal_set_raw_dac_requires_output_enabled)
{
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_OK);
	zassert_equal(ch.raw_dac_readback, 100);
}

ZTEST(vc_channel_state, test_cal_max_raw_dac_limit)
{
	vc_channel_cal_set_max_raw_dac(&ch, 50);
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 51), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 50), VC_OK);
}

ZTEST(vc_channel_state, test_cal_sample_captures_raw)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 123);
	zassert_equal(vc_channel_cal_sample(&ch), VC_OK);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_VALID);
	zassert_equal(ch.raw_adc_voltage, 123);
}

ZTEST(vc_channel_state, test_cal_commit_requires_output_disabled)
{
	vc_channel_cal_set_output_enable(&ch, true);
	zassert_equal(vc_channel_cal_commit(&ch), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, false);
	zassert_equal(vc_channel_cal_commit(&ch), VC_OK);
}

ZTEST(vc_channel_state, test_cal_disable_clears_sample_state)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 50);
	vc_channel_cal_sample(&ch);
	vc_channel_cal_set_output_enable(&ch, false);
	zassert_equal(ch.raw_adc_voltage, 0);
	zassert_equal(ch.raw_adc_current, 0);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_NONE);
}

ZTEST(vc_channel_state, test_reset_calibration_entering)
{
	vc_channel_cal_set_output_enable(&ch, true);
	vc_channel_cal_set_raw_dac(&ch, 100);
	vc_channel_reset_calibration(&ch, true);

	zassert_equal(ch.raw_dac_readback, 0);
	zassert_equal(ch.cal_output_enabled, 0);
	zassert_equal(ch.cal_max_raw_dac_limit, 0xFFFF);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_CALIBRATION_OUTPUT);
}

ZTEST(vc_channel_state, test_reset_calibration_exiting)
{
	vc_channel_reset_calibration(&ch, false);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
}

/* ---- vc_channel_run ---- */

ZTEST(vc_channel_state, test_run_consumes_meas_buffer)
{
	struct vc_meas_buffer meas = {
		.channel_id = 0,
		.raw_voltage = 1500,
		.voltage_timestamp_ms = 100,
		.raw_current = 300,
		.current_timestamp_ms = 100,
	};
	struct vc_channel run_ch;

	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, &meas, NULL, NULL);

	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 1500);
	zassert_equal(run_ch.measured_current, 300);
	zassert_equal(run_ch.last_consumed_voltage_ts, 100);
	zassert_equal(run_ch.last_consumed_current_ts, 100);
}

ZTEST(vc_channel_state, test_run_skips_unchanged_timestamps)
{
	struct vc_meas_buffer meas = {
		.channel_id = 0,
		.raw_voltage = 1500,
		.voltage_timestamp_ms = 100,
		.raw_current = 300,
		.current_timestamp_ms = 100,
	};
	struct vc_channel run_ch;

	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, &meas, NULL, NULL);
	vc_channel_run(&run_ch, 100, &default_sys);

	meas.raw_voltage = 9999;
	vc_channel_run(&run_ch, 100, &default_sys);

	zassert_equal(run_ch.measured_voltage, 1500,
		      "should not re-consume with same timestamp");
}

ZTEST(vc_channel_state, test_run_null_meas_is_safe)
{
	struct vc_channel run_ch;

	vc_channel_init(&run_ch, NULL, 0, FULL_CAPS, NULL, NULL, NULL);
	vc_channel_run(&run_ch, 100, &default_sys);
	zassert_equal(run_ch.measured_voltage, 0);
}

/* ---- apply_hw with stub driver ---- */

ZTEST(vc_channel_state, test_apply_hw_calls_driver)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, NULL, NULL);

	struct vc_channel_config cfg;

	vc_channel_get_config(&hw_ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&hw_ch, &cfg, false);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_ENABLE, false);
	vc_channel_tick_ramp(&hw_ch, 100, &default_sys);

	/* Verify stub driver recorded the hw calls */
	struct vc_stub_data *stub = dev->data;

	zassert_true(stub->last_enable, "hw should be enabled");
	zassert_equal(stub->last_output_code, 5000, "hw should have DAC code 5000");
}

ZTEST(vc_channel_state, test_apply_hw_disable_zeros_output)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, NULL, NULL);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_ENABLE, false);
	vc_channel_output_action(&hw_ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE, false);

	struct vc_stub_data *stub = dev->data;

	zassert_false(stub->last_enable);
	zassert_equal(stub->last_output_code, 0);
}

ZTEST(vc_channel_state, test_current_protection_skipped_during_ramping)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.current_limit_threshold = 100;
	cfg.current_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.current_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 100;
	cfg.ramp_up_interval = 1;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);

	vc_channel_tick_ramp(&ch, 100, &default_sys);
	zassert_true(ch.ramping);

	vc_channel_consume_current(&ch, 200);

	zassert_equal(ch.active_fault_cause, 0,
		      "current protection must not fire during ramping");
	zassert_equal(ch.fault_history_cause, 0,
		      "current protection must not flag during ramping");
}

/* ---- Meas callback registration ---- */

ZTEST(vc_channel_state, test_meas_callback_registered_with_hw)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vc_ch0));
	struct vc_channel hw_ch;

	vc_channel_init(&hw_ch, dev, 0, FULL_CAPS, NULL, test_wake_fn, &hw_ch);

	struct vc_stub_data *stub = dev->data;

	zassert_not_null(stub->meas_cb, "init should register meas callback on hw");

	wake_count = 0;
	stub->meas_cb(0, stub->meas_cb_user_data);
	zassert_equal(wake_count, 1, "meas callback should invoke wake_fn");
}
```

- [ ] **Step 5: The test references `struct vc_stub_data` — add forward declaration**

Add at the top of the test file, after the includes:

```c
/* From vc_channel_stub.c — access stub internals for verification */
struct vc_stub_data {
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	uint16_t last_output_code;
	bool last_enable;
};
```

(This duplicates the struct definition from the stub driver for test access. In Zephyr test patterns, this is common practice.)

- [ ] **Step 6: Run vc_channel_state tests**

Run: `west twister -T tests/voltage_control/vc_channel_state --no-clean -p native_posix`

Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add tests/voltage_control/vc_channel_state/
git commit -m "test(vc): update vc_channel_state tests for single-writer API"
```

---

### Task 6: Rewrite `vc_controller` — Thin Wrapper + DTS Construction + Central Meas Buffer

**Files:**
- Modify: `include/voltage_control/vc_controller.h`
- Modify: `lib/voltage_control/vc_controller.c`
- Modify: `include/voltage_control/vc_types.h` (remove `vc_channel_entry`)
- Modify: `lib/voltage_control/CMakeLists.txt`
- Delete: `include/voltage_control/vc_channel_table.h`
- Delete: `lib/voltage_control/vc_channel_table.c`
- Delete: `include/voltage_control/vc_channel.h`

This is the largest single task. The controller becomes a thin wrapper that routes commands and orchestrates collective mode changes. DTS channel construction and the central measurement buffer definition move here.

- [ ] **Step 1: Write the new controller header**

Replace `include/voltage_control/vc_controller.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CONTROLLER_H
#define VOLTAGE_CONTROL_VC_CONTROLLER_H

#include "voltage_control/vc_channel_state.h"
#include "voltage_control/vc_storage.h"

struct vc_controller {
	struct vc_channel channels[VC_MAX_CHANNELS];
	size_t channel_count;
	struct vc_meas_buffer *meas_index[VC_MAX_CHANNELS];
	enum vc_operating_mode operating_mode;
	uint8_t cal_unlock_step;
	bool cal_unlocked;
	const struct vc_storage_backend *storage;
	struct vc_system_config sys_cfg;
};

struct vc_controller *vc_controller_init(
	vc_wake_fn_t wake_fn, void *wake_user_data);

enum vc_status vc_controller_set_operating_mode(
	struct vc_controller *ctrl, enum vc_operating_mode mode);
enum vc_operating_mode vc_controller_get_operating_mode(
	const struct vc_controller *ctrl);
enum vc_status vc_controller_calibration_unlock(
	struct vc_controller *ctrl, uint16_t value);

enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value);
enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action);
enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd);

void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms);

enum vc_status vc_controller_system_param_action(
	struct vc_controller *ctrl, enum vc_param_action action);
enum vc_status vc_controller_channel_param_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_param_action action);

enum vc_status vc_controller_get_system_config(
	const struct vc_controller *ctrl, struct vc_system_config *cfg);
enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg);
void vc_controller_get_system_snapshot(
	const struct vc_controller *ctrl, struct vc_system_snapshot *snap);
enum vc_status vc_controller_get_channel_snapshot(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_snapshot *snap);
enum vc_status vc_controller_get_channel_config(
	const struct vc_controller *ctrl, uint8_t ch,
	struct vc_channel_config *cfg);

enum vc_status vc_controller_set_system_field(
	struct vc_controller *ctrl, enum vc_config_field field, uint16_t value);

enum vc_status vc_controller_channel_cal_output_enable(
	struct vc_controller *ctrl, uint8_t ch, bool enable);
enum vc_status vc_controller_channel_cal_raw_dac(
	struct vc_controller *ctrl, uint8_t ch, uint16_t code);
enum vc_status vc_controller_channel_cal_sample(
	struct vc_controller *ctrl, uint8_t ch);
enum vc_status vc_controller_channel_cal_commit(
	struct vc_controller *ctrl, uint8_t ch);
enum vc_status vc_controller_channel_cal_max_raw_dac(
	struct vc_controller *ctrl, uint8_t ch, uint16_t limit);

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend);

enum vc_status vc_controller_start_sampling(struct vc_controller *ctrl);

#endif
```

Key changes from old header:
- `vc_controller_init` replaces `vc_controller_init_static` — takes `(wake_fn, wake_user_data)` instead of `(entries, count)`, reads from DTS internally
- Removed: `vc_controller_consume_voltage/current/fault` — vc_channel_run handles measurement consumption
- Removed: `vc_controller_channel_count`, `vc_controller_channel_capabilities` — access `ctrl->channel_count` / `ctrl->channels[ch].capabilities` directly
- Added: `vc_controller_start_sampling` — iterates channels and starts hw sampling

- [ ] **Step 2: Remove `vc_channel_entry` from vc_types.h**

In `include/voltage_control/vc_types.h`, remove the `vc_channel_entry` struct definition (lines 17-21):

```c
struct vc_channel_entry {
	const struct device *dev;
	uint8_t            index;
	uint16_t           capabilities;
};
```

Also remove the `#include <zephyr/device.h>` if it's no longer needed (check other usages in the file — it's still needed for `DT_CHILD_NUM_STATUS_OKAY` macro).

- [ ] **Step 3: Write the new controller implementation**

Replace `lib/voltage_control/vc_controller.c`. This is the complete file — it includes DTS construction, central measurement buffer definition, and the thin wrapper functions.

Due to the length limitation, the key sections are:

**Central measurement buffer (top of file):**
```c
#ifdef CONFIG_VC_CHANNEL_CONTROLLER
#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define MEAS_ENTRY(node_id)                                             \
	STRUCT_SECTION_ITERABLE(vc_meas_buffer,                         \
		VC_MEAS_BUFFER_NAME(node_id)) = {                       \
		.channel_id = DT_REG_ADDR(node_id),                     \
	};

DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, MEAS_ENTRY)
#endif
```

**Init function:**
```c
struct vc_controller *vc_controller_init(
	vc_wake_fn_t wake_fn, void *wake_user_data)
{
	static struct vc_controller ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.operating_mode = VC_OPERATING_MODE_NORMAL;
	ctrl.sys_cfg = default_system_config();

#ifdef CONFIG_VC_CHANNEL_CONTROLLER
	ctrl.channel_count = DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE);

	STRUCT_SECTION_FOREACH(vc_meas_buffer, entry) {
		if (entry->channel_id < VC_MAX_CHANNELS) {
			ctrl.meas_index[entry->channel_id] = entry;
		}
	}

	size_t i = 0;
#define INIT_CHANNEL(node_id)                                            \
	vc_channel_init(&ctrl.channels[DT_REG_ADDR(node_id)],           \
			DEVICE_DT_GET(node_id),                          \
			DT_REG_ADDR(node_id),                            \
			DT_PROP(node_id, capabilities),                  \
			ctrl.meas_index[DT_REG_ADDR(node_id)],          \
			wake_fn, wake_user_data);                        \
	i++;

	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, INIT_CHANNEL)
#else
	ctrl.channel_count = 0;
#endif

	return &ctrl;
}
```

**Tick — delegates to vc_channel_run:**
```c
void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms)
{
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		vc_channel_run(&ctrl->channels[i], dt_ms, &ctrl->sys_cfg);
	}
}
```

**Thin wrappers (example):**
```c
enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;
	return vc_channel_output_action(&ctrl->channels[ch], action, cal);
}
```

**Cross-channel cal check (moved from vc_channel_state.c):**
```c
enum vc_status vc_controller_channel_cal_output_enable(
	struct vc_controller *ctrl, uint8_t ch, bool enable)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	if (enable) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			if (i == (size_t)ch) {
				continue;
			}
			if (ctrl->channels[i].cal_output_enabled ||
			    ctrl->channels[i].raw_dac_readback != 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	return vc_channel_cal_set_output_enable(&ctrl->channels[ch], enable);
}
```

**Start sampling:**
```c
enum vc_status vc_controller_start_sampling(struct vc_controller *ctrl)
{
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		const struct device *dev = ctrl->channels[i].dev;
		if (dev == NULL || dev->api == NULL) {
			continue;
		}
		const struct vc_channel_hw_api *api = dev->api;
		if (api->start_sampling) {
			int ret = api->start_sampling(dev);
			if (ret < 0 && ret != -ENOTSUP) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	return VC_OK;
}
```

All other functions (set_operating_mode, calibration_unlock, param_action, system config, snapshots, storage) remain structurally the same as the current version but without any `drain_pending()` calls.

- [ ] **Step 4: Delete dead files**

```bash
rm include/voltage_control/vc_channel_table.h
rm lib/voltage_control/vc_channel_table.c
rm include/voltage_control/vc_channel.h
```

- [ ] **Step 5: Update CMakeLists.txt**

In `lib/voltage_control/CMakeLists.txt`, remove the `vc_channel_table.c` line:

```
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER vc_channel_table.c)
```

- [ ] **Step 6: Update Kconfig — remove VC_CHANNEL_PROVIDER**

In `lib/voltage_control/Kconfig`, remove `VC_CHANNEL_PROVIDER` config (it was only used by the deleted channel table/provider system). Update `VC_CHANNEL_CONTROLLER` to not select it.

- [ ] **Step 7: Commit**

```bash
git add include/voltage_control/vc_controller.h include/voltage_control/vc_types.h
git add lib/voltage_control/vc_controller.c lib/voltage_control/CMakeLists.txt
git add lib/voltage_control/Kconfig
git rm include/voltage_control/vc_channel_table.h lib/voltage_control/vc_channel_table.c
git rm include/voltage_control/vc_channel.h
git commit -m "refactor(vc): vc_controller thin wrapper, DTS construction, central meas buffer"
```

---

### Task 7: Update `vc_controller` Tests

**Files:**
- Modify: `tests/voltage_control/vc_controller/src/main.c`
- Modify: `tests/voltage_control/vc_controller/prj.conf`

The test init changes from `vc_controller_init_static(entries, count)` to `vc_controller_init(wake_fn, user_data)`. Tests using `vc_controller_consume_*` and `pending.valid` are removed or rewritten. The test now relies on DTS overlay (already exists with stub channels).

- [ ] **Step 1: Update prj.conf**

Replace `tests/voltage_control/vc_controller/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_TEST_RANDOM_GENERATOR=y
CONFIG_SMF=y
```

- [ ] **Step 2: Write updated tests**

Replace `tests/voltage_control/vc_controller/src/main.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include "voltage_control/vc_controller.h"
#include "regmap/vc_regs.h"
#include <dt-bindings/voltage_control/capabilities.h>

#define FULL_CAPS (CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE | \
		   CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)

static struct vc_controller *ctrl;
static int wake_count;

static void test_wake_fn(void *user_data)
{
	ARG_UNUSED(user_data);
	wake_count++;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	wake_count = 0;
	ctrl = vc_controller_init(test_wake_fn, NULL);
	zassert_not_null(ctrl);
}

ZTEST_SUITE(vc_controller, NULL, NULL, before_each, NULL, NULL);

ZTEST(vc_controller, test_init)
{
	zassert_equal(ctrl->channel_count, 2);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_channels_have_devices)
{
	zassert_not_null(ctrl->channels[0].dev);
	zassert_not_null(ctrl->channels[1].dev);
}

ZTEST(vc_controller, test_channel_output_action_routes)
{
	zassert_equal(vc_controller_channel_output_action(ctrl, 0,
		VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_true(ctrl->channels[0].output_enabled);
	zassert_false(ctrl->channels[1].output_enabled);
}

ZTEST(vc_controller, test_channel_output_action_rejects_invalid_channel)
{
	zassert_equal(vc_controller_channel_output_action(ctrl, 5,
		VC_OUTPUT_ACTION_ENABLE), VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_controller, test_tick_ramps)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_controller_tick(ctrl, 100);

	zassert_equal(ctrl->channels[0].operational_target_voltage, 1000);
}

ZTEST(vc_controller, test_calibration_unlock_and_mode_entry)
{
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1),
		      VC_OK);
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2),
		      VC_OK);
	zassert_equal(vc_controller_set_operating_mode(ctrl,
		VC_OPERATING_MODE_CALIBRATION), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(vc_controller, test_calibration_rejected_without_unlock)
{
	zassert_equal(vc_controller_set_operating_mode(ctrl,
		VC_OPERATING_MODE_CALIBRATION), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_controller, test_system_snapshot)
{
	struct vc_system_snapshot snap;

	vc_controller_get_system_snapshot(ctrl, &snap);
	zassert_equal(snap.protocol_major, VC_PROTOCOL_MAJOR);
	zassert_equal(snap.protocol_minor, VC_PROTOCOL_MINOR);
	zassert_equal(snap.supported_channel_count, 2);
	zassert_equal(snap.active_channel_mask, 0x0003);
}

ZTEST(vc_controller, test_system_config_defaults)
{
	struct vc_system_config cfg;

	vc_controller_get_system_config(ctrl, &cfg);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
}

ZTEST(vc_controller, test_channel_set_field_routes)
{
	struct vc_channel_config cfg;

	zassert_equal(vc_controller_channel_set_field(ctrl, 0,
		VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 5000), VC_OK);
	vc_controller_get_channel_config(ctrl, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);
}

ZTEST(vc_controller, test_channel_set_field_rejects_invalid_channel)
{
	zassert_equal(vc_controller_channel_set_field(ctrl, 99,
		VC_FIELD_CONFIGURED_TARGET_VOLTAGE, 100),
		VC_ERR_UNSUPPORTED_CHANNEL);
}

ZTEST(vc_controller, test_mode_transition_auto_to_normal_clears_cooldown)
{
	struct vc_system_config sys;

	vc_controller_get_system_config(ctrl, &sys);
	sys.operating_mode = VC_OPERATING_MODE_AUTOMATIC;
	sys.recovery_policy_mode = VC_RECOVERY_AUTO_RETRY;
	sys.auto_retry_delay = 60;
	vc_controller_set_system_config(ctrl, &sys);

	ctrl->channels[0].cooldown_remaining_ms = 5000;
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_NORMAL);
	zassert_equal(ctrl->channels[0].cooldown_remaining_ms, 0);
}

ZTEST(vc_controller, test_calibration_entry_resets_channels)
{
	struct vc_channel_snapshot snap;

	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_CALIBRATION);

	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.operational_target_voltage, 0);
	zassert_equal(snap.raw_dac_readback, 0);
	zassert_equal(snap.cal_output_enabled, 0);
}

ZTEST(vc_controller, test_cal_single_output_across_channels)
{
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, true), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 0, false), VC_OK);
	zassert_equal(vc_controller_channel_cal_output_enable(ctrl, 1, true), VC_OK);
}

ZTEST(vc_controller, test_system_param_action_no_storage)
{
	zassert_equal(vc_controller_system_param_action(ctrl, VC_PARAM_ACTION_SAVE),
		      VC_ERR_STORAGE);
	zassert_equal(vc_controller_system_param_action(ctrl, VC_PARAM_ACTION_NONE),
		      VC_OK);
}

ZTEST(vc_controller, test_channel_param_action_no_storage)
{
	zassert_equal(vc_controller_channel_param_action(ctrl, 0, VC_PARAM_ACTION_SAVE),
		      VC_ERR_STORAGE);
}

ZTEST(vc_controller, test_start_sampling)
{
	zassert_equal(vc_controller_start_sampling(ctrl), VC_OK);
}
```

- [ ] **Step 3: Run tests**

Run: `west twister -T tests/voltage_control/vc_controller --no-clean -p native_posix`

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/voltage_control/vc_controller/
git commit -m "test(vc): update vc_controller tests for thin wrapper API"
```

---

### Task 8: Update `vc_runtime` — Single-Writer Measurement Loop

**Files:**
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `include/voltage_control/vc_runtime.h`

The runtime provides the `wake_fn` that vc_channel callbacks invoke. The worker loop calls `vc_controller_tick` which internally calls `vc_channel_run` for each channel. `vc_runtime_submit_measurement` is kept for simulation injection but simplified.

- [ ] **Step 1: Update runtime header**

In `include/voltage_control/vc_runtime.h`, change the create function signatures:

```c
struct vc_runtime *vc_runtime_create_static(void);
struct vc_runtime *vc_runtime_create(void);
```

Remove the `(channels, count)` parameters — the controller reads from DTS internally.

- [ ] **Step 2: Update runtime implementation**

Key changes to `lib/voltage_control/vc_runtime.c`:

**Wake function:**
```c
static void runtime_wake(void *user_data)
{
	struct vc_runtime *runtime = user_data;
	k_sem_give(&runtime->wake);
}
```

**Create function:**
```c
struct vc_runtime *vc_runtime_create_static(void)
{
	static struct vc_runtime runtime;
	struct vc_controller *ctrl;

	ctrl = vc_controller_init(runtime_wake, &runtime);
	if (ctrl == NULL) {
		return NULL;
	}

	memset(&runtime, 0, sizeof(runtime));
	runtime.ctrl = ctrl;
	/* ... rest of init unchanged ... */
}
```

**Worker loop** — simplified, calls `vc_controller_tick` which handles measurement consumption via `vc_channel_run`:

```c
static void vc_runtime_worker(void *p1, void *p2, void *p3)
{
	struct vc_runtime *runtime = p1;
	struct vc_runtime_work_item work;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!runtime->stop_requested) {
		k_sem_take(&runtime->wake,
			   K_MSEC(CONFIG_VC_RUNTIME_TICK_INTERVAL_MS));

		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;
			result = vc_runtime_dispatch_command(runtime, &work.command);
			if (work.command.result != NULL) {
				*work.command.result = result;
			}
			if (work.command.result_sem != NULL) {
				k_sem_give(work.command.result_sem);
			}
		}

		vc_controller_tick(runtime->ctrl,
				   CONFIG_VC_RUNTIME_TICK_INTERVAL_MS);

		vc_runtime_publish_snapshot(runtime);
	}
}
```

Remove the `wake_ret == -EAGAIN` check — `vc_controller_tick` always runs (it's cheap if nothing changed, and measurement consumption uses timestamp checks).

Remove the `#include "voltage_control/vc_channel_table.h"` and `vc_channel_table_init` call.

- [ ] **Step 3: Commit**

```bash
git add lib/voltage_control/vc_runtime.c include/voltage_control/vc_runtime.h
git commit -m "refactor(vc): runtime single-writer — wake_fn, simplified worker loop"
```

---

### Task 9: Simplify `vc.c` Facade

**Files:**
- Modify: `lib/voltage_control/vc.c`
- Modify: `include/voltage_control/vc.h`

Init no longer copies channel entries. `vc_ctx_start` calls `vc_controller_start_sampling`.

- [ ] **Step 1: Simplify vc.c**

```c
struct vc_ctx *vc_init(void)
{
	return init_from_runtime(vc_runtime_create_static());
}

enum vc_status vc_ctx_start(struct vc_ctx *ctx)
{
	if (ctx == NULL) {
		return VC_ERR_INVALID_VALUE;
	}
	/* Controller owns the channels and their device pointers */
	return vc_controller_start_sampling(/* get ctrl from runtime */);
}
```

Remove `#include "voltage_control/vc_channel_table.h"`. Remove the manual channel_entry copy loop from `vc_init()`. Remove `vc_init_custom` or keep it for simulation with a different controller init path.

- [ ] **Step 2: Run all tests**

Run: `west twister -T tests/voltage_control/ --no-clean -p native_posix`

Expected: All 5 suites pass.

- [ ] **Step 3: Commit**

```bash
git add lib/voltage_control/vc.c include/voltage_control/vc.h
git commit -m "refactor(vc): simplify facade — no channel_entry copies, controller owns init"
```

---

### Task 10: Update `hvb_vc_channel` Driver — Remove Upward Dependencies

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`

Remove `#include vc_controller.h` and `#include vc_channel_table.h`. Implement `get_capabilities` and `set_meas_callback`. Use `VC_MEAS_BUFFER_EXTERN`/`PTR` macros. Replace direct `vc_controller_consume_*` calls with writes to shared buffer + callback invocation.

- [ ] **Step 1: Update driver**

Key changes:

```c
/* Remove these includes */
// #include "voltage_control/vc_channel_table.h"
// #include "voltage_control/vc_controller.h"

/* Add to hvb_vc_data: */
struct hvb_vc_data {
	const struct device *dev;
	uint8_t channel;
	struct vc_meas_buffer *meas;
	vc_meas_ready_cb_t meas_cb;
	void *meas_cb_user_data;
	/* ... existing ADC fields ... */
};

/* New API implementations: */
static uint16_t hvb_vc_get_capabilities(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;
	return cfg->capabilities;
}

static int hvb_vc_set_meas_callback(const struct device *dev,
				    vc_meas_ready_cb_t cb, void *user_data)
{
	struct hvb_vc_data *data = dev->data;
	data->meas_cb = cb;
	data->meas_cb_user_data = user_data;
	return 0;
}

/* In hvb_vc_poll_handler, replace vc_controller_consume_* calls: */
if (data->adc_phase == ADC_PHASE_VOLTAGE) {
	data->meas->raw_voltage = raw;
	data->meas->voltage_timestamp_ms = k_uptime_get_32();
	if (data->meas_cb) {
		data->meas_cb(data->channel, data->meas_cb_user_data);
	}
	/* ... */
}

/* In init macro, use centrally defined measurement buffer: */
#define HVB_VC_INIT(n)                                                  \
	VC_MEAS_BUFFER_EXTERN(DT_DRV_INST(n));                         \
	static const struct hvb_vc_config hvb_vc_config_##n = { ... };  \
	static struct hvb_vc_data hvb_vc_data_##n = {                   \
		.meas = VC_MEAS_BUFFER_PTR(DT_DRV_INST(n)),            \
	};                                                              \
	DEVICE_DT_INST_DEFINE(n, ...);
```

- [ ] **Step 2: Build for hardware target**

```bash
west build -b jw_hvb app -- -DEXTRA_CONF_FILE=prj.conf
```

- [ ] **Step 3: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c
git commit -m "refactor(vc): hvb_vc_channel — remove upward deps, use shared meas buffer + callback"
```

---

### Task 11: Final Test Pass + Cleanup

**Files:**
- Verify: all test suites

- [ ] **Step 1: Run full test suite**

Run: `west twister -T tests/voltage_control/ --no-clean -p native_posix`

Expected: All 5 suites pass.

- [ ] **Step 2: Verify no references to deleted files**

```bash
grep -r "vc_channel_table\|vc_channel_entry\|vc_channel_api\|vc_channel\.h" include/ lib/ tests/ drivers/ --include="*.c" --include="*.h"
```

Expected: No matches (other than this plan file if it's in the tree).

- [ ] **Step 3: Verify no spinlock usage in vc_channel_state**

```bash
grep -n "k_spin" lib/voltage_control/vc_channel_state.c
```

Expected: No matches.

- [ ] **Step 4: Verify no drain_pending or set_pending**

```bash
grep -rn "drain_pending\|set_pending\|pending\.valid\|take_pending" lib/voltage_control/ include/voltage_control/
```

Expected: No matches.

- [ ] **Step 5: Commit cleanup if any stragglers found**

```bash
git add -A
git commit -m "refactor(vc): final cleanup — remove all references to deleted interfaces"
```

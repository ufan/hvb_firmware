# Plan A: Virtual Channel + Voltage Controller Implementation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract per-channel state machines from domain_state.c into independent virtual channel structs, and introduce a voltage controller manager that owns and routes commands to them.

**Architecture:** Each virtual channel is a self-contained SMF state machine with its own config, runtime state, and engines (ramp, protection, recovery, status). The voltage controller is a thin manager that owns the channel array, routes commands, and drains pending hardware commands. The existing domain tests are migrated to test the virtual channel API directly. The domain_state.c retains only system-level state.

**Tech Stack:** C99, Zephyr RTOS (SMF, k_spinlock), ztest on native_posix

**Spec:** `docs/superpowers/specs/2026-06-24-channel-table-direct-drive-design.md`

**Scope:** This plan covers only the domain logic layer (virtual channel + voltage controller). It does NOT touch: DTS bindings, channel table, hardware drivers, domain_runtime.c, vc.c, modbus_adapter.c, or provider_bus.c. Those are Plan B (hardware) and Plan C (integration/wiring).

**Build command:** `west build -b native_posix tests/voltage_control/vc_channel_state -p`
**Test command:** `west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe`

---

### Task 1: Create vc_channel_state.h header with structs and API declarations

**Files:**
- Create: `include/voltage_control/vc_channel_state.h`

This is the public header for the virtual channel state machine. All types and function declarations go here. No implementation yet.

- [ ] **Step 1: Create the header file**

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

#include "voltage_control/domain.h"

struct vc_pending_command {
	uint16_t output_code;
	bool output_enable;
	bool valid;
};

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
	struct k_spinlock lock;
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

	struct vc_pending_command pending;
};

void vc_channel_init(struct vc_channel *ch, uint8_t index, uint16_t caps);

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
						bool enable,
						const struct vc_channel *all_channels,
						size_t channel_count);
enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code);
enum vc_status vc_channel_cal_sample(struct vc_channel *ch);
enum vc_status vc_channel_cal_commit(struct vc_channel *ch);
enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit);

void vc_channel_reset_calibration(struct vc_channel *ch, bool entering);

bool vc_channel_has_pending_command(const struct vc_channel *ch);
struct vc_pending_command vc_channel_take_pending_command(struct vc_channel *ch);

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch);

#endif
```

Note: Several functions take `calibration_mode` or `sys_cfg` parameters because the virtual channel does not own system-level state — the voltage controller passes these in from its own state. The `cal_set_output_enable` function takes the full channel array to enforce the single-output-enabled-at-a-time invariant across channels.

- [ ] **Step 2: Commit**

```bash
git add include/voltage_control/vc_channel_state.h
git commit -m "feat: add vc_channel_state.h header with virtual channel API"
```

---

### Task 2: Create test scaffold and first test (init + defaults)

**Files:**
- Create: `tests/voltage_control/vc_channel_state/CMakeLists.txt`
- Create: `tests/voltage_control/vc_channel_state/prj.conf`
- Create: `tests/voltage_control/vc_channel_state/testcase.yaml`
- Create: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
get_filename_component(HVB_FIRMWARE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES ${HVB_FIRMWARE_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vc_channel_state_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
```

- [ ] **Step 2: Create prj.conf**

```
CONFIG_ZTEST=y
CONFIG_TEST_RANDOM_GENERATOR=y
CONFIG_SMF=y
```

Note: No CONFIG_VC_RUNTIME or CONFIG_VC_PROVIDER_BUS — the virtual channel tests are pure domain logic with no runtime or bus dependencies.

- [ ] **Step 3: Create testcase.yaml**

```yaml
tests:
  voltage_control.vc_channel_state:
    platform_allow: native_posix
    tags: voltage_control
```

- [ ] **Step 4: Create test main.c with init + default config tests**

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

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	vc_channel_init(&ch, 0, FULL_CAPS);
}

ZTEST_SUITE(vc_channel_state, NULL, NULL, before_each, NULL, NULL);

ZTEST(vc_channel_state, test_init_defaults)
{
	zassert_equal(ch.index, 0);
	zassert_equal(ch.capabilities, FULL_CAPS);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(ch.active_fault_cause, 0);
	zassert_equal(ch.measured_voltage, 0);
	zassert_equal(ch.measured_current, 0);
	zassert_false(ch.pending.valid);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
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
	zassert_equal(cfg.voltage_limit_threshold, 20000);
	zassert_equal(cfg.current_limit_threshold, 32767);
	zassert_equal(cfg.voltage_protection_mode, VC_PROTECTION_MODE_DISABLED);
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
```

- [ ] **Step 5: Commit**

```bash
git add tests/voltage_control/vc_channel_state/
git commit -m "test: add vc_channel_state test scaffold with init + default tests"
```

---

### Task 3: Implement vc_channel_init, get_config, get_snapshot, SMF state query

**Files:**
- Create: `lib/voltage_control/vc_channel_state.c`
- Modify: `lib/voltage_control/CMakeLists.txt` — add `vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/CMakeLists.txt` — add source if needed

- [ ] **Step 1: Add vc_channel_state.c to lib CMakeLists**

Add this line to `lib/voltage_control/CMakeLists.txt` after the `domain_state.c` line:

```cmake
zephyr_library_sources(vc_channel_state.c)
```

- [ ] **Step 2: Create vc_channel_state.c with init + accessors**

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
		.voltage_limit_threshold = VC_DEFAULT_MAX_VOLTAGE_RAW,
		.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
		.output_calib_k = 10000,
		.measured_voltage_calib_k = 10000,
		.measured_current_calib_k = 10000,
	};
}

void vc_channel_init(struct vc_channel *ch, uint8_t index, uint16_t caps)
{
	memset(ch, 0, sizeof(*ch));
	ch->index = index;
	ch->capabilities = caps;
	ch->config = default_channel_config();
	ch->cal_max_raw_dac_limit = VC_DEFAULT_MAX_RAW_DAC;
	smf_set_initial(SMF_CTX(ch), &vc_channel_states[VC_CHANNEL_SMF_DISABLED_SAFE]);
}

enum vc_status vc_channel_get_config(const struct vc_channel *ch,
				     struct vc_channel_config *cfg)
{
	k_spinlock_key_t key = k_spin_lock((struct k_spinlock *)&ch->lock);
	*cfg = ch->config;
	k_spin_unlock((struct k_spinlock *)&ch->lock, key);
	return VC_OK;
}

void vc_channel_get_snapshot(const struct vc_channel *ch,
			     struct vc_channel_snapshot *snap)
{
	k_spinlock_key_t key = k_spin_lock((struct k_spinlock *)&ch->lock);

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

	k_spin_unlock((struct k_spinlock *)&ch->lock, key);
}

enum vc_channel_smf_state vc_channel_get_smf_state(const struct vc_channel *ch)
{
	const struct smf_ctx *ctx = SMF_CTX((struct vc_channel *)ch);

	return (enum vc_channel_smf_state)(ctx->current - &vc_channel_states[0]);
}

bool vc_channel_has_pending_command(const struct vc_channel *ch)
{
	return ch->pending.valid;
}

struct vc_pending_command vc_channel_take_pending_command(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	struct vc_pending_command cmd = ch->pending;

	ch->pending.valid = false;
	k_spin_unlock(&ch->lock, key);
	return cmd;
}
```

- [ ] **Step 3: Run tests to verify init + defaults pass**

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe
```

Expected: 3 tests PASS (test_init_defaults, test_default_config, test_snapshot_defaults)

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c lib/voltage_control/CMakeLists.txt
git commit -m "feat: implement vc_channel_init, get_config, get_snapshot"
```

---

### Task 4: Implement output_action + fault_command with tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add output action + fault command tests**

Add to `tests/voltage_control/vc_channel_state/src/main.c`:

```c
ZTEST(vc_channel_state, test_output_action_enable)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false), VC_OK);
	zassert_true(ch.output_enabled);
	zassert_true(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_RAMPING);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_output_action_disable_immediate)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE, false), VC_OK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_true(ch.pending.valid);
	zassert_equal(ch.pending.output_code, 0);
	zassert_false(ch.pending.output_enable);
}

ZTEST(vc_channel_state, test_output_action_rejected_in_calibration)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, true),
		      VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_output_action_rejected_with_active_fault)
{
	ch.active_fault_cause = VC_FAULT_VOLTAGE;
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false),
		      VC_ERR_UNSAFE_STATE);
}

ZTEST(vc_channel_state, test_output_action_invalid_host_action)
{
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO, false),
		      VC_ERR_INVALID_COMMAND);
	zassert_equal(vc_channel_output_action(&ch, VC_OUTPUT_ACTION_CLAMP, false),
		      VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_fault_command_clear_history)
{
	ch.fault_history_cause = VC_FAULT_VOLTAGE;
	zassert_equal(vc_channel_fault_command(&ch, VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY,
					       &(struct vc_system_config){0}), VC_OK);
	zassert_equal(ch.fault_history_cause, 0);
}

ZTEST(vc_channel_state, test_fault_command_invalid)
{
	zassert_equal(vc_channel_fault_command(&ch, 3, &(struct vc_system_config){0}),
		      VC_ERR_INVALID_COMMAND);
}
```

- [ ] **Step 2: Run tests — they should fail (functions not implemented)**

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe
```

Expected: link errors or assertion failures for vc_channel_output_action / vc_channel_fault_command.

- [ ] **Step 3: Implement output_action and fault_command**

Add to `vc_channel_state.c`:

```c
static void set_smf_state(struct vc_channel *ch, enum vc_channel_smf_state state)
{
	smf_set_state(SMF_CTX(ch), &vc_channel_states[state]);
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

static bool channel_has_cap(const struct vc_channel *ch, uint16_t cap)
{
	return (ch->capabilities & cap) == cap;
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

static void set_pending(struct vc_channel *ch)
{
	ch->pending.valid = true;
	ch->pending.output_enable = ch->output_enabled;
	ch->pending.output_code = ch->output_enabled ?
		raw_drive_from_target(&ch->config, ch->operational_target_voltage) : 0;
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
	ch->auto_cooldown_remaining_snapshot = (uint16_t)(ch->cooldown_remaining_ms / 1000);
	ch->status_bits = bits;
}

enum vc_status vc_channel_output_action(struct vc_channel *ch,
					enum vc_output_action action,
					bool calibration_mode)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!is_valid_host_output_action(action)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}
	if (calibration_mode) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}

	switch (action) {
	case VC_OUTPUT_ACTION_ENABLE:
		if (ch->active_fault_cause != 0) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
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
		goto out;
	}
	set_pending(ch);
	update_status_bits(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

static bool is_safe_to_clear_active(const struct vc_channel *ch,
				    const struct vc_system_config *sys_cfg)
{
	const struct vc_channel_config *cfg = &ch->config;
	int32_t safe_limit;

	if (ch->active_fault_cause & VC_FAULT_VOLTAGE) {
		safe_limit = (int32_t)cfg->voltage_limit_threshold *
			     (100 - (int32_t)sys_cfg->voltage_safe_band_pct) / 100;
		if (ch->measured_voltage > safe_limit) {
			return false;
		}
	}
	if (ch->active_fault_cause & VC_FAULT_CURRENT) {
		safe_limit = (int32_t)cfg->current_limit_threshold *
			     (100 - (int32_t)sys_cfg->current_safe_band_pct) / 100;
		if (ch->measured_current > safe_limit) {
			return false;
		}
	}
	return true;
}

enum vc_status vc_channel_fault_command(struct vc_channel *ch,
					enum vc_channel_fault_command cmd,
					const struct vc_system_config *sys_cfg)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!is_valid_channel_fault_command(cmd)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}

	switch (cmd) {
	case VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE:
		if (ch->active_fault_cause == 0) {
			break;
		}
		if (!is_safe_to_clear_active(ch, sys_cfg)) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
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

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}
```

- [ ] **Step 4: Run tests — all should pass**

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe
```

Expected: All tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel output_action and fault_command"
```

---

### Task 5: Implement consume_voltage + voltage protection with tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add voltage consumption and protection tests**

```c
ZTEST(vc_channel_state, test_consume_voltage_applies_calibration)
{
	struct vc_channel_snapshot snap;

	vc_channel_consume_voltage(&ch, 1200);
	vc_channel_get_snapshot(&ch, &snap);
	zassert_equal(snap.measured_voltage, 1200);
	zassert_equal(snap.raw_adc_voltage, 1200);
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

ZTEST(vc_channel_state, test_voltage_protection_triggers_fault)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	ch.pending.valid = false;

	vc_channel_consume_voltage(&ch, 3100);

	zassert_true(ch.active_fault_cause & VC_FAULT_VOLTAGE);
	zassert_true(ch.fault_history_cause & VC_FAULT_VOLTAGE);
	zassert_equal(ch.operational_target_voltage, 0);
	zassert_true(ch.pending.valid);
	zassert_false(ch.pending.output_enable);
}

ZTEST(vc_channel_state, test_voltage_protection_flag_only_no_active_fault)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_FLAG_ONLY;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	ch.pending.valid = false;

	vc_channel_consume_voltage(&ch, 3100);

	zassert_equal(ch.active_fault_cause, 0);
	zassert_true(ch.fault_history_cause & VC_FAULT_VOLTAGE);
}

ZTEST(vc_channel_state, test_voltage_protection_clamp)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_CLAMP;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	ch.pending.valid = false;

	vc_channel_consume_voltage(&ch, 3100);

	zassert_equal(ch.operational_target_voltage, 3000);
	zassert_true(ch.active_fault_cause & VC_FAULT_VOLTAGE);
}
```

- [ ] **Step 2: Run tests — should fail (consume_voltage not implemented)**

- [ ] **Step 3: Implement consume_voltage + protection engine**

Add to `vc_channel_state.c`:

```c
static int16_t clamp_int16(int64_t value)
{
	if (value > INT16_MAX) return INT16_MAX;
	if (value < INT16_MIN) return INT16_MIN;
	return (int16_t)value;
}

static bool has_hard_safety_fault(const struct vc_channel *ch)
{
	return (ch->active_fault_cause & (VC_FAULT_HARDWARE | VC_FAULT_INTERLOCK)) != 0;
}

static void apply_protection_action(struct vc_channel *ch,
				    enum vc_output_action action,
				    int16_t clamp_limit)
{
	ch->last_protection_output_action = action;

	switch (action) {
	case VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO:
		ch->operational_target_voltage = 0;
		ch->output_enabled = false;
		break;
	case VC_OUTPUT_ACTION_CLAMP:
		ch->operational_target_voltage = clamp_limit;
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
	set_pending(ch);
}

static void tick_voltage_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
		return;
	}
	if (cfg->voltage_protection_mode == VC_PROTECTION_MODE_DISABLED) {
		return;
	}
	if (ch->measured_voltage <= cfg->voltage_limit_threshold) {
		return;
	}

	ch->fault_history_cause |= VC_FAULT_VOLTAGE;

	if (cfg->voltage_protection_mode != VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION) {
		return;
	}

	ch->active_fault_cause |= VC_FAULT_VOLTAGE;
	ch->last_fault_timestamp = ch->uptime_ref;
	apply_protection_action(ch, cfg->voltage_protection_output_action,
				cfg->voltage_limit_threshold);
	set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
}

void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->raw_adc_voltage = raw_voltage;
	ch->measured_voltage = clamp_int16(
		((int64_t)raw_voltage * ch->config.measured_voltage_calib_k) /
		10000 + ch->config.measured_voltage_calib_b);

	tick_voltage_protection(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}
```

- [ ] **Step 4: Run tests — all should pass**

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel_consume_voltage with protection engine"
```

---

### Task 6: Implement consume_current + consume_fault with tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add current protection and fault consumption tests**

```c
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
	ch.pending.valid = false;
	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	ch.pending.valid = false;

	vc_channel_consume_current(&ch, 200);

	zassert_true(ch.active_fault_cause & VC_FAULT_CURRENT);
	zassert_true(ch.pending.valid);
	zassert_false(ch.pending.output_enable);
}

ZTEST(vc_channel_state, test_consume_fault_sets_hardware_fault)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_consume_fault(&ch, VC_FAULT_HARDWARE);

	zassert_true(ch.active_fault_cause & VC_FAULT_HARDWARE);
	zassert_true(ch.fault_history_cause & VC_FAULT_HARDWARE);
	zassert_false(ch.output_enabled);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_DISABLED_SAFE);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_consume_fault_interlock)
{
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_consume_fault(&ch, VC_FAULT_INTERLOCK);

	zassert_true(ch.active_fault_cause & VC_FAULT_INTERLOCK);
	zassert_false(ch.output_enabled);
	zassert_equal(ch.operational_target_voltage, 0);
}
```

- [ ] **Step 2: Implement consume_current and consume_fault**

Add to `vc_channel_state.c`:

```c
static void tick_current_protection(struct vc_channel *ch)
{
	const struct vc_channel_config *cfg = &ch->config;

	if (ch->active_fault_cause != 0) {
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
	apply_protection_action(ch, cfg->current_protection_output_action,
				cfg->current_limit_threshold);
	set_smf_state(ch, VC_CHANNEL_SMF_FAULT_LATCHED);
}

void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->raw_adc_current = raw_current;
	ch->measured_current = clamp_int16(
		((int64_t)raw_current * ch->config.measured_current_calib_k) /
		10000 + ch->config.measured_current_calib_b);

	tick_current_protection(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

static void force_safe_state(struct vc_channel *ch)
{
	ch->output_enabled = false;
	ch->ramping = false;
	ch->cal_output_enabled = 0;
	ch->raw_dac_readback = 0;
	ch->operational_target_voltage = 0;
	set_smf_state(ch, VC_CHANNEL_SMF_DISABLED_SAFE);
	set_pending(ch);
}

void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

	ch->active_fault_cause |= fault_cause;
	ch->fault_history_cause |= fault_cause;
	force_safe_state(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}
```

- [ ] **Step 3: Run tests — all should pass**

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel consume_current and consume_fault"
```

---

### Task 7: Implement tick_ramp with tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add ramp tests**

```c
ZTEST(vc_channel_state, test_tick_ramp_instant)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ch, &cfg, false);
	vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE, false);
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});

	zassert_equal(ch.operational_target_voltage, 5000);
	zassert_false(ch.ramping);
	zassert_equal(vc_channel_get_smf_state(&ch), VC_CHANNEL_SMF_ENABLED_HOLDING);
	zassert_true(ch.pending.valid);
	zassert_equal(ch.pending.output_code, 5000);
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
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});

	zassert_equal(ch.operational_target_voltage, 100);
	zassert_true(ch.ramping);
}

ZTEST(vc_channel_state, test_tick_ramp_no_output_enabled)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.configured_target_voltage = 5000;
	vc_channel_set_config(&ch, &cfg, false);

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});

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
	ch.pending.valid = false;

	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	zassert_equal(ch.operational_target_voltage, 100);
	zassert_false(ch.ramping);

	ch.pending.valid = false;
	vc_channel_tick_ramp(&ch, 100, &(struct vc_system_config){0});
	zassert_false(ch.pending.valid);
}
```

- [ ] **Step 2: Implement tick_ramp**

Add to `vc_channel_state.c`:

```c
void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms,
			  const struct vc_system_config *sys_cfg)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	const struct vc_channel_config *cfg = &ch->config;
	int16_t target, current;
	uint16_t step, interval;
	uint32_t interval_ms;

	ARG_UNUSED(sys_cfg);

	if (!ch->output_enabled || ch->active_fault_cause != 0) {
		goto out;
	}

	target = cfg->configured_target_voltage;
	current = ch->operational_target_voltage;

	if (current == target) {
		if (ch->ramping) {
			ch->ramping = false;
			set_smf_state(ch, VC_CHANNEL_SMF_ENABLED_HOLDING);
		}
		goto out;
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
		set_pending(ch);
		update_status_bits(ch);
		goto out;
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
	set_pending(ch);
	update_status_bits(ch);

out:
	k_spin_unlock(&ch->lock, key);
}
```

- [ ] **Step 3: Run tests — all should pass**

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel_tick_ramp with gradual and instant modes"
```

---

### Task 8: Implement set_config, set_field with validation and tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add config validation tests**

```c
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

ZTEST(vc_channel_state, test_set_config_calibration_coefficients_blocked_outside_cal_mode)
{
	struct vc_channel_config cfg;

	vc_channel_get_config(&ch, &cfg);
	cfg.output_calib_k++;
	zassert_equal(vc_channel_set_config(&ch, &cfg, false), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_channel_state, test_set_config_calibration_coefficients_allowed_in_cal_mode)
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
	zassert_equal(vc_channel_set_field(&ch, VC_FIELD_SLAVE_ADDRESS, 42, false),
		      VC_ERR_INVALID_VALUE);
}

static struct vc_channel onoff_ch;

ZTEST(vc_channel_state, test_onoff_channel_rejects_output_drive_config)
{
	struct vc_channel_config cfg;

	vc_channel_init(&onoff_ch, 0, CH_CAP_OUTPUT_ENABLE);
	vc_channel_get_config(&onoff_ch, &cfg);
	cfg.configured_target_voltage = 100;
	zassert_equal(vc_channel_set_config(&onoff_ch, &cfg, false),
		      VC_ERR_UNSUPPORTED_CAPABILITY);
}
```

- [ ] **Step 2: Implement set_config and set_field**

These are extracted from `domain_state.c`'s `domain_set_channel_config()` and `domain_set_channel_field()` with the validation logic preserved. The full implementation follows the same patterns as the existing code — capability validation, calibration guard, range checks — but operates on `struct vc_channel` instead of the domain's parallel arrays.

Add to `vc_channel_state.c`:

```c
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
		if (new_cfg->voltage_protection_mode != VC_PROTECTION_MODE_DISABLED ||
		    new_cfg->voltage_protection_output_action != old_cfg->voltage_protection_output_action ||
		    new_cfg->voltage_limit_threshold != old_cfg->voltage_limit_threshold ||
		    new_cfg->measured_voltage_calib_k != old_cfg->measured_voltage_calib_k ||
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

enum vc_status vc_channel_set_config(struct vc_channel *ch,
				     const struct vc_channel_config *cfg,
				     bool calibration_mode)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st;

	st = validate_capability_config(ch, &ch->config, cfg);
	if (st != VC_OK) {
		goto out;
	}
	if (!calibration_mode && calibration_fields_changed(&ch->config, cfg)) {
		st = VC_ERR_INVALID_COMMAND;
		goto out;
	}
	if (cfg->configured_target_voltage > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->configured_target_voltage < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->voltage_limit_threshold > VC_DEFAULT_MAX_VOLTAGE_RAW ||
	    cfg->voltage_limit_threshold < VC_DEFAULT_MIN_VOLTAGE_RAW) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->current_limit_threshold < 0) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (!is_valid_protection_mode(cfg->voltage_protection_mode) ||
	    !is_valid_protection_output_action(cfg->voltage_protection_output_action, true) ||
	    !is_valid_protection_mode(cfg->current_protection_mode) ||
	    !is_valid_protection_output_action(cfg->current_protection_output_action, false)) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (cfg->save_target_policy > 1) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}

	ch->config = *cfg;
	if (!calibration_mode) {
		tick_voltage_protection(ch);
		tick_current_protection(ch);
	}
	set_pending(ch);
	update_status_bits(ch);
	st = VC_OK;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
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
		cfg.configured_target_voltage = (int16_t)value; break;
	case VC_FIELD_RAMP_UP_STEP:
		cfg.ramp_up_step = value; break;
	case VC_FIELD_RAMP_UP_INTERVAL:
		cfg.ramp_up_interval = value; break;
	case VC_FIELD_RAMP_DOWN_STEP:
		cfg.ramp_down_step = value; break;
	case VC_FIELD_RAMP_DOWN_INTERVAL:
		cfg.ramp_down_interval = value; break;
	case VC_FIELD_VOLTAGE_PROTECTION_MODE:
		cfg.voltage_protection_mode = (enum vc_protection_mode)value; break;
	case VC_FIELD_VOLTAGE_PROT_OUT_ACTION:
		cfg.voltage_protection_output_action = (enum vc_output_action)value; break;
	case VC_FIELD_VOLTAGE_LIMIT_THRESHOLD:
		cfg.voltage_limit_threshold = (int16_t)value; break;
	case VC_FIELD_CURRENT_PROTECTION_MODE:
		cfg.current_protection_mode = (enum vc_protection_mode)value; break;
	case VC_FIELD_CURRENT_PROT_OUT_ACTION:
		cfg.current_protection_output_action = (enum vc_output_action)value; break;
	case VC_FIELD_CURRENT_LIMIT_THRESHOLD:
		cfg.current_limit_threshold = (int16_t)value; break;
	case VC_FIELD_AUTO_DERATE_STEP:
		cfg.auto_derate_step = value; break;
	case VC_FIELD_SAVE_TARGET_POLICY:
		cfg.save_target_policy = value; break;
	case VC_FIELD_OUTPUT_CAL_K:
		cfg.output_calib_k = value; break;
	case VC_FIELD_OUTPUT_CAL_B:
		cfg.output_calib_b = (int16_t)value; break;
	case VC_FIELD_MEASURED_V_CAL_K:
		cfg.measured_voltage_calib_k = value; break;
	case VC_FIELD_MEASURED_V_CAL_B:
		cfg.measured_voltage_calib_b = (int16_t)value; break;
	case VC_FIELD_MEASURED_I_CAL_K:
		cfg.measured_current_calib_k = value; break;
	case VC_FIELD_MEASURED_I_CAL_B:
		cfg.measured_current_calib_b = (int16_t)value; break;
	default:
		return VC_ERR_INVALID_VALUE;
	}

	return vc_channel_set_config(ch, &cfg, calibration_mode);
}
```

- [ ] **Step 3: Run tests — all should pass**

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel set_config and set_field with validation"
```

---

### Task 9: Implement calibration functions with tests

**Files:**
- Modify: `lib/voltage_control/vc_channel_state.c`
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

- [ ] **Step 1: Add calibration tests**

```c
ZTEST(vc_channel_state, test_cal_set_output_enable)
{
	zassert_equal(vc_channel_cal_set_output_enable(&ch, true, NULL, 0), VC_OK);
	zassert_equal(ch.cal_output_enabled, 1);
	zassert_true(ch.pending.valid);
}

ZTEST(vc_channel_state, test_cal_set_raw_dac_requires_output_enabled)
{
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 100), VC_OK);
	zassert_equal(ch.raw_dac_readback, 100);
}

ZTEST(vc_channel_state, test_cal_max_raw_dac_limit)
{
	vc_channel_cal_set_max_raw_dac(&ch, 50);
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 51), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_channel_cal_set_raw_dac(&ch, 50), VC_OK);
}

ZTEST(vc_channel_state, test_cal_sample_captures_raw)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	vc_channel_cal_set_raw_dac(&ch, 123);
	zassert_equal(vc_channel_cal_sample(&ch), VC_OK);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_VALID);
	zassert_equal(ch.raw_adc_voltage, 123);
}

ZTEST(vc_channel_state, test_cal_commit_requires_output_disabled)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	zassert_equal(vc_channel_cal_commit(&ch), VC_ERR_UNSAFE_STATE);
	vc_channel_cal_set_output_enable(&ch, false, NULL, 0);
	zassert_equal(vc_channel_cal_commit(&ch), VC_OK);
}

ZTEST(vc_channel_state, test_cal_disable_clears_sample_state)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
	vc_channel_cal_set_raw_dac(&ch, 50);
	vc_channel_cal_sample(&ch);
	vc_channel_cal_set_output_enable(&ch, false, NULL, 0);
	zassert_equal(ch.raw_adc_voltage, 0);
	zassert_equal(ch.raw_adc_current, 0);
	zassert_equal(ch.cal_sample_status, VC_CAL_SAMPLE_NONE);
}

ZTEST(vc_channel_state, test_cal_single_output_enabled_across_channels)
{
	struct vc_channel channels[2];

	vc_channel_init(&channels[0], 0, FULL_CAPS);
	vc_channel_init(&channels[1], 1, FULL_CAPS);

	zassert_equal(vc_channel_cal_set_output_enable(&channels[0], true, channels, 2), VC_OK);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[1], true, channels, 2),
		      VC_ERR_UNSAFE_STATE);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[0], false, channels, 2), VC_OK);
	zassert_equal(vc_channel_cal_set_output_enable(&channels[1], true, channels, 2), VC_OK);
}

ZTEST(vc_channel_state, test_reset_calibration_entering)
{
	vc_channel_cal_set_output_enable(&ch, true, NULL, 0);
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
```

- [ ] **Step 2: Implement calibration functions**

Add to `vc_channel_state.c`:

```c
void vc_channel_reset_calibration(struct vc_channel *ch, bool entering)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);

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
	set_pending(ch);
	update_status_bits(ch);

	k_spin_unlock(&ch->lock, key);
}

enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch,
						bool enable,
						const struct vc_channel *all_channels,
						size_t channel_count)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}

	if (enable) {
		if (has_hard_safety_fault(ch)) {
			st = VC_ERR_UNSAFE_STATE;
			goto out;
		}
		if (all_channels != NULL) {
			for (size_t i = 0; i < channel_count; i++) {
				if (&all_channels[i] == ch) {
					continue;
				}
				if (all_channels[i].cal_output_enabled ||
				    all_channels[i].raw_dac_readback != 0) {
					st = VC_ERR_UNSAFE_STATE;
					goto out;
				}
			}
		}
		ch->cal_output_enabled = 1;
	} else {
		ch->cal_output_enabled = 0;
		ch->raw_dac_readback = 0;
		ch->raw_adc_voltage = 0;
		ch->raw_adc_current = 0;
		ch->cal_sample_status = VC_CAL_SAMPLE_NONE;
	}
	set_pending(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	if (code > ch->cal_max_raw_dac_limit) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (code != 0 && has_hard_safety_fault(ch)) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	if (code != 0 && !ch->cal_output_enabled) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	ch->raw_dac_readback = code;
	set_pending(ch);

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch,
					      uint16_t limit)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (!channel_has_cap(ch, CH_CAP_RAW_OUTPUT_DRIVE)) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	if (limit > VC_DEFAULT_MAX_RAW_DAC) {
		st = VC_ERR_INVALID_VALUE;
		goto out;
	}
	if (limit < ch->raw_dac_readback) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}
	ch->cal_max_raw_dac_limit = limit;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_sample(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if ((ch->capabilities & (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)) == 0) {
		st = VC_ERR_UNSUPPORTED_CAPABILITY;
		goto out;
	}
	ch->raw_adc_voltage = ch->raw_dac_readback;
	ch->raw_adc_current = 0;
	ch->cal_sample_status = VC_CAL_SAMPLE_VALID;

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}

enum vc_status vc_channel_cal_commit(struct vc_channel *ch)
{
	k_spinlock_key_t key = k_spin_lock(&ch->lock);
	enum vc_status st = VC_OK;

	if (ch->cal_output_enabled || ch->raw_dac_readback != 0 ||
	    has_hard_safety_fault(ch)) {
		st = VC_ERR_UNSAFE_STATE;
		goto out;
	}

out:
	k_spin_unlock(&ch->lock, key);
	return st;
}
```

- [ ] **Step 3: Run tests — all should pass**

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_channel_state.c tests/voltage_control/vc_channel_state/src/main.c
git commit -m "feat: implement vc_channel calibration functions"
```

---

### Task 10: Create vc_controller.h header and vc_controller.c implementation

**Files:**
- Create: `include/voltage_control/vc_controller.h`
- Create: `lib/voltage_control/vc_controller.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

- [ ] **Step 1: Create vc_controller.h**

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
	enum vc_operating_mode operating_mode;
	uint8_t cal_unlock_step;
	bool cal_unlocked;
	const struct vc_storage_backend *storage;
	struct vc_system_config sys_cfg;
};

struct vc_controller *vc_controller_init_static(
	const struct vc_channel_entry *entries, size_t count);

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

void vc_controller_consume_voltage(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_voltage);
void vc_controller_consume_current(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_current);
void vc_controller_consume_fault(
	struct vc_controller *ctrl, uint8_t ch, uint16_t fault_cause);

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
	const struct vc_controller *ctrl, uint8_t ch, struct vc_channel_snapshot *snap);
enum vc_status vc_controller_get_channel_config(
	const struct vc_controller *ctrl, uint8_t ch, struct vc_channel_config *cfg);

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend);

size_t vc_controller_channel_count(const struct vc_controller *ctrl);
uint16_t vc_controller_channel_capabilities(const struct vc_controller *ctrl,
					    uint8_t ch);

#endif
```

- [ ] **Step 2: Create vc_controller.c with init + routing + drain stub**

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_controller.h"
#include "regmap/vc_regs.h"
#include <string.h>
#include <zephyr/sys/reboot.h>

#define VC_VARIANT_ID 1
#define VC_DEFAULT_SYSTEM_CAPS (SYS_CAP_AUTOMATIC_MODE | SYS_CAP_ENV_SENSOR | SYS_CAP_CALIBRATION_MODE)
#define VC_CHANNEL_MASK(c) ((1U << (c)) - 1)

static struct vc_system_config default_system_config(void)
{
	return (struct vc_system_config){
		.operating_mode = VC_OPERATING_MODE_NORMAL,
		.slave_address = 1,
		.baud_rate_code = VC_BAUD_RATE_115200,
		.recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
		.voltage_safe_band_pct = 10,
		.current_safe_band_pct = 10,
	};
}

static bool channel_valid(const struct vc_controller *ctrl, uint8_t ch)
{
	return ch < ctrl->channel_count;
}

/* Stub: Plan B/C will wire this to vc_channel_table_set_output/set_enable */
static void drain_pending(struct vc_controller *ctrl, uint8_t ch)
{
	(void)vc_channel_take_pending_command(&ctrl->channels[ch]);
}

struct vc_controller *vc_controller_init_static(
	const struct vc_channel_entry *entries, size_t count)
{
	static struct vc_controller ctrl;

	if (entries == NULL || count == 0 || count > VC_MAX_CHANNELS) {
		return NULL;
	}

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.channel_count = count;
	ctrl.operating_mode = VC_OPERATING_MODE_NORMAL;
	ctrl.sys_cfg = default_system_config();

	for (size_t i = 0; i < count; i++) {
		vc_channel_init(&ctrl.channels[i], entries[i].index,
				entries[i].capabilities);
	}

	return &ctrl;
}

enum vc_status vc_controller_channel_output_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;
	enum vc_status st = vc_channel_output_action(&ctrl->channels[ch], action, cal);

	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_fault_command(
	struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	enum vc_status st = vc_channel_fault_command(&ctrl->channels[ch], cmd,
						     &ctrl->sys_cfg);
	drain_pending(ctrl, ch);
	return st;
}

enum vc_status vc_controller_channel_set_field(
	struct vc_controller *ctrl, uint8_t ch,
	enum vc_config_field field, uint16_t value)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;
	enum vc_status st = vc_channel_set_field(&ctrl->channels[ch], field, value, cal);

	drain_pending(ctrl, ch);
	return st;
}

void vc_controller_consume_voltage(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_voltage)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_voltage(&ctrl->channels[ch], raw_voltage);
	drain_pending(ctrl, ch);
}

void vc_controller_consume_current(
	struct vc_controller *ctrl, uint8_t ch, int32_t raw_current)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_current(&ctrl->channels[ch], raw_current);
	drain_pending(ctrl, ch);
}

void vc_controller_consume_fault(
	struct vc_controller *ctrl, uint8_t ch, uint16_t fault_cause)
{
	if (!channel_valid(ctrl, ch)) {
		return;
	}
	vc_channel_consume_fault(&ctrl->channels[ch], fault_cause);
	drain_pending(ctrl, ch);
}

void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms)
{
	if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		return;
	}
	for (size_t i = 0; i < ctrl->channel_count; i++) {
		vc_channel_tick_ramp(&ctrl->channels[i], dt_ms, &ctrl->sys_cfg);
		drain_pending(ctrl, i);
	}
}

enum vc_status vc_controller_set_operating_mode(
	struct vc_controller *ctrl, enum vc_operating_mode mode)
{
	if (mode != VC_OPERATING_MODE_NORMAL &&
	    mode != VC_OPERATING_MODE_AUTOMATIC &&
	    mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_VALUE;
	}
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION &&
	    mode == VC_OPERATING_MODE_CALIBRATION && !ctrl->cal_unlocked) {
		return VC_ERR_INVALID_COMMAND;
	}

	if (ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
	    mode == VC_OPERATING_MODE_NORMAL) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			ctrl->channels[i].cooldown_remaining_ms = 0;
		}
	}

	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], true);
			drain_pending(ctrl, i);
		}
	} else if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], false);
			drain_pending(ctrl, i);
		}
	}

	ctrl->operating_mode = mode;
	if (mode != VC_OPERATING_MODE_CALIBRATION) {
		ctrl->sys_cfg.operating_mode = mode;
	}
	if (mode == VC_OPERATING_MODE_CALIBRATION ||
	    ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->cal_unlock_step = 0;
		ctrl->cal_unlocked = false;
	}

	return VC_OK;
}

enum vc_operating_mode vc_controller_get_operating_mode(
	const struct vc_controller *ctrl)
{
	return ctrl->operating_mode;
}

enum vc_status vc_controller_calibration_unlock(
	struct vc_controller *ctrl, uint16_t value)
{
	if (value == CAL_UNLOCK_STEP1) {
		ctrl->cal_unlock_step = 1;
		ctrl->cal_unlocked = false;
		return VC_OK;
	}
	if (value == CAL_UNLOCK_STEP2 && ctrl->cal_unlock_step == 1) {
		ctrl->cal_unlock_step = 0;
		ctrl->cal_unlocked = true;
		return VC_OK;
	}
	ctrl->cal_unlock_step = 0;
	ctrl->cal_unlocked = false;
	return VC_ERR_INVALID_COMMAND;
}

enum vc_status vc_controller_get_system_config(
	const struct vc_controller *ctrl, struct vc_system_config *cfg)
{
	*cfg = ctrl->sys_cfg;
	return VC_OK;
}

enum vc_status vc_controller_set_system_config(
	struct vc_controller *ctrl, const struct vc_system_config *cfg)
{
	if (cfg->slave_address > 247) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->baud_rate_code > VC_BAUD_RATE_9600) {
		return VC_ERR_INVALID_VALUE;
	}
	if (cfg->voltage_safe_band_pct > 50 || cfg->current_safe_band_pct > 50) {
		return VC_ERR_INVALID_VALUE;
	}

	enum vc_operating_mode old_cfg_mode = ctrl->sys_cfg.operating_mode;

	memcpy(&ctrl->sys_cfg, cfg, sizeof(*cfg));
	if (cfg->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->sys_cfg.operating_mode = old_cfg_mode;
	}

	if (cfg->operating_mode != ctrl->operating_mode) {
		return vc_controller_set_operating_mode(ctrl, cfg->operating_mode);
	}
	return VC_OK;
}

void vc_controller_get_system_snapshot(
	const struct vc_controller *ctrl, struct vc_system_snapshot *snap)
{
	memset(snap, 0, sizeof(*snap));
	snap->protocol_major = VC_PROTOCOL_MAJOR;
	snap->protocol_minor = VC_PROTOCOL_MINOR;
	snap->variant_id = VC_VARIANT_ID;
	snap->system_capability_flags = VC_DEFAULT_SYSTEM_CAPS;
	snap->supported_channel_count = (uint16_t)ctrl->channel_count;
	snap->active_channel_mask = VC_CHANNEL_MASK(ctrl->channel_count);
	snap->active_operating_mode = ctrl->operating_mode;
}

enum vc_status vc_controller_get_channel_snapshot(
	const struct vc_controller *ctrl, uint8_t ch, struct vc_channel_snapshot *snap)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	vc_channel_get_snapshot(&ctrl->channels[ch], snap);
	return VC_OK;
}

enum vc_status vc_controller_get_channel_config(
	const struct vc_controller *ctrl, uint8_t ch, struct vc_channel_config *cfg)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}
	return vc_channel_get_config(&ctrl->channels[ch], cfg);
}

void vc_controller_set_storage_backend(
	struct vc_controller *ctrl, const struct vc_storage_backend *backend)
{
	ctrl->storage = backend;
}

size_t vc_controller_channel_count(const struct vc_controller *ctrl)
{
	return ctrl->channel_count;
}

uint16_t vc_controller_channel_capabilities(const struct vc_controller *ctrl,
					    uint8_t ch)
{
	if (!channel_valid(ctrl, ch)) {
		return 0;
	}
	return ctrl->channels[ch].capabilities;
}

enum vc_status vc_controller_system_param_action(
	struct vc_controller *ctrl, enum vc_param_action action)
{
	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
		if (ctrl->storage == NULL || ctrl->storage->save_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		return ctrl->storage->save_system_config(&ctrl->sys_cfg) < 0
			? VC_ERR_STORAGE : VC_OK;
	case VC_PARAM_ACTION_LOAD: {
		struct vc_system_config cfg;

		if (ctrl->storage == NULL || ctrl->storage->load_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		if (ctrl->storage->load_system_config(&cfg) < 0) {
			return VC_ERR_STORAGE;
		}
		return vc_controller_set_system_config(ctrl, &cfg);
	}
	case VC_PARAM_ACTION_FACTORY_RESET:
		if (ctrl->storage != NULL && ctrl->storage->erase_all != NULL) {
			(void)ctrl->storage->erase_all();
		}
		ctrl->sys_cfg = default_system_config();
		ctrl->operating_mode = VC_OPERATING_MODE_NORMAL;
		return VC_OK;
	case VC_PARAM_ACTION_SOFTWARE_RESET:
#ifdef CONFIG_REBOOT
		sys_reboot(SYS_REBOOT_COLD);
#endif
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}

enum vc_status vc_controller_channel_param_action(
	struct vc_controller *ctrl, uint8_t ch, enum vc_param_action action)
{
	if (!channel_valid(ctrl, ch)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
		if (ctrl->storage == NULL || ctrl->storage->save_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		{
			struct vc_channel_config cfg;

			vc_channel_get_config(&ctrl->channels[ch], &cfg);
			return ctrl->storage->save_channel_config(ch, &cfg) < 0
				? VC_ERR_STORAGE : VC_OK;
		}
	case VC_PARAM_ACTION_LOAD: {
		struct vc_channel_config cfg;

		if (ctrl->storage == NULL || ctrl->storage->load_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		struct vc_channel_config loaded = cfg;

		if (ctrl->storage->load_channel_config(ch, &loaded) < 0) {
			return VC_ERR_STORAGE;
		}
		loaded.output_calib_k = cfg.output_calib_k;
		loaded.output_calib_b = cfg.output_calib_b;
		loaded.measured_voltage_calib_k = cfg.measured_voltage_calib_k;
		loaded.measured_voltage_calib_b = cfg.measured_voltage_calib_b;
		loaded.measured_current_calib_k = cfg.measured_current_calib_k;
		loaded.measured_current_calib_b = cfg.measured_current_calib_b;
		bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;

		return vc_channel_set_config(&ctrl->channels[ch], &loaded, cal);
	}
	case VC_PARAM_ACTION_FACTORY_RESET: {
		struct vc_channel_config cfg;

		vc_channel_get_config(&ctrl->channels[ch], &cfg);
		struct vc_channel_config defaults = (struct vc_channel_config){
			.voltage_limit_threshold = 20000,
			.current_limit_threshold = 32767,
			.output_calib_k = cfg.output_calib_k,
			.output_calib_b = cfg.output_calib_b,
			.measured_voltage_calib_k = cfg.measured_voltage_calib_k,
			.measured_voltage_calib_b = cfg.measured_voltage_calib_b,
			.measured_current_calib_k = cfg.measured_current_calib_k,
			.measured_current_calib_b = cfg.measured_current_calib_b,
		};
		bool cal = ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION;

		return vc_channel_set_config(&ctrl->channels[ch], &defaults, cal);
	}
	case VC_PARAM_ACTION_SOFTWARE_RESET:
#ifdef CONFIG_REBOOT
		sys_reboot(SYS_REBOOT_COLD);
#endif
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}
```

- [ ] **Step 3: Add vc_controller.c to CMakeLists**

Add to `lib/voltage_control/CMakeLists.txt`:

```cmake
zephyr_library_sources(vc_controller.c)
```

- [ ] **Step 4: Commit**

```bash
git add include/voltage_control/vc_controller.h lib/voltage_control/vc_controller.c lib/voltage_control/CMakeLists.txt
git commit -m "feat: add vc_controller with init, routing, drain, and system operations"
```

---

### Task 11: Create voltage controller tests

**Files:**
- Create: `tests/voltage_control/vc_controller/CMakeLists.txt`
- Create: `tests/voltage_control/vc_controller/prj.conf`
- Create: `tests/voltage_control/vc_controller/testcase.yaml`
- Create: `tests/voltage_control/vc_controller/src/main.c`

- [ ] **Step 1: Create test scaffold**

CMakeLists.txt:
```cmake
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
get_filename_component(HVB_FIRMWARE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES ${HVB_FIRMWARE_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vc_controller_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
```

prj.conf:
```
CONFIG_ZTEST=y
CONFIG_TEST_RANDOM_GENERATOR=y
CONFIG_SMF=y
```

testcase.yaml:
```yaml
tests:
  voltage_control.vc_controller:
    platform_allow: native_posix
    tags: voltage_control
```

- [ ] **Step 2: Create test main.c**

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

static const struct vc_channel_entry test_entries[] = {
	{ .dev = NULL, .index = 0, .capabilities = FULL_CAPS },
	{ .dev = NULL, .index = 1, .capabilities = FULL_CAPS },
};

static struct vc_controller *ctrl;

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	ctrl = vc_controller_init_static(test_entries, 2);
	zassert_not_null(ctrl);
}

ZTEST_SUITE(vc_controller, NULL, NULL, before_each, NULL, NULL);

ZTEST(vc_controller, test_init)
{
	zassert_equal(vc_controller_channel_count(ctrl), 2);
	zassert_equal(vc_controller_get_operating_mode(ctrl), VC_OPERATING_MODE_NORMAL);
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

ZTEST(vc_controller, test_consume_voltage_routes_and_drains)
{
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_consume_voltage(ctrl, 0, 1200);
	zassert_equal(ctrl->channels[0].measured_voltage, 1200);
	zassert_false(ctrl->channels[0].pending.valid);
}

ZTEST(vc_controller, test_consume_voltage_triggers_protection)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 5000;
	cfg.voltage_limit_threshold = 3000;
	cfg.voltage_protection_mode = VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION;
	cfg.voltage_protection_output_action = VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);
	vc_controller_tick(ctrl, 100);

	vc_controller_consume_voltage(ctrl, 0, 3100);

	zassert_true(ctrl->channels[0].active_fault_cause & VC_FAULT_VOLTAGE);
	zassert_false(ctrl->channels[0].output_enabled);
	zassert_false(ctrl->channels[0].pending.valid);
}

ZTEST(vc_controller, test_tick_ramps_and_drains)
{
	struct vc_channel_config cfg;

	vc_controller_get_channel_config(ctrl, 0, &cfg);
	cfg.configured_target_voltage = 1000;
	cfg.ramp_up_step = 0;
	vc_channel_set_config(&ctrl->channels[0], &cfg, false);
	vc_controller_channel_output_action(ctrl, 0, VC_OUTPUT_ACTION_ENABLE);

	vc_controller_tick(ctrl, 100);

	zassert_equal(ctrl->channels[0].operational_target_voltage, 1000);
	zassert_false(ctrl->channels[0].pending.valid);
}

ZTEST(vc_controller, test_calibration_unlock_and_mode_entry)
{
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP1), VC_OK);
	zassert_equal(vc_controller_calibration_unlock(ctrl, CAL_UNLOCK_STEP2), VC_OK);
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
	zassert_equal(cfg.slave_address, 1);
	zassert_equal(cfg.baud_rate_code, VC_BAUD_RATE_115200);
	zassert_equal(cfg.recovery_policy_mode, VC_RECOVERY_MANUAL_LATCH);
}
```

- [ ] **Step 3: Build and run**

```bash
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe
```

Expected: All tests PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/voltage_control/vc_controller/
git commit -m "test: add vc_controller tests for routing, drain, mode transitions"
```

---

### Task 12: Run both test suites and verify all pass

- [ ] **Step 1: Run vc_channel_state tests**

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe
```

Expected: All tests PASS.

- [ ] **Step 2: Run vc_controller tests**

```bash
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe
```

Expected: All tests PASS.

- [ ] **Step 3: Run existing domain tests (should still pass — nothing removed yet)**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe
```

Expected: All existing tests PASS. The old domain_state.c is untouched in this plan — Plan C will wire the new modules in and retire the old code.

- [ ] **Step 4: Final commit with all test results verified**

```bash
git log --oneline -12
```

Verify the commit history shows the progression: header → test scaffold → init → output action → consume voltage → consume current → tick ramp → set config → calibration → controller → controller tests.

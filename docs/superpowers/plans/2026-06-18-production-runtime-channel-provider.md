# Production Runtime Channel Provider Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the production runtime seam between the Domain Runtime Library and Virtual Channel Providers using provider-applied Runtime Config Snapshots and provider-published Measurement Snapshots.

**Architecture:** The Domain Runtime Library remains the single owner of product policy and Domain Snapshots. Virtual Channel Providers apply latest-wins raw runtime config and publish raw evidence asynchronously. Frontend adapters continue to use the domain facade only and never touch providers.

**Tech Stack:** Zephyr 3.7.2, ztest, `k_msgq`, `k_mutex`, `k_work_delayable`, devicetree channel aggregation, existing `vc_channel_api`, native_posix tests, `jw_hvb` board build.

---

## File Structure

- Create `include/voltage_control/runtime.h`: public runtime data contracts and runtime facade declarations used by the app, tests, and providers.
- Modify `include/voltage_control/domain.h`: expose only narrow runtime-facing domain functions for config/evidence/snapshot behavior; keep existing Modbus/domain APIs intact.
- Modify `lib/voltage_control/domain.c`: add runtime config snapshot generation, measurement snapshot consumption, provider fault projection, and coherent snapshot helpers.
- Create `lib/voltage_control/runtime.c`: Zephyr runtime service wrapper around `struct domain`; owns command queue, policy tick, provider evidence drain, and snapshot publication.
- Modify `lib/voltage_control/CMakeLists.txt`: compile `runtime.c` behind `CONFIG_VC_RUNTIME`.
- Modify `lib/voltage_control/Kconfig`: add `VC_RUNTIME` and queue sizing symbols.
- Create `tests/voltage_control/runtime/`: ztest target for runtime/provider seam with fake providers and no hardware.
- Modify `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`: add provider runtime data for latest config application and measurement publication after the domain/runtime contracts exist.
- Modify `applications/hvb_controller/src/main.c`: replace hard-coded `dev = NULL` channels with devicetree `vc_domain_channels`, initialize runtime, confirm safe state, then start Modbus.
- Modify `applications/hvb_controller/prj.conf`: enable `CONFIG_VC_RUNTIME` after tests prove the runtime seam.
- Modify `boards/jianwei/jw_hvb/jw_hvb.dts`: enable provider nodes only in the final hardware-wiring slice.

---

### Task 1: Runtime Data Contracts

**Files:**
- Create: `include/voltage_control/runtime.h`
- Modify: `include/voltage_control/domain.h`
- Test: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Add failing compile-level test for runtime contracts**

Append this test near the top of `tests/voltage_control/domain/src/main.c`, after the existing includes:

```c
#include "voltage_control/runtime.h"
```

Append this test after the suite declaration:

```c
ZTEST(voltage_control_domain, test_runtime_contract_defaults_are_zeroable)
{
	struct vc_runtime_config_snapshot cfg = {0};
	struct vc_measurement_snapshot meas = {0};

	zassert_equal(cfg.channel, 0);
	zassert_equal(cfg.version, 0);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_false(cfg.calibration_mode);
	zassert_false(cfg.calibration_output_enable);
	zassert_equal(cfg.calibration_raw_output_drive, 0);
	zassert_false(cfg.force_safe_state);

	zassert_equal(meas.channel, 0);
	zassert_equal(meas.generation, 0);
	zassert_equal(meas.timestamp_ms, 0);
	zassert_equal(meas.present_mask, 0);
	zassert_equal(meas.raw_voltage, 0);
	zassert_equal(meas.raw_current, 0);
	zassert_equal(meas.provider_status, 0);
	zassert_equal(meas.provider_fault_cause, 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because `voltage_control/runtime.h` does not exist.

- [ ] **Step 3: Create runtime contract header**

Create `include/voltage_control/runtime.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_RUNTIME_H
#define VOLTAGE_CONTROL_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "voltage_control/domain.h"

#define VC_MEAS_PRESENT_VOLTAGE         0x0001
#define VC_MEAS_PRESENT_CURRENT         0x0002
#define VC_MEAS_PRESENT_PROVIDER_STATUS 0x0004

#define VC_PROVIDER_STATUS_READY        0x0001
#define VC_PROVIDER_STATUS_APPLY_FAILED 0x0002
#define VC_PROVIDER_STATUS_SAMPLE_ERROR 0x0004
#define VC_PROVIDER_STATUS_INTERLOCK    0x0008

struct vc_runtime_config_snapshot {
	uint8_t channel;
	uint32_t version;
	uint16_t capability_flags;

	bool output_enable;
	uint16_t raw_output_drive;

	bool calibration_mode;
	bool calibration_output_enable;
	uint16_t calibration_raw_output_drive;

	bool force_safe_state;
};

struct vc_measurement_snapshot {
	uint8_t channel;
	uint32_t generation;
	uint32_t timestamp_ms;

	uint16_t present_mask;
	int32_t raw_voltage;
	int32_t raw_current;
	uint16_t provider_status;
	uint16_t provider_fault_cause;
};

struct vc_runtime;

#endif
```

- [ ] **Step 4: Add domain runtime facade declarations**

Modify `include/voltage_control/domain.h` by adding forward declarations before `#endif`:

```c
struct vc_runtime_config_snapshot;
struct vc_measurement_snapshot;

enum vc_status domain_get_runtime_config(const struct domain *domain,
						 uint8_t channel,
						 struct vc_runtime_config_snapshot *cfg);
enum vc_status domain_consume_measurement(struct domain *domain,
						  const struct vc_measurement_snapshot *meas);
```

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS, including `test_runtime_contract_defaults_are_zeroable`.

- [ ] **Step 6: Commit**

```bash
git add include/voltage_control/runtime.h include/voltage_control/domain.h tests/voltage_control/domain/src/main.c
git commit -m "feat: add voltage-control runtime contracts"
```

---

### Task 2: Domain Runtime Config Snapshot Generation

**Files:**
- Modify: `lib/voltage_control/domain.c`
- Test: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing test for initial safe runtime config**

Append this test to `tests/voltage_control/domain/src/main.c`:

```c
ZTEST(voltage_control_domain, test_initial_runtime_config_is_safe)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_equal(cfg.channel, 0);
	zassert_equal(cfg.version, 1);
	zassert_equal(cfg.capability_flags,
		      CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
		      CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_false(cfg.calibration_mode);
	zassert_false(cfg.calibration_output_enable);
	zassert_equal(cfg.calibration_raw_output_drive, 0);
	zassert_true(cfg.force_safe_state);

	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: FAIL at link time because `domain_get_runtime_config` is declared but not defined.

- [ ] **Step 3: Include runtime header and add config version state**

Modify `lib/voltage_control/domain.c` includes:

```c
#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"
#include "regmap/hvb_regs.h"
```

Modify `struct vc_channel_runtime` in `lib/voltage_control/domain.c`:

```c
struct vc_channel_runtime {
	bool output_enabled;
	bool ramping;
	uint32_t ramp_accum_ms;
	uint32_t cooldown_remaining_ms;
	uint16_t cal_max_raw_dac_limit;
	uint32_t runtime_config_version;
};
```

In `domain_create`, after `domain->runtime[i].cal_max_raw_dac_limit = 0xFFFF;`, add:

```c
		domain->runtime[i].runtime_config_version = 1;
```

- [ ] **Step 4: Implement runtime config snapshot getter**

Add this function near the other public domain functions in `lib/voltage_control/domain.c`:

```c
enum vc_status domain_get_runtime_config(const struct domain *domain,
						 uint8_t channel,
						 struct vc_runtime_config_snapshot *cfg)
{
	const struct vc_channel_snapshot *snap;
	const struct vc_channel_runtime *rt;

	if (!cfg) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!channel_valid(domain, channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	snap = &domain->snapshots[channel];
	rt = &domain->runtime[channel];

	*cfg = (struct vc_runtime_config_snapshot){
		.channel = channel,
		.version = rt->runtime_config_version,
		.capability_flags = domain->ch_entry[channel].capabilities,
		.output_enable = rt->output_enabled,
		.raw_output_drive = snap->raw_dac_readback,
		.calibration_mode = domain->operating_mode == VC_OPERATING_MODE_CALIBRATION,
		.calibration_output_enable = snap->cal_output_enabled != 0,
		.calibration_raw_output_drive = snap->raw_dac_readback,
		.force_safe_state = !rt->output_enabled && snap->raw_dac_readback == 0,
	};

	return VC_OK;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c
git commit -m "feat: expose domain runtime config snapshots"
```

---

### Task 3: Runtime Config Version Bumps On Intent Changes

**Files:**
- Modify: `lib/voltage_control/domain.c`
- Test: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing test for version bump on output action**

Append this test:

```c
ZTEST(voltage_control_domain, test_runtime_config_version_bumps_on_output_enable)
{
	struct domain *d = domain_setup_fresh();
	struct vc_runtime_config_snapshot before;
	struct vc_runtime_config_snapshot after;

	zassert_equal(domain_get_runtime_config(d, 0, &before), VC_OK);
	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_get_runtime_config(d, 0, &after), VC_OK);

	zassert_true(after.version > before.version);
	zassert_true(after.output_enable);
	zassert_false(after.force_safe_state);

	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because `version` does not increase when output intent changes.

- [ ] **Step 3: Add runtime config dirty helper**

Add this helper after `channel_has_cap` in `lib/voltage_control/domain.c`:

```c
static void mark_runtime_config_changed(struct domain *domain, uint8_t channel)
{
	if (channel_valid(domain, channel)) {
		domain->runtime[channel].runtime_config_version++;
		if (domain->runtime[channel].runtime_config_version == 0) {
			domain->runtime[channel].runtime_config_version = 1;
		}
	}
}
```

- [ ] **Step 4: Call helper from output intent paths**

In `domain_channel_output_action`, after each successful change to `runtime[channel].output_enabled`, `runtime[channel].ramping`, `snapshots[channel].raw_dac_readback`, or `snapshots[channel].operational_target_voltage`, call:

```c
mark_runtime_config_changed(domain, channel);
```

Minimum required placements for this task:

```c
case VC_OUTPUT_ACTION_ENABLE:
	domain->runtime[channel].output_enabled = true;
	domain->runtime[channel].ramping = true;
	mark_runtime_config_changed(domain, channel);
	return VC_OK;

case VC_OUTPUT_ACTION_DISABLE_IMMEDIATE:
	domain->runtime[channel].output_enabled = false;
	domain->runtime[channel].ramping = false;
	domain->snapshots[channel].raw_dac_readback = 0;
	domain->snapshots[channel].operational_target_voltage = 0;
	mark_runtime_config_changed(domain, channel);
	return VC_OK;
```

If the existing switch body has additional status-bit updates, preserve them and add the helper without removing behavior.

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c
git commit -m "feat: version runtime config intent changes"
```

---

### Task 4: Measurement Snapshot Consumption

**Files:**
- Modify: `lib/voltage_control/domain.c`
- Test: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing test for raw evidence consumption**

Append this test:

```c
ZTEST(voltage_control_domain, test_domain_consumes_measurement_snapshot)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 1234,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE |
			      VC_MEAS_PRESENT_CURRENT |
			      VC_MEAS_PRESENT_PROVIDER_STATUS,
		.raw_voltage = 1200,
		.raw_current = 34,
		.provider_status = VC_PROVIDER_STATUS_READY,
	};
	struct vc_channel_snapshot snap;

	zassert_equal(domain_consume_measurement(d, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 1200);
	zassert_equal(snap.raw_adc_current, 34);
	zassert_equal(snap.measured_voltage, 1200);
	zassert_equal(snap.measured_current, 34);
	zassert_equal(snap.active_fault_cause, 0);

	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: FAIL at link time because `domain_consume_measurement` is not defined.

- [ ] **Step 3: Implement measurement consumption**

Add this function to `lib/voltage_control/domain.c`:

```c
enum vc_status domain_consume_measurement(struct domain *domain,
						  const struct vc_measurement_snapshot *meas)
{
	struct vc_channel_snapshot *snap;
	const struct vc_channel_config *cfg;

	if (!meas) {
		return VC_ERR_INVALID_VALUE;
	}
	if (!channel_valid(domain, meas->channel)) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	snap = &domain->snapshots[meas->channel];
	cfg = &domain->channels[meas->channel];

	if (meas->present_mask & VC_MEAS_PRESENT_VOLTAGE) {
		snap->raw_adc_voltage = meas->raw_voltage;
		snap->measured_voltage = (int16_t)
			((meas->raw_voltage * cfg->measured_voltage_calib_k) / 10000 +
			 cfg->measured_voltage_calib_b);
	}

	if (meas->present_mask & VC_MEAS_PRESENT_CURRENT) {
		snap->raw_adc_current = meas->raw_current;
		snap->measured_current = (int16_t)
			((meas->raw_current * cfg->measured_current_calib_k) / 10000 +
			 cfg->measured_current_calib_b);
	}

	if ((meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) &&
	    (meas->provider_status & VC_PROVIDER_STATUS_INTERLOCK)) {
		snap->active_fault_cause |= VC_FAULT_INTERLOCK;
		snap->fault_history_cause |= VC_FAULT_INTERLOCK;
	}

	if ((meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) &&
	    (meas->provider_status & VC_PROVIDER_STATUS_APPLY_FAILED)) {
		snap->active_fault_cause |= VC_FAULT_HARDWARE;
		snap->fault_history_cause |= VC_FAULT_HARDWARE;
	}

	return VC_OK;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c
git commit -m "feat: consume provider measurement snapshots"
```

---

### Task 5: Provider Failure Evidence Projection

**Files:**
- Modify: `lib/voltage_control/domain.c`
- Test: `tests/voltage_control/domain/src/main.c`

- [ ] **Step 1: Write failing test for provider apply failure forcing safe config**

Append this test:

```c
ZTEST(voltage_control_domain, test_provider_apply_failure_blocks_channel_and_forces_safe_config)
{
	struct domain *d = domain_setup_fresh();
	struct vc_measurement_snapshot fail = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 1,
		.present_mask = VC_MEAS_PRESENT_PROVIDER_STATUS,
		.provider_status = VC_PROVIDER_STATUS_APPLY_FAILED,
	};
	struct vc_channel_snapshot snap;
	struct vc_runtime_config_snapshot cfg;

	zassert_equal(domain_channel_output_action(d, 0,
					     VC_OUTPUT_ACTION_ENABLE), VC_OK);
	zassert_equal(domain_consume_measurement(d, &fail), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_true((snap.active_fault_cause & VC_FAULT_HARDWARE) != 0);
	zassert_true((snap.fault_history_cause & VC_FAULT_HARDWARE) != 0);

	zassert_equal(domain_get_runtime_config(d, 0, &cfg), VC_OK);
	zassert_false(cfg.output_enable);
	zassert_equal(cfg.raw_output_drive, 0);
	zassert_true(cfg.force_safe_state);

	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: FAIL because provider apply failure records fault but does not force safe runtime intent.

- [ ] **Step 3: Add safe-state helper**

Add this helper to `lib/voltage_control/domain.c`:

```c
static void force_channel_safe(struct domain *domain, uint8_t channel)
{
	domain->runtime[channel].output_enabled = false;
	domain->runtime[channel].ramping = false;
	domain->snapshots[channel].raw_dac_readback = 0;
	domain->snapshots[channel].operational_target_voltage = 0;
	mark_runtime_config_changed(domain, channel);
}
```

- [ ] **Step 4: Use safe-state helper on hard provider evidence**

Modify the `VC_PROVIDER_STATUS_INTERLOCK` and `VC_PROVIDER_STATUS_APPLY_FAILED` branches in `domain_consume_measurement`:

```c
	if ((meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) &&
	    (meas->provider_status & VC_PROVIDER_STATUS_INTERLOCK)) {
		snap->active_fault_cause |= VC_FAULT_INTERLOCK;
		snap->fault_history_cause |= VC_FAULT_INTERLOCK;
		force_channel_safe(domain, meas->channel);
	}

	if ((meas->present_mask & VC_MEAS_PRESENT_PROVIDER_STATUS) &&
	    (meas->provider_status & VC_PROVIDER_STATUS_APPLY_FAILED)) {
		snap->active_fault_cause |= VC_FAULT_HARDWARE;
		snap->fault_history_cause |= VC_FAULT_HARDWARE;
		force_channel_safe(domain, meas->channel);
	}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/domain.c tests/voltage_control/domain/src/main.c
git commit -m "feat: project provider failures into safe runtime intent"
```

---

### Task 6: Runtime Service Skeleton With Command Queue

**Files:**
- Create: `lib/voltage_control/runtime.c`
- Modify: `include/voltage_control/runtime.h`
- Modify: `lib/voltage_control/CMakeLists.txt`
- Modify: `lib/voltage_control/Kconfig`
- Create: `tests/voltage_control/runtime/CMakeLists.txt`
- Create: `tests/voltage_control/runtime/prj.conf`
- Create: `tests/voltage_control/runtime/testcase.yaml`
- Create: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Create runtime test target**

Create `tests/voltage_control/runtime/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
get_filename_component(HVB_FIRMWARE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES ${HVB_FIRMWARE_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(voltage_control_runtime)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
```

Create `tests/voltage_control/runtime/prj.conf`:

```conf
CONFIG_ZTEST=y
CONFIG_VC_RUNTIME=y
```

Create `tests/voltage_control/runtime/testcase.yaml`:

```yaml
tests:
  voltage_control.runtime:
    platform_allow: native_posix
    integration_platforms:
      - native_posix
```

- [ ] **Step 2: Write failing runtime creation test**

Create `tests/voltage_control/runtime/src/main.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/runtime.h"

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
};

ZTEST_SUITE(voltage_control_runtime, NULL, NULL, NULL, NULL, NULL);

ZTEST(voltage_control_runtime, test_runtime_create_and_destroy)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);
	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: FAIL because `CONFIG_VC_RUNTIME`, `vc_runtime_create`, and `vc_runtime_destroy` are undefined.

- [ ] **Step 4: Add runtime Kconfig and CMake wiring**

Append to `lib/voltage_control/Kconfig`:

```kconfig

config VC_RUNTIME
	bool "Voltage-control production runtime service"
	help
	  Enable the Zephyr-native domain runtime service wrapper.

config VC_RUNTIME_COMMAND_QUEUE_DEPTH
	int "Voltage-control runtime command queue depth"
	default 8
	depends on VC_RUNTIME

config VC_RUNTIME_EVIDENCE_QUEUE_DEPTH
	int "Voltage-control runtime evidence queue depth"
	default 4
	depends on VC_RUNTIME
```

Modify `lib/voltage_control/CMakeLists.txt`:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_RUNTIME runtime.c)
```

- [ ] **Step 5: Add runtime facade declarations**

Append to `include/voltage_control/runtime.h` before `#endif`:

```c
struct vc_runtime *vc_runtime_create(struct domain *domain);
void vc_runtime_destroy(struct vc_runtime *runtime);
```

- [ ] **Step 6: Implement minimal runtime create/destroy**

Create `lib/voltage_control/runtime.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "voltage_control/runtime.h"

struct vc_runtime {
	struct domain *domain;
	struct k_mutex lock;
};

struct vc_runtime *vc_runtime_create(struct domain *domain)
{
	struct vc_runtime *runtime;

	if (!domain) {
		return NULL;
	}

	runtime = calloc(1, sizeof(*runtime));
	if (!runtime) {
		return NULL;
	}

	runtime->domain = domain;
	k_mutex_init(&runtime->lock);

	return runtime;
}

void vc_runtime_destroy(struct vc_runtime *runtime)
{
	free(runtime);
}
```

- [ ] **Step 7: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add include/voltage_control/runtime.h lib/voltage_control/runtime.c lib/voltage_control/CMakeLists.txt lib/voltage_control/Kconfig tests/voltage_control/runtime
git commit -m "feat: add voltage-control runtime service skeleton"
```

---

### Task 7: Runtime Evidence Submission

**Files:**
- Modify: `include/voltage_control/runtime.h`
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Write failing test for runtime evidence submission**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_runtime_submit_measurement_updates_domain_snapshot)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_measurement_snapshot meas = {
		.channel = 0,
		.generation = 1,
		.timestamp_ms = 10,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 77,
	};
	struct vc_channel_snapshot snap;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_submit_measurement(rt, &meas), VC_OK);
	zassert_equal(domain_get_channel_snapshot(d, 0, &snap), VC_OK);
	zassert_equal(snap.raw_adc_voltage, 77);
	zassert_equal(snap.measured_voltage, 77);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: FAIL because `vc_runtime_submit_measurement` is undefined.

- [ ] **Step 3: Add declaration**

Append to `include/voltage_control/runtime.h` before `#endif`:

```c
enum vc_status vc_runtime_submit_measurement(struct vc_runtime *runtime,
						     const struct vc_measurement_snapshot *meas);
```

- [ ] **Step 4: Implement locked evidence submission**

Append to `lib/voltage_control/runtime.c`:

```c
enum vc_status vc_runtime_submit_measurement(struct vc_runtime *runtime,
						     const struct vc_measurement_snapshot *meas)
{
	enum vc_status status;

	if (!runtime || !meas) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&runtime->lock, K_FOREVER);
	status = domain_consume_measurement(runtime->domain, meas);
	k_mutex_unlock(&runtime->lock);

	return status;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/voltage_control/runtime.h lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: submit provider evidence through runtime service"
```

---

### Task 8: Runtime Config Read For Providers

**Files:**
- Modify: `include/voltage_control/runtime.h`
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Write failing test for provider config read**

Append this test:

```c
ZTEST(voltage_control_runtime, test_runtime_get_channel_config_for_provider)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_runtime_config_snapshot cfg;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	zassert_equal(vc_runtime_get_channel_config(rt, 0, &cfg), VC_OK);
	zassert_equal(cfg.channel, 0);
	zassert_equal(cfg.version, 1);
	zassert_true(cfg.force_safe_state);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: FAIL because `vc_runtime_get_channel_config` is undefined.

- [ ] **Step 3: Add declaration**

Append to `include/voltage_control/runtime.h` before `#endif`:

```c
enum vc_status vc_runtime_get_channel_config(struct vc_runtime *runtime,
						     uint8_t channel,
						     struct vc_runtime_config_snapshot *cfg);
```

- [ ] **Step 4: Implement locked config read**

Append to `lib/voltage_control/runtime.c`:

```c
enum vc_status vc_runtime_get_channel_config(struct vc_runtime *runtime,
						     uint8_t channel,
						     struct vc_runtime_config_snapshot *cfg)
{
	enum vc_status status;

	if (!runtime || !cfg) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&runtime->lock, K_FOREVER);
	status = domain_get_runtime_config(runtime->domain, channel, cfg);
	k_mutex_unlock(&runtime->lock);

	return status;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/voltage_control/runtime.h lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: expose runtime config snapshots to providers"
```

---

### Task 9: Provider Apply Helper

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add provider apply unit seam test using fake API**

Append this local fake helper and test to `tests/voltage_control/runtime/src/main.c`:

```c
struct fake_apply_state {
	uint16_t output;
	bool enabled;
	int output_ret;
	int enable_ret;
};

static int fake_apply_config(struct fake_apply_state *state,
				     const struct vc_runtime_config_snapshot *cfg)
{
	if (cfg->force_safe_state) {
		state->output = 0;
		state->enabled = false;
		return 0;
	}

	if (state->output_ret < 0) {
		return state->output_ret;
	}
	state->output = cfg->raw_output_drive;

	if (state->enable_ret < 0) {
		return state->enable_ret;
	}
	state->enabled = cfg->output_enable;

	return 0;
}

ZTEST(voltage_control_runtime, test_fake_provider_apply_safe_state)
{
	struct fake_apply_state state = {
		.output = 123,
		.enabled = true,
	};
	struct vc_runtime_config_snapshot cfg = {
		.force_safe_state = true,
	};

	zassert_equal(fake_apply_config(&state, &cfg), 0);
	zassert_equal(state.output, 0);
	zassert_false(state.enabled);
}
```

- [ ] **Step 2: Run runtime tests**

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS. This test captures provider apply semantics before changing the hardware provider.

- [ ] **Step 3: Add runtime include and provider data fields**

Modify `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c` includes:

```c
#include "voltage_control/runtime.h"
```

Add a provider data struct after `struct hvb_vc_config`:

```c
struct hvb_vc_data {
	uint32_t applied_config_version;
	uint32_t generation;
	uint16_t provider_status;
};
```

- [ ] **Step 4: Add provider apply helper**

Add this helper before `hvb_vc_init`:

```c
static int hvb_vc_apply_config(const struct device *dev,
				       const struct vc_runtime_config_snapshot *cfg)
{
	struct hvb_vc_data *data = dev->data;
	int ret;

	if (cfg->version == data->applied_config_version) {
		return 0;
	}

	if (cfg->force_safe_state) {
		(void)hvb_vc_set_output(dev, 0);
		(void)hvb_vc_set_enable(dev, false);
		data->applied_config_version = cfg->version;
		return 0;
	}

	if (cfg->calibration_mode) {
		ret = hvb_vc_set_output(dev, cfg->calibration_raw_output_drive);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		ret = hvb_vc_set_enable(dev, cfg->calibration_output_enable);
	} else {
		ret = hvb_vc_set_output(dev, cfg->raw_output_drive);
		if (ret < 0) {
			data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
			return ret;
		}
		ret = hvb_vc_set_enable(dev, cfg->output_enable);
	}

	if (ret < 0) {
		data->provider_status |= VC_PROVIDER_STATUS_APPLY_FAILED;
		return ret;
	}

	data->provider_status |= VC_PROVIDER_STATUS_READY;
	data->provider_status &= ~VC_PROVIDER_STATUS_APPLY_FAILED;
	data->applied_config_version = cfg->version;

	return 0;
}
```

- [ ] **Step 5: Wire provider data into device definition**

Modify `HVB_VC_INIT(n)` in `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`:

```c
#define HVB_VC_INIT(n) \
	static struct hvb_vc_data hvb_vc_data_##n; \
	static const struct hvb_vc_config hvb_vc_config_##n = { \
		.dac = DEVICE_DT_GET(DT_INST_PHANDLE(n, dac)), \
		.adc = DEVICE_DT_GET(DT_INST_PHANDLE(n, adc)), \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
		.max_raw_dac = DT_INST_PROP(n, max_raw_dac), \
		.sample_rate_ms = DT_INST_PROP(n, sample_rate_ms), \
		.capabilities = DT_INST_PROP(n, capabilities), \
	}; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		&hvb_vc_data_##n, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_api);
```

- [ ] **Step 6: Run runtime and domain tests**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: add HVB provider runtime config apply helper"
```

---

### Task 10: Production App Runtime Wiring

**Files:**
- Modify: `applications/hvb_controller/src/main.c`
- Modify: `applications/hvb_controller/prj.conf`

- [ ] **Step 1: Add runtime and controller externs**

Modify includes in `applications/hvb_controller/src/main.c`:

```c
#include "voltage_control/runtime.h"
```

Add extern declarations near the globals:

```c
extern const struct vc_channel_entry vc_domain_channels[];
extern const size_t vc_domain_channel_count;

static struct vc_runtime *runtime;
```

- [ ] **Step 2: Replace hard-coded channel table with DTS table**

Replace the hard-coded `static const struct vc_channel_entry hvb_channels[] = { ... };` block and `domain_create(hvb_channels, 2);` call with:

```c
	domain = domain_create(vc_domain_channels, vc_domain_channel_count);
```

- [ ] **Step 3: Create runtime before adapters**

After successful `domain_create`, add:

```c
	runtime = vc_runtime_create(domain);
	if (!runtime) {
		printk("Failed to create voltage-control runtime\n");
		return 0;
	}
```

- [ ] **Step 4: Enable runtime and provider configs in app config**

Append to `applications/hvb_controller/prj.conf`:

```conf
CONFIG_VC_RUNTIME=y
CONFIG_VC_CHANNEL_CONTROLLER=y
```

- [ ] **Step 5: Build native tests first**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 6: Build HVB app**

Run: `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: If `vc-controller` and provider nodes are still disabled, build may fail because `CONFIG_VC_CHANNEL_CONTROLLER` depends on enabled DTS nodes. If it fails for that reason, do not force-enable hardware nodes in this task; proceed to Task 11 where hardware node enablement is deliberate.

- [ ] **Step 7: Commit if native tests pass**

```bash
git add applications/hvb_controller/src/main.c applications/hvb_controller/prj.conf
git commit -m "feat: initialize controller runtime from DTS channel table"
```

---

### Task 11: Enable HVB Provider Nodes Deliberately

**Files:**
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts`
- Modify: `applications/hvb_controller/prj.conf`

- [ ] **Step 1: Enable ADS1232 and VC channel nodes**

Modify `boards/jianwei/jw_hvb/jw_hvb.dts`:

```dts
ads1232_hv1: ads1232_hv1 {
	compatible = "ti,ads1232";
	status = "okay";
```

```dts
ads1232_hv2: ads1232_hv2 {
	compatible = "ti,ads1232";
	status = "okay";
```

```dts
vc_ch0: vc-channel {
	compatible = "jianwei,hvb-vc-channel";
	status = "okay";
```

```dts
vc_ch1: vc-channel {
	compatible = "jianwei,hvb-vc-channel";
	status = "okay";
```

- [ ] **Step 2: Enable provider driver dependencies**

Append to `applications/hvb_controller/prj.conf`:

```conf
CONFIG_ADC=y
CONFIG_DAC=y
CONFIG_SPI=y
CONFIG_HVB_VC_CHANNEL=y
CONFIG_AD5541=y
CONFIG_ADS1232=y
```

These symbols match the current driver Kconfig files: `CONFIG_AD5541` is defined by `drivers/dac/ad5541/Kconfig`, and `CONFIG_ADS1232` is defined by `drivers/sensor/ads1232/Kconfig`.

- [ ] **Step 3: Build HVB app**

Run: `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: PASS. If it fails because of device init priority or missing Kconfig symbol names, fix only the minimal Kconfig/CMake wiring needed for these already-present drivers to compile.

- [ ] **Step 4: Verify native tests still pass**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add boards/jianwei/jw_hvb/jw_hvb.dts applications/hvb_controller/prj.conf
git commit -m "feat: enable HVB virtual channel providers"
```

---

### Task 12: Runtime Design Documentation Cross-References

**Files:**
- Modify: `docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md`
- Modify: `docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md`

- [ ] **Step 1: Mark production runtime spec as the active runtime design**

In `docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md`, add this sentence after line 10:

```markdown
The concrete production runtime interaction between the Domain Runtime Library and Virtual Channel Providers is defined in `docs/superpowers/specs/2026-06-18-production-runtime-channel-provider-design.md`.
```

- [ ] **Step 2: Mark old runtime draft as superseded**

In `docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md`, change the status block to:

```markdown
Status: Superseded by `2026-06-18-production-runtime-channel-provider-design.md`
```

- [ ] **Step 3: Commit docs**

```bash
git add docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md docs/superpowers/specs/2026-06-19-vc-channel-runtime-design.md
git commit -m "docs: link active production runtime design"
```

---

## Final Verification

- [ ] Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`

Expected: PASS.

- [ ] Run: `west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS.

- [ ] Run: `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: PASS.

- [ ] Run architecture smoke test if still present: `tests/architecture/controller_split.sh`

Expected: PASS.

---

## Plan Self-Review

Spec coverage:

- Domain-owned policy and single-writer state: Tasks 2, 4, 5, 6, 7, 8.
- Runtime Config Snapshot: Tasks 1, 2, 3, 8, 9.
- Measurement Snapshot: Tasks 1, 4, 5, 7.
- Frontend/provider separation: Tasks 6, 10, final verification.
- Provider apply path: Task 9.
- Safe startup and controllable channel exposure: Tasks 10, 11.
- Calibration Mode interaction: Task 2 establishes runtime fields; Tasks 4, 7, and 8 establish the evidence/config path used by calibration sample requests.
- Transport choice: Tasks 6, 7, and 8 establish the runtime facade and locking seam. The next plan should extend `runtime.c` with queued command processing, delayed policy ticks, provider-owned delayed sample work, per-source stale timestamp tracking, and hardware safe-state confirmation.

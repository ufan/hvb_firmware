# Evidence Freshness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect stale measurement data and surface it as a diagnostic flag, while migrating `VC_MAX_CHANNELS` from Kconfig to a DTS-derived compile-time constant.

**Architecture:** Per-channel measurement buffer as a RAM iterable section with numeric sorting for O(1) indexed access. Staleness is derived at snapshot publish time from buffer timestamps — flag only, no output action. Channel count comes from the `vc-controller` DTS node's `channels` phandle array length.

**Tech Stack:** Zephyr RTOS, devicetree, iterable sections (linker), C99, Twister test framework.

---

### Task 1: Test DTS Binding and Overlays

**Files:**
- Create: `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml`
- Create: `tests/voltage_control/domain/boards/native_posix.overlay`
- Create: `tests/voltage_control/runtime/boards/native_posix.overlay`
- Create: `tests/voltage_control/provider_bus/boards/native_posix.overlay`
- Create: `tests/voltage_control/modbus_adapter/boards/native_posix.overlay`

- [ ] **Step 1: Create stub channel binding**

Create `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml`:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: Stub VC channel for native_posix tests

compatible: "jianwei,vc-channel-stub"

properties:
  channel-index:
    type: int
    required: true

  capabilities:
    type: int
    required: true
```

- [ ] **Step 2: Create native_posix overlay for domain tests**

Create `tests/voltage_control/domain/boards/native_posix.overlay`:

```dts
/ {
	vc_ch0: vc-channel-0 {
		compatible = "jianwei,vc-channel-stub";
		channel-index = <0>;
		capabilities = <0x000f>;
	};
	vc_ch1: vc-channel-1 {
		compatible = "jianwei,vc-channel-stub";
		channel-index = <1>;
		capabilities = <0x000f>;
	};
	vc_controller: vc-controller {
		compatible = "jianwei,vc-controller";
		channels = <&vc_ch0 &vc_ch1>;
		status = "okay";
	};
};
```

- [ ] **Step 3: Copy overlay for runtime, provider_bus, and modbus_adapter tests**

Create these directories and identical overlay files:
- `tests/voltage_control/runtime/boards/native_posix.overlay`
- `tests/voltage_control/provider_bus/boards/native_posix.overlay`
- `tests/voltage_control/modbus_adapter/boards/native_posix.overlay`

Each is identical to the domain overlay from Step 2.

- [ ] **Step 4: Build all tests to verify overlays parse**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: 4 of 4 test configurations passed, 149 total test cases.

- [ ] **Step 5: Commit**

```bash
git add dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml \
       tests/voltage_control/*/boards/native_posix.overlay
git commit -m "feat: add stub DTS binding and test overlays for native_posix"
```

---

### Task 2: Replace VC_MAX_CHANNELS with DTS-Derived Constant

**Files:**
- Modify: `include/voltage_control/domain.h:14`
- Modify: `lib/voltage_control/Kconfig:65-71`

- [ ] **Step 1: Replace macro in domain.h**

In `include/voltage_control/domain.h`, replace line 14:

```c
#define VC_MAX_CHANNELS CONFIG_VC_MAX_CHANNELS
```

with:

```c
#include <zephyr/devicetree.h>

#define VC_MAX_CHANNELS DT_PROP_LEN(DT_NODELABEL(vc_controller), channels)
```

- [ ] **Step 2: Remove Kconfig entry**

In `lib/voltage_control/Kconfig`, delete lines 65-71:

```kconfig
config VC_MAX_CHANNELS
	int "Maximum voltage-control channels"
	default 2
	range 1 16
	help
	  Build-time maximum number of logical voltage-control channels.
	  Board topology should keep this aligned with devicetree channel records.
```

- [ ] **Step 3: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: 4 of 4 passed, 149 total test cases. `VC_MAX_CHANNELS` now resolves to `2` from the DTS overlay instead of Kconfig.

- [ ] **Step 4: Build jw_hvb to verify production board**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5`

Expected: clean build. `VC_MAX_CHANNELS` resolves to `2` from `jw_hvb.dts`.

- [ ] **Step 5: Commit**

```bash
git add include/voltage_control/domain.h lib/voltage_control/Kconfig
git commit -m "feat: derive VC_MAX_CHANNELS from devicetree"
```

---

### Task 3: Add VC_FAULT_STALE and Status Bit

**Files:**
- Modify: `include/voltage_control/domain.h:28`
- Modify: `lib/voltage_control/Kconfig` (append)

- [ ] **Step 1: Add fault flag to domain.h**

In `include/voltage_control/domain.h`, after the `VC_FAULT_CFG_INVALID` line, add:

```c
#define VC_FAULT_STALE         0x0080
```

- [ ] **Step 2: Add Kconfig for stale timeout**

In `lib/voltage_control/Kconfig`, after the `VC_PROVIDER_MSGQ_DEPTH` config block (before `VC_MODBUS_UNIT_ID`), add:

```kconfig
config VC_MEASUREMENT_STALE_TIMEOUT_MS
	int "Measurement staleness timeout in milliseconds"
	default 5000
	depends on VC_PROVIDER_BUS
	help
	  Time after the last measurement arrival before the channel
	  is flagged stale. Only applies to channels with voltage or
	  current measurement capability.
```

- [ ] **Step 3: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: 4 of 4 passed, 149 test cases. No behavior change yet — just new constants.

- [ ] **Step 4: Commit**

```bash
git add include/voltage_control/domain.h lib/voltage_control/Kconfig
git commit -m "feat: add VC_FAULT_STALE flag and staleness timeout config"
```

---

### Task 4: Measurement Buffer Iterable Section

**Files:**
- Modify: `include/voltage_control/provider_bus.h`
- Modify: `lib/voltage_control/provider_bus.c`
- Modify: `lib/voltage_control/provider_bus_sections.ld`
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c:308-328`

- [ ] **Step 1: Add buffer struct and API to provider_bus.h**

In `include/voltage_control/provider_bus.h`, after the `vc_provider_binding` struct (before the `extern` line), add:

```c
struct vc_measurement_buffer_entry {
	struct k_mutex lock;
	struct vc_measurement_snapshot snapshot;
};

void vc_measurement_buffer_init(void);
enum vc_status vc_measurement_buffer_store(uint8_t channel,
					   const struct vc_measurement_snapshot *meas);
enum vc_status vc_measurement_buffer_read(uint8_t channel,
					  struct vc_measurement_snapshot *meas);
size_t vc_measurement_buffer_count(void);
```

- [ ] **Step 2: Add linker section**

In `lib/voltage_control/provider_bus_sections.ld`, after the existing `ITERABLE_SECTION_RAM` line, add:

```
	ITERABLE_SECTION_RAM_NUMERIC(vc_measurement_buffer_entry, 4)
```

- [ ] **Step 3: Implement buffer functions in provider_bus.c**

In `lib/voltage_control/provider_bus.c`, add at the end of the file:

```c
void vc_measurement_buffer_init(void)
{
	STRUCT_SECTION_FOREACH(vc_measurement_buffer_entry, entry) {
		k_mutex_init(&entry->lock);
		memset(&entry->snapshot, 0, sizeof(entry->snapshot));
	}
}

enum vc_status vc_measurement_buffer_store(uint8_t channel,
					   const struct vc_measurement_snapshot *meas)
{
	struct vc_measurement_buffer_entry *entry;
	size_t count;

	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	if (channel >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	STRUCT_SECTION_GET(vc_measurement_buffer_entry, channel, &entry);
	k_mutex_lock(&entry->lock, K_FOREVER);
	entry->snapshot = *meas;
	k_mutex_unlock(&entry->lock);

	return VC_OK;
}

enum vc_status vc_measurement_buffer_read(uint8_t channel,
					  struct vc_measurement_snapshot *meas)
{
	struct vc_measurement_buffer_entry *entry;
	size_t count;

	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	if (channel >= count) {
		return VC_ERR_UNSUPPORTED_CHANNEL;
	}

	STRUCT_SECTION_GET(vc_measurement_buffer_entry, channel, &entry);
	k_mutex_lock(&entry->lock, K_FOREVER);
	*meas = entry->snapshot;
	k_mutex_unlock(&entry->lock);

	return VC_OK;
}

size_t vc_measurement_buffer_count(void)
{
	size_t count;

	STRUCT_SECTION_COUNT(vc_measurement_buffer_entry, &count);
	return count;
}
```

- [ ] **Step 4: Register buffer entries in driver macro**

In `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`, inside the `HVB_VC_INIT(n)` macro, after the `STRUCT_SECTION_ITERABLE(vc_provider_binding, ...)` block (before the closing `;` of the macro), add:

```c
	STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry, \
		_##n##_, hvb_meas_buf_##n) = {0};
```

- [ ] **Step 5: Call buffer init from provider_bus_init**

In `lib/voltage_control/provider_bus.c`, at the end of `vc_provider_bus_init()` (after the evidence msgq purge), add:

```c
	vc_measurement_buffer_init();
```

- [ ] **Step 6: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: 4 of 4 passed, 149 test cases. Buffer infrastructure exists but isn't wired into the runtime yet.

- [ ] **Step 7: Build jw_hvb**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5`

Expected: clean build. Two `vc_measurement_buffer_entry` instances linked via the numeric section.

- [ ] **Step 8: Commit**

```bash
git add include/voltage_control/provider_bus.h \
       lib/voltage_control/provider_bus.c \
       lib/voltage_control/provider_bus_sections.ld \
       drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c
git commit -m "feat: add measurement buffer as RAM iterable section"
```

---

### Task 5: Wire Buffer into Runtime and Compute Staleness

**Files:**
- Modify: `lib/voltage_control/domain_runtime.c:93-111` (publish_snapshot)
- Modify: `lib/voltage_control/domain_runtime.c:156-161` (worker measurement loop)

- [ ] **Step 1: Store measurement in buffer before domain consumption**

In `lib/voltage_control/domain_runtime.c`, in `vc_runtime_worker()`, change the measurement consumption loop (lines 156-161) from:

```c
		while (vc_provider_bus_take_measurement(&evidence.measurement) == VC_OK) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			vc_runtime_publish_all_configs(runtime);
			k_mutex_unlock(&runtime->lock);
		}
```

to:

```c
		while (vc_provider_bus_take_measurement(&evidence.measurement) == VC_OK) {
			(void)vc_measurement_buffer_store(evidence.measurement.channel,
							  &evidence.measurement);
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			vc_runtime_publish_all_configs(runtime);
			k_mutex_unlock(&runtime->lock);
		}
```

- [ ] **Step 2: Compute staleness in vc_runtime_publish_snapshot**

In `lib/voltage_control/domain_runtime.c`, in `vc_runtime_publish_snapshot()`, after the loop that calls `domain_get_channel_snapshot` and `domain_get_channel_config` (after the `for` loop, before the uptime line), add staleness computation:

Replace the current function body (lines 93-111):

```c
static void vc_runtime_publish_snapshot(struct vc_runtime *runtime)
{
	uint16_t count = domain_get_supported_channel_count(runtime->domain);

	k_mutex_lock(&runtime->lock, K_FOREVER);
	domain_process_periodic(runtime->domain, 0);
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	domain_get_system_snapshot(runtime->domain, &runtime->published.system);
	domain_get_system_config(runtime->domain, &runtime->published.sys_config);
	for (uint8_t ch = 0; ch < count; ch++) {
		domain_get_channel_snapshot(runtime->domain, ch,
					   &runtime->published.channels[ch]);
		domain_get_channel_config(runtime->domain, ch,
					  &runtime->published.configs[ch]);
	}
	runtime->published.system.uptime = (uint32_t)(k_uptime_get() / 1000);
	k_mutex_unlock(&runtime->snapshot_lock);
	k_mutex_unlock(&runtime->lock);
}
```

with:

```c
static void vc_runtime_publish_snapshot(struct vc_runtime *runtime)
{
	uint16_t count = domain_get_supported_channel_count(runtime->domain);
	uint32_t now_ms = k_uptime_get_32();

	k_mutex_lock(&runtime->lock, K_FOREVER);
	domain_process_periodic(runtime->domain, 0);
	k_mutex_lock(&runtime->snapshot_lock, K_FOREVER);
	domain_get_system_snapshot(runtime->domain, &runtime->published.system);
	domain_get_system_config(runtime->domain, &runtime->published.sys_config);
	for (uint8_t ch = 0; ch < count; ch++) {
		domain_get_channel_snapshot(runtime->domain, ch,
					   &runtime->published.channels[ch]);
		domain_get_channel_config(runtime->domain, ch,
					  &runtime->published.configs[ch]);

		struct vc_channel_snapshot *snap = &runtime->published.channels[ch];
		uint16_t caps = snap->channel_capability_flags;
		bool has_meas = (caps & CH_CAP_VOLTAGE_MEASUREMENT) ||
				(caps & CH_CAP_CURRENT_MEASUREMENT);

		if (has_meas) {
			struct vc_measurement_snapshot meas;

			if (vc_measurement_buffer_read(ch, &meas) == VC_OK &&
			    meas.timestamp_ms != 0) {
				uint32_t elapsed = now_ms - meas.timestamp_ms;

				if (elapsed >= CONFIG_VC_MEASUREMENT_STALE_TIMEOUT_MS) {
					snap->fault_history_cause |= VC_FAULT_STALE;
					snap->active_fault_cause |= VC_FAULT_STALE;
					snap->status_bits |= 0x0040;
				}
			}
		}
	}
	runtime->published.system.uptime = (uint32_t)(k_uptime_get() / 1000);
	k_mutex_unlock(&runtime->snapshot_lock);
	k_mutex_unlock(&runtime->lock);
}
```

Note: include the capabilities header at the top of `domain_runtime.c`. Add after the existing includes:

```c
#include "jianwei,vc-channel-capabilities.h"
```

- [ ] **Step 3: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: 4 of 4 passed, 149 test cases. No staleness triggered in tests because no measurement buffer entries exist in test builds (no driver macro registrations). All existing behavior unchanged.

- [ ] **Step 4: Build jw_hvb**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/domain_runtime.c
git commit -m "feat: wire measurement buffer and staleness detection into runtime"
```

---

### Task 6: Staleness Tests

**Files:**
- Modify: `tests/voltage_control/runtime/src/main.c`
- Modify: `tests/voltage_control/runtime/prj.conf`

- [ ] **Step 1: Enable stale timeout in runtime test config**

In `tests/voltage_control/runtime/prj.conf`, add:

```
CONFIG_VC_MEASUREMENT_STALE_TIMEOUT_MS=100
```

- [ ] **Step 2: Add test for VC_FAULT_STALE flag value**

In `tests/voltage_control/runtime/src/main.c`, add at the end (before the closing of the file):

```c
ZTEST(voltage_control_runtime, test_stale_fault_flag_is_distinct)
{
	zassert_equal(VC_FAULT_STALE, 0x0080);
	zassert_equal(VC_FAULT_STALE & VC_FAULT_MEASUREMENT, 0);
	zassert_equal(VC_FAULT_STALE & VC_FAULT_VOLTAGE, 0);
}
```

- [ ] **Step 3: Add test for measurement buffer store and read round-trip**

```c
ZTEST(voltage_control_runtime, test_measurement_buffer_store_and_read)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 5,
		.timestamp_ms = 12345,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 100,
	};
	struct vc_measurement_snapshot out;
	size_t count = vc_measurement_buffer_count();

	if (count == 0) {
		ztest_test_skip();
		return;
	}

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in), VC_OK);
	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 5);
	zassert_equal(out.timestamp_ms, 12345);
	zassert_equal(out.raw_voltage, 100);
}
```

- [ ] **Step 4: Add test for buffer validation**

```c
ZTEST(voltage_control_runtime, test_measurement_buffer_rejects_null)
{
	struct vc_measurement_snapshot meas;

	zassert_equal(vc_measurement_buffer_store(0, NULL), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_measurement_buffer_read(0, NULL), VC_ERR_INVALID_VALUE);
	zassert_equal(vc_measurement_buffer_store(99, &meas),
		      VC_ERR_UNSUPPORTED_CHANNEL);
	zassert_equal(vc_measurement_buffer_read(99, &meas),
		      VC_ERR_UNSUPPORTED_CHANNEL);
}
```

- [ ] **Step 5: Run tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: all pass. Buffer tests may skip if no buffer entries exist in the test build (no driver registrations). Validation tests should pass regardless.

- [ ] **Step 6: Commit**

```bash
git add tests/voltage_control/runtime/src/main.c \
       tests/voltage_control/runtime/prj.conf
git commit -m "feat: add staleness and measurement buffer tests"
```

---

### Task 7: Register Test Buffer Entries for Provider Bus Tests

**Files:**
- Modify: `tests/voltage_control/provider_bus/src/main.c`
- Modify: `tests/voltage_control/provider_bus/prj.conf`

- [ ] **Step 1: Add buffer entries to provider_bus test**

In `tests/voltage_control/provider_bus/src/main.c`, after the existing `STRUCT_SECTION_ITERABLE(vc_provider_binding, ...)` declarations, add:

```c
STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry,
	_0_, test_meas_buf_0) = {0};
STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry,
	_1_, test_meas_buf_1) = {0};
```

- [ ] **Step 2: Add stale timeout to test config**

In `tests/voltage_control/provider_bus/prj.conf`, add:

```
CONFIG_VC_MEASUREMENT_STALE_TIMEOUT_MS=100
```

- [ ] **Step 3: Add buffer tests to provider_bus suite**

In `tests/voltage_control/provider_bus/src/main.c`, add at the end:

```c
ZTEST(voltage_control_provider_bus, test_measurement_buffer_count_matches_entries)
{
	zassert_equal(vc_measurement_buffer_count(), 2);
}

ZTEST(voltage_control_provider_bus, test_measurement_buffer_store_read_round_trip)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 3,
		.timestamp_ms = 500,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 42,
	};
	struct vc_measurement_snapshot out;

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in), VC_OK);
	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 3);
	zassert_equal(out.timestamp_ms, 500);
	zassert_equal(out.raw_voltage, 42);
}

ZTEST(voltage_control_provider_bus, test_measurement_buffer_indexed_access)
{
	struct vc_measurement_snapshot in0 = {
		.channel = 0, .generation = 1, .timestamp_ms = 100,
	};
	struct vc_measurement_snapshot in1 = {
		.channel = 1, .generation = 2, .timestamp_ms = 200,
	};
	struct vc_measurement_snapshot out;

	vc_measurement_buffer_init();
	zassert_equal(vc_measurement_buffer_store(0, &in0), VC_OK);
	zassert_equal(vc_measurement_buffer_store(1, &in1), VC_OK);

	zassert_equal(vc_measurement_buffer_read(0, &out), VC_OK);
	zassert_equal(out.generation, 1);

	zassert_equal(vc_measurement_buffer_read(1, &out), VC_OK);
	zassert_equal(out.generation, 2);
}
```

- [ ] **Step 4: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Expected: all pass with additional buffer tests.

- [ ] **Step 5: Commit**

```bash
git add tests/voltage_control/provider_bus/src/main.c \
       tests/voltage_control/provider_bus/prj.conf
git commit -m "feat: add measurement buffer tests with fake iterable entries"
```

---

### Task 8: Final Verification and Ledger Update

**Files:**
- Modify: `docs/superpowers/project-status.md`

- [ ] **Step 1: Run full test suite**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -20`

Record exact test counts.

- [ ] **Step 2: Build jw_hvb**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5`

Expected: clean build.

- [ ] **Step 3: Update project status ledger**

In `docs/superpowers/project-status.md`:

1. Add a new row to the **Verified Committed Work** table:

```
| Evidence freshness | `verified` | Measurement buffer as RAM iterable section, VC_FAULT_STALE flag, DTS-derived VC_MAX_CHANNELS. Verified N/N tests, `jw_hvb` build clean. |
```

2. Clear **Active Uncommitted Work** section (set to `None.`).

3. Update the **Evidence freshness** row in **Remaining Gaps** from `deferred` to `verified`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/project-status.md
git commit -m "docs: record evidence freshness verification status"
```

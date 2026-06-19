# Static Provider Bus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a static Zephyr Provider Bus so Virtual Channel Providers can act as independent actuators/samplers coordinated by the Domain Runtime without runtime pointer wiring.

**Architecture:** Static kernel objects carry tiny routing/evidence messages, while runtime config lives in per-channel static slots acquired by const pointer. Provider bindings are declared from device instances and collected through iterable sections; runtime/domain topology remains DTS/Kconfig/device-instance driven.

**Tech Stack:** Zephyr 3.7.2, `k_msgq`, `k_mutex`, `k_work_delayable`, iterable sections, devicetree instance macros, ztest, native_posix, `jw_hvb` board build.

---

## File Structure

- Create `include/voltage_control/provider_bus.h`: neutral provider bus contracts, config-slot lease API, provider message/evidence API, provider binding descriptor.
- Create `lib/voltage_control/provider_bus.c`: static queues, static config slots, config acquire/release, config publication, provider message/evidence publication and drain helpers, iterable binding helpers.
- Modify `include/voltage_control/domain.h`: replace the literal channel-count macro with a Kconfig-backed build-time channel count.
- Modify `lib/voltage_control/CMakeLists.txt`: compile `provider_bus.c` behind `CONFIG_VC_PROVIDER_BUS`.
- Modify `lib/voltage_control/Kconfig`: add `VC_PROVIDER_BUS`, queue depths, and select it from `VC_RUNTIME`.
- Modify `include/voltage_control/vc_channel.h`: add provider lifecycle/notification callbacks to `vc_channel_api`.
- Modify `lib/voltage_control/runtime.c`: publish runtime config slots/messages after domain changes and drain static provider evidence queue instead of private evidence queue.
- Modify `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`: remove runtime dependency, add provider worker, add static iterable binding records, publish measurement evidence through Provider Bus.
- Modify `tests/voltage_control/runtime/src/main.c`: add provider bus/config/evidence tests.
- Modify `tests/voltage_control/runtime/prj.conf`: enable provider bus test settings.
- Verify `applications/hvb_controller` builds for `jw_hvb`.

---

### Task 1: Provider Bus Contracts

**Files:**
- Create: `include/voltage_control/provider_bus.h`
- Modify: `include/voltage_control/domain.h`
- Modify: `lib/voltage_control/CMakeLists.txt`
- Modify: `lib/voltage_control/Kconfig`
- Modify: `tests/voltage_control/runtime/prj.conf`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing compile test for provider bus contracts**

Add this include to `tests/voltage_control/runtime/src/main.c` after the existing voltage-control includes:

```c
#include "voltage_control/provider_bus.h"
```

Append this test after `test_runtime_command_contract_defaults_are_zeroable`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_contract_defaults_are_zeroable)
{
	struct vc_provider_msg msg = {0};
	struct vc_runtime_config_slot slot = {0};
	struct vc_provider_binding binding = {0};

	zassert_equal(msg.type, VC_PROVIDER_MSG_CONFIG_CHANGED);
	zassert_equal(msg.channel, 0);
	zassert_equal(msg.config_version, 0);
	zassert_equal(slot.snapshot.channel, 0);
	zassert_is_null(binding.dev);
	zassert_is_null(binding.config_slot);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: FAIL because `voltage_control/provider_bus.h` does not exist.

- [ ] **Step 3: Add provider bus and channel-count Kconfig symbols**

Append to `lib/voltage_control/Kconfig` after `VC_RUNTIME_TICK_INTERVAL_MS`:

```kconfig
config VC_MAX_CHANNELS
	int "Maximum voltage-control channels"
	default 2
	range 1 16
	help
	  Build-time maximum number of logical voltage-control channels.
	  Board topology should keep this aligned with devicetree channel records.

config VC_PROVIDER_BUS
	bool "VC static provider bus"
	help
	  Enable the static provider bus used between the domain runtime and
	  virtual channel providers.

config VC_PROVIDER_MSGQ_DEPTH
	int "VC provider message queue depth"
	default 8
	range 1 64
	depends on VC_PROVIDER_BUS
	help
	  Maximum number of queued runtime-to-provider notifications.
```

Modify `include/voltage_control/domain.h` to replace the literal macro:

```c
#define VC_MAX_CHANNELS 2
```

with:

```c
#define VC_MAX_CHANNELS CONFIG_VC_MAX_CHANNELS
```

Modify `config VC_RUNTIME` in `lib/voltage_control/Kconfig` to select the provider bus:

```kconfig
config VC_RUNTIME
	bool "VC runtime service"
	select SMF
	select VC_PROVIDER_BUS
	help
	  Enable the voltage-control runtime service.
```

Modify `tests/voltage_control/runtime/prj.conf` to include:

```conf
CONFIG_VC_PROVIDER_BUS=y
CONFIG_VC_PROVIDER_MSGQ_DEPTH=2
```

- [ ] **Step 4: Create provider bus contract header**

Create `include/voltage_control/provider_bus.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_PROVIDER_BUS_H
#define VOLTAGE_CONTROL_PROVIDER_BUS_H

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>

#include "voltage_control/domain.h"
#include "voltage_control/runtime.h"

enum vc_provider_msg_type {
	VC_PROVIDER_MSG_CONFIG_CHANGED = 0,
	VC_PROVIDER_MSG_SAMPLE_NOW,
	VC_PROVIDER_MSG_STOP,
};

struct vc_provider_msg {
	enum vc_provider_msg_type type;
	uint8_t channel;
	uint32_t config_version;
};

struct vc_runtime_config_slot {
	struct k_mutex lock;
	struct vc_runtime_config_snapshot snapshot;
};

struct vc_provider_binding {
	uint8_t channel;
	const struct device *dev;
	struct vc_runtime_config_slot *config_slot;
	uint32_t route_bit;
};

extern struct vc_runtime_config_slot vc_runtime_config_slots[VC_MAX_CHANNELS];

void vc_provider_bus_init(void);
enum vc_status vc_provider_bus_publish_config(uint8_t channel,
						 const struct vc_runtime_config_snapshot *cfg);
const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel);
void vc_provider_bus_release_config(uint8_t channel);
enum vc_status vc_provider_bus_publish_measurement(
	const struct vc_measurement_snapshot *meas);
enum vc_status vc_provider_bus_take_measurement(struct vc_measurement_snapshot *meas);
enum vc_status vc_provider_bus_take_message(struct vc_provider_msg *msg,
					       k_timeout_t timeout);

#endif
```

- [ ] **Step 5: Add provider bus source to build**

Create an empty `lib/voltage_control/provider_bus.c` with only includes and stubs so the header links in later tasks:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/provider_bus.h"
```

Modify `lib/voltage_control/CMakeLists.txt` to add:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_PROVIDER_BUS provider_bus.c)
```

- [ ] **Step 6: Run test to verify compile passes**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime`

Expected: PASS compile for the new contract test. Link may fail for unimplemented provider bus functions only if tests call them; at this task they should not.

- [ ] **Step 7: Commit**

```bash
git add include/voltage_control/provider_bus.h include/voltage_control/domain.h lib/voltage_control/provider_bus.c lib/voltage_control/CMakeLists.txt lib/voltage_control/Kconfig tests/voltage_control/runtime/prj.conf tests/voltage_control/runtime/src/main.c
git commit -m "feat: add static provider bus contracts"
```

---

### Task 2: Static Config Slots And Provider Messages

**Files:**
- Modify: `lib/voltage_control/provider_bus.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing test for zero-copy config lease**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_config_slot_acquire_release)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 7,
		.capability_flags = CH_CAP_OUTPUT_ENABLE,
		.output_enable = true,
		.raw_output_drive = 123,
	};
	const struct vc_runtime_config_snapshot *borrowed;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);

	borrowed = vc_provider_bus_acquire_config(0);
	zassert_not_null(borrowed);
	zassert_equal(borrowed->version, 7);
	zassert_equal(borrowed->raw_output_drive, 123);
	vc_provider_bus_release_config(0);
}
```

- [ ] **Step 2: Add failing test for config-changed message**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_publish_config_posts_message)
{
	struct vc_runtime_config_snapshot cfg = {
		.channel = 0,
		.version = 9,
	};
	struct vc_provider_msg msg;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_config(0, &cfg), VC_OK);
	zassert_equal(vc_provider_bus_take_message(&msg, K_NO_WAIT), VC_OK);
	zassert_equal(msg.type, VC_PROVIDER_MSG_CONFIG_CHANGED);
	zassert_equal(msg.channel, 0);
	zassert_equal(msg.config_version, 9);
}
```

- [ ] **Step 3: Implement static provider bus objects**

Replace `lib/voltage_control/provider_bus.c` with:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "voltage_control/provider_bus.h"

struct vc_runtime_config_slot vc_runtime_config_slots[VC_MAX_CHANNELS];

K_MSGQ_DEFINE(vc_provider_msgq,
	      sizeof(struct vc_provider_msg),
	      CONFIG_VC_PROVIDER_MSGQ_DEPTH,
	      4);

K_MSGQ_DEFINE(vc_runtime_evidence_msgq,
	      sizeof(struct vc_measurement_snapshot),
	      CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH,
	      4);

void vc_provider_bus_init(void)
{
	for (size_t i = 0; i < VC_MAX_CHANNELS; i++) {
		k_mutex_init(&vc_runtime_config_slots[i].lock);
		memset(&vc_runtime_config_slots[i].snapshot, 0,
		       sizeof(vc_runtime_config_slots[i].snapshot));
	}
	k_msgq_purge(&vc_provider_msgq);
	k_msgq_purge(&vc_runtime_evidence_msgq);
}

enum vc_status vc_provider_bus_publish_config(uint8_t channel,
						 const struct vc_runtime_config_snapshot *cfg)
{
	struct vc_provider_msg msg;

	if (cfg == NULL || channel >= VC_MAX_CHANNELS) {
		return VC_ERR_INVALID_VALUE;
	}

	k_mutex_lock(&vc_runtime_config_slots[channel].lock, K_FOREVER);
	vc_runtime_config_slots[channel].snapshot = *cfg;
	k_mutex_unlock(&vc_runtime_config_slots[channel].lock);

	msg = (struct vc_provider_msg){
		.type = VC_PROVIDER_MSG_CONFIG_CHANGED,
		.channel = channel,
		.config_version = cfg->version,
	};

	if (k_msgq_put(&vc_provider_msgq, &msg, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

const struct vc_runtime_config_snapshot *vc_provider_bus_acquire_config(uint8_t channel)
{
	if (channel >= VC_MAX_CHANNELS) {
		return NULL;
	}

	k_mutex_lock(&vc_runtime_config_slots[channel].lock, K_FOREVER);
	return &vc_runtime_config_slots[channel].snapshot;
}

void vc_provider_bus_release_config(uint8_t channel)
{
	if (channel < VC_MAX_CHANNELS) {
		k_mutex_unlock(&vc_runtime_config_slots[channel].lock);
	}
}

enum vc_status vc_provider_bus_take_message(struct vc_provider_msg *msg,
					       k_timeout_t timeout)
{
	if (msg == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_get(&vc_provider_msgq, msg, timeout) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}
```

- [ ] **Step 4: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS including the two provider bus config tests.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/provider_bus.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: add static provider config slots"
```

---

### Task 3: Static Evidence Queue

**Files:**
- Modify: `lib/voltage_control/provider_bus.c`
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing test for provider bus evidence queue**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_measurement_queue_round_trip)
{
	struct vc_measurement_snapshot in = {
		.channel = 0,
		.generation = 3,
		.present_mask = VC_MEAS_PRESENT_VOLTAGE,
		.raw_voltage = 456,
	};
	struct vc_measurement_snapshot out;

	vc_provider_bus_init();
	zassert_equal(vc_provider_bus_publish_measurement(&in), VC_OK);
	zassert_equal(vc_provider_bus_take_measurement(&out), VC_OK);
	zassert_equal(out.channel, 0);
	zassert_equal(out.generation, 3);
	zassert_equal(out.raw_voltage, 456);
}
```

- [ ] **Step 2: Implement evidence queue APIs**

Append to `lib/voltage_control/provider_bus.c`:

```c
enum vc_status vc_provider_bus_publish_measurement(
	const struct vc_measurement_snapshot *meas)
{
	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_put(&vc_runtime_evidence_msgq, meas, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}

enum vc_status vc_provider_bus_take_measurement(struct vc_measurement_snapshot *meas)
{
	if (meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	if (k_msgq_get(&vc_runtime_evidence_msgq, meas, K_NO_WAIT) != 0) {
		return VC_ERR_UNSAFE_STATE;
	}

	return VC_OK;
}
```

- [ ] **Step 3: Switch runtime measurement submission to static evidence queue**

Modify `lib/voltage_control/runtime.c` includes to add:

```c
#include "voltage_control/provider_bus.h"
```

Replace the body of `vc_runtime_submit_measurement()` with:

```c
{
	if (runtime == NULL || meas == NULL) {
		return VC_ERR_INVALID_VALUE;
	}

	ARG_UNUSED(runtime);
	return vc_provider_bus_publish_measurement(meas);
}
```

Modify the runtime worker evidence drain loop to use the static queue. Replace:

```c
		while (k_msgq_get(&runtime->evidence_queue, &evidence, K_NO_WAIT) == 0) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			k_mutex_unlock(&runtime->lock);
		}
```

with:

```c
		while (vc_provider_bus_take_measurement(&evidence.measurement) == VC_OK) {
			k_mutex_lock(&runtime->lock, K_FOREVER);
			(void)domain_consume_measurement(runtime->domain, &evidence.measurement);
			k_mutex_unlock(&runtime->lock);
		}
```

- [ ] **Step 4: Initialize provider bus in runtime create**

In `vc_runtime_create()`, after `k_sem_init(&runtime->wake, 0, 1);`, add:

```c
	vc_provider_bus_init();
```

- [ ] **Step 5: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS. Existing `test_runtime_submit_measurement_updates_domain_snapshot` still proves runtime drains the static evidence queue.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/provider_bus.c lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: route measurement evidence through provider bus"
```

---

### Task 4: Runtime Publishes Config Slots

**Files:**
- Modify: `lib/voltage_control/runtime.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add failing test for runtime publishing initial config**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_runtime_create_publishes_initial_provider_config)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	const struct vc_runtime_config_snapshot *cfg;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	cfg = vc_provider_bus_acquire_config(0);
	zassert_not_null(cfg);
	zassert_equal(cfg->channel, 0);
	zassert_true(cfg->version >= 1);
	zassert_true(cfg->force_safe_state);
	vc_provider_bus_release_config(0);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 2: Add failing test for config-changed message after command**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_runtime_command_posts_provider_config_message)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_runtime *rt;
	struct vc_provider_msg msg;

	zassert_not_null(d);
	rt = vc_runtime_create(d);
	zassert_not_null(rt);

	while (vc_provider_bus_take_message(&msg, K_NO_WAIT) == VC_OK) {
	}

	zassert_equal(vc_runtime_set_operating_mode(rt, VC_OPERATING_MODE_AUTOMATIC, K_SECONDS(1)), VC_OK);
	zassert_equal(vc_provider_bus_take_message(&msg, K_SECONDS(1)), VC_OK);
	zassert_equal(msg.type, VC_PROVIDER_MSG_CONFIG_CHANGED);
	zassert_equal(msg.channel, 0);
	zassert_true(msg.config_version >= 1);

	vc_runtime_destroy(rt);
	free(d);
}
```

- [ ] **Step 3: Add runtime publish helper**

Add this helper to `lib/voltage_control/runtime.c` before `vc_runtime_worker()`:

```c
static void vc_runtime_publish_all_configs(struct vc_runtime *runtime)
{
	uint16_t count = domain_get_supported_channel_count(runtime->domain);

	for (uint8_t ch = 0; ch < count; ch++) {
		struct vc_runtime_config_snapshot cfg;

		if (domain_get_runtime_config(runtime->domain, ch, &cfg) == VC_OK) {
			(void)vc_provider_bus_publish_config(ch, &cfg);
		}
	}
}
```

- [ ] **Step 4: Publish initial configs in runtime create**

In `vc_runtime_create()`, after `vc_provider_bus_init();`, add:

```c
	vc_runtime_publish_all_configs(runtime);
```

- [ ] **Step 5: Publish configs after commands, evidence, and tick**

In `vc_runtime_worker()`, call `vc_runtime_publish_all_configs(runtime);` after processing a command, after consuming evidence, and after `domain_tick()`. The command loop should look like this:

```c
		while (k_msgq_get(&runtime->command_queue, &work, K_NO_WAIT) == 0) {
			enum vc_status result;

			k_mutex_lock(&runtime->lock, K_FOREVER);
			result = vc_runtime_dispatch_command(runtime, &work.command);
			vc_runtime_publish_all_configs(runtime);
			k_mutex_unlock(&runtime->lock);
			...
		}
```

The evidence loop should call the helper after `domain_consume_measurement()`. The tick block should call the helper after `domain_tick()`.

- [ ] **Step 6: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS including initial config and config-changed message tests.

- [ ] **Step 7: Commit**

```bash
git add lib/voltage_control/runtime.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: publish runtime configs to provider bus"
```

---

### Task 5: Provider Binding And Notification API

**Files:**
- Modify: `include/voltage_control/vc_channel.h`
- Modify: `include/voltage_control/provider_bus.h`
- Modify: `lib/voltage_control/provider_bus.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add provider API callbacks**

Modify `struct vc_channel_api` in `include/voltage_control/vc_channel.h` to add:

```c
	int (*start)(const struct device *dev);
	int (*stop)(const struct device *dev);
	int (*notify_config_changed)(const struct device *dev, uint32_t version);
```

Place them after `apply_config` and before measurement callbacks.

- [ ] **Step 2: Add binding iteration helpers to header**

Append these declarations to `include/voltage_control/provider_bus.h` before `#endif`:

```c
size_t vc_provider_bus_binding_count(void);
enum vc_status vc_provider_bus_start_all(void);
enum vc_status vc_provider_bus_notify_channel(uint8_t channel, uint32_t version);
```

- [ ] **Step 3: Implement binding count and notification routing**

Append this to `lib/voltage_control/provider_bus.c`:

```c
#include "voltage_control/vc_channel.h"

size_t vc_provider_bus_binding_count(void)
{
	size_t count = 0;
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		ARG_UNUSED(binding);
		count++;
	}
	return count;
}

enum vc_status vc_provider_bus_start_all(void)
{
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		const struct vc_channel_api *api = binding->dev->api;

		if (api != NULL && api->start != NULL) {
			int ret = api->start(binding->dev);
			if (ret < 0) {
				return VC_ERR_UNSAFE_STATE;
			}
		}
	}
	return VC_OK;
}

enum vc_status vc_provider_bus_notify_channel(uint8_t channel, uint32_t version)
{
	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		if (binding->channel == channel) {
			const struct vc_channel_api *api = binding->dev->api;

			if (api != NULL && api->notify_config_changed != NULL) {
				int ret = api->notify_config_changed(binding->dev, version);
				return ret < 0 ? VC_ERR_UNSAFE_STATE : VC_OK;
			}
			return VC_OK;
		}
	}
	return VC_ERR_UNSUPPORTED_CHANNEL;
}
```

- [ ] **Step 4: Route provider messages to bindings**

Add this helper to `lib/voltage_control/provider_bus.c`:

```c
enum vc_status vc_provider_bus_dispatch_one(k_timeout_t timeout)
{
	struct vc_provider_msg msg;
	enum vc_status status = vc_provider_bus_take_message(&msg, timeout);

	if (status != VC_OK) {
		return status;
	}

	switch (msg.type) {
	case VC_PROVIDER_MSG_CONFIG_CHANGED:
	case VC_PROVIDER_MSG_SAMPLE_NOW:
		return vc_provider_bus_notify_channel(msg.channel, msg.config_version);
	case VC_PROVIDER_MSG_STOP:
		return VC_OK;
	default:
		return VC_ERR_INVALID_VALUE;
	}
}
```

Add its declaration to `provider_bus.h`:

```c
enum vc_status vc_provider_bus_dispatch_one(k_timeout_t timeout);
```

- [ ] **Step 5: Add compile-only API test**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_binding_api_is_callable)
{
	zassert_true(vc_provider_bus_binding_count() >= 0);
	zassert_equal(vc_provider_bus_notify_channel(0, 1), VC_ERR_UNSUPPORTED_CHANNEL);
}
```

- [ ] **Step 6: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS. Native runtime tests have no provider bindings, so notify returns unsupported channel.

- [ ] **Step 7: Commit**

```bash
git add include/voltage_control/vc_channel.h include/voltage_control/provider_bus.h lib/voltage_control/provider_bus.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: add static provider binding routing"
```

---

### Task 6: HVB Provider Worker And Binding Records

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`

- [ ] **Step 1: Remove runtime include and include provider bus**

In `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`, replace:

```c
#include "voltage_control/runtime.h"
```

with:

```c
#include "voltage_control/provider_bus.h"
```

- [ ] **Step 2: Extend provider data**

Replace `struct hvb_vc_data` with:

```c
struct hvb_vc_data {
	struct k_work_delayable work;
	uint8_t channel;
	uint32_t applied_config_version;
	uint32_t pending_config_version;
	uint32_t generation;
	uint16_t provider_status;
};
```

- [ ] **Step 3: Add provider evidence helper**

Add before `hvb_vc_apply_config()`:

```c
static void hvb_vc_publish_measurement(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct hvb_vc_data *data = dev->data;
	struct vc_measurement_snapshot meas = {
		.channel = data->channel,
		.generation = ++data->generation,
		.timestamp_ms = (uint32_t)k_uptime_get_32(),
		.provider_status = data->provider_status | VC_PROVIDER_STATUS_READY,
	};
	int32_t value;

	if ((cfg->capabilities & CH_CAP_VOLTAGE_MEASUREMENT) != 0) {
		if (hvb_vc_measure_voltage(dev, &value) == 0) {
			meas.present_mask |= VC_MEAS_PRESENT_VOLTAGE;
			meas.raw_voltage = value;
		} else {
			meas.provider_status |= VC_PROVIDER_STATUS_SAMPLE_ERROR;
		}
	}

	if ((cfg->capabilities & CH_CAP_CURRENT_MEASUREMENT) != 0) {
		if (hvb_vc_measure_current(dev, &value) == 0) {
			meas.present_mask |= VC_MEAS_PRESENT_CURRENT;
			meas.raw_current = value;
		} else {
			meas.provider_status |= VC_PROVIDER_STATUS_SAMPLE_ERROR;
		}
	}

	meas.present_mask |= VC_MEAS_PRESENT_PROVIDER_STATUS;
	meas.provider_fault_cause = (meas.provider_status & VC_PROVIDER_STATUS_SAMPLE_ERROR) != 0 ?
		VC_FAULT_MEASUREMENT : 0;
	(void)vc_provider_bus_publish_measurement(&meas);
}
```

- [ ] **Step 4: Add provider work handler**

Add before `hvb_vc_api`:

```c
static void hvb_vc_work_handler(struct k_work *work)
{
	struct k_work_delayable *delayable = k_work_delayable_from_work(work);
	struct hvb_vc_data *data = CONTAINER_OF(delayable, struct hvb_vc_data, work);
	const struct device *dev = NULL;

	STRUCT_SECTION_FOREACH(vc_provider_binding, binding) {
		if (binding->channel == data->channel) {
			dev = binding->dev;
			break;
		}
	}

	if (dev != NULL) {
		const struct hvb_vc_config *cfg = dev->config;
		const struct vc_runtime_config_snapshot *runtime_cfg;

		runtime_cfg = vc_provider_bus_acquire_config(data->channel);
		if (runtime_cfg != NULL) {
			if (runtime_cfg->version != data->applied_config_version) {
				if (hvb_vc_apply_config(dev, runtime_cfg) == 0) {
					data->applied_config_version = runtime_cfg->version;
				}
			}
			vc_provider_bus_release_config(data->channel);
		}

		hvb_vc_publish_measurement(dev);
		k_work_schedule(&data->work, K_MSEC(cfg->sample_rate_ms));
	}
}
```

- [ ] **Step 5: Add start/stop/notify callbacks**

Add before `hvb_vc_api`:

```c
static int hvb_vc_start(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	return k_work_schedule(&data->work, K_NO_WAIT) < 0 ? -EIO : 0;
}

static int hvb_vc_stop(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	return k_work_cancel_delayable(&data->work);
}

static int hvb_vc_notify_config_changed(const struct device *dev, uint32_t version)
{
	struct hvb_vc_data *data = dev->data;

	data->pending_config_version = version;
	return k_work_schedule(&data->work, K_NO_WAIT) < 0 ? -EIO : 0;
}
```

Update `hvb_vc_api` to include:

```c
	.start = hvb_vc_start,
	.stop = hvb_vc_stop,
	.notify_config_changed = hvb_vc_notify_config_changed,
```

- [ ] **Step 6: Initialize work and channel in init**

In `hvb_vc_init()`, after GPIO configuration, add:

```c
	struct hvb_vc_data *data = dev->data;

	data->channel = cfg->channel_index;
	k_work_init_delayable(&data->work, hvb_vc_work_handler);
```

This requires adding `uint8_t channel_index;` to `struct hvb_vc_config` and initializing it in `HVB_VC_INIT(n)`:

```c
		.channel_index = DT_INST_PROP(n, channel_index), \
```

- [ ] **Step 7: Add iterable provider binding records**

Extend `HVB_VC_INIT(n)` after `DEVICE_DT_INST_DEFINE(...)` with:

```c
	STRUCT_SECTION_ITERABLE(vc_provider_binding, hvb_vc_binding_##n) = { \
		.channel = DT_INST_PROP(n, channel_index), \
		.dev = DEVICE_DT_INST_GET(n), \
		.config_slot = &vc_runtime_config_slots[DT_INST_PROP(n, channel_index)], \
		.route_bit = BIT(DT_INST_PROP(n, channel_index)), \
	};
```

The provider still acquires config through `vc_provider_bus_acquire_config(channel)` in this slice, but the binding record statically captures the config slot pointer for later direct descriptor-driven paths and for topology validation.

- [ ] **Step 8: Run board build**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c
git commit -m "feat: add HVB provider worker bindings"
```

---

### Task 7: Runtime Provider Dispatch Loop

**Files:**
- Modify: `lib/voltage_control/runtime.c`
- Modify: `applications/hvb_controller/src/main.c`
- Test: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add runtime dispatch call after config publish**

In `vc_runtime_publish_all_configs()` in `lib/voltage_control/runtime.c`, after each successful `vc_provider_bus_publish_config(ch, &cfg)`, add:

```c
			(void)vc_provider_bus_dispatch_one(K_NO_WAIT);
```

This keeps provider notification routing driven by static provider bus messages. It is intentionally non-blocking.

- [ ] **Step 2: Start all statically bound providers in app**

Add this include to `applications/hvb_controller/src/main.c`:

```c
#include "voltage_control/provider_bus.h"
```

After successful `vc_runtime_create(domain)`, add:

```c
	ret = vc_provider_bus_start_all();
	if (ret != VC_OK) {
		printk("Failed to start provider bus: %d\n", ret);
		return 0;
	}
```

- [ ] **Step 3: Add native no-binding start-all test**

Append this test to `tests/voltage_control/runtime/src/main.c`:

```c
ZTEST(voltage_control_runtime, test_provider_bus_start_all_without_bindings)
{
	zassert_equal(vc_provider_bus_start_all(), VC_OK);
}
```

- [ ] **Step 4: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: PASS.

- [ ] **Step 5: Run board build**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/runtime.c applications/hvb_controller/src/main.c tests/voltage_control/runtime/src/main.c
git commit -m "feat: dispatch static provider bus messages"
```

---

### Task 8: Final Verification

**Files:**
- No source changes expected.

- [ ] **Step 1: Run domain tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-domain tests/voltage_control/domain && ./build/test-domain/zephyr/zephyr.exe`

Expected: `SUITE PASS - 100.00% [voltage_control_domain]`.

- [ ] **Step 2: Run runtime tests**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b native_posix -d build/test-runtime tests/voltage_control/runtime && ./build/test-runtime/zephyr/zephyr.exe`

Expected: `SUITE PASS - 100.00% [voltage_control_runtime]`.

- [ ] **Step 3: Run production board build**

Run: `/home/yong/backup/src/xlab/jianwei/hvb_wkspc/.venv/bin/west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

Expected: build completes and links `zephyr/zephyr.elf`.

- [ ] **Step 4: Inspect git status**

Run: `git status --short --branch`

Expected: only intentional changes are present. Existing unrelated host-tools docs/diagrams must not be staged for this work.

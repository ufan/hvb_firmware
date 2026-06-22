# Settings Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist system and channel configs across power cycles using Zephyr Settings/NVS, with calibration stored separately so factory-reset preserves it.

**Architecture:** A `vc_storage_backend` interface decouples the domain from Zephyr Settings. The domain calls backend function pointers for save/load/erase. A Zephyr Settings/NVS implementation provides the real backend. The runtime injects the backend and auto-loads saved config at startup. Tests use a fake backend.

**Tech Stack:** Zephyr Settings subsystem, NVS, STM32 internal flash, C99, Twister.

---

### Task 1: Storage Interface Header

**Files:**
- Create: `include/voltage_control/vc_storage.h`

- [ ] **Step 1: Create the storage backend header**

Create `include/voltage_control/vc_storage.h`:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_STORAGE_H
#define VOLTAGE_CONTROL_VC_STORAGE_H

#include "voltage_control/domain.h"

struct vc_storage_backend {
	int (*save_system_config)(const struct vc_system_config *cfg);
	int (*load_system_config)(struct vc_system_config *cfg);
	int (*save_channel_config)(uint8_t ch, const struct vc_channel_config *cfg);
	int (*load_channel_config)(uint8_t ch, struct vc_channel_config *cfg);
	int (*save_channel_cal)(uint8_t ch, const struct vc_channel_config *cfg);
	int (*load_channel_cal)(uint8_t ch, struct vc_channel_config *cfg);
	int (*erase_all)(void);
};

#endif
```

- [ ] **Step 2: Run all tests to verify no breakage**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: 4 of 4 passed, 154 test cases. Header-only change, no impact.

- [ ] **Step 3: Commit**

```bash
git add include/voltage_control/vc_storage.h
git commit -m "feat: add vc_storage_backend interface header"
```

---

### Task 2: Inject Storage Backend into Domain

**Files:**
- Modify: `lib/voltage_control/domain_state.c:61-74` (struct domain)
- Modify: `include/voltage_control/domain.h`

- [ ] **Step 1: Add storage pointer to domain struct**

In `lib/voltage_control/domain_state.c`, add `#include "voltage_control/vc_storage.h"` after the existing includes (after line 8), and add the storage pointer to the `struct domain` (after the `cal_unlocked` field):

```c
const struct vc_storage_backend *storage;
```

So the struct becomes:

```c
struct domain {
	struct vc_domain_smf_ctx smf;
	const struct vc_channel_entry *ch_entry;
	size_t channel_count;
	enum vc_operating_mode operating_mode;
	uint32_t uptime_seconds;
	struct vc_system_config sys_cfg;
	struct vc_channel_config channels[VC_MAX_CHANNELS];
	struct vc_channel_snapshot snapshots[VC_MAX_CHANNELS];
	struct vc_channel_runtime runtime[VC_MAX_CHANNELS];
	uint16_t system_fault_cause;
	uint8_t cal_unlock_step;
	bool cal_unlocked;
	const struct vc_storage_backend *storage;
};
```

- [ ] **Step 2: Add domain_set_storage_backend declaration to domain.h**

In `include/voltage_control/domain.h`, after the forward declaration of `struct vc_measurement_snapshot;` (before `domain_get_runtime_config`), add:

```c
struct vc_storage_backend;

void domain_set_storage_backend(struct domain *domain,
				const struct vc_storage_backend *backend);
```

- [ ] **Step 3: Implement domain_set_storage_backend in domain_state.c**

In `lib/voltage_control/domain_state.c`, after `domain_init()` (after line 465), add:

```c
void domain_set_storage_backend(struct domain *domain,
				const struct vc_storage_backend *backend)
{
	if (domain != NULL) {
		domain->storage = backend;
	}
}
```

- [ ] **Step 4: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: 4 of 4 passed, 154 test cases. Storage pointer defaults to NULL (memset in domain_init). Existing param_action stubs still return VC_ERR_STORAGE.

- [ ] **Step 5: Commit**

```bash
git add include/voltage_control/domain.h lib/voltage_control/domain_state.c
git commit -m "feat: add storage backend injection to domain"
```

---

### Task 3: Implement param_action Functions

**Files:**
- Modify: `lib/voltage_control/domain_state.c:1177-1214`

- [ ] **Step 1: Define default config helpers**

In `lib/voltage_control/domain_state.c`, before `domain_system_param_action()` (before line 1177), add two static helpers:

```c
static struct vc_system_config domain_default_system_config(void)
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

static struct vc_channel_config domain_default_channel_config(void)
{
	return (struct vc_channel_config){
		.voltage_limit_threshold = VC_DEFAULT_MAX_VOLTAGE_RAW,
		.current_limit_threshold = VC_DEFAULT_MAX_CURRENT_RAW,
		.output_calib_k = 10000,
		.measured_voltage_calib_k = 10000,
		.measured_current_calib_k = 10000,
	};
}
```

Also update `domain_init()` to use the system helper (replace the inline initializer at line 443-450):

```c
domain->sys_cfg = domain_default_system_config();
```

And the channel loop body (replace lines 453-458):

```c
domain->channels[i] = domain_default_channel_config();
```

- [ ] **Step 2: Implement domain_system_param_action**

Replace the existing `domain_system_param_action()` (lines 1177-1192) with:

```c
enum vc_status domain_system_param_action(struct domain *domain,
					     enum vc_param_action action)
{
	switch (action) {
	case VC_PARAM_ACTION_NONE:
		return VC_OK;
	case VC_PARAM_ACTION_SAVE:
		if (domain->storage == NULL || domain->storage->save_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		return domain->storage->save_system_config(&domain->sys_cfg) < 0
			? VC_ERR_STORAGE : VC_OK;
	case VC_PARAM_ACTION_LOAD: {
		struct vc_system_config cfg;

		if (domain->storage == NULL || domain->storage->load_system_config == NULL) {
			return VC_ERR_STORAGE;
		}
		if (domain->storage->load_system_config(&cfg) < 0) {
			return VC_ERR_STORAGE;
		}
		return domain_set_system_config(domain, &cfg);
	}
	case VC_PARAM_ACTION_FACTORY_RESET:
		if (domain->storage != NULL && domain->storage->erase_all != NULL) {
			(void)domain->storage->erase_all();
		}
		domain->sys_cfg = domain_default_system_config();
		domain->operating_mode = VC_OPERATING_MODE_NORMAL;
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
```

Add `#include <zephyr/sys/reboot.h>` at the top of domain_state.c alongside the other includes.

- [ ] **Step 3: Implement domain_channel_param_action**

Replace the existing `domain_channel_param_action()` (lines 1194-1214) with:

```c
enum vc_status domain_channel_param_action(struct domain *domain,
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
		if (domain->storage == NULL || domain->storage->save_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		return domain->storage->save_channel_config(channel,
			&domain->channels[channel]) < 0 ? VC_ERR_STORAGE : VC_OK;
	case VC_PARAM_ACTION_LOAD: {
		struct vc_channel_config cfg = domain->channels[channel];

		if (domain->storage == NULL || domain->storage->load_channel_config == NULL) {
			return VC_ERR_STORAGE;
		}
		if (domain->storage->load_channel_config(channel, &cfg) < 0) {
			return VC_ERR_STORAGE;
		}
		cfg.output_calib_k = domain->channels[channel].output_calib_k;
		cfg.output_calib_b = domain->channels[channel].output_calib_b;
		cfg.measured_voltage_calib_k = domain->channels[channel].measured_voltage_calib_k;
		cfg.measured_voltage_calib_b = domain->channels[channel].measured_voltage_calib_b;
		cfg.measured_current_calib_k = domain->channels[channel].measured_current_calib_k;
		cfg.measured_current_calib_b = domain->channels[channel].measured_current_calib_b;
		return domain_set_channel_config(domain, channel, &cfg);
	}
	case VC_PARAM_ACTION_FACTORY_RESET: {
		struct vc_channel_config defaults = domain_default_channel_config();

		defaults.output_calib_k = domain->channels[channel].output_calib_k;
		defaults.output_calib_b = domain->channels[channel].output_calib_b;
		defaults.measured_voltage_calib_k = domain->channels[channel].measured_voltage_calib_k;
		defaults.measured_voltage_calib_b = domain->channels[channel].measured_voltage_calib_b;
		defaults.measured_current_calib_k = domain->channels[channel].measured_current_calib_k;
		defaults.measured_current_calib_b = domain->channels[channel].measured_current_calib_b;
		return domain_set_channel_config(domain, channel, &defaults);
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

- [ ] **Step 4: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: 4 of 4 passed, 154 test cases. With NULL storage backend, SAVE/LOAD/FACTORY_RESET still return VC_ERR_STORAGE (NULL check triggers). Existing tests unaffected.

- [ ] **Step 5: Commit**

```bash
git add lib/voltage_control/domain_state.c
git commit -m "feat: implement param_action with storage backend dispatch"
```

---

### Task 4: Tests with Fake Storage Backend

**Files:**
- Modify: `tests/voltage_control/runtime/src/main.c`

- [ ] **Step 1: Add fake storage backend**

In `tests/voltage_control/runtime/src/main.c`, after the existing includes, add:

```c
#include "voltage_control/vc_storage.h"

static struct vc_system_config fake_saved_sys;
static struct vc_channel_config fake_saved_ch[2];
static bool fake_sys_saved;
static bool fake_ch_saved[2];
static int fake_save_ret;
static int fake_load_ret;
static bool fake_erased;

static int fake_save_system_config(const struct vc_system_config *cfg)
{
	if (fake_save_ret < 0) {
		return fake_save_ret;
	}
	fake_saved_sys = *cfg;
	fake_sys_saved = true;
	return 0;
}

static int fake_load_system_config(struct vc_system_config *cfg)
{
	if (fake_load_ret < 0 || !fake_sys_saved) {
		return -ENOENT;
	}
	*cfg = fake_saved_sys;
	return 0;
}

static int fake_save_channel_config(uint8_t ch, const struct vc_channel_config *cfg)
{
	if (fake_save_ret < 0 || ch >= 2) {
		return fake_save_ret < 0 ? fake_save_ret : -EINVAL;
	}
	fake_saved_ch[ch] = *cfg;
	fake_ch_saved[ch] = true;
	return 0;
}

static int fake_load_channel_config(uint8_t ch, struct vc_channel_config *cfg)
{
	if (fake_load_ret < 0 || ch >= 2 || !fake_ch_saved[ch]) {
		return -ENOENT;
	}
	*cfg = fake_saved_ch[ch];
	return 0;
}

static int fake_save_channel_cal(uint8_t ch, const struct vc_channel_config *cfg)
{
	if (ch >= 2) {
		return -EINVAL;
	}
	fake_saved_ch[ch].output_calib_k = cfg->output_calib_k;
	fake_saved_ch[ch].output_calib_b = cfg->output_calib_b;
	fake_saved_ch[ch].measured_voltage_calib_k = cfg->measured_voltage_calib_k;
	fake_saved_ch[ch].measured_voltage_calib_b = cfg->measured_voltage_calib_b;
	fake_saved_ch[ch].measured_current_calib_k = cfg->measured_current_calib_k;
	fake_saved_ch[ch].measured_current_calib_b = cfg->measured_current_calib_b;
	return 0;
}

static int fake_load_channel_cal(uint8_t ch, struct vc_channel_config *cfg)
{
	if (ch >= 2 || !fake_ch_saved[ch]) {
		return -ENOENT;
	}
	cfg->output_calib_k = fake_saved_ch[ch].output_calib_k;
	cfg->output_calib_b = fake_saved_ch[ch].output_calib_b;
	cfg->measured_voltage_calib_k = fake_saved_ch[ch].measured_voltage_calib_k;
	cfg->measured_voltage_calib_b = fake_saved_ch[ch].measured_voltage_calib_b;
	cfg->measured_current_calib_k = fake_saved_ch[ch].measured_current_calib_k;
	cfg->measured_current_calib_b = fake_saved_ch[ch].measured_current_calib_b;
	return 0;
}

static int fake_erase_all(void)
{
	fake_sys_saved = false;
	fake_ch_saved[0] = false;
	fake_ch_saved[1] = false;
	fake_erased = true;
	return 0;
}

static const struct vc_storage_backend fake_storage = {
	.save_system_config = fake_save_system_config,
	.load_system_config = fake_load_system_config,
	.save_channel_config = fake_save_channel_config,
	.load_channel_config = fake_load_channel_config,
	.save_channel_cal = fake_save_channel_cal,
	.load_channel_cal = fake_load_channel_cal,
	.erase_all = fake_erase_all,
};

static void reset_fake_storage(void)
{
	memset(&fake_saved_sys, 0, sizeof(fake_saved_sys));
	memset(fake_saved_ch, 0, sizeof(fake_saved_ch));
	fake_sys_saved = false;
	fake_ch_saved[0] = false;
	fake_ch_saved[1] = false;
	fake_save_ret = 0;
	fake_load_ret = 0;
	fake_erased = false;
}
```

- [ ] **Step 2: Add system param_action tests**

At the end of the test file, add:

```c
ZTEST(voltage_control_runtime, test_system_save_and_load_round_trip)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_system_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 42), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_true(fake_sys_saved);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 99), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_LOAD), VC_OK);

	domain_get_system_config(d, &cfg);
	zassert_equal(cfg.slave_address, 42);

	free(d);
}

ZTEST(voltage_control_runtime, test_system_factory_reset_restores_defaults)
{
	struct domain *d = domain_create(test_channels, 1);
	struct vc_system_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_system_field(d, VC_FIELD_SLAVE_ADDRESS, 42), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_FACTORY_RESET), VC_OK);

	domain_get_system_config(d, &cfg);
	zassert_equal(cfg.slave_address, 1);

	free(d);
}

ZTEST(voltage_control_runtime, test_system_param_action_null_storage_returns_error)
{
	struct domain *d = domain_create(test_channels, 1);

	zassert_not_null(d);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_SAVE), VC_ERR_STORAGE);
	zassert_equal(domain_system_param_action(d, VC_PARAM_ACTION_LOAD), VC_ERR_STORAGE);

	free(d);
}
```

- [ ] **Step 3: Add channel param_action tests**

```c
ZTEST(voltage_control_runtime, test_channel_save_and_load_round_trip)
{
	struct domain *d = domain_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_SAVE), VC_OK);
	zassert_true(fake_ch_saved[0]);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       9999), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_LOAD), VC_OK);

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 5000);

	free(d);
}

ZTEST(voltage_control_runtime, test_channel_factory_reset_preserves_calibration)
{
	struct domain *d = domain_create(full_cap_channels, 1);
	struct vc_channel_config cfg;

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_MEASURED_V_CAL_K, 12345), VC_OK);
	zassert_equal(domain_set_channel_field(d, 0, VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
					       5000), VC_OK);
	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_FACTORY_RESET), VC_OK);

	domain_get_channel_config(d, 0, &cfg);
	zassert_equal(cfg.configured_target_voltage, 0);
	zassert_equal(cfg.measured_voltage_calib_k, 12345);

	free(d);
}

ZTEST(voltage_control_runtime, test_channel_load_no_saved_data_returns_error)
{
	struct domain *d = domain_create(full_cap_channels, 1);

	zassert_not_null(d);
	reset_fake_storage();
	domain_set_storage_backend(d, &fake_storage);

	zassert_equal(domain_channel_param_action(d, 0, VC_PARAM_ACTION_LOAD),
		      VC_ERR_STORAGE);

	free(d);
}
```

- [ ] **Step 4: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: 4 of 4 passed. New tests exercise save/load/factory-reset through fake backend.

- [ ] **Step 5: Commit**

```bash
git add tests/voltage_control/runtime/src/main.c
git commit -m "feat: add settings persistence tests with fake storage backend"
```

---

### Task 5: Kconfig, CMake, and Flash Partition

**Files:**
- Modify: `lib/voltage_control/Kconfig`
- Modify: `lib/voltage_control/CMakeLists.txt`
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts`
- Modify: `applications/hvb_controller/prj.conf`

- [ ] **Step 1: Add Kconfig entry**

In `lib/voltage_control/Kconfig`, after the `VC_MEASUREMENT_STALE_TIMEOUT_MS` config block (before `VC_MODBUS_UNIT_ID`), add:

```kconfig
config VC_SETTINGS_PERSISTENCE
	bool "VC settings persistence via NVS"
	depends on VC_RUNTIME
	select SETTINGS
	select NVS
	select FLASH
	select FLASH_MAP
	help
	  Enable save/load/factory-reset of system and channel configs
	  using Zephyr Settings backed by NVS on internal flash.
```

- [ ] **Step 2: Add CMake source**

In `lib/voltage_control/CMakeLists.txt`, after the `provider_bus.c` line, add:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_SETTINGS_PERSISTENCE vc_storage_settings.c)
```

- [ ] **Step 3: Add flash partition to jw_hvb.dts**

At the end of `boards/jianwei/jw_hvb/jw_hvb.dts`, before the closing, add:

```dts
&flash0 {
	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		storage_partition: partition@1e0000 {
			label = "storage";
			reg = <0x1e0000 0x20000>;
		};
	};
};
```

- [ ] **Step 4: Enable persistence in hvb_controller prj.conf**

In `applications/hvb_controller/prj.conf`, append:

```
CONFIG_VC_SETTINGS_PERSISTENCE=y
```

- [ ] **Step 5: Create stub vc_storage_settings.c**

Create `lib/voltage_control/vc_storage_settings.c` with a minimal stub so the build succeeds:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/settings/settings.h>
#include <zephyr/kernel.h>

#include "voltage_control/vc_storage.h"

static int settings_save_sys(const struct vc_system_config *cfg)
{
	return settings_save_one("vc/sys", cfg, sizeof(*cfg));
}

static int settings_load_sys(struct vc_system_config *cfg)
{
	/* TODO: implement in Task 6 */
	return -ENOTSUP;
}

static int settings_save_ch_cfg(uint8_t ch, const struct vc_channel_config *cfg)
{
	return -ENOTSUP;
}

static int settings_load_ch_cfg(uint8_t ch, struct vc_channel_config *cfg)
{
	return -ENOTSUP;
}

static int settings_save_ch_cal(uint8_t ch, const struct vc_channel_config *cfg)
{
	return -ENOTSUP;
}

static int settings_load_ch_cal(uint8_t ch, struct vc_channel_config *cfg)
{
	return -ENOTSUP;
}

static int settings_erase(void)
{
	return -ENOTSUP;
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

- [ ] **Step 6: Build jw_hvb to verify Kconfig and DTS**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -10`

Expected: clean build. Flash partition visible, settings subsystem linked.

- [ ] **Step 7: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: all pass. Tests don't enable CONFIG_VC_SETTINGS_PERSISTENCE.

- [ ] **Step 8: Commit**

```bash
git add lib/voltage_control/Kconfig lib/voltage_control/CMakeLists.txt \
       lib/voltage_control/vc_storage_settings.c \
       boards/jianwei/jw_hvb/jw_hvb.dts \
       applications/hvb_controller/prj.conf
git commit -m "feat: add settings persistence Kconfig, flash partition, and stub backend"
```

---

### Task 6: Implement Zephyr Settings Backend

**Files:**
- Modify: `lib/voltage_control/vc_storage_settings.c`

- [ ] **Step 1: Implement the full settings backend**

Replace the contents of `lib/voltage_control/vc_storage_settings.c` with:

```c
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "voltage_control/vc_storage.h"

struct vc_channel_config_no_cal {
	int16_t configured_target_voltage;
	uint16_t ramp_up_step;
	uint16_t ramp_up_interval;
	uint16_t ramp_down_step;
	uint16_t ramp_down_interval;
	enum vc_protection_mode voltage_protection_mode;
	enum vc_output_action voltage_protection_output_action;
	int16_t voltage_limit_threshold;
	enum vc_protection_mode current_protection_mode;
	enum vc_output_action current_protection_output_action;
	int16_t current_limit_threshold;
	uint16_t auto_derate_step;
	uint16_t save_target_policy;
};

struct vc_channel_cal {
	uint16_t output_calib_k;
	int16_t output_calib_b;
	uint16_t measured_voltage_calib_k;
	int16_t measured_voltage_calib_b;
	uint16_t measured_current_calib_k;
	int16_t measured_current_calib_b;
};

static void pack_no_cal(struct vc_channel_config_no_cal *dst,
			const struct vc_channel_config *src)
{
	dst->configured_target_voltage = src->configured_target_voltage;
	dst->ramp_up_step = src->ramp_up_step;
	dst->ramp_up_interval = src->ramp_up_interval;
	dst->ramp_down_step = src->ramp_down_step;
	dst->ramp_down_interval = src->ramp_down_interval;
	dst->voltage_protection_mode = src->voltage_protection_mode;
	dst->voltage_protection_output_action = src->voltage_protection_output_action;
	dst->voltage_limit_threshold = src->voltage_limit_threshold;
	dst->current_protection_mode = src->current_protection_mode;
	dst->current_protection_output_action = src->current_protection_output_action;
	dst->current_limit_threshold = src->current_limit_threshold;
	dst->auto_derate_step = src->auto_derate_step;
	dst->save_target_policy = src->save_target_policy;
}

static void unpack_no_cal(struct vc_channel_config *dst,
			  const struct vc_channel_config_no_cal *src)
{
	dst->configured_target_voltage = src->configured_target_voltage;
	dst->ramp_up_step = src->ramp_up_step;
	dst->ramp_up_interval = src->ramp_up_interval;
	dst->ramp_down_step = src->ramp_down_step;
	dst->ramp_down_interval = src->ramp_down_interval;
	dst->voltage_protection_mode = src->voltage_protection_mode;
	dst->voltage_protection_output_action = src->voltage_protection_output_action;
	dst->voltage_limit_threshold = src->voltage_limit_threshold;
	dst->current_protection_mode = src->current_protection_mode;
	dst->current_protection_output_action = src->current_protection_output_action;
	dst->current_limit_threshold = src->current_limit_threshold;
	dst->auto_derate_step = src->auto_derate_step;
	dst->save_target_policy = src->save_target_policy;
}

static void pack_cal(struct vc_channel_cal *dst,
		     const struct vc_channel_config *src)
{
	dst->output_calib_k = src->output_calib_k;
	dst->output_calib_b = src->output_calib_b;
	dst->measured_voltage_calib_k = src->measured_voltage_calib_k;
	dst->measured_voltage_calib_b = src->measured_voltage_calib_b;
	dst->measured_current_calib_k = src->measured_current_calib_k;
	dst->measured_current_calib_b = src->measured_current_calib_b;
}

static void unpack_cal(struct vc_channel_config *dst,
		       const struct vc_channel_cal *src)
{
	dst->output_calib_k = src->output_calib_k;
	dst->output_calib_b = src->output_calib_b;
	dst->measured_voltage_calib_k = src->measured_voltage_calib_k;
	dst->measured_voltage_calib_b = src->measured_voltage_calib_b;
	dst->measured_current_calib_k = src->measured_current_calib_k;
	dst->measured_current_calib_b = src->measured_current_calib_b;
}

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
	struct vc_channel_config_no_cal packed;

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	pack_no_cal(&packed, cfg);
	return settings_save_one(key, &packed, sizeof(packed));
}

static int settings_load_ch_cfg(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_config_no_cal packed;
	int rc;

	snprintk(key, sizeof(key), "vc/ch%u/cfg", ch);
	rc = settings_load_key(key, &packed, sizeof(packed));
	if (rc < 0) {
		return rc;
	}
	unpack_no_cal(cfg, &packed);
	return 0;
}

static int settings_save_ch_cal(uint8_t ch, const struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_cal packed;

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	pack_cal(&packed, cfg);
	return settings_save_one(key, &packed, sizeof(packed));
}

static int settings_load_ch_cal(uint8_t ch, struct vc_channel_config *cfg)
{
	char key[16];
	struct vc_channel_cal packed;
	int rc;

	snprintk(key, sizeof(key), "vc/ch%u/cal", ch);
	rc = settings_load_key(key, &packed, sizeof(packed));
	if (rc < 0) {
		return rc;
	}
	unpack_cal(cfg, &packed);
	return 0;
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

- [ ] **Step 2: Build jw_hvb**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -10`

Expected: clean build.

- [ ] **Step 3: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: all pass. Tests don't link vc_storage_settings.c.

- [ ] **Step 4: Commit**

```bash
git add lib/voltage_control/vc_storage_settings.c
git commit -m "feat: implement Zephyr Settings/NVS storage backend"
```

---

### Task 7: Auto-load at Runtime Startup

**Files:**
- Modify: `lib/voltage_control/domain_runtime.c`
- Modify: `include/voltage_control/vc_storage.h`

- [ ] **Step 1: Declare vc_settings_storage extern in header**

In `include/voltage_control/vc_storage.h`, before the `#endif`, add:

```c
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
extern const struct vc_storage_backend vc_settings_storage;
#endif
```

- [ ] **Step 2: Add auto-load helper in domain_runtime.c**

In `lib/voltage_control/domain_runtime.c`, add after the includes:

```c
#include "voltage_control/vc_storage.h"
```

Then add a static helper before `vc_runtime_init()`:

```c
static void vc_runtime_auto_load(struct vc_runtime *runtime)
{
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
	struct domain *d = runtime->domain;
	uint16_t count = domain_get_supported_channel_count(d);

	domain_set_storage_backend(d, &vc_settings_storage);
	settings_subsys_init();

	struct vc_system_config sys_cfg;

	if (vc_settings_storage.load_system_config(&sys_cfg) == 0) {
		(void)domain_set_system_config(d, &sys_cfg);
	}

	for (uint8_t ch = 0; ch < count; ch++) {
		struct vc_channel_config ch_cfg;

		domain_get_channel_config(d, ch, &ch_cfg);
		if (vc_settings_storage.load_channel_config(ch, &ch_cfg) == 0) {
			struct vc_channel_config merged = ch_cfg;

			domain_get_channel_config(d, ch, &ch_cfg);
			merged.output_calib_k = ch_cfg.output_calib_k;
			merged.output_calib_b = ch_cfg.output_calib_b;
			merged.measured_voltage_calib_k = ch_cfg.measured_voltage_calib_k;
			merged.measured_voltage_calib_b = ch_cfg.measured_voltage_calib_b;
			merged.measured_current_calib_k = ch_cfg.measured_current_calib_k;
			merged.measured_current_calib_b = ch_cfg.measured_current_calib_b;
			(void)domain_set_channel_config(d, ch, &merged);
		}

		domain_get_channel_config(d, ch, &ch_cfg);
		if (vc_settings_storage.load_channel_cal(ch, &ch_cfg) == 0) {
			(void)domain_set_channel_config(d, ch, &ch_cfg);
		}
	}
#endif
}
```

Add `#include <zephyr/settings/settings.h>` at the top (guarded):

```c
#ifdef CONFIG_VC_SETTINGS_PERSISTENCE
#include <zephyr/settings/settings.h>
#endif
```

- [ ] **Step 3: Call auto-load from vc_runtime_init**

In `vc_runtime_init()`, after `vc_provider_bus_init();` and before `vc_runtime_publish_all_configs(runtime);`, add:

```c
	vc_runtime_auto_load(runtime);
```

- [ ] **Step 4: Build jw_hvb**

Run: `west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -10`

Expected: clean build with settings auto-load wired in.

- [ ] **Step 5: Run all tests**

Run: `west twister -T tests/ --no-shuffle -v 2>&1 | tail -15`

Expected: all pass. Auto-load is guarded by `#ifdef CONFIG_VC_SETTINGS_PERSISTENCE`, which is not enabled in test prj.conf.

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/domain_runtime.c include/voltage_control/vc_storage.h
git commit -m "feat: auto-load saved settings at runtime startup"
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
| Settings persistence | `verified` | vc_storage_backend interface, Zephyr Settings/NVS backend, param_action implementation, auto-load at startup, flash partition. Calibration preserved on factory-reset. Verified N/N tests, `jw_hvb` build clean. |
```

2. Update the **Settings persistence** row in **Remaining Gaps** from `deferred` to `verified`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/project-status.md
git commit -m "docs: record settings persistence verification status"
```

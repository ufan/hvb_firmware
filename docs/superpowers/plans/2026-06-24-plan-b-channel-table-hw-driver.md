# Plan B: Channel Table + DTS Bindings + HVB Driver Refactor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the hardware abstraction layer — static channel table, new DTS child-node bindings, new 4-function hardware API, and refactored HVB driver with interrupt-driven ADC sampling via k_work_poll.

**Architecture:** The channel table is a static dispatch layer mapping channel index → device + hw API, built from DTS at compile time using child-node composition. The HVB driver is rewritten to implement the new `vc_channel_hw_api` (4 functions) and uses k_work_poll with DRDY GPIO interrupts for non-blocking ADC reads. The ADS1232 driver gains a split-phase API: select channel, arm DRDY interrupt callback, and read-data-now (bit-bang only, no DRDY wait). Each measurement is written directly to a per-channel buffer and immediately consumed by the voltage controller.

**Tech Stack:** C99, Zephyr RTOS (k_work_poll, k_poll_signal, GPIO interrupts, SMF), DTS bindings, ztest on native_posix

**Spec:** `docs/superpowers/specs/2026-06-24-channel-table-direct-drive-design.md`

**Depends on:** Plan A (vc_channel_state.c, vc_controller.c) — completed and tested.

**Constraint:** The existing domain tests (84 tests), vc_channel_state tests (40 tests), and vc_controller tests (17 tests) must continue to pass throughout. They don't depend on the hardware layer.

---

## File Map

### New files
| File | Responsibility |
|---|---|
| `include/voltage_control/vc_channel_hw.h` | New 4-function hardware API vtable |
| `include/voltage_control/vc_channel_table.h` | Channel table API + `vc_measurement_buffer_entry` |
| `lib/voltage_control/vc_channel_table.c` | Static dispatch implementation |

### Modified files
| File | Change |
|---|---|
| `dts/bindings/voltage_control/jianwei,vc-controller.yaml` | Child-node composition with `#address-cells`/`#size-cells` |
| `dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml` | Child binding of controller (removes base include) |
| `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml` | Child binding for tests (adds `reg`) |
| `boards/jianwei/jw_hvb/jw_hvb.dts` | Channels as children of controller node |
| `include/voltage_control/domain.h` | `VC_MAX_CHANNELS` macro update |
| `lib/voltage_control/controller.c` | Use `DT_FOREACH_CHILD_STATUS_OKAY` |
| `drivers/sensor/ads1232/ads1232.c` | Add split-phase API: select, arm DRDY, read-now |
| `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c` | Full rewrite with `vc_channel_hw_api` + k_work_poll |
| `lib/voltage_control/CMakeLists.txt` | Add `vc_channel_table.c` |
| All test `boards/native_posix.overlay` files (5 total) | Child-node DTS format |

### Deleted files
| File | Reason |
|---|---|
| `dts/bindings/voltage_control/jianwei,vc-channel-base.yaml` | Properties absorbed into controller child-binding |
| `include/voltage_control/vc_channel.h` | Replaced by `vc_channel_hw.h` |

---

### Task 1: Create vc_channel_hw.h (new hardware API)

**Files:**
- Create: `include/voltage_control/vc_channel_hw.h`

This is the new 4-function hardware API vtable that replaces the legacy 9-function `vc_channel_api`.

- [ ] **Step 1: Create the header file**

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

struct vc_channel_hw_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*start_sampling)(const struct device *dev);
	int (*stop_sampling)(const struct device *dev);
};

#endif
```

- [ ] **Step 2: Commit**

```bash
git add include/voltage_control/vc_channel_hw.h
git commit -m "feat: add vc_channel_hw.h — new 4-function hardware API vtable"
```

---

### Task 2: Create vc_channel_table.h/c (static dispatch + measurement buffer)

**Files:**
- Create: `include/voltage_control/vc_channel_table.h`
- Create: `lib/voltage_control/vc_channel_table.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

- [ ] **Step 1: Create vc_channel_table.h**

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H
#define VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/device.h>

struct vc_measurement_buffer_entry {
	int32_t raw_voltage;
	uint32_t voltage_timestamp_ms;
	int32_t raw_current;
	uint32_t current_timestamp_ms;
};

struct vc_channel_table_entry {
	const struct device *dev;
	uint8_t index;
	uint16_t capabilities;
	struct vc_measurement_buffer_entry *meas;
};

struct vc_controller;

void vc_channel_table_init(struct vc_controller *ctrl);

int vc_channel_table_set_output(uint8_t ch, uint16_t code);
int vc_channel_table_set_enable(uint8_t ch, bool enable);
int vc_channel_table_start_sampling(uint8_t ch);
int vc_channel_table_stop_sampling(uint8_t ch);

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch);
size_t vc_channel_table_count(void);
uint16_t vc_channel_table_capabilities(uint8_t ch);

#endif
```

- [ ] **Step 2: Create vc_channel_table.c**

This file provides static dispatch from channel index to device + hw API. It uses the existing `controller.c` DTS table (which will be updated to the new child-node format in Task 3). For now, it compiles alongside the existing code.

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel_table.h"
#include "voltage_control/vc_channel_hw.h"
#include "voltage_control/vc_controller.h"
#include <zephyr/devicetree.h>

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

static struct vc_controller *g_ctrl;

static struct vc_measurement_buffer_entry
	meas_buffers[DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE)];

struct vc_channel_table_entry vc_channel_table[] = {
#define CH_TABLE_ENTRY(node_id) \
	{ \
		.dev = DEVICE_DT_GET(node_id), \
		.index = DT_REG_ADDR(node_id), \
		.capabilities = DT_PROP(node_id, capabilities), \
		.meas = &meas_buffers[DT_REG_ADDR(node_id)], \
	},
	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_TABLE_ENTRY)
};

static const size_t vc_channel_table_size = ARRAY_SIZE(vc_channel_table);

void vc_channel_table_init(struct vc_controller *ctrl)
{
	g_ctrl = ctrl;
}

static const struct vc_channel_hw_api *get_hw_api(uint8_t ch)
{
	if (ch >= vc_channel_table_size) {
		return NULL;
	}
	return vc_channel_table[ch].dev->api;
}

int vc_channel_table_set_output(uint8_t ch, uint16_t code)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->set_output == NULL) {
		return -ENOTSUP;
	}
	return api->set_output(vc_channel_table[ch].dev, code);
}

int vc_channel_table_set_enable(uint8_t ch, bool enable)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->set_enable == NULL) {
		return -ENOTSUP;
	}
	return api->set_enable(vc_channel_table[ch].dev, enable);
}

int vc_channel_table_start_sampling(uint8_t ch)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->start_sampling == NULL) {
		return -ENOTSUP;
	}
	return api->start_sampling(vc_channel_table[ch].dev);
}

int vc_channel_table_stop_sampling(uint8_t ch)
{
	const struct vc_channel_hw_api *api = get_hw_api(ch);

	if (api == NULL || api->stop_sampling == NULL) {
		return -ENOTSUP;
	}
	return api->stop_sampling(vc_channel_table[ch].dev);
}

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch)
{
	if (ch >= vc_channel_table_size) {
		return NULL;
	}
	return vc_channel_table[ch].meas;
}

size_t vc_channel_table_count(void)
{
	return vc_channel_table_size;
}

uint16_t vc_channel_table_capabilities(uint8_t ch)
{
	if (ch >= vc_channel_table_size) {
		return 0;
	}
	return vc_channel_table[ch].capabilities;
}
```

Note: This file won't compile yet — it uses `DT_FOREACH_CHILD_STATUS_OKAY` and `DT_REG_ADDR` which require the new DTS bindings from Task 3. It's added to CMakeLists conditionally.

- [ ] **Step 3: Add vc_channel_table.c to CMakeLists**

Add to `lib/voltage_control/CMakeLists.txt` after the existing `zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER controller.c)` line:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER vc_channel_table.c)
```

- [ ] **Step 4: Commit**

```bash
git add include/voltage_control/vc_channel_table.h lib/voltage_control/vc_channel_table.c lib/voltage_control/CMakeLists.txt
git commit -m "feat: add vc_channel_table static dispatch + measurement buffer type"
```

---

### Task 3: Update DTS bindings to child-node composition

This is an atomic change — all bindings, DTS files, overlays, and the `VC_MAX_CHANNELS` macro must be updated together for the build to work.

**Files:**
- Modify: `dts/bindings/voltage_control/jianwei,vc-controller.yaml`
- Modify: `dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml`
- Modify: `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml`
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts`
- Modify: `include/voltage_control/domain.h` (VC_MAX_CHANNELS)
- Modify: `lib/voltage_control/controller.c`
- Modify: All 7 test `boards/native_posix.overlay` files
- Delete: `dts/bindings/voltage_control/jianwei,vc-channel-base.yaml`

- [ ] **Step 1: Rewrite vc-controller.yaml with child-binding**

Replace the entire file:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: Voltage controller — owns virtual channels as children

compatible: "jianwei,vc-controller"

include: base.yaml

properties:
  "#address-cells":
    type: int
    const: 1
  "#size-cells":
    type: int
    const: 0

child-binding:
  description: Virtual voltage channel base properties
  properties:
    reg:
      type: array
      required: true
      description: Channel index (0-based).
    label:
      type: string
      required: true
      description: Human-readable channel name.
    capabilities:
      type: int
      required: true
      description: Bitmask of CH_CAP_* flags.
    enable-gpios:
      type: phandle-array
      required: false
      description: GPIO that gates channel output.
```

- [ ] **Step 2: Rewrite hvb-vc-channel.yaml as child binding**

Replace the entire file:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: HVB Virtual Voltage Channel (child of vc-controller)

compatible: "jianwei,hvb-vc-channel"

properties:
  dac:
    type: phandle
    required: true
    description: Phandle to the DAC device for raw output drive.
  adc:
    type: phandle
    required: true
    description: Phandle to the ADC device for voltage/current measurement.
  max-raw-dac:
    type: int
    required: true
    default: 0xFFFF
    description: Maximum raw DAC code for this channel.
  sample-rate-ms:
    type: int
    required: false
    default: 100
    description: Target measurement sampling period in milliseconds.
```

- [ ] **Step 3: Rewrite vc-channel-stub.yaml as child binding**

Replace the entire file:

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: Stub VC channel for native_posix tests (child of vc-controller)

compatible: "jianwei,vc-channel-stub"
```

The stub now inherits `reg`, `label`, `capabilities` from the controller's child-binding. No extra properties needed.

- [ ] **Step 4: Delete vc-channel-base.yaml**

```bash
git rm dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
```

- [ ] **Step 5: Update jw_hvb.dts — channels as children of controller**

Replace the vc_controller section at the bottom of the file (lines 261-293). Remove the old top-level `vc_ch0`, `vc_ch1`, and `vc_controller` nodes and replace with:

```dts
/ {
	vc_controller: vc-controller {
		compatible = "jianwei,vc-controller";
		#address-cells = <1>;
		#size-cells = <0>;
		status = "okay";

		vc_ch0: channel@0 {
			compatible = "jianwei,hvb-vc-channel";
			reg = <0>;
			label = "HV1";
			capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
					 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
			dac = <&dac_hv1>;
			adc = <&ads1232_hv1>;
			enable-gpios = <&gpiod 9 GPIO_ACTIVE_LOW>;
			max-raw-dac = <0xFFFF>;
			sample-rate-ms = <100>;
		};

		vc_ch1: channel@1 {
			compatible = "jianwei,hvb-vc-channel";
			reg = <1>;
			label = "HV2";
			capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
					 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
			dac = <&dac_hv2>;
			adc = <&ads1232_hv2>;
			enable-gpios = <&gpioc 4 GPIO_ACTIVE_LOW>;
			max-raw-dac = <0xFFFF>;
			sample-rate-ms = <100>;
		};
	};
};
```

- [ ] **Step 6: Update VC_MAX_CHANNELS macro in domain.h**

In `include/voltage_control/domain.h`, change line 14 from:

```c
#define VC_MAX_CHANNELS DT_PROP_LEN(DT_NODELABEL(vc_controller), channels)
```

to:

```c
#define VC_MAX_CHANNELS DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))
```

- [ ] **Step 7: Update controller.c to use DT_FOREACH_CHILD_STATUS_OKAY**

Replace the entire content of `lib/voltage_control/controller.c`:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include "voltage_control/domain.h"

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define CH_ENTRY(node_id) \
	{ \
		.dev = DEVICE_DT_GET(node_id), \
		.index = DT_REG_ADDR(node_id), \
		.capabilities = DT_PROP(node_id, capabilities), \
	},

const struct vc_channel_entry vc_domain_channels[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_ENTRY)
};

const size_t vc_domain_channel_count = ARRAY_SIZE(vc_domain_channels);
```

- [ ] **Step 8: Update all 7 test overlays to child-node format**

The new overlay format for all test suites. Create a shared content and apply to each:

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

Apply this to all 7 overlay files:
- `tests/voltage_control/domain/boards/native_posix.overlay`
- `tests/voltage_control/modbus_adapter/boards/native_posix.overlay`
- `tests/voltage_control/vc/boards/native_posix.overlay`
- `tests/voltage_control/provider_bus/boards/native_posix.overlay`
- `tests/voltage_control/runtime/boards/native_posix.overlay`
- `tests/voltage_control/vc_channel_state/boards/native_posix.overlay`
- `tests/voltage_control/vc_controller/boards/native_posix.overlay`

- [ ] **Step 9: Update vc_channel_stub.c to remove channel-index usage**

The stub driver currently uses `DT_INST_PROP(n, channel_index)` — this property no longer exists. The stub is a minimal device that doesn't need any properties beyond what Zephyr provides. The existing stub at `tests/voltage_control/vc/src/vc_channel_stub.c` should still compile since it doesn't reference `channel-index` (it's just a `DEVICE_DT_INST_DEFINE` with NULL api). Verify this.

- [ ] **Step 10: Build and run all existing test suites**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

Expected: All 141 tests pass (84 + 40 + 17).

- [ ] **Step 11: Commit the atomic DTS change**

```bash
git rm dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
git add dts/bindings/voltage_control/ boards/jianwei/jw_hvb/jw_hvb.dts \
  include/voltage_control/domain.h lib/voltage_control/controller.c \
  tests/voltage_control/*/boards/native_posix.overlay \
  tests/voltage_control/vc/src/vc_channel_stub.c
git commit -m "refactor: DTS child-node composition for vc-controller channels

Channels are now children of vc-controller with reg for index and
label for human-readable name. Delete vc-channel-base.yaml. Update
all test overlays and VC_MAX_CHANNELS macro."
```

---

### Task 4: Add split-phase API to ADS1232 driver

The ADS1232 driver currently only supports blocking reads through the Zephyr ADC API. The HVB driver needs non-blocking access to: (1) select ADC channel, (2) get DRDY GPIO spec for interrupt setup, (3) bit-bang read without DRDY wait.

**Files:**
- Modify: `drivers/sensor/ads1232/ads1232.c`

- [ ] **Step 1: Add public helpers to ads1232.c**

Add these functions after `ads1232_read` and before `ads1232_init`:

```c
int ads1232_select_channel(const struct device *dev, int ch)
{
	const struct ads1232_config *cfg = dev->config;

	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}
	return 0;
}

const struct gpio_dt_spec *ads1232_get_drdy(const struct device *dev)
{
	const struct ads1232_config *cfg = dev->config;

	return &cfg->drdy;
}

int ads1232_read_data_now(const struct device *dev, int32_t *out)
{
	const struct ads1232_config *cfg = dev->config;
	int32_t val = 0;

	for (int i = 0; i < 24; i++) {
		gpio_pin_set_dt(&cfg->sclk, 1);
		k_busy_wait(1);
		val = (val << 1) |
		      gpio_pin_get_raw(cfg->drdy.port, cfg->drdy.pin);
		gpio_pin_set_dt(&cfg->sclk, 0);
		k_busy_wait(1);
	}

	gpio_pin_set_dt(&cfg->sclk, 1);
	k_busy_wait(1);
	gpio_pin_set_dt(&cfg->sclk, 0);
	k_busy_wait(1);

	if (val & BIT(23)) {
		val |= (int32_t)0xFF000000;
	}

	*out = val;
	return 0;
}
```

These functions are called by the HVB driver from its k_work_poll handler. They don't go through the ADC API — they're direct GPIO operations on the ADS1232's pins.

- [ ] **Step 2: Verify existing domain tests still pass**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

Expected: All 84 domain tests pass (the ADS1232 changes don't affect native_posix tests).

- [ ] **Step 3: Commit**

```bash
git add drivers/sensor/ads1232/ads1232.c
git commit -m "feat: add split-phase API to ADS1232 — select_channel, get_drdy, read_data_now"
```

---

### Task 5: Rewrite HVB VC channel driver with vc_channel_hw_api + k_work_poll

This is the most complex task — a full rewrite of `hvb_vc_channel.c`. The driver changes from:
- Old: shared workq + blocking `adc_read()` + `vc_channel_api` (9 functions) + provider bus interaction
- New: shared workq + k_work_poll on DRDY + `vc_channel_hw_api` (4 functions) + direct measurement buffer write + `vc_controller_consume_*` calls

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c` (full rewrite)

- [ ] **Step 1: Rewrite hvb_vc_channel.c**

Replace the entire file content:

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/voltage_control/capabilities.h>
#include "voltage_control/vc_channel_hw.h"
#include "voltage_control/vc_channel_table.h"
#include "voltage_control/vc_controller.h"

LOG_MODULE_REGISTER(hvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_hvb_vc_channel

#define HVB_VC_WORKQ_STACK_SIZE 2048
#define HVB_VC_WORKQ_PRIORITY   CONFIG_SYSTEM_WORKQUEUE_PRIORITY
#define HVB_VC_DRDY_TIMEOUT_MS  420

static K_THREAD_STACK_DEFINE(hvb_vc_workq_stack, HVB_VC_WORKQ_STACK_SIZE);
static struct k_work_q hvb_vc_workq;
static bool hvb_vc_workq_started;

static void hvb_vc_ensure_workq(void)
{
	if (!hvb_vc_workq_started) {
		k_work_queue_init(&hvb_vc_workq);
		k_work_queue_start(&hvb_vc_workq, hvb_vc_workq_stack,
				   K_THREAD_STACK_SIZEOF(hvb_vc_workq_stack),
				   HVB_VC_WORKQ_PRIORITY, NULL);
		k_thread_name_set(&hvb_vc_workq.thread, "hvb_vc_workq");
		hvb_vc_workq_started = true;
	}
}

/* Forward declarations for ADS1232 split-phase API */
extern int ads1232_select_channel(const struct device *dev, int ch);
extern const struct gpio_dt_spec *ads1232_get_drdy(const struct device *dev);
extern int ads1232_read_data_now(const struct device *dev, int32_t *out);

struct hvb_vc_config {
	const struct device *dac;
	const struct device *adc;
	struct gpio_dt_spec enable;
	uint16_t max_raw_dac;
	uint32_t sample_rate_ms;
	uint16_t capabilities;
	uint8_t channel_index;
};

enum hvb_adc_phase {
	ADC_PHASE_VOLTAGE,
	ADC_PHASE_CURRENT,
};

struct hvb_vc_data {
	const struct device *dev;
	uint8_t channel;
	struct vc_measurement_buffer_entry *meas;

	struct k_work_poll poll_work;
	struct k_poll_signal drdy_signal;
	struct k_poll_event drdy_event;
	struct gpio_callback drdy_cb;

	enum hvb_adc_phase adc_phase;
	struct k_work_delayable next_cycle_work;
	bool sampling_active;
};

/* ---- Hardware API ---- */

static int hvb_vc_set_output(const struct device *dev, uint16_t code)
{
	const struct hvb_vc_config *cfg = dev->config;

	if (code > cfg->max_raw_dac) {
		return -EINVAL;
	}
	return dac_write_value(cfg->dac, 0, code);
}

static int hvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct hvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

/* ---- DRDY interrupt handler ---- */

static void hvb_vc_drdy_isr(const struct device *port, struct gpio_callback *cb,
			     gpio_port_pins_t pins)
{
	struct hvb_vc_data *data = CONTAINER_OF(cb, struct hvb_vc_data, drdy_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_poll_signal_raise(&data->drdy_signal, 0);
}

static void hvb_vc_arm_drdy(struct hvb_vc_data *data)
{
	const struct hvb_vc_config *cfg = data->dev->config;
	const struct gpio_dt_spec *drdy = ads1232_get_drdy(cfg->adc);

	k_poll_signal_reset(&data->drdy_signal);
	gpio_pin_interrupt_configure_dt(drdy, GPIO_INT_EDGE_TO_ACTIVE);
}

static void hvb_vc_disarm_drdy(struct hvb_vc_data *data)
{
	const struct hvb_vc_config *cfg = data->dev->config;
	const struct gpio_dt_spec *drdy = ads1232_get_drdy(cfg->adc);

	gpio_pin_interrupt_configure_dt(drdy, GPIO_INT_DISABLE);
}

/* ---- k_work_poll handler: bit-bang + consume ---- */

static void hvb_vc_poll_handler(struct k_work *work)
{
	struct k_work_poll *poll_work = CONTAINER_OF(work, struct k_work_poll, work);
	struct hvb_vc_data *data = CONTAINER_OF(poll_work, struct hvb_vc_data, poll_work);
	const struct hvb_vc_config *cfg = data->dev->config;
	int32_t raw_value;
	int ret;

	hvb_vc_disarm_drdy(data);

	if (data->poll_work.poll_result != 0) {
		LOG_WRN("ch%d DRDY timeout (phase=%d)", data->channel, data->adc_phase);
		vc_controller_consume_fault(NULL, data->channel, VC_FAULT_MEASUREMENT);
		k_work_schedule_for_queue(&hvb_vc_workq, &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
		return;
	}

	ret = ads1232_read_data_now(cfg->adc, &raw_value);
	if (ret < 0) {
		LOG_ERR("ch%d read failed: %d", data->channel, ret);
		vc_controller_consume_fault(NULL, data->channel, VC_FAULT_MEASUREMENT);
		k_work_schedule_for_queue(&hvb_vc_workq, &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
		return;
	}

	if (data->adc_phase == ADC_PHASE_VOLTAGE) {
		data->meas->raw_voltage = raw_value;
		data->meas->voltage_timestamp_ms = k_uptime_get_32();
		vc_controller_consume_voltage(NULL, data->channel, raw_value);

		if (cfg->capabilities & CH_CAP_CURRENT_MEASUREMENT) {
			data->adc_phase = ADC_PHASE_CURRENT;
			ads1232_select_channel(cfg->adc, 1);
			hvb_vc_arm_drdy(data);
			k_work_poll_submit_to_queue(&hvb_vc_workq,
						    &data->poll_work,
						    &data->drdy_event, 1,
						    K_MSEC(HVB_VC_DRDY_TIMEOUT_MS));
		} else {
			data->adc_phase = ADC_PHASE_VOLTAGE;
			k_work_schedule_for_queue(&hvb_vc_workq,
						  &data->next_cycle_work,
						  K_MSEC(cfg->sample_rate_ms));
		}
	} else {
		data->meas->raw_current = raw_value;
		data->meas->current_timestamp_ms = k_uptime_get_32();
		vc_controller_consume_current(NULL, data->channel, raw_value);

		data->adc_phase = ADC_PHASE_VOLTAGE;
		k_work_schedule_for_queue(&hvb_vc_workq,
					  &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
	}
}

/* ---- Next cycle: start a new V+I measurement cycle ---- */

static void hvb_vc_next_cycle_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hvb_vc_data *data = CONTAINER_OF(dwork, struct hvb_vc_data,
						next_cycle_work);
	const struct hvb_vc_config *cfg = data->dev->config;

	if (!data->sampling_active) {
		return;
	}

	data->adc_phase = ADC_PHASE_VOLTAGE;
	ads1232_select_channel(cfg->adc, 0);
	hvb_vc_arm_drdy(data);
	k_work_poll_submit_to_queue(&hvb_vc_workq,
				    &data->poll_work,
				    &data->drdy_event, 1,
				    K_MSEC(HVB_VC_DRDY_TIMEOUT_MS));
}

/* ---- start/stop sampling ---- */

static int hvb_vc_start_sampling(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;
	const struct hvb_vc_config *cfg = dev->config;

	if (!(cfg->capabilities & (CH_CAP_VOLTAGE_MEASUREMENT |
				   CH_CAP_CURRENT_MEASUREMENT))) {
		return 0;
	}

	data->sampling_active = true;
	data->adc_phase = ADC_PHASE_VOLTAGE;
	ads1232_select_channel(cfg->adc, 0);
	hvb_vc_arm_drdy(data);
	return k_work_poll_submit_to_queue(&hvb_vc_workq,
					   &data->poll_work,
					   &data->drdy_event, 1,
					   K_MSEC(HVB_VC_DRDY_TIMEOUT_MS)) < 0
		? -EIO : 0;
}

static int hvb_vc_stop_sampling(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;

	data->sampling_active = false;
	hvb_vc_disarm_drdy(data);
	k_work_poll_cancel(&data->poll_work);
	k_work_cancel_delayable(&data->next_cycle_work);
	return 0;
}

/* ---- API vtable ---- */

static const struct vc_channel_hw_api hvb_vc_hw_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.start_sampling = hvb_vc_start_sampling,
	.stop_sampling = hvb_vc_stop_sampling,
};

/* ---- Device init ---- */

static int hvb_vc_init(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct hvb_vc_data *data = dev->data;
	const struct gpio_dt_spec *drdy;

	hvb_vc_ensure_workq();

	if (!device_is_ready(cfg->dac)) {
		LOG_ERR("DAC not ready");
		return -ENODEV;
	}
	{
		struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };
		int ret = dac_channel_setup(cfg->dac, &dac_cfg);

		if (ret < 0) {
			LOG_ERR("DAC channel setup failed: %d", ret);
			return ret;
		}
	}
	if (!device_is_ready(cfg->adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->enable)) {
		LOG_ERR("Enable GPIO not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);

	data->dev = dev;
	data->channel = cfg->channel_index;
	data->meas = vc_channel_table[cfg->channel_index].meas;

	k_poll_signal_init(&data->drdy_signal);
	k_poll_event_init(&data->drdy_event, K_POLL_TYPE_SIGNAL,
			  K_POLL_MODE_NOTIFY_ONLY, &data->drdy_signal);
	k_work_poll_init(&data->poll_work, hvb_vc_poll_handler);
	k_work_init_delayable(&data->next_cycle_work, hvb_vc_next_cycle_handler);

	drdy = ads1232_get_drdy(cfg->adc);
	gpio_init_callback(&data->drdy_cb, hvb_vc_drdy_isr, BIT(drdy->pin));
	gpio_add_callback(drdy->port, &data->drdy_cb);

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x (k_work_poll)",
		cfg->channel_index,
		cfg->dac->name, cfg->adc->name, cfg->capabilities);
	return 0;
}

/* ---- DTS instantiation ---- */

#define HVB_VC_INIT(n) \
	static const struct hvb_vc_config hvb_vc_config_##n = { \
		.dac = DEVICE_DT_GET(DT_INST_PHANDLE(n, dac)), \
		.adc = DEVICE_DT_GET(DT_INST_PHANDLE(n, adc)), \
		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
		.max_raw_dac = DT_INST_PROP(n, max_raw_dac), \
		.sample_rate_ms = DT_INST_PROP(n, sample_rate_ms), \
		.capabilities = DT_INST_PROP(n, capabilities), \
		.channel_index = DT_REG_ADDR(DT_INST(n, jianwei_hvb_vc_channel)), \
	}; \
	static struct hvb_vc_data hvb_vc_data_##n; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		&hvb_vc_data_##n, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_hw_api);

DT_INST_FOREACH_STATUS_OKAY(HVB_VC_INIT)
```

Key changes from the old driver:
- `vc_channel_api` (9 functions) → `vc_channel_hw_api` (4 functions)
- Blocking `adc_read()` → k_work_poll on DRDY signal + `ads1232_read_data_now()`
- Provider bus interaction removed — no `vc_provider_bus_*` calls
- ADC phase state machine (VOLTAGE → CURRENT → schedule next cycle)
- Direct measurement buffer write + `vc_controller_consume_*` calls
- STRUCT_SECTION_ITERABLE for provider_binding and measurement_buffer removed (managed by vc_channel_table now)
- `vc_controller_consume_*` calls pass NULL for the controller pointer — Plan C will wire this to the real controller. For now, these calls will be no-ops or need a stub.

Note: The `vc_controller_consume_*` functions in `vc_controller.c` take a `struct vc_controller *` as the first argument. The HVB driver needs access to the controller singleton. The channel table provides this via `vc_channel_table_init(ctrl)`. The driver accesses it through an extern or a getter. For this task, we'll add a getter to vc_channel_table.

- [ ] **Step 2: Add controller getter to vc_channel_table**

Add to `include/voltage_control/vc_channel_table.h`:

```c
struct vc_controller *vc_channel_table_get_controller(void);
```

Add to `lib/voltage_control/vc_channel_table.c`:

```c
struct vc_controller *vc_channel_table_get_controller(void)
{
	return g_ctrl;
}
```

Then update the `hvb_vc_poll_handler` and `hvb_vc_next_cycle_handler` to use:

```c
struct vc_controller *ctrl = vc_channel_table_get_controller();
```

instead of passing NULL to the `vc_controller_consume_*` calls.

- [ ] **Step 3: Delete legacy vc_channel.h**

```bash
git rm include/voltage_control/vc_channel.h
```

Update any files that include `vc_channel.h` to include `vc_channel_hw.h` instead:
- `lib/voltage_control/controller.c` — remove the include (it no longer needs the channel API)
- `lib/voltage_control/provider_bus.c` — still includes it; change to `vc_channel_hw.h` (provider_bus.c uses `vc_channel_api` in `start_all` and `notify_channel`; these functions will be updated in Plan C when provider_bus is deleted. For now, this file will have a build error on hardware targets but still compiles for native_posix tests since provider_bus depends on `CONFIG_VC_PROVIDER_BUS`)

Actually — this is a problem. We can't delete `vc_channel.h` yet because `provider_bus.c` depends on `vc_channel_api` and provider_bus is still used by `domain_runtime.c`. The deletion must happen in Plan C. For now, just ensure the new driver doesn't include it.

- [ ] **Step 4: Verify domain/vc_channel_state/vc_controller tests still pass**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

Expected: All 141 tests pass.

- [ ] **Step 5: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c \
  drivers/sensor/ads1232/ads1232.c \
  include/voltage_control/vc_channel_table.h \
  lib/voltage_control/vc_channel_table.c
git commit -m "refactor: rewrite HVB driver with k_work_poll + vc_channel_hw_api

Replace blocking adc_read() with DRDY interrupt + k_work_poll.
ADC phase state machine (voltage → current → next cycle).
Direct measurement buffer write + vc_controller_consume_* calls.
4-function vc_channel_hw_api replaces 9-function vc_channel_api.
Shared workq occupied ~60µs per read instead of ~400ms."
```

---

### Task 6: Verify full build on hardware target (jw_hvb)

This task verifies that the firmware builds for the actual hardware target, not just native_posix.

- [ ] **Step 1: Build for jw_hvb**

```bash
west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -10
```

This may fail because the application main.c still uses the old provider bus and vc.c wiring. Build errors are expected for the application — but the drivers, lib, and DTS should compile cleanly. If there are DTS or driver-level errors, fix them.

- [ ] **Step 2: Note any build issues for Plan C**

Record any link errors or missing symbol errors that are caused by the old wiring in `domain_runtime.c`, `vc.c`, or `main.c`. These are Plan C's responsibility to fix.

- [ ] **Step 3: Commit any fixes**

```bash
git add -u
git commit -m "fix: resolve hardware build issues from Plan B changes"
```

---

### Task 7: Run all native_posix test suites (final verification)

- [ ] **Step 1: Run all 5 test suites that should still pass**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/modbus_adapter -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

Expected: All test suites pass. The provider_bus and runtime test suites may have issues due to the DTS overlay changes — fix any that arise.

- [ ] **Step 2: Verify commit history**

```bash
git log --oneline -10
```

Verify the commit progression shows: hw API header → channel table → DTS bindings (atomic) → ADS1232 split-phase → HVB driver rewrite → hw build check → final verification.

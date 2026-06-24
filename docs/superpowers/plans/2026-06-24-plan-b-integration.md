# Plan B: Hardware Layer + Integration + Provider Bus Removal

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the Channel Table & Direct-Drive Architecture by building the hardware abstraction layer, rewiring the domain runtime and CQRS facade to use the new voltage controller, rewriting the HVB driver with interrupt-driven ADC sampling, and deleting the provider bus.

**Architecture:** This plan merges the former Plans B and C because they are deeply entangled — the HVB driver rewrite needs the vc_controller pointer (wiring), deleting provider_bus.c requires updating domain_runtime.c (wiring), and the DTS binding changes affect both layers. The work proceeds bottom-up: foundation headers → DTS → ADS1232 → HVB driver → runtime/facade rewrite → delete legacy → application update → verify.

**Tech Stack:** C99, Zephyr RTOS (k_work_poll, k_poll_signal, GPIO interrupts), DTS bindings, ztest on native_posix

**Spec:** `docs/superpowers/specs/2026-06-24-channel-table-direct-drive-design.md`

**Depends on:** Plan A (vc_channel_state.c, vc_controller.c) — completed, 57 passing tests.

---

## Task Overview

| # | Task | What changes |
|---|---|---|
| 1 | New headers (additive) | `vc_channel_hw.h`, `vc_channel_table.h/c`, measurement buffer type |
| 2 | DTS binding migration (atomic) | All bindings, overlays, `VC_MAX_CHANNELS`, `controller.c` |
| 3 | ADS1232 interrupt-driven mode | Add `select_channel()`, `get_drdy()`, `read_data_now()` alongside existing `adc_read()` |
| 4 | HVB driver rewrite | k_work_poll + DRDY ISR + phase state machine + `vc_channel_hw_api` |
| 5 | domain_runtime.c rewrite | Thin facade using `vc_controller`, no provider bus |
| 6 | vc.c rewrite | Route dispatch/query to `vc_controller` directly |
| 7 | vc.h simplification | Remove `vc_runtime_command` types, simplify to `vc_controller` routing |
| 8 | modbus_adapter.c update | Route through new `vc.h` / vc_controller path |
| 9 | Application main.c update | Use new `vc_init()` that creates `vc_controller` + channel table |
| 10 | Delete legacy modules | `provider_bus.c/h`, `vc_channel.h`, `runtime.h` unused types |
| 11 | Full build + test verification | All native_posix tests + `west build -b jw_hvb` |

---

### Task 1: Create vc_channel_hw.h + vc_channel_table.h/c

Additive — no conflicts with existing code. These compile alongside the old modules.

**Files:**
- Create: `include/voltage_control/vc_channel_hw.h`
- Create: `include/voltage_control/vc_channel_table.h`
- Create: `lib/voltage_control/vc_channel_table.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

- [ ] **Step 1: Create vc_channel_hw.h**

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

- [ ] **Step 2: Create vc_channel_table.h**

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H
#define VOLTAGE_CONTROL_VC_CHANNEL_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
struct vc_controller *vc_channel_table_get_controller(void);

int vc_channel_table_set_output(uint8_t ch, uint16_t code);
int vc_channel_table_set_enable(uint8_t ch, bool enable);
int vc_channel_table_start_sampling(uint8_t ch);
int vc_channel_table_stop_sampling(uint8_t ch);

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch);
size_t vc_channel_table_count(void);
uint16_t vc_channel_table_capabilities(uint8_t ch);

extern struct vc_channel_table_entry vc_channel_table[];

#endif
```

- [ ] **Step 3: Create vc_channel_table.c**

Note: This file uses `DT_FOREACH_CHILD_STATUS_OKAY` and `DT_REG_ADDR` which require the new DTS bindings from Task 2. It's added to CMakeLists behind `CONFIG_VC_CHANNEL_CONTROLLER` so it won't compile until the DTS change lands.

```c
/*
 * Copyright (c) 2026 Jianwei
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voltage_control/vc_channel_table.h"
#include "voltage_control/vc_channel_hw.h"
#include "voltage_control/vc_controller.h"
#include <zephyr/devicetree.h>
#include <errno.h>

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

static struct vc_controller *g_ctrl;

static struct vc_measurement_buffer_entry
	meas_buffers[DT_CHILD_NUM_STATUS_OKAY(VC_CONTROLLER_NODE)];

#define CH_TABLE_ENTRY(node_id) \
	{ \
		.dev = DEVICE_DT_GET(node_id), \
		.index = DT_REG_ADDR(node_id), \
		.capabilities = DT_PROP(node_id, capabilities), \
		.meas = &meas_buffers[DT_REG_ADDR(node_id)], \
	},

struct vc_channel_table_entry vc_channel_table[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_TABLE_ENTRY)
};

static const size_t vc_channel_table_size = ARRAY_SIZE(vc_channel_table);

void vc_channel_table_init(struct vc_controller *ctrl)
{
	g_ctrl = ctrl;
}

struct vc_controller *vc_channel_table_get_controller(void)
{
	return g_ctrl;
}

static const struct vc_channel_hw_api *get_hw_api(uint8_t ch)
{
	if (ch >= vc_channel_table_size || vc_channel_table[ch].dev == NULL) {
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

- [ ] **Step 4: Add vc_channel_table.c to CMakeLists**

In `lib/voltage_control/CMakeLists.txt`, add after the `controller.c` line:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER vc_channel_table.c)
```

- [ ] **Step 5: Commit**

```bash
git add include/voltage_control/vc_channel_hw.h \
  include/voltage_control/vc_channel_table.h \
  lib/voltage_control/vc_channel_table.c \
  lib/voltage_control/CMakeLists.txt
git commit -m "feat: add vc_channel_hw.h, vc_channel_table with measurement buffer"
```

---

### Task 2: DTS binding migration (atomic)

All bindings, DTS files, overlays, VC_MAX_CHANNELS, and controller.c must change together.

**Files:**
- Modify: `dts/bindings/voltage_control/jianwei,vc-controller.yaml`
- Modify: `dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml`
- Modify: `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml`
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts`
- Modify: `include/voltage_control/domain.h` (VC_MAX_CHANNELS)
- Modify: `lib/voltage_control/controller.c`
- Modify: All 7 test overlay files
- Delete: `dts/bindings/voltage_control/jianwei,vc-channel-base.yaml`

- [ ] **Step 1: Rewrite vc-controller.yaml**

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

- [ ] **Step 2: Rewrite hvb-vc-channel.yaml**

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: HVB Virtual Voltage Channel (child of vc-controller)

compatible: "jianwei,hvb-vc-channel"

properties:
  dac:
    type: phandle
    required: true
    description: Phandle to the DAC device.
  adc:
    type: phandle
    required: true
    description: Phandle to the ADC device.
  max-raw-dac:
    type: int
    required: true
    default: 0xFFFF
    description: Maximum raw DAC code.
  sample-rate-ms:
    type: int
    required: false
    default: 100
    description: Target measurement sampling period in milliseconds.
```

- [ ] **Step 3: Rewrite vc-channel-stub.yaml**

```yaml
# Copyright (c) 2026 Jianwei
# SPDX-License-Identifier: Apache-2.0

description: Stub VC channel for native_posix tests (child of vc-controller)

compatible: "jianwei,vc-channel-stub"
```

- [ ] **Step 4: Delete vc-channel-base.yaml**

```bash
git rm dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
```

- [ ] **Step 5: Update jw_hvb.dts**

Replace the vc_controller section (lines 261-293) with child-node composition:

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

- [ ] **Step 6: Update VC_MAX_CHANNELS in domain.h**

Change `include/voltage_control/domain.h` line 14 from:
```c
#define VC_MAX_CHANNELS DT_PROP_LEN(DT_NODELABEL(vc_controller), channels)
```
to:
```c
#define VC_MAX_CHANNELS DT_CHILD_NUM_STATUS_OKAY(DT_NODELABEL(vc_controller))
```

- [ ] **Step 7: Update controller.c**

Replace entire file:

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

- [ ] **Step 8: Update all 7 test overlays**

Replace each overlay file with the child-node format:

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

Apply to all 7 files:
- `tests/voltage_control/domain/boards/native_posix.overlay`
- `tests/voltage_control/modbus_adapter/boards/native_posix.overlay`
- `tests/voltage_control/vc/boards/native_posix.overlay`
- `tests/voltage_control/provider_bus/boards/native_posix.overlay`
- `tests/voltage_control/runtime/boards/native_posix.overlay`
- `tests/voltage_control/vc_channel_state/boards/native_posix.overlay`
- `tests/voltage_control/vc_controller/boards/native_posix.overlay`

- [ ] **Step 9: Build and run core test suites**

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
west build -b native_posix tests/voltage_control/vc_controller -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

Expected: All 141 tests pass.

- [ ] **Step 10: Commit**

```bash
git rm dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
git add dts/bindings/voltage_control/ boards/jianwei/jw_hvb/jw_hvb.dts \
  include/voltage_control/domain.h lib/voltage_control/controller.c \
  tests/voltage_control/*/boards/native_posix.overlay
git commit -m "refactor: DTS child-node composition for vc-controller channels"
```

---

### Task 3: Add interrupt-driven mode to ADS1232 driver

The ADS1232 driver keeps its existing Zephyr ADC API (`adc_read`) for demo/shell compatibility. We add three new functions for the HVB driver's non-blocking path: select ADC input channel, get DRDY GPIO spec for interrupt setup, and bit-bang read without DRDY wait.

**Files:**
- Modify: `drivers/sensor/ads1232/ads1232.c`

- [ ] **Step 1: Add split-phase functions after ads1232_read and before ads1232_init**

```c
/* ---- Split-phase API for interrupt-driven consumers ---- */

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

	/* 25th SCLK to force DRDY/DOUT high */
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

- [ ] **Step 2: Commit**

```bash
git add drivers/sensor/ads1232/ads1232.c
git commit -m "feat: add ADS1232 split-phase API for interrupt-driven consumers"
```

---

### Task 4: Rewrite HVB VC channel driver

Full rewrite: blocking adc_read → k_work_poll on DRDY signal + ads1232_read_data_now. Provider bus dependency removed. Implements vc_channel_hw_api. Writes directly to measurement buffer and calls vc_controller_consume_*.

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`

- [ ] **Step 1: Rewrite hvb_vc_channel.c**

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

static void hvb_vc_start_next_cycle(struct hvb_vc_data *data);

static void hvb_vc_poll_handler(struct k_work *work)
{
	struct k_work_poll *pw = CONTAINER_OF(work, struct k_work_poll, work);
	struct hvb_vc_data *data = CONTAINER_OF(pw, struct hvb_vc_data, poll_work);
	const struct hvb_vc_config *cfg = data->dev->config;
	struct vc_controller *ctrl = vc_channel_table_get_controller();
	int32_t raw;

	hvb_vc_disarm_drdy(data);

	if (data->poll_work.poll_result != 0) {
		LOG_WRN("ch%d DRDY timeout phase=%d", data->channel, data->adc_phase);
		if (ctrl) {
			vc_controller_consume_fault(ctrl, data->channel,
						    VC_FAULT_MEASUREMENT);
		}
		k_work_schedule_for_queue(&hvb_vc_workq, &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
		return;
	}

	if (ads1232_read_data_now(cfg->adc, &raw) < 0) {
		LOG_ERR("ch%d read failed", data->channel);
		if (ctrl) {
			vc_controller_consume_fault(ctrl, data->channel,
						    VC_FAULT_MEASUREMENT);
		}
		k_work_schedule_for_queue(&hvb_vc_workq, &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
		return;
	}

	if (data->adc_phase == ADC_PHASE_VOLTAGE) {
		data->meas->raw_voltage = raw;
		data->meas->voltage_timestamp_ms = k_uptime_get_32();
		if (ctrl) {
			vc_controller_consume_voltage(ctrl, data->channel, raw);
		}

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
		data->meas->raw_current = raw;
		data->meas->current_timestamp_ms = k_uptime_get_32();
		if (ctrl) {
			vc_controller_consume_current(ctrl, data->channel, raw);
		}

		data->adc_phase = ADC_PHASE_VOLTAGE;
		k_work_schedule_for_queue(&hvb_vc_workq,
					  &data->next_cycle_work,
					  K_MSEC(cfg->sample_rate_ms));
	}
}

static void hvb_vc_next_cycle_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hvb_vc_data *data = CONTAINER_OF(dwork, struct hvb_vc_data,
						next_cycle_work);

	if (!data->sampling_active) {
		return;
	}
	hvb_vc_start_next_cycle(data);
}

static void hvb_vc_start_next_cycle(struct hvb_vc_data *data)
{
	const struct hvb_vc_config *cfg = data->dev->config;

	data->adc_phase = ADC_PHASE_VOLTAGE;
	ads1232_select_channel(cfg->adc, 0);
	hvb_vc_arm_drdy(data);
	k_work_poll_submit_to_queue(&hvb_vc_workq,
				    &data->poll_work,
				    &data->drdy_event, 1,
				    K_MSEC(HVB_VC_DRDY_TIMEOUT_MS));
}

static int hvb_vc_start_sampling(const struct device *dev)
{
	struct hvb_vc_data *data = dev->data;
	const struct hvb_vc_config *cfg = dev->config;

	if (!(cfg->capabilities & (CH_CAP_VOLTAGE_MEASUREMENT |
				   CH_CAP_CURRENT_MEASUREMENT))) {
		return 0;
	}

	data->sampling_active = true;
	hvb_vc_start_next_cycle(data);
	return 0;
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

static const struct vc_channel_hw_api hvb_vc_hw_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.start_sampling = hvb_vc_start_sampling,
	.stop_sampling = hvb_vc_stop_sampling,
};

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

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x",
		cfg->channel_index,
		cfg->dac->name, cfg->adc->name, cfg->capabilities);
	return 0;
}

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

- [ ] **Step 2: Verify native_posix tests still pass** (HVB driver not compiled for native_posix)

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

- [ ] **Step 3: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c
git commit -m "refactor: rewrite HVB driver — k_work_poll + DRDY interrupt + vc_channel_hw_api"
```

---

### Task 5: Rewrite domain_runtime.c as thin facade

Replace the 500-line runtime with a thin facade that uses vc_controller. No more provider bus, no measurement draining, no config publishing.

**Files:**
- Modify: `lib/voltage_control/domain_runtime.c`

- [ ] **Step 1: Rewrite domain_runtime.c**

The new runtime keeps: command queue + worker thread + published snapshot cache. It drops: evidence queue, provider bus calls, direct domain_state calls for channel operations.

The `vc_runtime` struct simplifies to hold a `vc_controller *` instead of a `domain *`. All channel operations route through `vc_controller_*` functions. The runtime still owns system config (slave address, baud rate) separately, and delegates channel state entirely to the controller.

Write the full replacement (approximately 250 lines — half the current size). The worker loop becomes:

```
while (!stop_requested):
    k_sem_take(&wake, K_MSEC(TICK_INTERVAL_MS))
    drain command queue → route to vc_controller_*
    if tick expired: vc_controller_tick(ctrl, dt_ms)
    publish_snapshot() → copy from vc_controller_get_*_snapshot()
```

The `vc_runtime_submit_measurement` function is removed — measurements come from the HVB driver directly to vc_controller.

- [ ] **Step 2: Verify domain tests still build** (they use the runtime indirectly via vc.c)

Note: The domain test suite creates a `vc_runtime` which depends on the old runtime. Some domain tests may need updating to use the new API. The modbus adapter tests also depend on the runtime. These tests may need a migration step — if they fail, adapt them to the new runtime interface.

- [ ] **Step 3: Commit**

```bash
git add lib/voltage_control/domain_runtime.c
git commit -m "refactor: rewrite domain_runtime.c as thin vc_controller facade"
```

---

### Task 6: Rewrite vc.c — route to vc_controller directly

The CQRS facade (`vc_dispatch`/`vc_query`) routes to `vc_controller_*` instead of constructing `vc_runtime_command` structs and submitting them to the runtime's command queue.

**Files:**
- Modify: `lib/voltage_control/vc.c`
- Modify: `include/voltage_control/vc.h` (simplify types — remove `VC_CMD_SUBMIT_MEASUREMENT`, `vc_runtime_command` references)

- [ ] **Step 1: Rewrite vc.c**

The `vc_ctx` now holds a `vc_controller *` and a `vc_runtime *` (for the worker thread lifecycle). `vc_dispatch` calls `vc_controller_*` directly via the runtime's command queue (to maintain thread safety — commands from Modbus must be serialized). `vc_query` reads from the runtime's published snapshot cache.

- [ ] **Step 2: Commit**

```bash
git add lib/voltage_control/vc.c include/voltage_control/vc.h
git commit -m "refactor: rewrite vc.c — route dispatch/query to vc_controller"
```

---

### Task 7: Update modbus_adapter.c

The modbus adapter should work unchanged if `vc_dispatch`/`vc_query` maintain the same external API. Verify and fix any issues.

- [ ] **Step 1: Build and run modbus_adapter tests**

```bash
west build -b native_posix tests/voltage_control/modbus_adapter -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

- [ ] **Step 2: Fix any issues and commit**

---

### Task 8: Update application main.c

The application `main.c` calls `vc_init()` → `vc_ctx_start()`. The new `vc_init` creates the `vc_controller` + channel table + runtime. `vc_ctx_start` calls `vc_channel_table_start_sampling` for each channel instead of `vc_provider_bus_start_all`.

- [ ] **Step 1: Update main.c if needed** (may work unchanged if vc.c API is stable)

- [ ] **Step 2: Commit**

---

### Task 9: Delete legacy modules

**Files to delete:**
- `lib/voltage_control/provider_bus.c`
- `include/voltage_control/provider_bus.h`
- `include/voltage_control/vc_channel.h`
- `lib/voltage_control/provider_bus_sections.ld`
- `tests/voltage_control/provider_bus/` (entire directory)

**Files to update:**
- `lib/voltage_control/CMakeLists.txt` — remove provider_bus.c, provider_bus_sections.ld
- `lib/voltage_control/Kconfig` — remove `CONFIG_VC_PROVIDER_BUS`, `CONFIG_VC_PROVIDER_MSGQ_DEPTH`, `CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH`
- `include/voltage_control/runtime.h` — remove `vc_runtime_config_snapshot`, `vc_measurement_snapshot`, `vc_runtime_command` (if no longer needed by the runtime)

- [ ] **Step 1: Delete files**

```bash
git rm lib/voltage_control/provider_bus.c
git rm include/voltage_control/provider_bus.h
git rm include/voltage_control/vc_channel.h
git rm lib/voltage_control/provider_bus_sections.ld
git rm -r tests/voltage_control/provider_bus/
```

- [ ] **Step 2: Update CMakeLists.txt** — remove provider_bus lines

- [ ] **Step 3: Update Kconfig** — remove provider bus config options

- [ ] **Step 4: Clean up runtime.h** — remove types that are no longer used

- [ ] **Step 5: Commit**

```bash
git add -u
git commit -m "refactor: delete provider bus, legacy vc_channel_api, and unused runtime types"
```

---

### Task 10: Full build and test verification

- [ ] **Step 1: Run all native_posix test suites**

```bash
for t in domain vc_channel_state vc_controller vc modbus_adapter runtime; do
  echo "=== $t ==="
  west build -b native_posix tests/voltage_control/$t -p 2>&1 | tail -2
  ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
done
```

Note: `runtime` and `provider_bus` test suites may no longer exist or may need updating. Skip any that were deleted.

- [ ] **Step 2: Build for hardware target**

```bash
west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5
```

- [ ] **Step 3: Final commit log**

```bash
git log --oneline -15
```

Verify clean progression: headers → DTS → ADS1232 → HVB driver → runtime → vc.c → modbus → main → delete legacy → verify.

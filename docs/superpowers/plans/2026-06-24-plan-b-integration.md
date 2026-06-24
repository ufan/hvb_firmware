# Plan B: Hardware Layer + Integration + Provider Bus Removal

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the Channel Table & Direct-Drive Architecture by building the hardware abstraction layer, rewiring the domain runtime and CQRS facade to use the new voltage controller, rewriting the HVB driver with interrupt-driven ADC sampling, and deleting the provider bus.

**Architecture:** The ADS1232 driver gains a Kconfig-selectable interrupt-driven mode (`CONFIG_ADS1232_INTERRUPT_DRIVEN`) that implements Zephyr's standard `adc_read_async()` API using DRDY GPIO interrupt + ISR bit-bang. The HVB driver uses `adc_read_async()` + `k_work_poll` — zero ADS1232 internals leaked, pure Zephyr ADC API. Demos keep the legacy blocking `adc_read()` path. The domain runtime and vc.c facade are rewritten to route through vc_controller. The provider bus is deleted.

**Tech Stack:** C99, Zephyr RTOS (k_work_poll, k_poll_signal, adc_read_async, GPIO interrupts), DTS bindings, ztest on native_posix

**Spec:** `docs/superpowers/specs/2026-06-24-channel-table-direct-drive-design.md`

**Depends on:** Plan A (vc_channel_state.c, vc_controller.c) — completed, 57 passing tests.

---

## Task Overview

| # | Task | What changes |
|---|---|---|
| 1 | New headers (additive) | `vc_channel_hw.h`, `vc_channel_table.h/c`, measurement buffer type |
| 2 | DTS binding migration (atomic) | All bindings, overlays, `VC_MAX_CHANNELS`, `controller.c` |
| 3 | ADS1232 interrupt-driven mode | `CONFIG_ADS1232_INTERRUPT_DRIVEN` Kconfig, `adc_read_async()` via DRDY ISR + bit-bang |
| 4 | HVB driver rewrite | `adc_read_async()` + k_work_poll + `vc_channel_hw_api` — no ADS1232 internals |
| 5 | domain_runtime.c rewrite | Thin facade using `vc_controller`, no provider bus |
| 6 | vc.c rewrite | Route dispatch/query to `vc_controller` directly |
| 7 | modbus_adapter.c + main.c update | Adapt to new vc.h / vc_controller path |
| 8 | Delete legacy modules | `provider_bus.c/h`, `vc_channel.h`, `runtime.h` unused types |
| 9 | Full build + test verification | All native_posix tests + `west build -b jw_hvb` |

---

### Task 1: Create vc_channel_hw.h + vc_channel_table.h/c

Additive — no conflicts with existing code. Compiles alongside old modules.

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

Uses `DT_FOREACH_CHILD_STATUS_OKAY` — won't compile until DTS bindings change in Task 2. Added behind `CONFIG_VC_CHANNEL_CONTROLLER`.

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

- [ ] **Step 4: Add to CMakeLists**

Add after the `controller.c` line in `lib/voltage_control/CMakeLists.txt`:
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

All bindings, DTS files, overlays, VC_MAX_CHANNELS, and controller.c change together.

**Files:** Same as in the previous plan version — refer to that task for complete step-by-step. The steps are identical:

- [ ] **Step 1:** Rewrite `jianwei,vc-controller.yaml` with `child-binding` (reg, label, capabilities, enable-gpios)
- [ ] **Step 2:** Rewrite `jianwei,hvb-vc-channel.yaml` (child binding with dac, adc, max-raw-dac, sample-rate-ms)
- [ ] **Step 3:** Rewrite `jianwei,vc-channel-stub.yaml` (empty — inherits from parent child-binding)
- [ ] **Step 4:** `git rm jianwei,vc-channel-base.yaml`
- [ ] **Step 5:** Update `jw_hvb.dts` — channels as children of controller with `reg`/`label`
- [ ] **Step 6:** Update `VC_MAX_CHANNELS` in `domain.h` to `DT_CHILD_NUM_STATUS_OKAY`
- [ ] **Step 7:** Update `controller.c` to use `DT_FOREACH_CHILD_STATUS_OKAY` + `DT_REG_ADDR`
- [ ] **Step 8:** Update all 7 test overlays to child-node format
- [ ] **Step 9:** Build and run core tests (domain, vc_channel_state, vc_controller — 141 tests)
- [ ] **Step 10:** Commit

(See previous plan version for exact file contents of each step.)

---

### Task 3: ADS1232 interrupt-driven mode via Kconfig

The ADS1232 driver gains `CONFIG_ADS1232_INTERRUPT_DRIVEN` which implements Zephyr's standard `adc_read_async()` API. The DRDY interrupt fires, the ISR bit-bangs 24 bits (~50µs, acceptable in ISR on Cortex-M4), stores the result in the sequence buffer, and raises the caller's `k_poll_signal`. The blocking `adc_read()` path remains for demos.

**Files:**
- Modify: `drivers/sensor/ads1232/Kconfig`
- Modify: `drivers/sensor/ads1232/ads1232.c`

- [ ] **Step 1: Add Kconfig option**

Add to `drivers/sensor/ads1232/Kconfig`:

```kconfig
config ADS1232_INTERRUPT_DRIVEN
	bool "Use interrupt-driven DRDY wait for ADS1232"
	depends on ADS1232
	select ADC_ASYNC
	help
	  Use GPIO interrupt on DRDY pin instead of polling.
	  Enables adc_read_async() for non-blocking ADC reads.
	  The ISR performs the 24-bit bit-bang (~50us) and signals
	  the caller via k_poll_signal.
```

- [ ] **Step 2: Add interrupt-driven state and functions to ads1232.c**

Add to `struct ads1232_data`:

```c
struct ads1232_data {
	struct k_mutex lock;
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	struct gpio_callback drdy_cb;
	struct k_poll_signal *async_signal;
	const struct adc_sequence *async_seq;
	int async_channel;
#endif
};
```

Add DRDY ISR and async read function:

```c
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN

static void ads1232_drdy_isr(const struct device *port,
			     struct gpio_callback *cb,
			     gpio_port_pins_t pins)
{
	struct ads1232_data *data = CONTAINER_OF(cb, struct ads1232_data, drdy_cb);
	/* Bit-bang is ~50µs — acceptable in ISR on Cortex-M4 */
	const struct ads1232_config *cfg =
		CONTAINER_OF(/* need device reference — store in data */);
	/* ... see Step 3 for full implementation */
}

static int ads1232_read_async(const struct device *dev,
			      const struct adc_sequence *sequence,
			      struct k_poll_signal *async)
{
	const struct ads1232_config *cfg = dev->config;
	struct ads1232_data *data = dev->data;
	int ch;

	if (sequence->channels & BIT(0)) {
		ch = 0;
	} else if (sequence->channels & BIT(1)) {
		ch = 1;
	} else {
		return -EINVAL;
	}

	/* Select ADC input channel */
	if (cfg->a0.port) {
		gpio_pin_set_dt(&cfg->a0, ch);
	}

	/* Save async context */
	data->async_signal = async;
	data->async_seq = sequence;
	data->async_channel = ch;

	/* Arm DRDY interrupt — fires when conversion ready */
	k_poll_signal_reset(async);
	gpio_pin_interrupt_configure_dt(&cfg->drdy, GPIO_INT_EDGE_TO_ACTIVE);

	return 0;
}

#endif /* CONFIG_ADS1232_INTERRUPT_DRIVEN */
```

- [ ] **Step 3: Implement the DRDY ISR with bit-bang**

The ISR needs the device pointer to access the config (SCLK, DRDY pins). Add `const struct device *dev` to `ads1232_data`:

```c
struct ads1232_data {
	struct k_mutex lock;
	const struct device *dev;
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	struct gpio_callback drdy_cb;
	struct k_poll_signal *async_signal;
	const struct adc_sequence *async_seq;
	int async_channel;
#endif
};
```

Set `data->dev = dev` in `ads1232_init()`.

Full ISR:

```c
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN

static void ads1232_drdy_isr(const struct device *port,
			     struct gpio_callback *cb,
			     gpio_port_pins_t pins)
{
	struct ads1232_data *data = CONTAINER_OF(cb, struct ads1232_data, drdy_cb);
	const struct ads1232_config *cfg = data->dev->config;
	int32_t val = 0;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&cfg->drdy, GPIO_INT_DISABLE);

	/* Bit-bang 24 bits MSB-first (~50µs) */
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

	/* Sign-extend 24-bit to 32-bit */
	if (val & BIT(23)) {
		val |= (int32_t)0xFF000000;
	}

	/* Store result in caller's buffer */
	if (data->async_seq != NULL) {
		int32_t *buf = (int32_t *)data->async_seq->buffer;
		*buf = val;
	}

	/* Signal completion to caller */
	if (data->async_signal != NULL) {
		k_poll_signal_raise(data->async_signal, 0);
		data->async_signal = NULL;
	}
}

#endif
```

- [ ] **Step 4: Register DRDY callback in ads1232_init when interrupt mode enabled**

Add to `ads1232_init()`, after GPIO configuration:

```c
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	data->dev = dev;
	gpio_init_callback(&data->drdy_cb, ads1232_drdy_isr, BIT(cfg->drdy.pin));
	gpio_add_callback(cfg->drdy.port, &data->drdy_cb);
#endif
```

- [ ] **Step 5: Update the adc_driver_api struct**

```c
static const struct adc_driver_api ads1232_api = {
	.channel_setup = ads1232_channel_setup,
	.read          = ads1232_read,
#ifdef CONFIG_ADS1232_INTERRUPT_DRIVEN
	.read_async    = ads1232_read_async,
#endif
	.ref_internal  = 5000,
};
```

- [ ] **Step 6: Verify native_posix tests still pass** (ADS1232 not compiled for native_posix)

```bash
west build -b native_posix tests/voltage_control/domain -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

- [ ] **Step 7: Commit**

```bash
git add drivers/sensor/ads1232/
git commit -m "feat: ADS1232 interrupt-driven mode via CONFIG_ADS1232_INTERRUPT_DRIVEN

Implements Zephyr adc_read_async() API. DRDY GPIO interrupt triggers
ISR which bit-bangs 24 bits (~50us), stores result in sequence buffer,
and raises caller's k_poll_signal. Blocking adc_read() preserved for
demos. Selected via Kconfig."
```

---

### Task 4: Rewrite HVB VC channel driver

The HVB driver uses standard Zephyr `adc_read_async()` + `k_work_poll` — no ADS1232 internals leaked. The ADC phase state machine (voltage → current → next cycle) is driven by k_work_poll handlers triggered when `adc_read_async` signals completion.

**Files:**
- Modify: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`
- Modify: `drivers/voltage_control/hvb_vc_channel/Kconfig` (select ADS1232_INTERRUPT_DRIVEN)

- [ ] **Step 1: Update HVB Kconfig to select interrupt-driven ADS1232**

Add to `drivers/voltage_control/hvb_vc_channel/Kconfig`:

```kconfig
config HVB_VC_CHANNEL
	bool "HVB virtual voltage channel provider"
	default y
	depends on DT_HAS_JIANWEI_HVB_VC_CHANNEL_ENABLED
	select VC_CHANNEL_PROVIDER
	select ADS1232_INTERRUPT_DRIVEN
	help
	  Virtual voltage channel provider for Jianwei HVB boards.
	  Uses interrupt-driven ADC reads via adc_read_async().
```

- [ ] **Step 2: Rewrite hvb_vc_channel.c**

The driver now uses `adc_read_async()` for non-blocking ADC reads. Per-instance state:

```c
struct hvb_vc_data {
	const struct device *dev;
	uint8_t channel;
	struct vc_measurement_buffer_entry *meas;

	/* Async ADC read */
	struct k_work_poll poll_work;
	struct k_poll_signal adc_signal;
	struct k_poll_event adc_event;
	struct adc_sequence adc_seq;
	int32_t adc_buf;

	enum { ADC_PHASE_VOLTAGE, ADC_PHASE_CURRENT } adc_phase;
	struct k_work_delayable next_cycle_work;
	bool sampling_active;
};
```

The k_work_poll handler:

```c
static void hvb_vc_poll_handler(struct k_work *work)
{
	struct k_work_poll *pw = CONTAINER_OF(work, struct k_work_poll, work);
	struct hvb_vc_data *data = CONTAINER_OF(pw, struct hvb_vc_data, poll_work);
	const struct hvb_vc_config *cfg = data->dev->config;
	struct vc_controller *ctrl = vc_channel_table_get_controller();
	int32_t raw = data->adc_buf;

	if (data->poll_work.poll_result != 0) {
		/* adc_read_async timed out — DRDY never fired */
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
			/* Start current read */
			data->adc_phase = ADC_PHASE_CURRENT;
			data->adc_seq.channels = BIT(1);
			k_poll_signal_reset(&data->adc_signal);
			adc_read_async(cfg->adc, &data->adc_seq, &data->adc_signal);
			k_work_poll_submit_to_queue(&hvb_vc_workq,
						    &data->poll_work,
						    &data->adc_event, 1,
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
```

The start_sampling function:

```c
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
	data->adc_seq.channels = BIT(0);
	k_poll_signal_reset(&data->adc_signal);
	adc_read_async(cfg->adc, &data->adc_seq, &data->adc_signal);
	return k_work_poll_submit_to_queue(&hvb_vc_workq,
					   &data->poll_work,
					   &data->adc_event, 1,
					   K_MSEC(HVB_VC_DRDY_TIMEOUT_MS)) < 0
		? -EIO : 0;
}
```

The driver init sets up `adc_seq` with the shared `adc_buf`:

```c
data->adc_seq.buffer = &data->adc_buf;
data->adc_seq.buffer_size = sizeof(data->adc_buf);
data->adc_seq.resolution = 24;
```

No `extern` declarations for ADS1232 functions. No DRDY GPIO management. No bit-bang code. Pure Zephyr ADC API.

The `HVB_VC_INIT` macro uses `DT_REG_ADDR` for channel_index and registers `vc_channel_hw_api` (4 functions).

Write the complete file (full rewrite, approximately 200 lines).

- [ ] **Step 3: Verify native_posix tests still pass**

```bash
west build -b native_posix tests/voltage_control/vc_channel_state -p && ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
```

- [ ] **Step 4: Commit**

```bash
git add drivers/voltage_control/hvb_vc_channel/
git commit -m "refactor: rewrite HVB driver — adc_read_async + k_work_poll + vc_channel_hw_api

Uses standard Zephyr adc_read_async() API. No ADS1232 internals.
ADC phase state machine: voltage → current → next cycle.
Shared workq occupied ~60µs per read instead of ~400ms."
```

---

### Task 5: Rewrite domain_runtime.c as thin facade

The 500-line runtime becomes ~250 lines. It keeps: command queue + worker thread + published snapshot cache. It drops: evidence queue, provider bus calls, direct domain_state calls for channel operations. All channel operations route through `vc_controller_*`.

**Files:**
- Modify: `lib/voltage_control/domain_runtime.c`

- [ ] **Step 1: Rewrite domain_runtime.c**

The `vc_runtime` struct changes to hold `vc_controller *` instead of `domain *`. The worker loop simplifies to:

```
while (!stop_requested):
    k_sem_take(&wake, K_MSEC(TICK_INTERVAL_MS))
    drain command queue → route to vc_controller_*
    if tick expired: vc_controller_tick(ctrl, dt_ms)
    publish_snapshot() → copy from vc_controller_get_*_snapshot()
```

Remove: `vc_runtime_publish_all_configs`, `vc_runtime_publish_snapshot` staleness check (staleness is now implicit from measurement buffer timestamps), evidence queue draining, `vc_runtime_submit_measurement`.

Write the complete replacement file.

- [ ] **Step 2: Run tests and fix any breakage**
- [ ] **Step 3: Commit**

---

### Task 6: Rewrite vc.c

The CQRS facade routes to `vc_controller_*` via the runtime's command queue (for thread safety). `vc_query` reads from published snapshot cache.

**Files:**
- Modify: `lib/voltage_control/vc.c`
- Modify: `include/voltage_control/vc.h` (remove `VC_CMD_SUBMIT_MEASUREMENT` and related types)

- [ ] **Step 1: Rewrite vc.c**

`vc_init()` creates the `vc_controller` + channel table + runtime. `vc_ctx_start()` calls `vc_channel_table_start_sampling` for each channel. `vc_dispatch()` routes commands via the runtime's command queue. `vc_query()` reads from published snapshot cache.

- [ ] **Step 2: Run tests and fix any breakage**
- [ ] **Step 3: Commit**

---

### Task 7: Update modbus_adapter.c + main.c

The modbus adapter should work unchanged if `vc_dispatch`/`vc_query` maintain the same external API. `main.c` calls `vc_init()` / `vc_ctx_start()` which are updated in Task 6.

- [ ] **Step 1: Build and run modbus_adapter tests**
- [ ] **Step 2: Verify main.c compiles for jw_hvb target**
- [ ] **Step 3: Fix any issues and commit**

---

### Task 8: Delete legacy modules

**Delete:**
```bash
git rm lib/voltage_control/provider_bus.c
git rm include/voltage_control/provider_bus.h
git rm include/voltage_control/vc_channel.h
git rm lib/voltage_control/provider_bus_sections.ld
git rm -r tests/voltage_control/provider_bus/
```

**Update:**
- `lib/voltage_control/CMakeLists.txt` — remove provider_bus lines
- `lib/voltage_control/Kconfig` — remove `CONFIG_VC_PROVIDER_BUS`, `CONFIG_VC_PROVIDER_MSGQ_DEPTH`, `CONFIG_VC_RUNTIME_EVIDENCE_QUEUE_DEPTH`
- `include/voltage_control/runtime.h` — remove unused types (`vc_runtime_config_snapshot`, `vc_measurement_snapshot`, `vc_runtime_command` if no longer needed)

- [ ] **Step 1: Delete files**
- [ ] **Step 2: Update CMakeLists and Kconfig**
- [ ] **Step 3: Clean up runtime.h**
- [ ] **Step 4: Build and run all tests**
- [ ] **Step 5: Commit**

---

### Task 9: Full build + test verification

- [ ] **Step 1: Run all native_posix test suites**

```bash
for t in domain vc_channel_state vc_controller vc modbus_adapter; do
  echo "=== $t ==="
  west build -b native_posix tests/voltage_control/$t -p 2>&1 | tail -2
  ./build/zephyr/zephyr.exe 2>&1 | grep -E "SUITE|PROJECT"
done
```

- [ ] **Step 2: Build for hardware target**

```bash
west build -b jw_hvb applications/hvb_controller -p 2>&1 | tail -5
```

- [ ] **Step 3: Verify commit log**

```bash
git log --oneline -15
```

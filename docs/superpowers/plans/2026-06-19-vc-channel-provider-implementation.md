# VC Channel Provider Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hand-written `hvb_variant.c` with DTS-composed VC channel providers using Zephyr device model pattern.

**Architecture:** DTS bindings define channel capabilities and hardware phandles. Each channel is a Zephyr device implementing `struct vc_channel_api`. A compile-time controller macro aggregates channels from DTS into a static table consumed by `domain_create`. Tests use a synthetic channel table with no hardware dependencies.

**Tech Stack:** Zephyr 3.7.2, devicetree bindings, `DEVICE_DT_INST_DEFINE`, `DT_FOREACH_PROP_ELEM`, ztest on `native_posix`.

---

### Task 1: DTS Bindings + Capability Header

**Files:**
- Create: `dts/bindings/voltage_control/jianwei,vc-channel-base.yaml`
- Create: `dts/bindings/voltage_control/jianwei,vc-controller.yaml`
- Create: `dts/bindings/include/jianwei,vc-channel-capabilities.h`

- [ ] **Step 1: Create base channel binding**

```yaml
# dts/bindings/voltage_control/jianwei,vc-channel-base.yaml
include: base.yaml

properties:
  channel-index:
    type: int
    required: true
    description: |
      Zero-based channel index. Must match the position in the
      controller's channels phandle array.

  capabilities:
    type: int
    required: true
    description: |
      Bitmask of CH_CAP_* flags. Static board-design facts;
      not runtime-selectable.

  enable-gpios:
    type: phandle-array
    required: true
    description: |
      GPIO that gates channel output. Mandatory because on/off is
      the minimum virtual channel capability.
```

- [ ] **Step 2: Create controller binding**

```yaml
# dts/bindings/voltage_control/jianwei,vc-controller.yaml
include: base.yaml

properties:
  channels:
    type: phandle-array
    required: true
    description: |
      Ordered list of VC channel device phandles. The order defines
      the channel index domain-side.
```

- [ ] **Step 3: Create capability constants header for DTS**

```c
/* dts/bindings/include/jianwei,vc-channel-capabilities.h */
#ifndef JIANWEI_VC_CHANNEL_CAPABILITIES_H
#define JIANWEI_VC_CHANNEL_CAPABILITIES_H

#define CH_CAP_OUTPUT_ENABLE           0x0001
#define CH_CAP_RAW_OUTPUT_DRIVE        0x0002
#define CH_CAP_VOLTAGE_MEASUREMENT     0x0004
#define CH_CAP_CURRENT_MEASUREMENT     0x0008
#define CH_CAP_HARDWARE_STATUS         0x0010

#endif
```

- [ ] **Step 4: Verify build does not break (bindings are inert until DTS references them)**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`
Expected: Build succeeds (bindings have no effect yet).

- [ ] **Step 5: Commit**

```bash
git add dts/bindings/voltage_control/ dts/bindings/include/jianwei,vc-channel-capabilities.h
git commit -m "feat: add VC channel DTS bindings and capability constants"
```

---

### Task 2: Shared VC Channel API Header

**Files:**
- Create: `include/voltage_control/vc_channel.h`

- [ ] **Step 1: Create the API header**

```c
/* include/voltage_control/vc_channel.h */
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VOLTAGE_CONTROL_VC_CHANNEL_H
#define VOLTAGE_CONTROL_VC_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct vc_channel_api {
	int (*set_output)(const struct device *dev, uint16_t code);
	int (*set_enable)(const struct device *dev, bool enable);
	int (*measure_voltage)(const struct device *dev, int32_t *value);
	int (*measure_current)(const struct device *dev, int32_t *value);
	uint16_t (*get_capabilities)(const struct device *dev);
};

#endif
```

- [ ] **Step 2: Verify build**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`
Expected: Build succeeds.

- [ ] **Step 3: Commit**

```bash
git add include/voltage_control/vc_channel.h
git commit -m "feat: add shared vc_channel_api header"
```

---

### Task 3: Controller Aggregation (Compile-time Channel Table)

**Files:**
- Create: `lib/voltage_control/controller.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

In this task the controller file is created but not compiled until DTS nodes exist. We create the file, add it to CMakeLists protected by a Kconfig, and verify the existing build still works.

- [ ] **Step 1: Create controller.c**

```c
/* lib/voltage_control/controller.c */
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include "voltage_control/vc_channel.h"

#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define CH_ENTRY(node_id, prop, idx) \
	[idx] = { \
		.dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)), \
		.index = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
				 channel_index), \
		.capabilities = DT_PROP(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
					 capabilities), \
	},

const struct vc_channel_entry {
	const struct device *dev;
	uint8_t            index;
	uint16_t           capabilities;
} vc_domain_channels[] = {
	DT_FOREACH_PROP_ELEM(VC_CONTROLLER_NODE, channels, CH_ENTRY)
};

const size_t vc_domain_channel_count = ARRAY_SIZE(vc_domain_channels);
```

- [ ] **Step 2: Add to CMakeLists.txt**

In `lib/voltage_control/CMakeLists.txt`, wrap in Kconfig guard:

```cmake
zephyr_library_sources_ifdef(CONFIG_VC_CHANNEL_CONTROLLER controller.c)
```

At the bottom of the file, add:

```cmake
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
```

- [ ] **Step 3: Add VC_CHANNEL_PROVIDER Kconfig and controller Kconfig**

In `lib/voltage_control/Kconfig` (create if missing):

```kconfig
config VC_CHANNEL_PROVIDER
	bool "VC channel provider support"
	help
	  Enable the virtual voltage channel provider subsystem.
	  Selected by concrete provider drivers.

config VC_CHANNEL_CONTROLLER
	bool "VC channel controller aggregation"
	default y
	depends on DT_HAS_JIANWEI_VC_CONTROLLER_ENABLED
	select VC_CHANNEL_PROVIDER
	help
	  Enable compile-time channel table aggregation from the
	  vc-controller DTS node.
```

Wire `lib/Kconfig` to source `lib/voltage_control/Kconfig` if not already present.

- [ ] **Step 4: Mark controller source pending activation**

Tell the compiler DTS nodes are needed:

```c
/* Placeholder comment in controller.c line 1: */
/* Compiles only when DTS vc_controller node exists (Kconfig guard). */
```

- [ ] **Step 5: Verify existing build still passes**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`
Expected: Build succeeds (Kconfig guard keeps controller.c out).

- [ ] **Step 6: Commit**

```bash
git add lib/voltage_control/controller.c lib/voltage_control/CMakeLists.txt
git commit -m "feat: add compile-time VC channel controller aggregation"
```

---

### Task 4: HVB VC Channel Provider Binding

**Files:**
- Create: `dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml`

- [ ] **Step 1: Create HVB provider binding**

```yaml
# dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml
include: jianwei,vc-channel-base.yaml

properties:
  dac:
    type: phandle
    required: true
    description: Phandle to the DAC device for raw output drive.

  adc:
    type: phandle
    required: true
    description: Phandle to the ADC device for voltage/current measurement.

  interlock-gpios:
    type: phandle-array
    required: false
    description: GPIO that reports hardware interlock status.

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

- [ ] **Step 2: Commit**

```bash
git add dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml
git commit -m "feat: add HVB VC channel provider DTS binding"
```

---

### Task 5: HVB VC Channel Provider Driver

**Files:**
- Create: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`
- Create: `drivers/voltage_control/hvb_vc_channel/CMakeLists.txt`
- Create: `drivers/voltage_control/hvb_vc_channel/Kconfig`
- Create: `drivers/voltage_control/CMakeLists.txt`
- Create: `drivers/voltage_control/Kconfig`

- [ ] **Step 1: Create driver Kconfig**

```kconfig
# drivers/voltage_control/hvb_vc_channel/Kconfig
config HVB_VC_CHANNEL
	bool "HVB virtual voltage channel provider"
	default y
	depends on DT_HAS_JIANWEI_HVB_VC_CHANNEL_ENABLED
	select VC_CHANNEL_PROVIDER
	help
	  Virtual voltage channel provider for Jianwei HVB boards.
	  Wraps AD5541 DAC and ADS1232 ADC devices.
```

- [ ] **Step 2: Create driver CMakeLists.txt**

```cmake
# drivers/voltage_control/hvb_vc_channel/CMakeLists.txt
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_HVB_VC_CHANNEL hvb_vc_channel.c)
```

- [ ] **Step 3: Create parent CMakeLists.txt and Kconfig**

```cmake
# drivers/voltage_control/CMakeLists.txt
add_subdirectory_ifdef(CONFIG_HVB_VC_CHANNEL hvb_vc_channel)
```

```kconfig
# drivers/voltage_control/Kconfig
rsource "hvb_vc_channel/Kconfig"
```

- [ ] **Step 4: Wire into top-level drivers**

In `drivers/CMakeLists.txt`, add:

```cmake
add_subdirectory_ifdef(CONFIG_VC_CHANNEL_PROVIDER voltage_control)
```

In `drivers/Kconfig`, add:

```kconfig
rsource "voltage_control/Kconfig"
```

- [ ] **Step 5: Create the HVB provider driver**

```c
/* drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c */
/*
 * Copyright (c) 2026 Jianwei
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "regmap/hvb_regs.h"
#include "voltage_control/vc_channel.h"

LOG_MODULE_REGISTER(hvb_vc_channel, LOG_LEVEL_INF);

#define DT_DRV_COMPAT jianwei_hvb_vc_channel

struct hvb_vc_config {
	const struct device *dac;
	const struct device *adc;
	struct gpio_dt_spec enable;
	uint16_t max_raw_dac;
	uint32_t sample_rate_ms;
	uint16_t capabilities;
};

static int hvb_vc_set_output(const struct device *dev, uint16_t code)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };

	if (code > cfg->max_raw_dac) {
		return -EINVAL;
	}
	dac_channel_setup(cfg->dac, &dac_cfg);
	return dac_write_value(cfg->dac, 0, code);
}

static int hvb_vc_set_enable(const struct device *dev, bool enable)
{
	const struct hvb_vc_config *cfg = dev->config;

	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

static int hvb_vc_measure_voltage(const struct device *dev, int32_t *value)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct adc_sequence seq = {
		.channels = BIT(0),
		.buffer = value,
		.buffer_size = sizeof(*value),
		.resolution = 24,
	};

	return adc_read(cfg->adc, &seq);
}

static int hvb_vc_measure_current(const struct device *dev, int32_t *value)
{
	const struct hvb_vc_config *cfg = dev->config;
	struct adc_sequence seq = {
		.channels = BIT(1),
		.buffer = value,
		.buffer_size = sizeof(*value),
		.resolution = 24,
	};

	return adc_read(cfg->adc, &seq);
}

static uint16_t hvb_vc_get_capabilities(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	return cfg->capabilities;
}

static const struct vc_channel_api hvb_vc_api = {
	.set_output = hvb_vc_set_output,
	.set_enable = hvb_vc_set_enable,
	.measure_voltage = hvb_vc_measure_voltage,
	.measure_current = hvb_vc_measure_current,
	.get_capabilities = hvb_vc_get_capabilities,
};

static int hvb_vc_init(const struct device *dev)
{
	const struct hvb_vc_config *cfg = dev->config;

	if (!device_is_ready(cfg->dac)) {
		LOG_ERR("DAC not ready");
		return -ENODEV;
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

	LOG_INF("ch%d ready dac=%s adc=%s caps=0x%04x",
		DT_INST_PROP(0, channel_index),
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
	}; \
	DEVICE_DT_INST_DEFINE(n, hvb_vc_init, NULL, \
		NULL, &hvb_vc_config_##n, \
		POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
		&hvb_vc_api);

DT_INST_FOREACH_STATUS_OKAY(HVB_VC_INIT)
```

- [ ] **Step 6: Verify build (compile only, no DTS nodes yet — safe because DT_INST_FOREACH_STATUS_OKAY is empty)**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`
Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add drivers/voltage_control/ drivers/CMakeLists.txt drivers/Kconfig
git commit -m "feat: add HVB VC channel provider driver"
```

---

### Task 6: Board DTS + Kconfig Wiring

**Files:**
- Modify: `boards/jianwei/jw_hvb/jw_hvb.dts`
- Modify: `boards/jianwei/jw_hvb/Kconfig.defconfig`

- [ ] **Step 1: Add DTS include and controller + channel nodes to jw_hvb.dts**

After the existing `#include` lines at the top, add:

```dts
#include <jianwei,vc-channel-capabilities.h>
```

At the bottom of the file, before the final closing, add:

```dts
vc_controller: vc-controller {
	compatible = "jianwei,vc-controller";
	channels = <&vc_ch0 &vc_ch1>;
};

vc_ch0: vc-channel {
	compatible = "jianwei,hvb-vc-channel";
	channel-index = <0>;
	capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
	dac = <&ad5541_0>;
	adc = <&ads1232_hv1>;
	enable-gpios = <&gpioh 12 GPIO_ACTIVE_LOW>;
	max-raw-dac = <0xFFFF>;
	sample-rate-ms = <100>;
	status = "disabled";
};

vc_ch1: vc-channel {
	compatible = "jianwei,hvb-vc-channel";
	channel-index = <1>;
	capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
			 CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT)>;
	dac = <&ad5541_1>;
	adc = <&ads1232_hv2>;
	enable-gpios = <&gpioh 13 GPIO_ACTIVE_LOW>;
	max-raw-dac = <0xFFFF>;
	sample-rate-ms = <100>;
	status = "disabled";
};
```

- [ ] **Step 2: Add Kconfig default to enable for jw_hvb**

In `boards/jianwei/jw_hvb/Kconfig.defconfig`, add:

```kconfig
config HVB_VC_CHANNEL
	default y
```

- [ ] **Step 3: Apply the jw_hvb DTS to applications and simulator**

In `applications/hvb_controller/prj.conf` and `demos/modbus_sim/prj.conf`, add:

```
CONFIG_HVB_VC_CHANNEL=y
CONFIG_VC_CHANNEL_CONTROLLER=y
```

- [ ] **Step 4: Verify the jw_hvb build compiles**

Run: `west build -b jianwei/jw_hvb -d build/hvb applications/hvb_controller`
Expected: Build succeeds (channel nodes include the binding; `DT_HAS_JIANWEI_VC_CONTROLLER_ENABLED` becomes true).

- [ ] **Step 5: Commit**

```bash
git add boards/jianwei/jw_hvb/ applications/hvb_controller/prj.conf demos/modbus_sim/prj.conf
git commit -m "feat: add VC controller and channel nodes to jw_hvb DTS"
```

---

### Task 7: Domain Integration

**Files:**
- Modify: `include/voltage_control/domain.h`
- Modify: `lib/voltage_control/domain.c`

- [ ] **Step 1: Add CH_ENTRY type to domain.h and change domain_create**

In `include/voltage_control/domain.h`, after the existing includes, add:

```c
#include "voltage_control/vc_channel.h"

struct vc_channel_entry {
	const struct device *dev;
	uint8_t            index;
	uint16_t           capabilities;
};

extern const struct vc_channel_entry vc_domain_channels[];
extern const size_t vc_domain_channel_count;
```

Change domain_create signature:

```c
struct domain *domain_create(const struct vc_channel_entry *channels,
			     size_t count);
```

- [ ] **Step 2: Rewrite domain_create in domain.c**

In `lib/voltage_control/domain.c`, replace the `struct domain` definition fields that reference variant:

Replace:
```c
	const struct vc_variant_profile *variant;
```

With:
```c
	const struct vc_channel_entry *ch_entry;
	size_t channel_count;
```

Replace `domain_create` body:

```c
struct domain *domain_create(const struct vc_channel_entry *channels,
			     size_t count)
{
	struct domain *domain;

	if (!channels || count > VC_MAX_CHANNELS) {
		return NULL;
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		return NULL;
	}

	domain->ch_entry = channels;
	domain->channel_count = count;
	domain->operating_mode = VC_OPERATING_MODE_NORMAL;

	for (size_t i = 0; i < count && i < VC_MAX_CHANNELS; i++) {
		domain->runtime[i].cal_max_raw_dac_limit = 0xFFFF;
	}

	return domain;
}
```

- [ ] **Step 3: Update domain internal helpers**

`channel_valid`:
```c
static bool channel_valid(const struct domain *domain, uint8_t channel)
{
	return channel < domain->channel_count;
}
```

`channel_has_cap`:
```c
static bool channel_has_cap(const struct domain *domain, uint16_t cap)
{
	if (domain->channel_count == 0) {
		return false;
	}
	return (domain->ch_entry->capabilities & cap) == cap;
}
```

Note: `channel_has_cap` currently uses a single capability for all channels from variant. Since the new design gives per-channel capabilities, update to index:

```c
static bool channel_has_cap(const struct domain *domain,
			    uint8_t channel, uint16_t cap)
{
	if (channel >= domain->channel_count) {
		return false;
	}
	return (domain->ch_entry[channel].capabilities & cap) == cap;
}
```

Update all call sites `channel_has_cap(domain, cap)` to `channel_has_cap(domain, channel, cap)`.

- [ ] **Step 4: Update snapshot projection**

In `domain_get_system_snapshot`, replace variant-derived fields:

```c
snap->supported_channel_count = domain->channel_count;
// active_channel_mask — compute from domain->ch_entry
snap->active_channel_mask = 0;
for (size_t i = 0; i < domain->channel_count; i++) {
	snap->active_channel_mask |= BIT(domain->ch_entry[i].index);
}
```

In `domain_get_channel_snapshot`, replace:

```c
snap->channel_capability_flags = domain->variant->channel_capability_flags;
```

With:

```c
snap->channel_capability_flags = domain->ch_entry[channel].capabilities;
```

- [ ] **Step 5: Remove variant_profile references**

Replace `domain->variant->num_channels` with `domain->channel_count`. Replace `domain->variant->max_raw_dac_code` with a stored constant or per-entry DTS property (defer to Task 7 refinement).

For `calibration_output_disable_confirmed`, store as a domain field initialized to `true` (the HVB default). This was the only user of this variant field.

- [ ] **Step 5: Replace variant-derived domain fields**

Every `domain->variant->X` access in `domain.c` must be replaced. Complete substitution table:

| Variant field | Replacement |
|---|---|
| `->num_channels` | `domain->channel_count` |
| `->max_raw_dac_code` | `#define VC_DEFAULT_MAX_RAW_DAC 0xFFFF` in domain.c |
| `->calibration_output_disable_confirmed` | Always `true` in the check (`if (!true) return` → remove the check and always allow). Remove the `enter_calibration_mode` guard since no variant reports false anymore. |
| `->max_voltage_raw` | `#define VC_DEFAULT_MAX_VOLTAGE_RAW 20000` |
| `->min_voltage_raw` | `#define VC_DEFAULT_MIN_VOLTAGE_RAW 0` |
| `->max_current_raw` | `#define VC_DEFAULT_MAX_CURRENT_RAW 32767` |
| `->channel_capability_flags` | `domain->ch_entry[channel].capabilities` |
| `->system_capability_flags` | `#define VC_DEFAULT_SYSTEM_CAPS (SYS_CAP_AUTOMATIC_MODE \| SYS_CAP_ENV_SENSOR \| SYS_CAP_CALIBRATION_MODE)` |
| `->variant_id` | `#define VC_VARIANT_ID 1` |
| `->channel_mask` | `#define VC_CHANNEL_MASK(ch_count) ((1U << (ch_count)) - 1)` compute from `channel_count` |
| `->default_system_config` | Inline literal initializer in `domain_create` (see Step 5b below) |
| `->default_channel_config` | `memset` to zero in `domain_create` (safe defaults) |

- [ ] **Step 5b: Replace default config initialization in domain_create**

Replace:
```c
memcpy(&domain->sys_cfg, &variant->default_system_config, sizeof(domain->sys_cfg));
for (int i = 0; i < variant->num_channels && i < VC_MAX_CHANNELS; i++) {
    memcpy(&domain->channels[i], &variant->default_channel_config, ...);
}
```

With:
```c
domain->sys_cfg = (struct vc_system_config){
    .operating_mode = VC_OPERATING_MODE_NORMAL,
    .slave_address = 1,
    .baud_rate_code = VC_BAUD_RATE_115200,
    .recovery_policy_mode = VC_RECOVERY_MANUAL_LATCH,
    .voltage_safe_band_pct = 10,
    .current_safe_band_pct = 10,
};
/* Channel config defaults to all-zero (target=0, protections disabled, calib defaults) */
```

- [ ] **Step 6: Build domain test**

Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`
Expected: Build fails because variant.h is still included and used in tests.

- [ ] **Step 7: Update test domain_setup_fresh**

In `tests/voltage_control/domain/src/main.c`, replace variant usage with a minimal fake channel table:

```c
#include "voltage_control/vc_channel.h"

/* Minimal fake device that always succeeds */
static int fake_get_caps(const struct device *dev) {
	return CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
	       CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT;
}
static int fake_noop(const struct device *dev) { return 0; }
static int fake_set_output(const struct device *dev, uint16_t code) { return 0; }
static int fake_set_enable(const struct device *dev, bool en) { return 0; }
static int fake_measure_v(const struct device *dev, int32_t *v) { *v = 0; return 0; }
static int fake_measure_i(const struct device *dev, int32_t *v) { *v = 0; return 0; }

static const struct vc_channel_api fake_api = {
	.get_capabilities = fake_get_caps,
	.set_output = fake_set_output,
	.set_enable = fake_set_enable,
	.measure_voltage = fake_measure_v,
	.measure_current = fake_measure_i,
};

static const struct vc_channel_entry test_channels[] = {
	{ .dev = NULL, .index = 0,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
	                  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
	{ .dev = NULL, .index = 1,
	  .capabilities = CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
	                  CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT },
};

static void *domain_setup_fresh(void)
{
	struct domain *d = domain_create(test_channels, 2);
	zassert_not_null(d);
	return d;
}
```

Also update `on_off_only_variant` to use the fake channel approach:

```c
static struct domain *domain_setup_on_off_only(void)
{
	static const struct vc_channel_entry onoff_ch[] = {
		{ .dev = NULL, .index = 0,
		  .capabilities = CH_CAP_OUTPUT_ENABLE },
	};
	struct domain *d = domain_create(onoff_ch, 1);
	zassert_not_null(d);
	return d;
}
```

Update tests that used `on_off_only_variant()` + `domain_create(&variant)` to use `domain_setup_on_off_only()`.

- [ ] **Step 8: Run tests to confirm all pass**

Run: `west twister -T tests/voltage_control/domain -p native_posix`
Expected: 48 test cases, 0 failures.

- [ ] **Step 9: Commit**

```bash
git add include/voltage_control/domain.h lib/voltage_control/domain.c \
        tests/voltage_control/domain/src/main.c
git commit -m "refactor: switch domain_create to VC channel table"
```

---

### Task 8: Update App and Simulator Callers

**Files:**
- Modify: `applications/hvb_controller/src/main.c`
- Modify: `demos/modbus_sim/src/main.c`

- [ ] **Step 1: Update hvb_controller main.c**

Replace:
```c
#include "voltage_control/variant.h"
...
const struct vc_variant_profile *variant = vc_hvb_get_variant();
struct domain *domain = domain_create(variant);
```

With:
```c
#include "voltage_control/domain.h"
...
/* Domain channels are aggregated from DTS by controller.c */
extern const struct vc_channel_entry vc_domain_channels[];
extern const size_t         vc_domain_channel_count;
...
struct domain *domain = domain_create(vc_domain_channels,
				       vc_domain_channel_count);
```

- [ ] **Step 2: Remove variant.h include**

Remove `#include "voltage_control/variant.h"` from all files.

- [ ] **Step 3: Build hvb_controller for jw_hvb**

Run: `west build -b jianwei/jw_hvb -d build/hvb applications/hvb_controller`
Expected: Build succeeds.

- [ ] **Step 4: Build modbus_sim for jw_hvb**

Run: `west build -b jianwei/jw_hvb -d build/sim demos/modbus_sim`
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add applications/hvb_controller/src/main.c demos/modbus_sim/src/main.c
git commit -m "refactor: switch app/demo to VC channel table"
```

---

### Task 9: Remove variant.h and hvb_variant.c

**Files:**
- Remove: `include/voltage_control/variant.h`
- Remove: `lib/voltage_control/hvb_variant.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

- [ ] **Step 1: Remove hvb_variant.c from CMakeLists.txt**

In `lib/voltage_control/CMakeLists.txt`, remove `hvb_variant.c` from `zephyr_library_sources`:
```cmake
zephyr_library_sources(
	domain.c
	modbus_adapter.c
)
```

- [ ] **Step 2: Delete variant.h and hvb_variant.c**

```c
rm include/voltage_control/variant.h
rm lib/voltage_control/hvb_variant.c
```

- [ ] **Step 3: Build and test**

Run: `west twister -T tests/voltage_control/domain -p native_posix`
Expected: 48 test cases, 0 failures.

Run: `west build -b jianwei/jw_hvb -d build/sim demos/modbus_sim`
Expected: Build succeeds.

Run: `sh tests/architecture/controller_split.sh`
Expected: Pass.

- [ ] **Step 4: Final verification**

```bash
rg -n "vc_variant|variant_profile|hvb_variant" --glob '!build/**' --glob '!docs/**'
```
Expected: No matches in source code (docs may still reference the old design).

- [ ] **Step 5: Commit**

```bash
git add include/voltage_control/variant.h lib/voltage_control/hvb_variant.c \
        lib/voltage_control/CMakeLists.txt
git commit -m "refactor: remove variant.h and hvb_variant.c"
```

---

## Verification Summary

Run these after all tasks complete:

```bash
# Domain tests (no hardware)
west twister -T tests/voltage_control/domain -p native_posix

# Architecture split check
sh tests/architecture/controller_split.sh

# Production build
west build -b jianwei/jw_hvb -d build/hvb applications/hvb_controller

# Simulator build
west build -b jianwei/jw_hvb -d build/sim demos/modbus_sim

# Clean name check
rg -n "vc_domain|vc_variant|VC_DOMAIN" --glob '!build/**' --glob '!docs/**'
```

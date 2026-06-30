# LVB Board Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the jw_lvb 12-channel isolated 12 V power controller into the hvb_firmware module framework, creating a new board definition, a new `lvb_vc_channel` driver, and a new `lvb_controller` application — reusing the existing vc_channel state machine, Modbus v3 protocol, calibration infrastructure, and NVS persistence unchanged.

**Architecture:** The LVB board uses STM32F103ZE with direct GPIO for channel on/off (no DAC, fixed 12 V output) and STM32 internal 12-bit SAR ADC for V/I measurement. A new driver `lvb_vc_channel` implements `vc_channel_api.h` declaring capabilities `CH_CAP_OUTPUT_ENABLE | CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT` (no `CH_CAP_RAW_OUTPUT_DRIVE`). The `vc_channel.c` state machine already guards all DAC operations behind `CH_CAP_RAW_OUTPUT_DRIVE` checks — no shared library code changes are required. The migration makes `VC_VARIANT_ID` Kconfig-configurable (LVB = 2, HVB stays 1), and encodes old-firmware calibration defaults as per-channel DTS properties.

**Tech Stack:** Zephyr T2, STM32F103ZE (72 MHz), Zephyr STM32 ADC driver (`CONFIG_ADC_STM32`), Zephyr GPIO API, Zephyr Settings + NVS, existing Modbus RTU adapter and vc_runtime/vc_controller/vc_channel libraries (all unmodified).

## Global Constraints

- Do NOT modify `lib/voltage_control/*.c`, `include/voltage_control/`, `lib/modbus_adapter/`, or `lib/reg_store/`. The LVB plugs in via driver + DTS only.
- Protocol stays v3 (`VC_PROTOCOL_MAJOR=3`, `VC_PROTOCOL_MINOR=0`).
- LVB `VC_VARIANT_ID = 2`; HVB keeps 1 (Kconfig default).
- Voltage calibration default (all channels): `measured_voltage_calib_k=28125`, `measured_voltage_calib_b=0`. Derivation: `3300 mV × 5.6 (divider) / 4096 × 10000 = 28125`. Assumes Vref = 3.3 V internal, 5.6× resistor divider on board. Update if hardware differs.
- Current calibration defaults: `measured_current_calib_k=10000` (unity). Per-channel zero offsets handled in the driver (subtracted before publishing); initial NVS values start at k=10000, b=0. The per-channel `current-zero-offset-raw` DTS property applies the zero-current offset at the driver level so the calibration layer provides gain refinement only.
  - Old firmware zero-offset table (raw 12-bit ADC counts): ch0=1822, ch1=1682, ch2=1640, ch3=1730, ch4=1715, ch5=1707, ch6=1812, ch7=1717, ch8=1670, ch9=1662.
  - ch10 and ch11 (new): use 1696 (average of ch0–ch9) pending board-specific measurement.
- ADC sample period: 150 ms (`sample-rate-ms = <150>`).
- Modbus RTU on USART1 @ 115200 baud, RS-485 DE on GPIOG.3 (active-low, pull-up). Baud upgraded from old firmware's 19200.
- Console / shell on USART2 @ 115200 (PA2 TX, PA3 RX).
- All 12 channel enable GPIOs are active-high (unlike HVB which is active-low).
- **Channels 10 and 11** GPIO and ADC pin assignments are placeholders derived from remaining STM32F103ZE pins — they MUST be verified against the hardware schematic before flashing. See DTS in Task 3.
- `SYS_CAP_CALIBRATION_MODE` is kept (V/I measurement calibration still supported). `SYS_CAP_ENV_SENSOR` is not set (no SHT3xD on LVB). No `CONFIG_SYS_STATUS` in prj.conf.
- No `CONFIG_AD5541`, no `CONFIG_ADS1232` — LVB uses STM32 internal ADC only.
- Zephyr T2 west workspace (same as current repo; do not update west.yml).

---

## File Map

**Created:**
```
dts/bindings/voltage_control/jianwei,lvb-vc-channel.yaml
drivers/voltage_control/lvb_vc_channel/
    Kconfig
    CMakeLists.txt
    lvb_vc_channel.c
boards/jianwei/jw_lvb/
    board.yml
    Kconfig.jw_lvb
    Kconfig.defconfig
    jw_lvb_defconfig
    board.cmake
    jw_lvb.dts
applications/lvb_controller/
    CMakeLists.txt
    prj.conf
    src/main.c
```

**Modified:**
```
lib/voltage_control/Kconfig                     add CONFIG_VC_VARIANT_ID symbol
lib/voltage_control/vc_controller.c:28          use CONFIG_VC_VARIANT_ID
drivers/voltage_control/Kconfig                 add rsource for lvb_vc_channel
drivers/voltage_control/CMakeLists.txt          add add_subdirectory_ifdef
tests/voltage_control/vc_channel_state/src/main.c  add vc_channel_no_dac suite
```

---

### Task 1: Make VC_VARIANT_ID a Kconfig symbol

Currently `vc_controller.c:28` has `#define VC_VARIANT_ID 1` hardcoded. LVB needs value 2. Making it Kconfig-configurable (default 1) means no HVB change is required.

**Files:**
- Modify: `lib/voltage_control/Kconfig`
- Modify: `lib/voltage_control/vc_controller.c:28`

**Interfaces:**
- Produces: `CONFIG_VC_VARIANT_ID` Kconfig int, default 1. Used only by `vc_controller.c`.

- [ ] **Step 1: Write the failing test**

  The existing `vc_controller` test lives at `tests/voltage_control/vc_controller/`. Confirm it passes before touching anything:

  ```bash
  west build -b native_posix tests/voltage_control/vc_controller -p && \
  ./build/zephyr/zephyr.exe
  ```

  Expected: all tests PASS.

- [ ] **Step 2: Add `VC_VARIANT_ID` to Kconfig**

  In `lib/voltage_control/Kconfig`, find the existing calibration defaults block (around `VC_CAL_SAMPLE_TIMEOUT_MS`) and insert after the `VC_SETTINGS_PERSISTENCE` block:

  ```kconfig
  config VC_VARIANT_ID
  	int "Variant identifier reported in the protocol system snapshot"
  	default 1
  	range 1 255
  	help
  	  Modbus protocol hardware variant ID returned in the VARIANT_ID system
  	  input register.  1 = HVB (2-channel high-voltage DAC), 2 = LVB
  	  (12-channel low-voltage GPIO).
  ```

- [ ] **Step 3: Replace the hardcoded define in vc_controller.c**

  In `lib/voltage_control/vc_controller.c`, line 28, replace:

  ```c
  #define VC_VARIANT_ID 1
  ```

  with:

  ```c
  #define VC_VARIANT_ID CONFIG_VC_VARIANT_ID
  ```

- [ ] **Step 4: Verify existing tests still pass**

  ```bash
  west build -b native_posix tests/voltage_control/vc_controller -p && \
  ./build/zephyr/zephyr.exe
  ```

  Expected: all tests PASS. HVB behaviour is unchanged (Kconfig default = 1).

- [ ] **Step 5: Commit**

  ```bash
  git add lib/voltage_control/Kconfig lib/voltage_control/vc_controller.c
  git commit -m "feat(vc): make VC_VARIANT_ID a Kconfig symbol (default 1)"
  ```

---

### Task 2: LVB DTS binding, driver, and build wiring

Create the `lvb_vc_channel` driver and hook it into the Kconfig/CMake tree. Compilation correctness is verified in Task 3 when the board DTS exists.

**Files:**
- Create: `dts/bindings/voltage_control/jianwei,lvb-vc-channel.yaml`
- Create: `drivers/voltage_control/lvb_vc_channel/Kconfig`
- Create: `drivers/voltage_control/lvb_vc_channel/CMakeLists.txt`
- Create: `drivers/voltage_control/lvb_vc_channel/lvb_vc_channel.c`
- Modify: `drivers/voltage_control/Kconfig` (add `rsource`)
- Modify: `drivers/voltage_control/CMakeLists.txt` (add `add_subdirectory_ifdef`)

**Interfaces:**
- Consumes: `vc_channel_api.h` (the hardware abstraction vtable), `dt-bindings/voltage_control/capabilities.h`
- Produces: Zephyr devices bound to `compatible = "jianwei,lvb-vc-channel"`, each implementing the `vc_channel_api`.

- [ ] **Step 1: Create the DTS binding**

  Create `dts/bindings/voltage_control/jianwei,lvb-vc-channel.yaml`:

  ```yaml
  # Copyright (c) 2026 Jianwei
  # SPDX-License-Identifier: Apache-2.0

  description: LVB virtual voltage channel — child of vc-controller.

  compatible: "jianwei,lvb-vc-channel"

  properties:
    reg:
      type: array
      required: true
      description: Channel index (0-based), used as Modbus channel address.

    label:
      type: string
      required: true
      description: Human-readable channel name (e.g. "CH1").

    capabilities:
      type: int
      required: true
      description: |
        Bitmask of CH_CAP_* flags from
        <dt-bindings/voltage_control/capabilities.h>.
        LVB channels declare CH_CAP_OUTPUT_ENABLE |
        CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT.
        Do NOT include CH_CAP_RAW_OUTPUT_DRIVE (no DAC).

    enable-gpios:
      type: phandle-array
      required: true
      description: >
        GPIO that enables channel output. Active-high on LVB (unlike HVB which
        is active-low). Configured GPIO_OUTPUT_INACTIVE at init (channel off).

    io-channels:
      type: phandle-array
      required: true
      description: >
        Two ADC channel specs in order: [0] voltage, [1] current.
        Use io-channel-names = "voltage", "current" to name them.

    io-channel-names:
      type: string-array
      required: false
      description: Names for io-channels. Must be "voltage" and "current".

    current-zero-offset-raw:
      type: int
      required: false
      default: 0
      description: >
        Raw 12-bit ADC count at zero current. The driver subtracts this
        from each current ADC reading before publishing to the measurement
        buffer. Derived from the old firmware current_zero_offset[] table.
        Update from board-specific calibration measurement.

    sample-rate-ms:
      type: int
      required: false
      default: 150
      description: >
        Measurement sampling period in milliseconds. Matches old firmware
        MEASUREMENT_PERIOD = 150.
  ```

- [ ] **Step 2: Create driver Kconfig**

  Create `drivers/voltage_control/lvb_vc_channel/Kconfig`:

  ```kconfig
  # SPDX-License-Identifier: Apache-2.0

  config LVB_VC_CHANNEL
  	bool "LVB virtual voltage channel provider"
  	default y
  	depends on DT_HAS_JIANWEI_LVB_VC_CHANNEL_ENABLED
  	select ADC_STM32
  	help
  	  Virtual voltage channel provider for Jianwei LVB boards.
  	  Uses synchronous STM32 internal ADC reads via adc_read_dt().
  	  Each channel has a dedicated GPIO output enable and two ADC
  	  inputs (voltage and current).
  ```

- [ ] **Step 3: Create driver CMakeLists.txt**

  Create `drivers/voltage_control/lvb_vc_channel/CMakeLists.txt`:

  ```cmake
  # SPDX-License-Identifier: Apache-2.0

  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_LVB_VC_CHANNEL lvb_vc_channel.c)
  ```

- [ ] **Step 4: Wire into parent Kconfig**

  In `drivers/voltage_control/Kconfig`, append after the existing `rsource "hvb_vc_channel/Kconfig"` line:

  ```kconfig
  rsource "lvb_vc_channel/Kconfig"
  ```

- [ ] **Step 5: Wire into parent CMakeLists.txt**

  In `drivers/voltage_control/CMakeLists.txt`, append after the existing `add_subdirectory_ifdef(CONFIG_HVB_VC_CHANNEL hvb_vc_channel)` line:

  ```cmake
  add_subdirectory_ifdef(CONFIG_LVB_VC_CHANNEL lvb_vc_channel)
  ```

- [ ] **Step 6: Implement the driver**

  Create `drivers/voltage_control/lvb_vc_channel/lvb_vc_channel.c`:

  ```c
  /*
   * Copyright (c) 2026 Jianwei
   * SPDX-License-Identifier: Apache-2.0
   */

  #include <zephyr/kernel.h>
  #include <zephyr/device.h>
  #include <zephyr/drivers/adc.h>
  #include <zephyr/drivers/gpio.h>
  #include <zephyr/logging/log.h>

  #include <dt-bindings/voltage_control/capabilities.h>
  #include "voltage_control/vc_channel_api.h"

  LOG_MODULE_REGISTER(lvb_vc_channel, LOG_LEVEL_INF);

  #define DT_DRV_COMPAT jianwei_lvb_vc_channel

  #define LVB_VC_WORKQ_STACK_SIZE 2048
  #define LVB_VC_WORKQ_PRIORITY   CONFIG_SYSTEM_WORKQUEUE_PRIORITY

  static K_THREAD_STACK_DEFINE(lvb_vc_workq_stack, LVB_VC_WORKQ_STACK_SIZE);
  static struct k_work_q lvb_vc_workq;
  static bool lvb_vc_workq_started;

  static void lvb_vc_ensure_workq(void)
  {
  	if (!lvb_vc_workq_started) {
  		k_work_queue_init(&lvb_vc_workq);
  		k_work_queue_start(&lvb_vc_workq, lvb_vc_workq_stack,
  				   K_THREAD_STACK_SIZEOF(lvb_vc_workq_stack),
  				   LVB_VC_WORKQ_PRIORITY, NULL);
  		k_thread_name_set(&lvb_vc_workq.thread, "lvb_vc_workq");
  		lvb_vc_workq_started = true;
  	}
  }

  struct lvb_vc_config {
  	struct gpio_dt_spec enable;
  	struct adc_dt_spec voltage_ch;
  	struct adc_dt_spec current_ch;
  	int16_t current_zero_offset;
  	uint16_t capabilities;
  	uint8_t channel_index;
  	uint16_t sample_rate_ms;
  };

  struct lvb_vc_data {
  	const struct device *dev;
  	vc_meas_ready_cb_t meas_cb;
  	void *meas_cb_user_data;
  	struct vc_channel_buffer *meas;
  	struct k_work sample_work;
  	struct k_timer sample_timer;
  	bool sampling_active;
  };

  /* ---- Sampling work ---- */

  static void lvb_vc_sample_work_fn(struct k_work *work)
  {
  	struct lvb_vc_data *data =
  		CONTAINER_OF(work, struct lvb_vc_data, sample_work);
  	const struct lvb_vc_config *cfg = data->dev->config;

  	if (!data->sampling_active) {
  		return;
  	}

  	/* Voltage */
  	uint16_t v_buf;
  	int32_t v_raw = 0;
  	struct adc_sequence v_seq = {
  		.buffer = &v_buf,
  		.buffer_size = sizeof(v_buf),
  	};
  	adc_sequence_init_dt(&cfg->voltage_ch, &v_seq);
  	if (adc_read_dt(&cfg->voltage_ch, &v_seq) == 0) {
  		v_raw = (int32_t)v_buf;
  	} else {
  		LOG_WRN("ch%u: voltage ADC read failed", cfg->channel_index);
  	}

  	/* Current — subtract hardware zero offset before publishing */
  	uint16_t i_buf;
  	int32_t i_raw = 0;
  	struct adc_sequence i_seq = {
  		.buffer = &i_buf,
  		.buffer_size = sizeof(i_buf),
  	};
  	adc_sequence_init_dt(&cfg->current_ch, &i_seq);
  	if (adc_read_dt(&cfg->current_ch, &i_seq) == 0) {
  		i_raw = (int32_t)i_buf - cfg->current_zero_offset;
  	} else {
  		LOG_WRN("ch%u: current ADC read failed", cfg->channel_index);
  	}

  	if (data->meas) {
  		vc_channel_buffer_publish_voltage(data->meas, v_raw);
  		vc_channel_buffer_publish_current(data->meas, i_raw);
  	}

  	if (data->meas_cb) {
  		data->meas_cb(cfg->channel_index, data->meas_cb_user_data);
  	}
  }

  static void lvb_vc_timer_fn(struct k_timer *timer)
  {
  	struct lvb_vc_data *data =
  		CONTAINER_OF(timer, struct lvb_vc_data, sample_timer);
  	k_work_submit_to_queue(&lvb_vc_workq, &data->sample_work);
  }

  /* ---- vc_channel_api ---- */

  static int lvb_vc_set_output(const struct device *dev, uint16_t code)
  {
  	ARG_UNUSED(dev);
  	ARG_UNUSED(code);
  	/* No DAC on LVB. vc_channel.c guards this behind CH_CAP_RAW_OUTPUT_DRIVE. */
  	return -ENOTSUP;
  }

  static int lvb_vc_set_enable(const struct device *dev, bool enable)
  {
  	const struct lvb_vc_config *cfg = dev->config;

  	return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
  }

  static int lvb_vc_start_sampling(const struct device *dev)
  {
  	struct lvb_vc_data *data = dev->data;
  	const struct lvb_vc_config *cfg = dev->config;

  	data->sampling_active = true;
  	k_timer_start(&data->sample_timer, K_NO_WAIT,
  		      K_MSEC(cfg->sample_rate_ms));
  	return 0;
  }

  static int lvb_vc_stop_sampling(const struct device *dev)
  {
  	struct lvb_vc_data *data = dev->data;

  	data->sampling_active = false;
  	k_timer_stop(&data->sample_timer);
  	return 0;
  }

  static uint16_t lvb_vc_get_capabilities(const struct device *dev)
  {
  	const struct lvb_vc_config *cfg = dev->config;

  	return cfg->capabilities;
  }

  static int lvb_vc_set_meas_callback(const struct device *dev,
  				     vc_meas_ready_cb_t cb, void *user_data)
  {
  	struct lvb_vc_data *data = dev->data;

  	data->meas_cb = cb;
  	data->meas_cb_user_data = user_data;
  	return 0;
  }

  static const struct vc_channel_api lvb_vc_api = {
  	.set_output       = lvb_vc_set_output,
  	.set_enable       = lvb_vc_set_enable,
  	.start_sampling   = lvb_vc_start_sampling,
  	.stop_sampling    = lvb_vc_stop_sampling,
  	.get_capabilities = lvb_vc_get_capabilities,
  	.set_meas_callback = lvb_vc_set_meas_callback,
  };

  /* ---- Init ---- */

  static int lvb_vc_init(const struct device *dev)
  {
  	struct lvb_vc_data *data = dev->data;
  	const struct lvb_vc_config *cfg = dev->config;

  	data->dev = dev;
  	data->meas = VC_CHANNEL_BUFFER_PTR(cfg->channel_index);
  	data->meas->channel_id = cfg->channel_index;

  	k_work_init(&data->sample_work, lvb_vc_sample_work_fn);
  	k_timer_init(&data->sample_timer, lvb_vc_timer_fn, NULL);

  	if (!gpio_is_ready_dt(&cfg->enable)) {
  		LOG_ERR("ch%u: enable GPIO not ready", cfg->channel_index);
  		return -ENODEV;
  	}
  	int ret = gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);

  	if (ret < 0) {
  		LOG_ERR("ch%u: GPIO configure failed: %d", cfg->channel_index, ret);
  		return ret;
  	}

  	if (!adc_is_ready_dt(&cfg->voltage_ch)) {
  		LOG_ERR("ch%u: voltage ADC not ready", cfg->channel_index);
  		return -ENODEV;
  	}
  	ret = adc_channel_setup_dt(&cfg->voltage_ch);
  	if (ret < 0) {
  		LOG_ERR("ch%u: voltage ADC setup failed: %d", cfg->channel_index, ret);
  		return ret;
  	}

  	if (!adc_is_ready_dt(&cfg->current_ch)) {
  		LOG_ERR("ch%u: current ADC not ready", cfg->channel_index);
  		return -ENODEV;
  	}
  	ret = adc_channel_setup_dt(&cfg->current_ch);
  	if (ret < 0) {
  		LOG_ERR("ch%u: current ADC setup failed: %d", cfg->channel_index, ret);
  		return ret;
  	}

  	lvb_vc_ensure_workq();

  	LOG_INF("ch%u (%s) ready, zero_offset=%d", cfg->channel_index,
  		dev->name, cfg->current_zero_offset);
  	return 0;
  }

  /* ---- Per-instance expansion ---- */

  #define LVB_VC_CHANNEL_DEFINE(n)                                                \
  	VC_CHANNEL_BUFFER_DEFINE(n);                                                \
                                                                                  \
  	static struct lvb_vc_data lvb_vc_data_##n;                                 \
                                                                                  \
  	static const struct lvb_vc_config lvb_vc_config_##n = {                    \
  		.enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios),                  \
  		.voltage_ch = ADC_DT_SPEC_INST_GET_BY_NAME(n, voltage),            \
  		.current_ch = ADC_DT_SPEC_INST_GET_BY_NAME(n, current),            \
  		.current_zero_offset =                                              \
  			(int16_t)DT_INST_PROP(n, current_zero_offset_raw),         \
  		.capabilities = DT_INST_PROP(n, capabilities),                     \
  		.channel_index = (uint8_t)DT_INST_REG_ADDR(n),                    \
  		.sample_rate_ms = DT_INST_PROP(n, sample_rate_ms),                 \
  	};                                                                          \
                                                                                  \
  	DEVICE_DT_INST_DEFINE(n, lvb_vc_init, NULL,                                \
  			      &lvb_vc_data_##n, &lvb_vc_config_##n,                \
  			      POST_KERNEL, CONFIG_ADC_INIT_PRIORITY + 1,            \
  			      &lvb_vc_api);

  DT_INST_FOREACH_STATUS_OKAY(LVB_VC_CHANNEL_DEFINE)
  ```

  **Design notes:**
  - `current_zero_offset` is subtracted in the driver (hardware zero) so the calibration layer provides gain-only refinement. `measured_current_calib_b` defaults to 0 in NVS; the per-channel offset lives in DTS.
  - Sampling uses `k_timer` → `k_work` on a shared `lvb_vc_workq`. All 12 work items run sequentially; each `adc_read_dt()` call is synchronous and completes in <5 μs per STM32F103 12-bit SAR at 12 MHz ADC clock. Total scan time <300 μs — negligible vs 150 ms period.
  - `set_output()` returns `-ENOTSUP`. `vc_channel.c` never calls it because `CH_CAP_RAW_OUTPUT_DRIVE` is not in the declared capabilities; the guard at line 112 of `vc_channel.c` checks `api->set_output` for non-null before calling, but the capability guard earlier prevents reaching that path.

- [ ] **Step 7: Commit driver and wiring**

  ```bash
  git add dts/bindings/voltage_control/jianwei,lvb-vc-channel.yaml \
          drivers/voltage_control/lvb_vc_channel/ \
          drivers/voltage_control/Kconfig \
          drivers/voltage_control/CMakeLists.txt
  git commit -m "feat(driver): add lvb_vc_channel driver for 12-ch fixed-voltage board"
  ```

---

### Task 3: jw_lvb board definition and lvb_controller application

Create the board support files and the application. Both are needed for the first real build test.

**Files:**
- Create: `boards/jianwei/jw_lvb/board.yml`
- Create: `boards/jianwei/jw_lvb/Kconfig.jw_lvb`
- Create: `boards/jianwei/jw_lvb/Kconfig.defconfig`
- Create: `boards/jianwei/jw_lvb/jw_lvb_defconfig`
- Create: `boards/jianwei/jw_lvb/board.cmake`
- Create: `boards/jianwei/jw_lvb/jw_lvb.dts`
- Create: `applications/lvb_controller/CMakeLists.txt`
- Create: `applications/lvb_controller/prj.conf`
- Create: `applications/lvb_controller/src/main.c`

**Interfaces:**
- Consumes: `jianwei,lvb-vc-channel` binding (Task 2), `CONFIG_LVB_VC_CHANNEL` (Task 2), `CONFIG_VC_VARIANT_ID` (Task 1).
- Produces: `west build -b jw_lvb applications/lvb_controller` succeeds.

- [ ] **Step 1: Create board.yml**

  Create `boards/jianwei/jw_lvb/board.yml`:

  ```yaml
  board:
    name: jw_lvb
    vendor: jianwei
    socs:
      - name: stm32f103xe
  ```

- [ ] **Step 2: Create Kconfig.jw_lvb**

  Create `boards/jianwei/jw_lvb/Kconfig.jw_lvb`:

  ```kconfig
  config BOARD_JW_LVB
  	bool "Jianwei Low Voltage Board"
  	select SOC_STM32F103XE
  ```

- [ ] **Step 3: Create Kconfig.defconfig**

  Create `boards/jianwei/jw_lvb/Kconfig.defconfig`:

  ```kconfig
  if BOARD_JW_LVB

  config BOARD
  	default "jw_lvb"

  config LVB_VC_CHANNEL
  	default y

  endif # BOARD_JW_LVB
  ```

- [ ] **Step 4: Create jw_lvb_defconfig**

  Create `boards/jianwei/jw_lvb/jw_lvb_defconfig`:

  ```
  CONFIG_CLOCK_CONTROL=y
  CONFIG_PINCTRL=y
  CONFIG_GPIO=y
  CONFIG_SERIAL=y
  CONFIG_UART_INTERRUPT_DRIVEN=y
  CONFIG_ADC=y
  ```

- [ ] **Step 5: Create board.cmake**

  Create `boards/jianwei/jw_lvb/board.cmake`:

  ```cmake
  board_runner_args(stm32flash "--baud-rate=115200" "--start-addr=0x08000000")
  board_runner_args(jlink "--device=STM32F103ZE" "--speed=4000")

  include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
  include(${ZEPHYR_BASE}/boards/common/stm32flash.board.cmake)
  ```

- [ ] **Step 6: Create jw_lvb.dts**

  **ADC channel mapping** (from old firmware `io-channels` array, corrected for sw bug):

  | Ch  | Label | Enable GPIO   | V ADC     | I ADC     | Zero offset |
  |-----|-------|--------------|-----------|-----------|-------------|
  | 0   | CH1   | GPIOG 1      | adc1 / 7  | adc1 / 6  | 1822        |
  | 1   | CH2   | GPIOE 8      | adc1 / 15 | adc1 / 14 | 1682        |
  | 2   | CH3   | GPIOF 14     | adc1 / 5  | adc1 / 4  | 1640        |
  | 3   | CH4   | GPIOE 11     | adc1 / 8  | adc1 / 9  | 1730        |
  | 4   | CH5   | GPIOE 6      | adc1 / 11 | adc1 / 10 | 1715        |
  | 5   | CH6   | GPIOA 12     | adc1 / 3  | adc1 / 2  | 1707        |
  | 6   | CH7   | GPIOE 5      | adc3 / 4  | adc3 / 5  | 1812        |
  | 7   | CH8   | GPIOE 2      | adc1 / 1  | adc1 / 13 | 1717        |
  | 8   | CH9   | GPIOE 4      | adc3 / 6  | adc3 / 7  | 1670        |
  | 9   | CH10  | GPIOE 3      | adc1 / 12 | adc3 / 8  | 1662        |
  | 10  | CH11  | GPIOE 0 *    | adc3 / 9* | adc3 / 10*| 1696 *      |
  | 11  | CH12  | GPIOE 1 *    | adc3 / 11*| adc1 / 0* | 1696 *      |

  `*` = placeholder — verify against hardware schematic. `adc3/9`=PF3, `adc3/10`=PF4, `adc3/11`=PF5, `adc1/0`=PA0 on STM32F103ZE. Zephyr pinctrl alias names: `adc3_in9_pf3`, `adc3_in10_pf4`, `adc3_in11_pf5`, `adc1_in0_pa0` — confirm these exist in the Zephyr `stm32f103z(c-d-e)tx-pinctrl.dtsi`.

  Create `boards/jianwei/jw_lvb/jw_lvb.dts`:

  ```dts
  /*
   * Copyright (c) 2026 Jianwei
   * SPDX-License-Identifier: Apache-2.0
   */

  /dts-v1/;

  #include <st/f1/stm32f103Xe.dtsi>
  #include <st/f1/stm32f103z(c-d-e)tx-pinctrl.dtsi>
  #include <zephyr/dt-bindings/gpio/gpio.h>
  #include <dt-bindings/voltage_control/capabilities.h>

  / {
  	model = "Jianwei Low Voltage Board";
  	compatible = "jianwei,jw-lvb";

  	chosen {
  		zephyr,console    = &usart2;
  		zephyr,shell-uart = &usart2;
  		zephyr,sram       = &sram0;
  		zephyr,flash      = &flash0;
  	};

  	vc_controller: vc-controller {
  		compatible = "jianwei,vc-controller";
  		#address-cells = <1>;
  		#size-cells = <0>;
  		status = "okay";

  		vc_ch0: channel@0 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <0>;
  			label = "CH1";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpiog 1 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 7>, <&adc1 6>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1822>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch1: channel@1 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <1>;
  			label = "CH2";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 8 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 15>, <&adc1 14>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1682>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch2: channel@2 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <2>;
  			label = "CH3";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpiof 14 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 5>, <&adc1 4>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1640>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch3: channel@3 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <3>;
  			label = "CH4";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 11 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 8>, <&adc1 9>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1730>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch4: channel@4 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <4>;
  			label = "CH5";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 6 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 11>, <&adc1 10>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1715>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch5: channel@5 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <5>;
  			label = "CH6";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioa 12 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 3>, <&adc1 2>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1707>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch6: channel@6 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <6>;
  			label = "CH7";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 5 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc3 4>, <&adc3 5>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1812>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch7: channel@7 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <7>;
  			label = "CH8";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 2 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 1>, <&adc1 13>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1717>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch8: channel@8 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <8>;
  			label = "CH9";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 4 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc3 6>, <&adc3 7>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1670>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch9: channel@9 {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <9>;
  			label = "CH10";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 3 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc1 12>, <&adc3 8>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1662>;
  			sample-rate-ms = <150>;
  		};

  		/* CH11-12: pin assignments MUST be verified against hardware schematic.
  		 * Proposed: enable=GPIOE.0/GPIOE.1, ADC3 ch9-11 (PF3-PF5), ADC1 ch0 (PA0).
  		 * Confirm adc3_in9_pf3, adc3_in10_pf4, adc3_in11_pf5, adc1_in0_pa0
  		 * exist in stm32f103z(c-d-e)tx-pinctrl.dtsi. Update current-zero-offset-raw
  		 * from board-specific calibration measurement. */
  		vc_ch10: channel@a {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <10>;
  			label = "CH11";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 0 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc3 9>, <&adc3 10>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1696>;
  			sample-rate-ms = <150>;
  		};

  		vc_ch11: channel@b {
  			compatible = "jianwei,lvb-vc-channel";
  			reg = <11>;
  			label = "CH12";
  			capabilities = <(CH_CAP_OUTPUT_ENABLE |
  					 CH_CAP_VOLTAGE_MEASUREMENT |
  					 CH_CAP_CURRENT_MEASUREMENT)>;
  			enable-gpios = <&gpioe 1 GPIO_ACTIVE_HIGH>;
  			io-channels = <&adc3 11>, <&adc1 0>;
  			io-channel-names = "voltage", "current";
  			current-zero-offset-raw = <1696>;
  			sample-rate-ms = <150>;
  		};
  	};
  };

  /* Clocks: HSE 8 MHz × 9 PLL = 72 MHz */
  &clk_hse {
  	clock-frequency = <DT_FREQ_M(8)>;
  	status = "okay";
  };

  &pll {
  	mul = <9>;
  	clocks = <&clk_hse>;
  	status = "okay";
  };

  &rcc {
  	clocks = <&pll>;
  	clock-frequency = <DT_FREQ_M(72)>;
  	ahb-prescaler = <1>;
  	apb1-prescaler = <2>;
  	apb2-prescaler = <1>;
  };

  /* Watchdog */
  &iwdg {
  	status = "okay";
  };

  /* USART1: Modbus RTU, 115200 baud, RS-485 DE on GPIOG.3 */
  &usart1 {
  	pinctrl-0 = <&usart1_tx_pa9 &usart1_rx_pa10>;
  	pinctrl-names = "default";
  	current-speed = <115200>;
  	status = "okay";

  	modbus0 {
  		compatible = "zephyr,modbus-serial";
  		status = "okay";
  		de-gpios = <&gpiog 3 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
  	};
  };

  /* USART2: console / shell */
  &usart2 {
  	pinctrl-0 = <&usart2_tx_pa2 &usart2_rx_pa3>;
  	pinctrl-names = "default";
  	current-speed = <115200>;
  	status = "okay";
  };

  /* ADC1: 15 channels across PA, PB, PC pins */
  &adc1 {
  	#address-cells = <1>;
  	#size-cells = <0>;
  	pinctrl-0 = <
  		&adc1_in0_pa0     /* CH12 current (placeholder) */
  		&adc1_in1_pa1     /* CH8 voltage */
  		&adc1_in2_pa2     /* CH6 current */
  		&adc1_in3_pa3     /* CH6 voltage */
  		&adc1_in4_pa4     /* CH3 current */
  		&adc1_in5_pa5     /* CH3 voltage */
  		&adc1_in6_pa6     /* CH1 current */
  		&adc1_in7_pa7     /* CH1 voltage */
  		&adc1_in8_pb0     /* CH4 voltage */
  		&adc1_in9_pb1     /* CH4 current */
  		&adc1_in10_pc0    /* CH5 current */
  		&adc1_in11_pc1    /* CH5 voltage */
  		&adc1_in12_pc2    /* CH10 voltage */
  		&adc1_in13_pc3    /* CH8 current */
  		&adc1_in14_pc4    /* CH2 current */
  		&adc1_in15_pc5    /* CH2 voltage */
  	>;
  	pinctrl-names = "default";
  	status = "okay";
  	st,adc-prescaler = <6>;

  	#define ADC1_CH(n) channel@n { reg = <n>; \
  		zephyr,gain = "ADC_GAIN_1"; \
  		zephyr,reference = "ADC_REF_INTERNAL"; \
  		zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; \
  		zephyr,resolution = <12>; };

  	ADC1_CH(0)  ADC1_CH(1)  ADC1_CH(2)  ADC1_CH(3)
  	ADC1_CH(4)  ADC1_CH(5)  ADC1_CH(6)  ADC1_CH(7)
  	ADC1_CH(8)  ADC1_CH(9)  ADC1_CH(10) ADC1_CH(11)
  	ADC1_CH(12) ADC1_CH(13) ADC1_CH(14) ADC1_CH(15)

  	#undef ADC1_CH
  };

  /* ADC3: 8 channels on PF pins (ch4-8 known, ch9-11 placeholders) */
  &adc3 {
  	#address-cells = <1>;
  	#size-cells = <0>;
  	pinctrl-0 = <
  		&adc3_in4_pf6     /* CH7 voltage */
  		&adc3_in5_pf7     /* CH7 current */
  		&adc3_in6_pf8     /* CH9 voltage */
  		&adc3_in7_pf9     /* CH9 current */
  		&adc3_in8_pf10    /* CH10 current */
  		&adc3_in9_pf3     /* CH11 voltage (placeholder) */
  		&adc3_in10_pf4    /* CH11 current (placeholder) */
  		&adc3_in11_pf5    /* CH12 voltage (placeholder) */
  	>;
  	pinctrl-names = "default";
  	status = "okay";
  	st,adc-prescaler = <6>;

  	#define ADC3_CH(n) channel@n { reg = <n>; \
  		zephyr,gain = "ADC_GAIN_1"; \
  		zephyr,reference = "ADC_REF_INTERNAL"; \
  		zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>; \
  		zephyr,resolution = <12>; };

  	ADC3_CH(4)  ADC3_CH(5)  ADC3_CH(6)  ADC3_CH(7)
  	ADC3_CH(8)  ADC3_CH(9)  ADC3_CH(10) ADC3_CH(11)

  	#undef ADC3_CH
  };

  /* Flash partition for NVS settings (8 KB at top of 512 KB flash) */
  &flash0 {
  	partitions {
  		compatible = "fixed-partitions";
  		#address-cells = <1>;
  		#size-cells = <1>;

  		storage_partition: partition@7e000 {
  			label = "storage";
  			reg = <0x0007e000 0x00002000>;
  		};
  	};
  };
  ```

  **Notes on DTS macro approach for ADC channels:** The `#define ADC1_CH(n)` / `#undef ADC1_CH` pattern keeps the DTS concise. If the Zephyr DTS preprocessor doesn't accept `#define`/`#undef` inside DTS files, expand each channel node longhand (copy the `channel@N { ... }` block for each of the 16 channels). The Z-macro approach works because DTS files are preprocessed by cpp before DTS parsing.

- [ ] **Step 7: Create application CMakeLists.txt**

  Create `applications/lvb_controller/CMakeLists.txt`:

  ```cmake
  # SPDX-License-Identifier: Apache-2.0

  cmake_minimum_required(VERSION 3.20.0)
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(lvb_controller)

  target_sources(app PRIVATE src/main.c)
  ```

- [ ] **Step 8: Create application prj.conf**

  Create `applications/lvb_controller/prj.conf`:

  ```
  CONFIG_GPIO=y
  CONFIG_SERIAL=y
  CONFIG_UART_INTERRUPT_DRIVEN=y
  CONFIG_CONSOLE=y
  CONFIG_UART_CONSOLE=y
  CONFIG_PRINTK=y
  CONFIG_LOG=y
  CONFIG_LOG_MODE_IMMEDIATE=y
  CONFIG_LOG_CMDS=y

  CONFIG_MODBUS=y
  CONFIG_MODBUS_ROLE_SERVER=y
  CONFIG_UI_MODBUS_RTU=y
  CONFIG_MODBUS_LOG_LEVEL_WRN=y

  CONFIG_VC_SHELL=y
  CONFIG_MODBUS_ADAPTER_SHELL=y
  # No SYS_STATUS_SHELL — LVB has no environmental sensor

  CONFIG_ADC=y
  CONFIG_ADC_STM32=y

  CONFIG_LVB_VC_CHANNEL=y
  CONFIG_VC_CHANNEL_CONTROLLER=y
  CONFIG_VC_RUNTIME=y
  CONFIG_VC_SETTINGS_PERSISTENCE=y

  # LVB variant ID
  CONFIG_VC_VARIANT_ID=2

  # Calibration defaults for LVB measurement channels
  # (output calibration k/b unused — no DAC, kept at defaults)
  CONFIG_VC_DEFAULT_MEASURED_V_CAL_K=28125
  CONFIG_VC_DEFAULT_MEASURED_I_CAL_K=10000

  CONFIG_REBOOT=y
  CONFIG_SYS_RESET=y
  ```

- [ ] **Step 9: Create application main.c**

  The LVB application entry is identical to `applications/hvb_controller/src/main.c` — the framework is the same. Create `applications/lvb_controller/src/main.c`:

  ```c
  /*
   * Copyright (c) 2026 Jianwei
   * SPDX-License-Identifier: Apache-2.0
   */

  #include <zephyr/kernel.h>
  #include <zephyr/sys/printk.h>

  #include "voltage_control/vc.h"

  #if IS_ENABLED(CONFIG_UI_MODBUS_RTU)
  #include "modbus_adapter/modbus_adapter.h"
  #endif

  #if IS_ENABLED(CONFIG_VC_SHELL)
  #include "voltage_control/vc_shell.h"
  #endif

  int main(void)
  {
  	struct vc_ctx *ctx = vc_init();

  	if (!ctx) {
  		printk("Failed to init vc\n");
  		return 0;
  	}

  	int ret = vc_ctx_start(ctx);

  	if (ret != VC_OK) {
  		printk("Failed to start vc: %d\n", ret);
  		return 0;
  	}

  #if IS_ENABLED(CONFIG_UI_MODBUS_RTU)
  	ret = modbus_adapter_init();
  	if (ret < 0) {
  		return 0;
  	}
  #endif

  #if IS_ENABLED(CONFIG_VC_SHELL)
  	vc_shell_init();
  #endif

  	printk("lvb_controller ready\n");
  	return 0;
  }
  ```

- [ ] **Step 10: Build and verify**

  ```bash
  west build -b jw_lvb applications/lvb_controller -p
  ```

  Expected: build completes, `build/zephyr/zephyr.elf` produced with no errors.

  If ADC3 pinctrl aliases for ch9-11 (PF3/PF4/PF5) are missing from the Zephyr STM32F103 dtsi, the build will fail with "undefined reference to `adc3_in9_pf3`". Resolution: check `${ZEPHYR_BASE}/dts/arm/st/f1/stm32f103z(c-d-e)tx-pinctrl.dtsi` for the available ADC3 aliases and update `jw_lvb.dts` ch10/ch11 to use pins that DO have defined aliases.

- [ ] **Step 11: Commit**

  ```bash
  git add boards/jianwei/jw_lvb/ applications/lvb_controller/
  git commit -m "feat(board,app): add jw_lvb board definition and lvb_controller application"
  ```

---

### Task 4: vc_channel_no_dac capability test

Verify that the `vc_channel` state machine correctly handles channels that lack `CH_CAP_RAW_OUTPUT_DRIVE`: DAC-specific operations are rejected, while GPIO enable, V/I measurement, and protection still work.

**Files:**
- Modify: `tests/voltage_control/vc_channel_state/src/main.c`

**Interfaces:**
- Consumes: `vc_channel_init`, `vc_channel_output_action`, `vc_channel_cal_set_output_enable`, `vc_channel_consume_voltage`, `vc_channel_consume_current`, `vc_channel_get_snapshot` — all already imported by the existing test file.

- [ ] **Step 1: Run existing tests to confirm baseline**

  ```bash
  west build -b native_posix tests/voltage_control/vc_channel_state -p && \
  ./build/zephyr/zephyr.exe
  ```

  Expected: all `vc_channel_state` tests PASS.

- [ ] **Step 2: Add LVB_CAPS constant and no-DAC test suite**

  In `tests/voltage_control/vc_channel_state/src/main.c`, after the existing `#define FULL_CAPS` line, add:

  ```c
  #define LVB_CAPS (CH_CAP_OUTPUT_ENABLE | \
  		  CH_CAP_VOLTAGE_MEASUREMENT | \
  		  CH_CAP_CURRENT_MEASUREMENT)
  ```

  Then, after the closing brace of the last existing `ZTEST_SUITE` registration, append the following new suite (before any `#endif` or end of file):

  ```c
  /* ---- no-DAC capability suite (LVB: no CH_CAP_RAW_OUTPUT_DRIVE) ---- */

  static void before_each_no_dac(void *fixture)
  {
  	ARG_UNUSED(fixture);
  	wake_count = 0;
  	vc_channel_init(&ch, NULL, 0, LVB_CAPS, NULL, test_wake_fn, &ch);
  }

  ZTEST_SUITE(vc_channel_no_dac, NULL, NULL, before_each_no_dac, NULL, NULL);

  ZTEST(vc_channel_no_dac, test_cal_output_enable_rejected)
  {
  	/* vc_channel_cal_set_output_enable must reject when RAW_OUTPUT_DRIVE absent */
  	enum vc_status st = vc_channel_cal_set_output_enable(&ch, true);

  	zassert_equal(st, VC_ERR_UNSUPPORTED_CAPABILITY,
  		      "expected VC_ERR_UNSUPPORTED_CAPABILITY, got %d", st);
  }

  ZTEST(vc_channel_no_dac, test_output_enable_action_accepted)
  {
  	/* ENABLE action must still work (GPIO path, no DAC write needed) */
  	struct vc_channel_config cfg = vc_channel_default_config();

  	/* target_voltage > 0 required for the enabled path (checked in SMF) */
  	cfg.configured_target_voltage = 12000;
  	zassert_equal(vc_channel_set_config(&ch, &cfg), VC_OK);

  	enum vc_status st = vc_channel_output_action(&ch, VC_OUTPUT_ACTION_ENABLE);

  	zassert_equal(st, VC_OK, "ENABLE should succeed without DAC, got %d", st);
  	zassert_true(ch.output_enabled);
  }

  ZTEST(vc_channel_no_dac, test_disable_immediate_action_accepted)
  {
  	/* DISABLE_IMMEDIATE must always be accepted */
  	enum vc_status st =
  		vc_channel_output_action(&ch, VC_OUTPUT_ACTION_DISABLE_IMMEDIATE);

  	zassert_equal(st, VC_OK, "DISABLE_IMMEDIATE failed: %d", st);
  	zassert_false(ch.output_enabled);
  }

  ZTEST(vc_channel_no_dac, test_voltage_measurement_consumed)
  {
  	/* V measurement path unaffected by absent RAW_OUTPUT_DRIVE */
  	vc_channel_consume_voltage(&ch, 3500);

  	struct vc_channel_snapshot snap;

  	vc_channel_get_snapshot(&ch, &snap);
  	/* With default calib k=10000 b=0: measured_voltage = 3500 */
  	zassert_equal(snap.measured_voltage, 3500,
  		      "measured_voltage=%d, expected 3500", snap.measured_voltage);
  }

  ZTEST(vc_channel_no_dac, test_current_measurement_consumed)
  {
  	/* I measurement path unaffected by absent RAW_OUTPUT_DRIVE */
  	vc_channel_consume_current(&ch, 200);

  	struct vc_channel_snapshot snap;

  	vc_channel_get_snapshot(&ch, &snap);
  	/* With default calib k=10000 b=0: measured_current = 200 */
  	zassert_equal(snap.measured_current, 200,
  		      "measured_current=%d, expected 200", snap.measured_current);
  }

  ZTEST(vc_channel_no_dac, test_capabilities_reported_correctly)
  {
  	struct vc_channel_snapshot snap;

  	vc_channel_get_snapshot(&ch, &snap);
  	zassert_equal(snap.channel_capability_flags, LVB_CAPS,
  		      "caps=0x%04x, expected 0x%04x",
  		      snap.channel_capability_flags, LVB_CAPS);
  }
  ```

- [ ] **Step 3: Run the updated test suite**

  ```bash
  west build -b native_posix tests/voltage_control/vc_channel_state -p && \
  ./build/zephyr/zephyr.exe
  ```

  Expected output includes both suites passing:
  ```
  Running TESTSUITE vc_channel_state
  ...
  Running TESTSUITE vc_channel_no_dac
  test_cal_output_enable_rejected        PASS
  test_output_enable_action_accepted     PASS
  test_disable_immediate_action_accepted PASS
  test_voltage_measurement_consumed      PASS
  test_current_measurement_consumed      PASS
  test_capabilities_reported_correctly   PASS
  PROJECT EXECUTION SUCCESSFUL
  ```

- [ ] **Step 4: Commit**

  ```bash
  git add tests/voltage_control/vc_channel_state/src/main.c
  git commit -m "test(vc_channel): add no-DAC capability suite for LVB channel profile"
  ```

---

## Self-Review

### Spec coverage

| Requirement | Task |
|-------------|------|
| 12-channel isolated 12 V controller | Task 3 (12 DTS channel nodes) |
| Fixed output (no DAC), on/off via GPIO | Task 2 driver `set_enable`, `set_output` returns ENOTSUP |
| Voltage + current measurement | Task 2 driver sampling, Task 4 test |
| Calibration parameters from old firmware as defaults | Task 3 `current-zero-offset-raw` per channel in DTS; voltage k=28125 in prj.conf |
| New vc_channel driver (`lvb_vc_channel`) under current framework | Task 2 |
| Respect `vc_channel_api.h` | Task 2 driver struct + vtable |
| Respect current architecture (no modifying shared libs) | Global constraint; all tasks |
| New board `jw_lvb` (STM32F103ZE) | Task 3 board files |
| ADC interrupt-driven sampling investigation | See note below |
| New application | Task 3 `applications/lvb_controller/` |
| Variant ID = 2 for LVB | Task 1 + Task 3 prj.conf |

**ADC interrupt-driven sampling note:** The old firmware used synchronous `adc_read()` with a k_timer/k_work pattern. The STM32F103 internal 12-bit SAR ADC completes in ~1.17 μs per channel at 12 MHz ADC clock. 24 channels × 1.17 μs = 28 μs total scan time per 150 ms cycle. This is trivially fast for synchronous reads in a workqueue thread — interrupt-driven (`adc_read_async`) adds complexity with no practical benefit. The plan uses synchronous reads. If hardware reveals an ADC contention issue across channels on the same peripheral (e.g., ADC1 called from 9 concurrent work items), add a per-ADC-peripheral mutex in `lvb_vc_channel.c`; the current design's shared workqueue serializes them naturally.

### Placeholder scan

- Channels 10–11 DTS pin assignments are explicitly labelled `(placeholder)` in comments and Step 6 instructions call out the required verification. These are hardware facts, not design gaps.
- `current-zero-offset-raw = <1696>` for ch10/ch11 is an average-of-known value with explicit "update from board-specific calibration measurement" instruction in Global Constraints.

### Type consistency

- `VC_VARIANT_ID` → `CONFIG_VC_VARIANT_ID` (int, same use site `vc_controller.c:315`).
- `LVB_CAPS` defined in test file matches DTS capabilities bitmask (same header `capabilities.h`).
- `ADC_DT_SPEC_INST_GET_BY_NAME(n, voltage)` matches `io-channel-names = "voltage", "current"` in DTS.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-30-lvb-board-port.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?

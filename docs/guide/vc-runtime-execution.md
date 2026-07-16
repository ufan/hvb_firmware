# VC Runtime & Channel Execution — Jianwei Voltage-Control Board

## Architecture Overview

The Voltage Control (VC) subsystem has a layered, single-writer architecture built on Zephyr RTOS primitives. All mutable channel state is owned by a single **runtime worker thread** — no locks are needed within the domain logic.

```
┌──────────────────────────────────────────────┐
│  Frontends (Modbus Adapter, Shell)           │
│  Read/write registers via reg_catalog         │
├──────────────────────────────────────────────┤
│  vc_runtime (vc_runtime.c)                    │
│  - Worker thread (single writer)              │
│  - Command queue (k_msgq) + dispatch          │
│  - Register catalog read/write vtable         │
├──────────────────────────────────────────────┤
│  vc_controller (vc_controller.c)              │
│  - Singleton aggregating all channels         │
│  - Orchestrates operating mode transitions    │
│  - Cross-channel calibration safety           │
│  - DTS-based channel construction             │
│  - Storage/persistence management             │
├──────────────────────────────────────────────┤
│  vc_channel (vc_channel.c)                    │
│  - SMF state machine per channel              │
│  - Applies calibration coefficients           │
│  - Ramps voltage targets                      │
│  - Evaluates current protection               │
│  - Calls apply_hw() → driver vtable           │
│  - Publishes read-only snapshots              │
├──────────────────────────────────────────────┤
│  vc_channel_api (vc_channel_api.h)            │
│  - Hardware driver vtable                     │
│  - Spinlock-protected measurement buffers     │
├──────────────────────────────────────────────┤
│  hvb_vc_channel driver (HW-specific)          │
│  - DAC + ADC + GPIO hardware                  │
│  - Async ADC sampling loop                    │
│  - Publishes to shared vc_channel_buffer      │
│  - Invokes measurement-ready callback         │
└──────────────────────────────────────────────┘
```

### Key Architectural Principles

1. **Single-writer**: Only the runtime worker thread writes to `vc_channel` and `vc_controller` state. No domain mutex needed.

2. **Static composition**: Channels, measurement buffers, and register descriptors are all built at compile time from devicetree via `DT_FOREACH_CHILD_STATUS_OKAY` and `STRUCT_SECTION_ITERABLE`.

3. **Layered separation**: Policy (channel SMF) is separate from mechanism (HW driver). The driver knows nothing about ramps, protection, or calibration.

4. **ISR-safe measurement layer**: The `vc_channel_buffer` uses `k_spinlock` so HW drivers can publish from ISR or workqueue context. The runtime thread consumes on its own tick cadence.

### Source File Map

| Layer | Header | Implementation |
|-------|--------|----------------|
| Types & Constants | `include/voltage_control/vc_types.h` | — |
| Channel (state machine) | `include/voltage_control/vc_channel.h` | `lib/voltage_control/vc_channel.c` |
| Hardware Driver API | `include/voltage_control/vc_channel_api.h` | (vtable, no impl) |
| Controller (aggregator) | `include/voltage_control/vc_controller.h` | `lib/voltage_control/vc_controller.c` |
| Runtime (thread, commands) | `include/voltage_control/vc_runtime.h` | `lib/voltage_control/vc_runtime.c` |
| Facade (init/start/destroy) | `include/voltage_control/vc.h` | `lib/voltage_control/vc.c` |
| Shell | `include/voltage_control/vc_shell.h` | `lib/voltage_control/vc_shell.c` |
| Storage (NVS) | `include/voltage_control/vc_storage.h` | `lib/voltage_control/vc_storage_settings.c` |

---

## 1. VC Runtime Engine

The runtime engine (`vc_runtime.c`) provides the thread, command queue, and register catalog that together form the execution core of the VC subsystem.

### 1.1 Lifecycle

```
vc_init()                                              // vc.c:45
  → vc_runtime_create_static()                         // vc_runtime.c:1
      → vc_controller_init(wake_fn, wake_user_data)    // vc_controller.c:65
          builds channels[] from DTS
          builds meas_index[] from iterable section
          calls vc_channel_init() for each channel
      → auto-load from NVS (3-phase)
      → k_thread_create(vc_runtime_worker, ...)        // starts worker thread

vc_ctx_start(ctx)                                       // vc.c
  → vc_runtime_start_sampling()
      → vc_controller_start_sampling()
          → api->start_sampling(dev) on every channel   // begins async ADC loops
```

### 1.2 Worker Thread Loop

The runtime thread is the **single writer** to all channel and controller state. It runs at a configurable tick interval (default 100 ms, set by `CONFIG_VC_RUNTIME_TICK_INTERVAL_MS`).

```
vc_runtime_worker(runtime, ...):
    while (!stop_requested):
        k_sem_take(wake, timeout=TICK_INTERVAL_MS)

        // Drain command queue (non-blocking)
        while (k_msgq_get(&command_queue, &work, K_NO_WAIT) == 0):
            vc_runtime_dispatch_command(runtime, &work.command)
            k_sem_give(work.command.result_sem)    // unblock caller

        // Tick all channels
        vc_controller_tick(ctrl, dt_ms)             // vc_controller.c:137
```

- **Tick timeout** drives `vc_channel_run()` at the configured interval when no measurement events arrive sooner.
- The `wake` semaphore is also signaled by the HW driver's measurement callback, providing lower-latency response to new ADC data.

### 1.3 Command Submission

External callers (Modbus adapter, shell, register catalog) submit commands through a blocking request-reply pattern:

```
vc_runtime_submit_command(runtime, cmd, timeout)     // vc_runtime.h:66
    1. Populate result_sem and result in cmd
    2. k_msgq_put(&runtime->command_queue, &work, timeout)
    3. k_sem_give(&runtime->wake)                     // wake worker thread
    4. k_sem_take(cmd->result_sem, K_FOREVER)         // block until processed
    5. return *cmd->result
```

Convenience wrappers exist for common operations:

```c
// vc_runtime.h:70-90
enum vc_status vc_runtime_set_operating_mode(struct vc_runtime *runtime,
                                             enum vc_operating_mode mode,
                                             k_timeout_t timeout);
enum vc_status vc_runtime_set_system_field(struct vc_runtime *runtime,
                                           enum vc_config_field field,
                                           uint16_t value, k_timeout_t timeout);
enum vc_status vc_runtime_set_channel_field(struct vc_runtime *runtime,
                                            uint8_t channel,
                                            enum vc_config_field field,
                                            uint16_t value, k_timeout_t timeout);
enum vc_status vc_runtime_set_channel_cal_field(struct vc_runtime *runtime,
                                                uint8_t channel,
                                                enum vc_cal_field field,
                                                uint16_t value, k_timeout_t timeout);
```

### 1.4 Command Dispatch

The worker thread dispatches each command to the appropriate controller function (`vc_runtime.c`):

```
vc_runtime_dispatch_command():
    switch (cmd->type):
        SET_OPERATING_MODE    → vc_controller_set_operating_mode()
        OUTPUT_ACTION         → vc_controller_channel_output_action()
        FAULT_COMMAND         → vc_controller_channel_fault_command()
        CALIBRATION_*         → vc_controller_channel_cal_*()
        SET_SYSTEM_FIELD      → vc_controller_set_system_field()
        SET_CHANNEL_FIELD     → vc_controller_channel_set_field()
        SET_CHANNEL_CAL_FIELD → vc_controller_channel_set_cal_field()
        SYSTEM_PARAM_ACTION   → vc_controller_system_param_action()
        CHANNEL_PARAM_ACTION  → vc_controller_channel_param_action()
```

### 1.5 Register Catalog Integration

The runtime exposes read/write vtable functions (`vc_runtime.c:155`) that the register catalog (`lib/reg_store/`) calls. This is how Modbus reads and writes flow through to the domain state:

- **Read**: `vc_catalog_read()` copies from `vc_controller` and `vc_channel` state under `runtime->lock` mutex.
- **Write**: `vc_catalog_write()` builds a `vc_runtime_command`, submits it, blocks for result, on success writes the new value to the catalog's local copy.
- **Capability guard**: `vc_catalog_supported()` (`vc_runtime.c:112`) rejects reads/writes to registers the channel does not support (e.g., current measurement on a channel without `CH_CAP_CURRENT_MEASUREMENT`).

### 1.6 Measurement Pipeline

```
HW Driver (hvb_vc_channel.c:98)          Runtime Worker (vc_channel.c:335)
  ───────────────────────                ────────────────────────────
  adc_read_async()                       vc_channel_run(ch, dt_ms)
    → k_work_poll callback                 → vc_channel_buffer_read(ch->meas)
        → publish_voltage(buf,raw,ts)      → if new timestamp:
        → publish_current(buf,raw,ts)          → vc_channel_consume_voltage(raw)
        → meas_cb(ch, user_data)               → vc_channel_consume_current(raw)
            → runtime_wake()                   → apply calibration
                → k_sem_give(wake)         → vc_channel_tick_ramp(ch, dt_ms)
```

The worker thread checks timestamps in the measurement buffer to avoid re-consuming stale data. This decouples hardware timing (ADC at 10–100 SPS for ADS1232) from policy timing (runtime tick at 100 ms).

---

## 2. VC Channel State Machine

Each channel has an SMF-based state machine with 6 states, defined in `include/voltage_control/vc_channel.h:19` and implemented in `lib/voltage_control/vc_channel.c:15`.

### 2.1 State Enum

```c
enum vc_channel_smf_state {
    VC_CHANNEL_SMF_DISABLED_SAFE,       // Output off, safe state
    VC_CHANNEL_SMF_ENABLED_HOLDING,     // Output enabled, at target voltage
    VC_CHANNEL_SMF_RAMPING,             // Ramping between targets
    VC_CHANNEL_SMF_FAULT_LATCHED,       // Protection fault active
    VC_CHANNEL_SMF_RETRY_COOLDOWN,      // Cooldown before auto-retry
    VC_CHANNEL_SMF_CALIBRATION_OUTPUT,  // Calibration session active
};
```

All SMF states use `SMF_CREATE_STATE(NULL, NULL, NULL, NULL, NULL)` — transitions are driven **manually** by `set_smf_state()` calls rather than automatic SMF entry/run/exit callbacks. See `vc_channel.c:15-22`.

### 2.2 State Transition Diagram

```
                        vc_channel_init()
                             │
                             ▼
                    ┌─────────────────┐
        ┌──────────►│ DISABLED_SAFE   │◄──────────┐
        │           └────────┬────────┘           │
        │                    │                    │
        │     ENABLE (no     │ fault              │ fault
        │     active fault)  │ (current/hw/       │ clear
        │                    │  interlock)        │
        │                    ▼                    │
        │           ┌─────────────────┐           │
        │           │ FAULT_LATCHED   │───────────┘
        │           └────────┬────────┘           │
        │                    │                    │
        │                    │ auto-retry
        │                    │ delay expired
        │                    ▼                    │
        │           ┌─────────────────┐           │
        │           │ RETRY_COOLDOWN  │           │
        │           └────────┬────────┘           │
        │                    │                    │
        │                    │ retry attempt      │
        │     ┌──────────────┘                    │
        ▼     ▼                                   │
   ┌──────────────┐                               │
   │   RAMPING    │                               │
   └──────┬───────┘                               │
          │                                       │
          │ ramp complete (target reached)        │
          ▼                                       │
   ┌──────────────┐                               │
   │ENABLED_HOLDING│                              │
   └──────┬───────┘                               │
          │                                       │
          │ DISABLE (graceful/immediate)          │
          ▼                                       │
        ┌─────────────────┐                       │
        │ DISABLED_SAFE    │◄──────────────────────┘
        └─────────────────┘
              ▲         ▲
              │         │
       exit   │         │ enter
      cal     │         │ cal
              │         │
   ┌─────────────────────┐
   │ CALIBRATION_OUTPUT  │
   └─────────────────────┘
```

### 2.3 Transition Triggers

| Trigger | Source | From → To |
|---------|--------|-----------|
| `vc_channel_output_action(ENABLE)` | `vc_channel.c:530` | DISABLED_SAFE → RAMPING |
| `vc_channel_output_action(DISABLE_GRACEFUL)` | `vc_channel.c:538` | any → DISABLED_SAFE |
| `vc_channel_output_action(DISABLE_IMMEDIATE)` | `vc_channel.c:545` | any → DISABLED_SAFE |
| `tick_current_protection()` overcurrent | `vc_channel.c:239` | *(holding/ramping)* → FAULT_LATCHED |
| `vc_channel_consume_fault(hw/interlock)` | `vc_channel.c:611` | any → DISABLED_SAFE (via `force_safe_state`) |
| `vc_channel_fault_command(CLEAR_ACTIVE)` | `vc_channel.c:568` | FAULT_LATCHED → DISABLED_SAFE |
| Ramp complete (target reached) | `vc_channel.c:682` | RAMPING → ENABLED_HOLDING |
| Auto-retry attempt | *(not yet implemented)* | RETRY_COOLDOWN → RAMPING |
| `vc_channel_reset_calibration(true)` | `vc_channel.c:692` | any → CALIBRATION_OUTPUT |
| `vc_channel_reset_calibration(false)` | `vc_channel.c:711` | CALIBRATION_OUTPUT → DISABLED_SAFE |

### 2.4 Status Bits

Computed by `update_status_bits()` at `vc_channel.c:120`. These bits are visible through Modbus `STATUS_BITS` register and shell `vc status`.

| Bit | Name | Meaning |
|-----|------|---------|
| 0x0001 | Enabled | State is ENABLED_HOLDING or RAMPING, or non-zero target/measured voltage |
| 0x0002 | Output Active | `output_enabled == true` |
| 0x0004 | Ramping | State is RAMPING |
| 0x0008 | Faulted | State is FAULT_LATCHED or RETRY_COOLDOWN |
| 0x0010 | Has Fault History | `fault_history_cause != 0` |
| 0x0020 | Cooldown | State is RETRY_COOLDOWN |

### 2.5 Ramping Logic

Defined in `vc_channel_tick_ramp()` at `vc_channel.c:619`.

```
Given: target = config.configured_target_voltage
       current = ch->operational_target_voltage (current ramp position)

If current == target: set state → ENABLED_HOLDING and return.

direction = current < target ? UP : DOWN
step = direction == UP ? config.ramp_up_step : config.ramp_down_step
interval = direction == UP ? config.ramp_up_interval : config.ramp_down_interval

accum_ms += dt_ms
while accum_ms >= interval * 100 and current != target:
    accum_ms -= interval * 100
    current += direction == UP ? +step : -step
    clamp current to target

ch->operational_target_voltage = current
apply_hw(ch)                        // writes DAC via driver vtable
```

If `step == 0` or `interval == 0`, the ramp is skipped: `operational_target_voltage` jumps directly to the configured target.

### 2.6 Current Protection

Defined in `tick_current_protection()` at `vc_channel.c:239`.

```
Guard: active_fault_cause != 0  → skip (already faulted)
       ramping == true          → skip (no protection during ramp)
       protection_mode disabled  → skip
       measured_current > limit → trigger fault

On fault:
    fault_history_cause |= VC_FAULT_CURRENT
    if protection_mode == APPLY_OUTPUT_ACTION:
        active_fault_cause |= VC_FAULT_CURRENT
        last_fault_timestamp = uptime_ref
        apply_protection_action(ch, config.current_protection_output_action)
            FORCE_OUTPUT_ZERO  → operational_target = 0, output_enabled = false
            DISABLE_IMMEDIATE  → output_enabled = false, ramping = false,
                                 operational_target = 0, raw_dac = 0
            DISABLE_GRACEFUL   → output_enabled = false, operational_target = 0
        set_smf_state → FAULT_LATCHED
```

**Safe band**: When clearing a current fault, `is_safe_to_clear_active()` (`vc_channel.c:279`) checks that `measured_current <= limit * (100 - safe_band_pct) / 100`, preventing immediate re-trigger on a borderline reading.

### 2.7 Hardware Write (`apply_hw`)

Defined at `vc_channel.c:87`. This is the sole function that writes to hardware:

```c
static void apply_hw(struct vc_channel *ch) {
    if (ch->cal_output_enabled) {
        enable = true;
        code   = ch->raw_dac_readback;         // raw, no calibration applied
    } else if (ch->output_enabled) {
        enable = true;
        code   = raw_drive_from_target(ch, ch->operational_target_voltage);
    } else {
        enable = false;
        code   = 0;
    }
    api->set_output(ch->dev, code);            // driver vtable call
    api->set_enable(ch->dev, enable);          // driver vtable call
}
```

The `raw_drive_from_target()` function at `vc_channel.c:73` applies the output calibration:
```c
raw_dac = target_mV * output_calib_k * 10^output_calib_k_exp + output_calib_b
```
(default `output_calib_k_exp = -4`, equivalent to the pre-v3.1 `/10000` — see
`docs/guide/channel-capability-model.md` for why a variable exponent exists.)

### 2.8 Calibration Override

During calibration, `apply_hw` bypasses normal target-to-DAC mapping and uses the raw DAC code directly. See calibration session commands in `vc_channel.c:717-793`:

- `vc_channel_cal_set_output_enable(ch, bool)` — enable/disable cal output, clears raw state on disable
- `vc_channel_cal_set_raw_dac(ch, code)` — write raw DAC code (must be ≤ `cal_max_raw_dac_limit`, requires cal output enabled)
- `vc_channel_cal_set_max_raw_dac(ch, limit)` — set DAC ceiling (must be ≥ current code)
- `vc_channel_cal_sample(ch)` — capture raw ADC snapshot
- `vc_channel_cal_commit(ch)` — validate commit preconditions

---

## 3. System Mode Logic

Three operating modes govern collective channel behavior. Defined in `include/voltage_control/vc_types.h:35`:

```c
enum vc_operating_mode {
    VC_OPERATING_MODE_NORMAL      = 0,  // Manual host control
    VC_OPERATING_MODE_AUTOMATIC   = 1,  // Auto-enable channels with non-zero target
    VC_OPERATING_MODE_CALIBRATION = 2,  // Factory calibration (volatile)
};
```

### 3.1 Mode Transitions

Handled by `vc_controller_set_operating_mode()` at `vc_controller.c:154`.

```
NORMAL ◄────────────────► AUTOMATIC
                              │
                              ▼
                        CALIBRATION
                     (volatile, 30s watchdog)
```

| Transition | Behavior |
|-----------|----------|
| AUTO → NORMAL | Gracefully disable all channel outputs (`vc_channel_output_action(DISABLE_GRACEFUL)` on every channel) |
| * → AUTOMATIC | Enable all non-faulted channels whose `configured_target_voltage != 0` |
| * → CALIBRATION | Save current mode as `pre_cal_mode`, reset all channels into `CALIBRATION_OUTPUT` state, reset calibration watchdog, clear unlock state |
| CALIBRATION → * | Reset all channels to `DISABLED_SAFE` state, restore `pre_cal_mode` |

### 3.2 Calibration Mode Entry Guard

Calibration mode requires a two-step unlock sequence before entry is permitted (`vc_controller.c:225`):

```
Step 1: CAL_UNLOCK_STEP1 (0xCA1B) → cal_unlock_step = 1, cal_unlocked = false
Step 2: CAL_UNLOCK_STEP2 (0xA11B) → cal_unlock_step = 0, cal_unlocked = true
```

If step sequence is broken (wrong value or wrong order), both `cal_unlock_step` and `cal_unlocked` reset to 0.

Entry into calibration mode is rejected with `VC_ERR_INVALID_COMMAND` if `cal_unlocked == false`.

On exit from calibration mode, unlock state is cleared.

### 3.3 Calibration Watchdog

A 30-second inactivity timer prevents indefinite calibration sessions. Managed in `vc_controller_tick()` at `vc_controller.c:137-145`:

- Watchdog is reset on every calibration command (output enable, DAC write, sample, commit, cal field set, max DAC set)
- Watchdog is decremented each tick while in calibration mode
- On expiry (0 ms remaining): `vc_controller_cal_exit()` → returns to `pre_cal_mode`
- Timeout configured via `CONFIG_VC_CAL_WATCHDOG_TIMEOUT_S` (Kconfig)

### 3.4 Cross-Channel Safety in Calibration

The controller enforces that only one channel may have `cal_output_enabled` or non-zero `raw_dac_readback` at a time. This is enforced at the controller level (not in `vc_channel.c`), since the per-channel code has no visibility of other channels.

### 3.5 AUTO Mode Channel Auto-Enable

In addition to bulk enable during mode transition, setting a non-zero `configured_target_voltage` in AUTO mode automatically enables the channel. See `vc_controller_channel_set_field()` at `vc_controller.c:126`:

```c
if (field == VC_FIELD_CONFIGURED_TARGET_VOLTAGE &&
    ctrl->operating_mode == VC_OPERATING_MODE_AUTOMATIC &&
    (int16_t)value != 0 &&
    ctrl->channels[ch].active_fault_cause == 0) {
    vc_channel_output_action(&ctrl->channels[ch], VC_OUTPUT_ACTION_ENABLE);
}
```

### 3.6 Mode Visibility

- **Persisted**: Only NORMAL and AUTOMATIC modes are persisted (`vc_system_config.operating_mode`). CALIBRATION is always volatile.
- **Snapshot**: `vc_system_snapshot.active_operating_mode` reflects the current runtime mode (including CALIBRATION).
- **Shell**: `vc mode <normal|auto|cal>`, `vc sys set mode <0|1|2>`
- **Modbus**: `SYS_OPERATING_MODE` input register (FC04) for read, holding register (FC03/06) for write.

---

## 4. VC Channel API — Writing a New Hardware Channel Driver

This section is a step-by-step guide for implementing a new hardware channel driver. The VC channel API is the interface between domain logic and physical hardware.

### 4.1 The Driver Vtable (`struct vc_channel_api`)

Defined in `include/voltage_control/vc_channel_api.h:22`. Every channel driver must implement the first six function pointers; `get_dt_defaults` is optional (NULL is fine — it exists purely so a driver can override Kconfig-derived defaults from per-channel DTS properties):

```c
typedef void (*vc_meas_ready_cb_t)(uint8_t channel, void *user_data);

struct vc_channel_config;
struct vc_channel_cal_config;

struct vc_channel_api {
    int (*set_output)(const struct device *dev, uint16_t code);
    int (*set_enable)(const struct device *dev, bool enable);
    int (*start_sampling)(const struct device *dev);
    int (*stop_sampling)(const struct device *dev);
    uint16_t (*get_capabilities)(const struct device *dev);
    int (*set_meas_callback)(const struct device *dev,
                             vc_meas_ready_cb_t cb, void *user_data);
    int (*get_dt_defaults)(const struct device *dev,
                           struct vc_channel_config *cfg,
                           struct vc_channel_cal_config *cal);
};
```

| Function | Called By | Must Do |
|----------|-----------|---------|
| `set_output` | `apply_hw()` in `vc_channel.c:87` | Write raw DAC code. Validate `code <= max_raw_dac`. Non-blocking. |
| `set_enable` | `apply_hw()` in `vc_channel.c:87` | Toggle output gate GPIO. Non-blocking. |
| `start_sampling` | `vc_controller_start_sampling()` | Start async ADC loop. Return 0 on success. |
| `stop_sampling` | Teardown path | Cancel pending reads. |
| `get_capabilities` | `vc_channel_init()`, queries | Return DTS-derived `CH_CAP_*` bitmask. |
| `set_meas_callback` | `vc_channel_init()` in `vc_channel.c:308` | Register measurement-ready callback. |
| `get_dt_defaults` (optional) | `vc_channel_init()`, after `cfg`/`cal` are Kconfig-populated | Only touch fields you want to override from a per-channel DTS property (e.g. a factory-default output-enabled flag, or an ADC zero-offset). Leave everything else alone — this runs *after* the Kconfig defaults, not instead of them. |

### 4.2 Measurement Buffer (`struct vc_channel_buffer`)

Defined in `include/voltage_control/vc_channel_api.h:29`. Each channel has exactly one buffer, allocated in a linker section, shared between the HW driver (producer) and the runtime thread (consumer).

```c
struct vc_channel_buffer {
    struct k_spinlock lock;
    uint8_t  channel_id;
    int32_t  raw_voltage;
    uint32_t voltage_timestamp_ms;
    int32_t  raw_current;
    uint32_t current_timestamp_ms;
};
```

**Inline accessors** — these are the only safe way to read/write the buffer:

```c
// Producer (HW driver, from ISR or workqueue):
static inline void vc_channel_buffer_publish_voltage(
    struct vc_channel_buffer *buffer, int32_t raw, uint32_t timestamp_ms);
static inline void vc_channel_buffer_publish_current(
    struct vc_channel_buffer *buffer, int32_t raw, uint32_t timestamp_ms);

// Consumer (runtime thread):
static inline void vc_channel_buffer_read(
    struct vc_channel_buffer *buffer,
    int32_t *raw_voltage, uint32_t *voltage_timestamp_ms,
    int32_t *raw_current, uint32_t *current_timestamp_ms);
```

**Timestamp**: Use `k_uptime_get_32()` for the timestamp. The domain runtime compares timestamps to detect stale vs. fresh data.

### 4.3 Capability Bitmask

Defined in `include/dt-bindings/voltage_control/capabilities.h`. Set in DTS, read by `get_capabilities()`. The capability mask gates which Modbus registers are readable/writable and which domain operations are valid.

```c
#define CH_CAP_OUTPUT_ENABLE           0x0001  // Has enable GPIO
#define CH_CAP_RAW_OUTPUT_DRIVE        0x0002  // Has DAC
#define CH_CAP_VOLTAGE_MEASUREMENT     0x0004  // Has ADC voltage channel
#define CH_CAP_CURRENT_MEASUREMENT     0x0008  // Has ADC current channel
#define CH_CAP_HARDWARE_STATUS         0x0010  // Has hardware status register
```

Typical combinations:
- **Full HVB channel**: `OUTPUT_ENABLE | RAW_OUTPUT_DRIVE | VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT`
- **Output-only channel**: `OUTPUT_ENABLE | RAW_OUTPUT_DRIVE`
- **Measurement-only channel**: `VOLTAGE_MEASUREMENT | CURRENT_MEASUREMENT`

### 4.4 Devicetree Binding

Create a new binding YAML in `dts/bindings/voltage_control/` with a unique compatible string. Must inherit the channel index and capabilities from the parent `vc-controller` child-binding (via `reg` and `capabilities`). Add HW-specific properties (DAC phandle, ADC phandle, GPIOs, etc.).

Example: `dts/bindings/voltage_control/jianwei,hvb-vc-channel.yaml`

### 4.5 Step-by-Step Driver Implementation

Reference implementation: `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c` (295 lines).

Test stub reference: `tests/voltage_control/vc/src/vc_channel_stub.c` (96 lines).

#### Step 1: Define Driver Config and Data Structs

```c
#define DT_DRV_COMPAT jianwei_my_vc_channel   // your compatible string

struct my_vc_config {
    const struct device *dac;       // if CH_CAP_RAW_OUTPUT_DRIVE
    const struct device *adc;       // if CH_CAP_VOLTAGE_MEASUREMENT
    struct gpio_dt_spec enable;     // if CH_CAP_OUTPUT_ENABLE
    uint16_t max_raw_dac;           // DAC ceiling
    uint16_t capabilities;          // DTS-derived
    uint8_t  channel_index;         // DTS reg address
};

struct my_vc_data {
    const struct device *dev;
    uint8_t channel;
    struct vc_channel_buffer *meas; // connected via VC_CHANNEL_BUFFER_PTR()
    vc_meas_ready_cb_t meas_cb;
    void *meas_cb_user_data;
    // ... async sampling state (ADC sequences, work items, signals)
};
```

#### Step 2: Connect to the Measurement Buffer

Use the macros defined in `vc_channel_api.h:72-79`:

```c
// In the DTS instantiation macro:
VC_CHANNEL_BUFFER_EXTERN(DT_DRV_INST(n));    // declares extern struct vc_channel_buffer

static struct my_vc_data my_vc_data_##n = {
    .meas = VC_CHANNEL_BUFFER_PTR(DT_DRV_INST(n)),  // pointer to the buffer
};
```

The buffer itself is allocated by the controller's `MEAS_ENTRY` macro (`vc_controller.c:17-23`) in the `vc_channel_buffer` iterable linker section.

#### Step 3: Implement the Vtable Functions

```c
static int my_set_output(const struct device *dev, uint16_t code)
{
    const struct my_vc_config *cfg = dev->config;
    if (code > cfg->max_raw_dac) { return -EINVAL; }
    return dac_write_value(cfg->dac, 0, code);
}

static int my_set_enable(const struct device *dev, bool enable)
{
    const struct my_vc_config *cfg = dev->config;
    return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

static int my_start_sampling(const struct device *dev)
{
    struct my_vc_data *data = dev->data;
    const struct my_vc_config *cfg = dev->config;

    if (!(cfg->capabilities & (CH_CAP_VOLTAGE_MEASUREMENT | CH_CAP_CURRENT_MEASUREMENT))) {
        return 0;  // no ADC, nothing to do
    }
    data->sampling_active = true;
    my_start_next_cycle(data);  // begin async ADC loop
    return 0;
}

static int my_stop_sampling(const struct device *dev)
{
    struct my_vc_data *data = dev->data;
    data->sampling_active = false;
    // cancel pending ADC reads
    return 0;
}

static uint16_t my_get_capabilities(const struct device *dev)
{
    const struct my_vc_config *cfg = dev->config;
    return cfg->capabilities;
}

static int my_set_meas_callback(const struct device *dev,
                                vc_meas_ready_cb_t cb, void *user_data)
{
    struct my_vc_data *data = dev->data;
    data->meas_cb = cb;
    data->meas_cb_user_data = user_data;
    return 0;
}
```

#### Step 3b (optional): Per-Channel DTS Defaults via `get_dt_defaults`

Skip this unless your channel needs a factory default that differs from
the board-wide Kconfig value — e.g. a specific channel starting disabled
by default, or a per-channel ADC zero-offset calibration value baked into
DTS. Reference implementation:
`drivers/voltage_control/lvb_vc_channel/lvb_vc_channel.c` (the only current
driver that implements this hook; `hvb_vc_channel.c` doesn't need it and
leaves the vtable slot NULL).

```c
struct my_vc_config {
    // ... existing fields ...
    int16_t calib_current_b;       // per-channel ADC zero offset from DTS
    bool default_output_disabled;  // per-channel startup override from DTS
};

static int my_get_dt_defaults(const struct device *dev,
                              struct vc_channel_config *cfg,
                              struct vc_channel_cal_config *cal)
{
    const struct my_vc_config *drv_cfg = dev->config;

    cal->measured_current_calib_b = drv_cfg->calib_current_b;
    if (drv_cfg->default_output_disabled) {
        cfg->configured_output_enabled = false;
    }
    return 0;
}
```

Add the corresponding optional property to your DTS binding YAML (`type:
int` / `type: boolean` as appropriate, `required: false`), populate the
driver config struct field from it in your `MY_VC_INIT` macro via
`DT_INST_PROP`/`DT_INST_PROP_OR`, and add `.get_dt_defaults =
my_get_dt_defaults,` to the vtable. A boolean property's *absence* should
mean "no override, follow the normal default" — design the property as an
opt-in exception (`default-output-disabled`), not a tri-state, so the
common case (every other channel) needs no DTS changes at all.

#### Step 4: Implement Async ADC Sampling

The driver must run an async ADC sampling loop. The canonical pattern (from `hvb_vc_channel.c:96-167`):

```
my_start_next_cycle(data):
    1. Determine next phase: voltage or current
    2. Configure ADC sequence for the phase
    3. adc_read_async(adc, &seq, &signal)
    4. k_work_poll_submit_to_queue(workq, &poll_work, &adc_event, 1, timeout)

my_poll_handler(work):                               // k_work_poll callback
    1. Read ADC result from buffer
    2. If voltage phase: vc_channel_buffer_publish_voltage(meas, raw, k_uptime_get_32())
       If current phase: vc_channel_buffer_publish_current(meas, raw, k_uptime_get_32())
    3. On completion of a full V+I cycle (or after voltage if no current): notify
       → data->meas_cb(data->channel, data->meas_cb_user_data)
    4. If sampling_active, start_next_cycle()
```

**Key rules for measurement publishing:**
- Always publish both voltage and current timestamps via `k_uptime_get_32()`
- The measurement callback (`meas_cb`) wakes the runtime thread. The domain side uses timestamps to detect new data, so the callback is only a wake hint — missed callbacks are tolerable.
- Use `k_work_poll` with reasonable timeout (420 ms for ADS1232 at 10 SPS) to avoid blocking the workqueue on stalled hardware.
- ADC channel selection via ADC sequence `channels` bitmask (e.g., `BIT(0)` = current on MUX ch0, `BIT(1)` = voltage on MUX ch1).

#### Step 5: Device Init Function

```c
static int my_vc_init(const struct device *dev)
{
    const struct my_vc_config *cfg = dev->config;
    struct my_vc_data *data = dev->data;

    // Validate devices
    if (cfg->dac && !device_is_ready(cfg->dac)) { return -ENODEV; }
    if (cfg->adc && !device_is_ready(cfg->adc)) { return -ENODEV; }
    if (cfg->capabilities & CH_CAP_OUTPUT_ENABLE) {
        if (!gpio_is_ready_dt(&cfg->enable)) { return -ENODEV; }
        gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
    }

    // Setup DAC channel if present
    if (cfg->dac) {
        struct dac_channel_cfg dac_cfg = { .channel_id = 0, .resolution = 16 };
        dac_channel_setup(cfg->dac, &dac_cfg);
    }

    // Initialize ADC sequence (if present)
    data->dev = dev;
    data->channel = cfg->channel_index;
    // ... setup adc_seq, adc_signal, poll_work ...

    return 0;
}
```

#### Step 6: DTS Instantiation Macro

```c
#define MY_VC_INIT(n) \
    VC_CHANNEL_BUFFER_EXTERN(DT_DRV_INST(n)); \
    static const struct my_vc_config my_vc_config_##n = { \
        .dac = DEVICE_DT_GET(DT_INST_PHANDLE(n, dac)), \
        .adc = DEVICE_DT_GET(DT_INST_PHANDLE(n, adc)), \
        .enable = GPIO_DT_SPEC_INST_GET(n, enable_gpios), \
        .max_raw_dac = DT_INST_PROP(n, max_raw_dac), \
        .capabilities = DT_INST_PROP(n, capabilities), \
        .channel_index = DT_REG_ADDR(DT_DRV_INST(n)), \
    }; \
    static struct my_vc_data my_vc_data_##n = { \
        .meas = VC_CHANNEL_BUFFER_PTR(DT_DRV_INST(n)), \
    }; \
    DEVICE_DT_INST_DEFINE(n, my_vc_init, NULL, \
        &my_vc_data_##n, &my_vc_config_##n, \
        POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, \
        &my_vc_hw_api);

DT_INST_FOREACH_STATUS_OKAY(MY_VC_INIT)
```

#### Step 7: DTS Fragment

Add channel nodes under the `vc_controller` node using your new compatible:

```dts
&vc_controller {
    my_ch0: channel@0 {
        compatible = "jianwei,my-vc-channel";
        reg = <0>;
        label = "My Channel 0";
        capabilities = <(CH_CAP_OUTPUT_ENABLE | CH_CAP_RAW_OUTPUT_DRIVE |
                         CH_CAP_VOLTAGE_MEASUREMENT)>;
        dac = <&my_dac>;
        adc = <&my_adc>;
        enable-gpios = <&gpioh 12 GPIO_ACTIVE_LOW>;
        max-raw-dac = <0xFFFF>;
    };
};
```

#### Step 8: Build Integration

1. **Driver**: Add `CMakeLists.txt` and `Kconfig` under `drivers/voltage_control/my_vc_channel/`
2. **Binding**: Add `jianwei,my-vc-channel.yaml` under `dts/bindings/voltage_control/`
3. **Parent CMakeLists/Kconfig**: Wire into `drivers/voltage_control/CMakeLists.txt` and `drivers/voltage_control/Kconfig`
4. **DTS**: Add channel nodes using the new compatible
5. **prj.conf**: Enable `CONFIG_MY_VC_CHANNEL=y`

### 4.6 Minimum Viable Driver (No ADC, No DAC)

For a channel that is purely a GPIO toggle (e.g., output enable only with fixed voltage, no feedback):

```c
// Capabilities: CH_CAP_OUTPUT_ENABLE only

static int my_set_output(const struct device *dev, uint16_t code) {
    ARG_UNUSED(dev); ARG_UNUSED(code);
    return 0;  // no DAC, accept any code
}

static int my_set_enable(const struct device *dev, bool enable) {
    const struct my_vc_config *cfg = dev->config;
    return gpio_pin_set_dt(&cfg->enable, enable ? 1 : 0);
}

static int my_start_sampling(const struct device *dev) { return 0; }
static int my_stop_sampling(const struct device *dev) { return 0; }
```

No measurement buffer connection is needed for channels without `CH_CAP_VOLTAGE_MEASUREMENT` or `CH_CAP_CURRENT_MEASUREMENT`, but it is still allocated by the controller at compile time.

### 4.7 Contract Between Driver and Domain

| The domain will _never_... | The driver must _always_... |
|---------------------------|---------------------------|
| Call more than one vtable fn concurrently per channel | Be safe under concurrent calls from different channels |
| Expect `set_output` to return anything other than 0 or `-EINVAL` | Validate `code <= max_raw_dac` |
| Call `set_output` with code that hasn't gone through calibration | Handle any code 0–65535 (for raw calibration writes) |
| Call `set_enable(true)` without `set_output` first | Make `set_enable(false)` safe to call at any time |
| Call `set_meas_callback` more than once per channel | Store the callback and invoke it when new data arrives |
| Read `vc_channel_buffer` outside `k_spin_lock` | Call the inline `vc_channel_buffer_publish_*` helpers (which hold the spinlock) |
| Consume measurement data without timestamp check | Publish fresh `k_uptime_get_32()` timestamps |

### 4.8 Testing a New Driver

The native_posix test infrastructure provides a stub driver (`tests/voltage_control/vc/src/vc_channel_stub.c`) and DTS binding (`jianwei,vc-channel-stub`) that can serve as a template. Key patterns:

1. **Test the vtable in isolation**: Verify each function pointer returns expected values and toggles hardware state.
2. **Test the ADC sampling loop**: Use `k_work_poll` and inject synthetic ADC results to verify the publish→callback→wake chain.
3. **Integration test**: Wire the driver into a full Zephyr test target with `vc_init()` and `vc_ctx_start()`, then exercise the shell and Modbus adapters.
4. **Snapshot consistency**: Verify that `vc_channel_get_snapshot()` returns values that match the last published measurement data.

---

## Appendix

### Configuration Dependencies

| Kconfig | Purpose |
|---------|---------|
| `CONFIG_VC_RUNTIME` | Enable the VC runtime subsystem |
| `CONFIG_VC_RUNTIME_TICK_INTERVAL_MS` | Worker thread tick interval (default 100) |
| `CONFIG_VC_RUNTIME_THREAD_STACK_SIZE` | Worker thread stack size |
| `CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH` | Max pending commands |
| `CONFIG_VC_CAL_WATCHDOG_TIMEOUT_S` | Calibration inactivity exit (default 30) |
| `CONFIG_VC_CHANNEL_CONTROLLER` | Enable DTS-based channel construction |
| `CONFIG_VC_SHELL` | Enable `vc` shell command tree |
| `CONFIG_VC_SETTINGS_PERSISTENCE` | Enable NVS persistence via Zephyr Settings |

### Key Design Decisions

1. **SMF states use NULL callbacks**: Transitions are purely manual via `set_smf_state()`. The SMF framework provides the state enum tracking; transitions implement their own entry/exit side effects inline.
2. **Calibration bypasses the normal DAC pipeline**: `apply_hw()` has a dedicated calibration branch that uses raw DAC code directly, bypassing `raw_drive_from_target()`. This allows arbitrary DAC codes during calibration while normal operation always routes through the calibration formula.
3. **Mode transitions are collective**: Setting the operating mode triggers side effects on all channels simultaneously (enable all, disable all, reset to calibration). This is intentional — individual channel control is done through output actions and config writes.
4. **Measurement timestamps prevent stale consumption**: The domain runtime tracks `last_consumed_voltage_ts` and `last_consumed_current_ts` per channel, comparing against the buffer timestamps. This avoids double-processing the same sample if the runtime tick fires faster than the ADC sampling rate, and avoids consuming stale data if the ADC stalls.

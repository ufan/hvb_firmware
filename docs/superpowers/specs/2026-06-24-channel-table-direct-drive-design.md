# Channel Table & Direct-Drive Architecture

**Date**: 2026-06-24
**Supersedes**: 2026-06-19-static-provider-bus-design.md, 2026-06-19-vc-channel-provider-design.md

## Summary

Replace the provider bus with a static channel table and direct hardware API calls. Decompose the monolithic domain into three clear layers: independent per-channel state machines (virtual channels), a thin voltage controller that manages them, and a domain runtime that handles system state, facade interaction, and command routing. Measurement acquisition uses interrupt-driven k_work_poll. Each measurement is consumed immediately by its virtual channel's engines (protection, status), with decisions made inline and hardware execution queued.

## Motivation

### Provider Bus is a Shallow Pass-Through

- **Config slots + provider_msgq**: domain computes output drive, writes to slot, enqueues CONFIG_CHANGED, bus dispatches to driver, driver acquires slot, applies to hardware. Five hops for what should be a direct function call.
- **evidence_msgq + measurement buffer**: driver builds snapshot, copies into message queue, worker copies out, copies into buffer, domain reads buffer again for staleness. Four memcpy's for what should be a direct write.
- **Shared work queue**: all channels serialize on one thread. ADS1232 DRDY wait (~400ms) blocks all other channels.

Topology and capabilities are fixed at compile time from DTS. No runtime dynamic management is needed. The bus adds interface cost without behavior.

### Domain and Channel State are Entangled

`domain_state.c` (1700 lines) owns everything: channel config, channel snapshots, channel SMF states, channel runtime state, AND system state. A virtual channel is not an independent entity — it's just arrays indexed by channel number inside the domain struct. This makes the channel state machine implicit and hard to reason about.

### controller.c is Underutilized

Currently 28 lines — just a DTS macro building a static array. It should be the voltage controller: the logical manager that owns and orchestrates virtual channels.

### Monolithic Worker Does Too Much

One channel's voltage measurement wakes the global runtime worker, which checks the command queue, iterates all channels, runs periodic tick, publishes full snapshot. A fine-grained, per-channel, per-function separation is needed.

## Architecture

### Layer Diagram

```
┌───────────────────────────────────────────────────────┐
│  Frontend Adapters (Modbus, future CAN/TCP)           │
│  vc_query() for monitoring, vc_dispatch() for cmds    │
├───────────────────────────────────────────────────────┤
│  Domain Runtime (domain_runtime.c)                    │
│  System state, CQRS facade, command routing           │
│  Worker thread: host commands, snapshot publishing     │
├───────────────────────────────────────────────────────┤
│  Voltage Controller (vc_controller.c)                 │
│  Thin manager of all virtual channels                 │
│  System-wide operations (mode transitions, start/stop)│
├──────────┬──────────┬──────────┬──────────────────────┤
│  VChan 0 │  VChan 1 │  VChan N │  (independent SMFs)  │
│  config   │  config   │  config   │                     │
│  state    │  state    │  state    │                     │
│  engines  │  engines  │  engines  │                     │
├──────────┴──────────┴──────────┴──────────────────────┤
│  Channel Table (vc_channel_table.c)                   │
│  Static dispatch: channel index → device + hw API     │
├──────────────┬──────────────┬─────────────────────────┤
│  hvb_vc_hw   │  lvb_vc_hw   │  future variants...     │
│  DAC+ADC+GPIO│  GPIO only   │                         │
└──────────────┴──────────────┴─────────────────────────┘
```

### Responsibility Separation

**Virtual Channel** (`vc_channel_state.c`) — per-channel, independent, **single arbiter**:
- Owns its SMF state machine (disabled_safe, enabled_holding, ramping, fault_latched, retry_cooldown, calibration_output)
- Owns its config (target voltage, ramp settings, protection settings, calibration coefficients)
- Owns its runtime state (output_enabled, ramp_accum, cooldown, measured values, faults, status bits)
- Owns its engines: ramp engine, protection engine, recovery engine, status engine
- **Single arbiter of state transitions** from three input sources:
  1. Frontend commands (output action, config change, fault clear) — via voltage controller from runtime worker
  2. Measurement updates (triggers protection policy) — via voltage controller from shared workq
  3. Periodic ramp and recovery — via voltage controller from runtime worker tick
- All three inputs are serialized through a per-channel `k_spinlock` (~10µs critical section, disables interrupts on Cortex-M4) so the state machine sees a consistent view regardless of which thread calls in
- Produces **decisions** (pending output commands + state transitions)
- Does NOT execute hardware writes directly

**Voltage Controller** (`vc_controller.c`) — thin wrapper/manager:
- Owns the array of virtual channels
- System-wide operations: mode transitions (normal → calibration affects all channels), start/stop all sampling
- Routes per-channel commands to the correct virtual channel
- Drains pending hardware commands from virtual channels and executes them via channel table
- Provides aggregate queries (system snapshot, active channel mask)

**Domain Runtime** (`domain_runtime.c`) — thin facade:
- System state management (operating mode, system config, uptime, calibration unlock)
- CQRS facade: `vc_dispatch()` / `vc_query()` translate high-level commands to voltage controller calls
- Worker thread handles: host commands (from Modbus), periodic ramp tick, published snapshot refresh
- Does NOT own channel state — delegates to voltage controller

**Channel Table** (`vc_channel_table.c`) — static dispatch:
- Maps channel index → device pointer + `vc_channel_hw_api`
- Pure lookup, no state, no lifecycle
- Built from DTS at compile time

**Hardware Drivers** (`hvb_vc_hw.c`, etc.) — pure adapters:
- Implement `vc_channel_hw_api` (set_output, set_enable, start_sampling, stop_sampling)
- Own interrupt-driven sampling (k_work_poll + DRDY)
- Write measurements directly to buffer, invoke virtual channel consumption
- No bus interaction, no config application, no state machines

### Data Flow

**Command path** (top-down):
```
vc_dispatch(cmd)
  → domain runtime: validate, route to voltage controller
    → voltage controller: route to virtual channel
      → virtual channel: update config/state, produce pending hw command
    → voltage controller: drain pending commands
      → channel table: vc_channel_set_output(ch, code)
        → hw driver: SPI/GPIO write
```

**Measurement path** (bottom-up, interrupt-driven):
```
DRDY interrupt → k_work_poll handler → bit-bang 24 bits
  → write raw value to measurement buffer (direct, no lock)
  → virtual channel: consume measurement (immediate)
      → calibration scaling
      → protection engine: check limits
      → DECISION: "disable output, transition to FAULT_LATCHED"
      → record decision as pending hw command + state transition
  → voltage controller: drain pending commands for this channel
      → channel table: vc_channel_set_output(ch, 0)
      → channel table: vc_channel_set_enable(ch, false)
```

**Query path** (read-only):
```
vc_query()
  → domain runtime: read published snapshot (refreshed periodically)
  → or read measurement buffer directly (monitoring, stale OK)
```

## Decision vs Execution Separation

On the measurement path, the virtual channel's protection engine runs immediately and produces a **decision**:

```c
struct vc_pending_command {
    uint16_t output_code;        /* desired DAC code */
    bool     output_enable;      /* desired enable state */
    bool     valid;              /* true if a hw write is needed */
};
```

The decision (state transition + fault bits + pending command) is **atomic with measurement consumption** — no measurement should be processed in the old state after the decision. The virtual channel records the pending command and transitions its SMF state immediately.

The hardware write (SPI to DAC, GPIO toggle) is a **side effect** executed by the voltage controller after the virtual channel returns. This separation means:
- The measurement path stays pure: read → update state → decide → return
- Hardware writes are batched and drained from a single execution point
- Future variants with expensive execution (I2C DAC, networked actuator) don't block the measurement path
- The virtual channel is testable without hardware — feed measurement, check decision

For HVB, the drain happens immediately after the virtual channel call returns (same work queue handler invocation). The pending command is consumed in the same call chain — no queue, no delay. The separation is logical, not temporal.

## Virtual Channel

### Structure

```c
struct vc_channel {
    struct k_spinlock lock;        /* single arbiter serialization */
    struct smf_ctx smf;
    uint8_t index;
    uint16_t capabilities;

    /* Config */
    struct vc_channel_config config;

    /* Runtime state */
    bool output_enabled;
    bool ramping;
    uint32_t ramp_accum_ms;
    uint32_t cooldown_remaining_ms;
    int16_t operational_target_voltage;

    /* Measurement state */
    int16_t measured_voltage;
    int16_t measured_current;

    /* Fault state */
    uint16_t active_fault_cause;
    uint16_t fault_history_cause;
    uint16_t status_bits;
    uint32_t last_fault_timestamp;

    /* Calibration state */
    uint16_t cal_max_raw_dac_limit;
    uint16_t raw_dac_readback;
    uint16_t cal_output_enabled;
    enum vc_cal_sample_status cal_sample_status;
    int32_t raw_adc_voltage;
    int32_t raw_adc_current;

    /* Pending hardware command (decision output) */
    struct vc_pending_command pending;
};
```

All fields are behind the per-channel `k_spinlock`. Every public function on `vc_channel` acquires the lock on entry and releases on exit. This is the current `vc_channel_snapshot` + `vc_channel_runtime` + `vc_channel_config` + per-channel SMF context, pulled out of the domain's parallel arrays into a single self-contained arbiter.

### API

```c
/* Lifecycle */
void vc_channel_init(struct vc_channel *ch, uint8_t index, uint16_t capabilities);

/* Command path (called by voltage controller) */
enum vc_status vc_channel_set_config(struct vc_channel *ch,
                                     const struct vc_channel_config *cfg);
enum vc_status vc_channel_get_config(const struct vc_channel *ch,
                                     struct vc_channel_config *cfg);
enum vc_status vc_channel_output_action(struct vc_channel *ch,
                                        enum vc_output_action action);
enum vc_status vc_channel_fault_command(struct vc_channel *ch,
                                        enum vc_channel_fault_command cmd);
enum vc_status vc_channel_set_field(struct vc_channel *ch,
                                    enum vc_config_field field, uint16_t value);

/* Measurement path (called by hw driver via voltage controller) */
void vc_channel_consume_voltage(struct vc_channel *ch, int32_t raw_voltage);
void vc_channel_consume_current(struct vc_channel *ch, int32_t raw_current);
void vc_channel_consume_fault(struct vc_channel *ch, uint16_t fault_cause);

/* Periodic (called by voltage controller from runtime tick) */
void vc_channel_tick_ramp(struct vc_channel *ch, uint32_t dt_ms);

/* Snapshot read (called by voltage controller for query path) */
void vc_channel_get_snapshot(const struct vc_channel *ch,
                             struct vc_channel_snapshot *snap);

/* Calibration (called by voltage controller) */
enum vc_status vc_channel_cal_set_output_enable(struct vc_channel *ch, bool enable);
enum vc_status vc_channel_cal_set_raw_dac(struct vc_channel *ch, uint16_t code);
enum vc_status vc_channel_cal_sample(struct vc_channel *ch);
enum vc_status vc_channel_cal_commit(struct vc_channel *ch);
enum vc_status vc_channel_cal_set_max_raw_dac(struct vc_channel *ch, uint16_t limit);

/* Pending command access */
bool vc_channel_has_pending_command(const struct vc_channel *ch);
struct vc_pending_command vc_channel_take_pending_command(struct vc_channel *ch);
```

The virtual channel is a **pure state machine** — no hardware calls, no device pointers, no Zephyr kernel dependencies. Fully testable with unit tests on native_posix.

## Voltage Controller

### Structure

```c
struct vc_controller {
    struct vc_channel channels[VC_MAX_CHANNELS];
    size_t channel_count;
    enum vc_operating_mode operating_mode;
    uint8_t cal_unlock_step;
    bool cal_unlocked;
};
```

### API

```c
/* Init from DTS-composed channel entries */
struct vc_controller *vc_controller_init(
    const struct vc_channel_entry *entries, size_t count);

/* System-level operations */
enum vc_status vc_controller_set_operating_mode(
    struct vc_controller *ctrl, enum vc_operating_mode mode);
enum vc_operating_mode vc_controller_get_operating_mode(
    const struct vc_controller *ctrl);
enum vc_status vc_controller_calibration_unlock(
    struct vc_controller *ctrl, uint16_t value);

/* Per-channel command routing */
enum vc_status vc_controller_channel_set_field(
    struct vc_controller *ctrl, uint8_t ch,
    enum vc_config_field field, uint16_t value);
enum vc_status vc_controller_channel_output_action(
    struct vc_controller *ctrl, uint8_t ch, enum vc_output_action action);
enum vc_status vc_controller_channel_fault_command(
    struct vc_controller *ctrl, uint8_t ch, enum vc_channel_fault_command cmd);

/* Measurement consumption + drain (called from hw driver work handler) */
void vc_controller_consume_voltage(struct vc_controller *ctrl,
                                   uint8_t ch, int32_t raw_voltage);
void vc_controller_consume_current(struct vc_controller *ctrl,
                                   uint8_t ch, int32_t raw_current);
void vc_controller_consume_fault(struct vc_controller *ctrl,
                                 uint8_t ch, uint16_t fault_cause);

/* Periodic tick */
void vc_controller_tick(struct vc_controller *ctrl, uint32_t dt_ms);

/* Param actions (save/load/reset) */
enum vc_status vc_controller_system_param_action(
    struct vc_controller *ctrl, enum vc_param_action action);
enum vc_status vc_controller_channel_param_action(
    struct vc_controller *ctrl, uint8_t ch, enum vc_param_action action);

/* Snapshot queries */
void vc_controller_get_system_snapshot(const struct vc_controller *ctrl,
                                      struct vc_system_snapshot *snap);
void vc_controller_get_channel_snapshot(const struct vc_controller *ctrl,
                                        uint8_t ch, struct vc_channel_snapshot *snap);
void vc_controller_get_channel_config(const struct vc_controller *ctrl,
                                      uint8_t ch, struct vc_channel_config *cfg);
```

The `vc_controller_consume_*` functions do two things:
1. Call `vc_channel_consume_voltage/current/fault` on the target channel (decision)
2. Drain the channel's pending command via `vc_channel_table_set_output` / `set_enable` (execution)

This is the single point where decision meets execution.

## Channel Hardware API

Fully replaces the legacy `vc_channel_api`. The old API is deleted entirely — not wrapped, not adapted, not kept for compatibility.

```c
struct vc_channel_hw_api {
    int (*set_output)(const struct device *dev, uint16_t code);
    int (*set_enable)(const struct device *dev, bool enable);
    int (*start_sampling)(const struct device *dev);
    int (*stop_sampling)(const struct device *dev);
};
```

All four function pointers must be non-NULL. Variant drivers that lack hardware for a given operation provide a no-op stub returning 0.

The old `vc_channel_api` had 9 functions mixing three concerns. The new API has 4 functions doing one thing: hardware I/O.

## Channel Table

Static dispatch layer. Maps channel index → device + hw API.

### Data Structure

```c
struct vc_channel_entry {
    const struct device *dev;
    uint8_t index;
    uint16_t capabilities;
    struct vc_measurement_buffer_entry *meas;
};
```

### API

```c
int vc_channel_table_set_output(uint8_t ch, uint16_t code);
int vc_channel_table_set_enable(uint8_t ch, bool enable);
int vc_channel_table_start_sampling(uint8_t ch);
int vc_channel_table_stop_sampling(uint8_t ch);

const struct vc_measurement_buffer_entry *vc_channel_table_get_measurement(uint8_t ch);
size_t vc_channel_table_count(void);
uint16_t vc_channel_table_capabilities(uint8_t ch);
const struct vc_channel_entry *vc_channel_table_get(uint8_t ch);
```

Built from the `controller.c` DTS macro expansion at compile time. Pure lookup — no state, no lifecycle.

### Voltage Controller Pointer Resolution

The voltage controller is a singleton, same as today's domain. The channel table holds a `struct vc_controller *` set once at init. Hardware drivers access it through the channel table to call `vc_controller_consume_*` after writing measurement data.

## Measurement Buffer

### Structure

```c
struct vc_measurement_buffer_entry {
    int32_t raw_voltage;
    uint32_t voltage_timestamp_ms;
    int32_t raw_current;
    uint32_t current_timestamp_ms;
};
```

This is bare storage for the monitoring/query path only. No `channel` (implicit from buffer index), no `present_mask` (the consumption functions `consume_voltage`/`consume_current` already know the measurement type), no `provider_status` or `provider_fault_cause` (faults go directly through `consume_fault` to the virtual channel arbiter).

Each ADC read updates its own value and timestamp independently. The driver writes one field pair at a time; the query path reads whatever is current.

## Interrupt-Driven ADC Sampling (HVB Variant)

### Per-Instance State

```c
struct hvb_vc_data {
    const struct device *dev;
    uint8_t channel;
    struct vc_measurement_buffer_entry *meas;  /* direct write to monitoring buffer */

    /* k_work_poll for non-blocking DRDY wait */
    struct k_work_poll poll_work;
    struct k_poll_signal drdy_signal;
    struct k_poll_event drdy_event;
    struct gpio_callback drdy_cb;

    /* ADC input selection state machine */
    enum { ADC_PHASE_VOLTAGE, ADC_PHASE_CURRENT } adc_phase;

    uint16_t provider_status;
    struct k_work_delayable next_cycle_work;
};
```

### State Machine

```
start_sampling()
  └→ adc_phase = ADC_PHASE_VOLTAGE
  └→ select A0 for voltage input
  └→ arm DRDY interrupt (falling edge)
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))

DRDY ISR:
  └→ k_poll_signal_raise(&drdy_signal)
  └→ re-enable DRDY interrupt for next edge

k_work_poll handler fires (adc_phase == ADC_PHASE_VOLTAGE):
  └→ bit-bang 24 bits (~50µs) → raw_voltage
  └→ write raw_voltage + voltage_timestamp to meas buffer
  └→ vc_controller_consume_voltage(ctrl, ch, raw_voltage)
       └→ vc_channel_consume_voltage() [under spinlock]
            → calibration scaling
            → voltage protection check → state transition + pending command if fault
       └→ drain pending command → vc_channel_table_set_output/set_enable
  └→ adc_phase = ADC_PHASE_CURRENT
  └→ select A0 for current input
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))

DRDY ISR: (same)

k_work_poll handler fires (adc_phase == ADC_PHASE_CURRENT):
  └→ bit-bang 24 bits (~50µs) → raw_current
  └→ write raw_current + current_timestamp to meas buffer
  └→ vc_controller_consume_current(ctrl, ch, raw_current)
       └→ vc_channel_consume_current() [under spinlock]
            → calibration scaling
            → current protection check → state transition + pending command if fault
       └→ drain pending command → vc_channel_table_set_output/set_enable
  └→ adc_phase = ADC_PHASE_VOLTAGE
  └→ schedule next_cycle_work after sample_rate_ms

next_cycle_work handler:
  └→ select A0 for voltage input
  └→ arm DRDY interrupt
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))
```

### Timeout Handling

If DRDY never fires (hardware fault), k_work_poll times out after 420ms. The handler checks `poll_work.poll_result`:
- If timeout: set provider_status error, call `vc_controller_consume_fault(ctrl, ch, VC_FAULT_MEASUREMENT)`, schedule retry after `sample_rate_ms`.

### Shared Work Queue Scaling

All channels use one shared `k_work_q`. The work queue thread is occupied for ~60µs per ADC read (50µs bit-bang + 10µs channel processing). For 16 channels with voltage + current:
- 16 × 2 × 60µs = 1.92ms per full sample cycle
- Work queue utilization: <0.2% at 100ms sample rate

## Domain Runtime Changes

The domain runtime becomes a thin facade:

### What it owns
- System state: `vc_system_config` (slave address, baud rate, recovery policy)
- Uptime tracking
- Published snapshot cache (for query path)
- Command queue + worker thread
- Storage backend attachment

### What it delegates
- All channel state → voltage controller → virtual channels
- Operating mode → voltage controller
- Calibration unlock → voltage controller
- Channel commands → voltage controller

### Worker Loop

```
vc_runtime_worker:
    while (!stop_requested):
        k_sem_take(&wake, K_MSEC(TICK_INTERVAL_MS))

        // Process host commands
        while (command_queue has items):
            dispatch_command(cmd)
            // routes to vc_controller_* functions

        // Periodic ramp tick
        if (tick timeout expired):
            vc_controller_tick(ctrl, dt_ms)
            // internally: for each channel, vc_channel_tick_ramp
            // drain any pending output commands from ramp changes

        // Refresh published snapshot for query path
        publish_snapshot()
```

Measurement processing is absent from this loop. It happens inline in the k_work_poll handlers via `vc_controller_consume_*`.

### Single-Arbiter Concurrency

The virtual channel state machine is the **single arbiter** of all state transitions. Three input sources call into it from two threads:

- **Shared workq thread**: measurement path → `vc_channel_consume_voltage/current/fault`
- **Runtime worker thread**: command path → `vc_channel_output_action/set_field/set_config`; ramp tick → `vc_channel_tick_ramp`

All virtual channel functions acquire a per-channel `k_spinlock` on entry and release on exit. On Cortex-M4, `k_spinlock` disables interrupts (~10µs critical section for calibration arithmetic + protection check). This guarantees the state machine sees a consistent view — no field-level reasoning about which thread writes what.

The spinlock also serializes pending command production. Only one pending command exists at a time per channel. If protection produces a "disable output" command while a ramp "update output" is pending, the protection command overwrites it — correct behavior since the channel is now faulted and the ramp output is no longer valid.

## What Gets Deleted

| Module | Fate |
|---|---|
| `lib/voltage_control/provider_bus.c` | **Deleted** |
| `include/voltage_control/provider_bus.h` | **Deleted** |
| `vc_channel_api` (entire legacy vtable) | **Deleted** — replaced by `vc_channel_hw_api` |
| `include/voltage_control/vc_channel.h` | **Rewritten** with `vc_channel_hw_api` |
| `vc_runtime_config_snapshot` struct | **Deleted** |
| `vc_runtime_config_slot` struct | **Deleted** |
| `vc_provider_msg` / `vc_provider_binding` | **Deleted** |
| `evidence_msgq` / `provider_msgq` | **Deleted** |
| All config slot and measurement buffer mutexes | **Deleted** |
| `domain_consume_measurement()` (bulk) | **Replaced** by per-channel per-field functions |
| Parallel arrays in `struct domain` (channels[], snapshots[], runtime[]) | **Replaced** by `struct vc_channel` per-channel structs inside voltage controller |

## What Gets Added

| Module | Purpose |
|---|---|
| `lib/voltage_control/vc_channel_state.c` | Per-channel virtual channel state machine + engines (~400 lines, extracted from domain_state.c) |
| `include/voltage_control/vc_channel_state.h` | Virtual channel API |
| `lib/voltage_control/vc_controller.c` | Voltage controller: channel manager + pending command drain (~200 lines, replaces provider_bus.c) |
| `include/voltage_control/vc_controller.h` | Voltage controller API |
| `lib/voltage_control/vc_channel_table.c` | Static dispatch table (~60 lines) |
| `include/voltage_control/vc_channel_table.h` | Channel table API |
| `include/voltage_control/vc_channel_hw.h` | Hardware API vtable (replaces vc_channel.h) |
| k_work_poll + phase state machine in hvb_vc_channel | Replaces blocking poll loop (~80 lines) |

## What Gets Simplified

| Module | Change |
|---|---|
| `domain_state.c` | Shrinks significantly — channel state extracted to `vc_channel_state.c`. Retains system-level validation and operating mode transitions. |
| `domain_runtime.c` | Shrinks — no measurement draining, no config publishing to bus. Thin command router. |
| `domain.h` | Shrinks — channel-level functions move to `vc_channel_state.h` and `vc_controller.h` |
| `hvb_vc_channel.c` | Simplified — no apply_config, no notify_config_changed, no measurement callbacks. Pure hw + sampling state machine. |
| `controller.c` | Expanded into `vc_controller.c` — from 28-line macro to full channel manager |

## DTS Binding Changes

### `jianwei,vc-controller.yaml` — Revised: child node composition

The controller binding changes from a phandle array to child node composition. Virtual channels become direct children of the controller node with `reg` for explicit index assignment:

```yaml
compatible: "jianwei,vc-controller"

child-binding:
  description: Virtual voltage channel
  properties:
    reg:
      type: int
      required: true
      description: Channel index (0-based).
    label:
      type: string
      required: true
      description: Human-readable channel name (e.g. "HV1", "HV2").
    capabilities:
      type: int
      required: true
      description: Bitmask of CH_CAP_* flags.
    enable-gpios:
      type: phandle-array
      required: true
      description: GPIO that gates channel output.
```

Variant-specific properties (dac, adc, sample-rate-ms, max-raw-dac) are declared in variant child bindings that extend this base child binding.

### New DTS Layout (jw_hvb.dts)

```dts
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
```

Benefits over the current phandle array approach:
- Channel index is `reg`, not a redundant `channel-index` property
- The `channels = <&vc_ch0 &vc_ch1>` phandle array is eliminated — child node order and `reg` values define the topology
- Channel names are explicit via `label`
- The controller node is self-contained — no dangling top-level channel nodes
- `DT_FOREACH_CHILD` iterates channels naturally
- Future variant boards override only the controller's children

### `jianwei,hvb-vc-channel.yaml` — Revised: child binding of controller

No longer a standalone top-level binding. Becomes a child-compatible of `jianwei,vc-controller` that adds HVB-specific properties:

```yaml
compatible: "jianwei,hvb-vc-channel"
description: HVB Virtual Voltage Channel (child of vc-controller)

properties:
  dac:
    type: phandle
    required: true
  adc:
    type: phandle
    required: true
  max-raw-dac:
    type: int
    default: 0xFFFF
  sample-rate-ms:
    type: int
    default: 100
```

Base properties (`reg`, `label`, `capabilities`, `enable-gpios`) are inherited from the controller's child-binding.

### `jianwei,vc-channel-base.yaml` — Deleted

The base properties move into the controller's child-binding. No separate base yaml needed.

### `ti,ads1232.yaml` — No changes

The `drdy-gpios` property already exists. GPIO interrupt configuration happens at runtime via `gpio_pin_interrupt_configure_dt()`.

### Channel Table Macro Update

The `controller.c` DTS macro changes from `DT_FOREACH_PROP_ELEM` on a phandle array to `DT_FOREACH_CHILD` on the controller node:

```c
#define VC_CONTROLLER_NODE DT_NODELABEL(vc_controller)

#define CH_ENTRY(node_id) \
    { \
        .dev = DEVICE_DT_GET(node_id), \
        .index = DT_REG_ADDR(node_id), \
        .capabilities = DT_PROP(node_id, capabilities), \
    },

const struct vc_channel_entry vc_channel_entries[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(VC_CONTROLLER_NODE, CH_ENTRY)
};
```

## Test Impact

- **Virtual channel unit tests** (new): Test `vc_channel_consume_voltage`, protection decisions, state transitions, ramp engine — all without hardware. Pure state machine tests.
- **Voltage controller tests** (new): Test command routing, mode transitions, pending command drain with mock channel table.
- **Domain unit tests**: Simplified — test system-level operations only, delegate channel tests to virtual channel tests.
- **VC channel stub**: Simplified — implements `vc_channel_hw_api` with no-op stubs.
- **Provider bus tests**: **Deleted**.

## Migration Notes

- `vc_channel_api` is deleted entirely, not wrapped. All drivers must implement `vc_channel_hw_api`.
- The `vc_measurement_snapshot` struct is deleted. Replaced by `vc_measurement_buffer_entry` with bare raw values + timestamps for the monitoring path only.
- Calibration mode: `domain_calibration_set_raw_dac` becomes `vc_channel_cal_set_raw_dac` on the virtual channel, with the voltage controller draining the pending command to `vc_channel_table_set_output`.
- The `struct domain` shrinks to system-level state only. `vc_controller` owns channel state.
- `VC_MAX_CHANNELS` macro changes from `DT_PROP_LEN(vc_controller, channels)` to `DT_CHILD_NUM_STATUS_OKAY(vc_controller)`.
- DTS layout changes: channel nodes move from top-level to children of `vc-controller`. `channel-index` property replaced by `reg`. New `label` property for human-readable channel names. Boards using the old phandle array layout must be updated.
- `jianwei,vc-channel-base.yaml` is deleted — base properties move into the controller's child-binding.
- `jianwei,vc-channel-stub.yaml` updated to work as a child of `vc-controller` with `reg` instead of `channel-index`.

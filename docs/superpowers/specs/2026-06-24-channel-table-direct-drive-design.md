# Channel Table & Direct-Drive Architecture

**Date**: 2026-06-24
**Supersedes**: 2026-06-19-static-provider-bus-design.md, 2026-06-19-vc-channel-provider-design.md

## Summary

Replace the provider bus with a static channel table and direct domain-to-hardware API calls. Eliminate all intermediate queues, config slots, and measurement copies. Measurement acquisition uses interrupt-driven k_work_poll for non-blocking ADC reads. Each individual measurement (voltage or current) is consumed by the domain immediately in the same call chain, enabling sub-cycle protection response.

## Motivation

The current provider bus is a shallow pass-through:

- **Config slots + provider_msgq**: domain runtime computes output drive, writes to slot, enqueues CONFIG_CHANGED, bus dispatches to driver, driver acquires slot, applies to hardware. Five hops for what should be a direct function call.
- **evidence_msgq + measurement buffer**: driver builds snapshot, copies into message queue, worker copies out, copies into buffer, domain reads buffer again for staleness. Four memcpy's for what should be a direct write.
- **Shared work queue**: all channels serialize on one thread. ADS1232 DRDY wait (~400ms) blocks all other channels.
- **Monolithic worker loop**: one channel's voltage measurement wakes the global worker, which checks command queue, iterates all channels, runs periodic tick, publishes full snapshot — all unnecessary overhead.

Topology and capabilities are fixed at compile time from DTS. No runtime dynamic management is needed.

## Architecture

### Layer Diagram

```
┌─────────────────────────────────────────────────────┐
│  Frontend Adapters (Modbus, future CAN/TCP)         │
│  vc_query() for monitoring, vc_dispatch() for cmds  │
├─────────────────────────────────────────────────────┤
│  Domain Runtime Worker Thread                       │
│  Commands, periodic ramp tick, snapshot publishing   │
├─────────────────────────────────────────────────────┤
│  Domain State (domain_state.c)                      │
│  Ramp, protection, recovery, calibration, status    │
│  Per-channel measurement consumption (lock-free)    │
├─────────────────────────────────────────────────────┤
│  Channel Table (vc_channel_table.c)                 │
│  Static dispatch: channel index → device + API      │
├──────────────┬──────────────┬───────────────────────┤
│  hvb_vc_hw   │  lvb_vc_hw   │  future variants...   │
│  DAC+ADC+GPIO│  GPIO only   │                       │
└──────────────┴──────────────┴───────────────────────┘
```

### Data Flow

**Command path** (top-down, synchronous):
```
vc_dispatch(cmd) → runtime worker → domain_state logic
  → vc_channel_set_output(ch, code)   // direct call
  → vc_channel_set_enable(ch, enable) // direct call
```

**Measurement path** (bottom-up, interrupt-driven, per-channel):
```
DRDY interrupt → k_work_poll handler → bit-bang 24 bits
  → write raw value to measurement buffer (direct, no lock)
  → domain_channel_consume_voltage(ch, raw_value)  // immediate
      → calibration scaling
      → protection check
      → if fault: vc_channel_set_output(ch, 0) + set_enable(ch, false)
      → update channel status bits
```

**Query path** (read-only, no synchronization needed):
```
vc_query() → read published snapshot (refreshed by runtime worker periodically)
           → or read measurement buffer directly (monitoring only, stale OK)
```

## Channel Hardware API

```c
struct vc_channel_hw_api {
    int (*set_output)(const struct device *dev, uint16_t code);
    int (*set_enable)(const struct device *dev, bool enable);
    int (*start_sampling)(const struct device *dev);
    int (*stop_sampling)(const struct device *dev);
};
```

All four function pointers must be non-NULL. Variant drivers that lack hardware for a given operation provide a no-op stub returning 0. For example, LVB on/off channels stub `set_output`; channels without ADC stub `start_sampling`/`stop_sampling`.

Removed from current `vc_channel_api`:
- `apply_config` — runtime calls `set_output`/`set_enable` directly
- `notify_config_changed` — no config slots to notify about
- `measure_voltage`, `measure_current` — driver pushes data, runtime doesn't pull
- `get_capabilities` — capabilities come from DTS `vc_channel_entry`, not runtime API
- `start`, `stop` — renamed to `start_sampling`, `stop_sampling` for clarity

## Channel Table

Replaces the provider bus as the static dispatch layer.

### Data Structure

```c
struct vc_channel_entry {
    const struct device *dev;
    uint8_t index;
    uint16_t capabilities;
    struct vc_measurement_buffer_entry *meas;
};
```

The `meas` pointer is resolved at compile time from the iterable section, giving each channel's driver direct write access to its measurement buffer entry.

### API

```c
int vc_channel_set_output(uint8_t ch, uint16_t code);
int vc_channel_set_enable(uint8_t ch, bool enable);
int vc_channel_start_sampling(uint8_t ch);
int vc_channel_stop_sampling(uint8_t ch);

const struct vc_measurement_snapshot *vc_channel_get_measurement(uint8_t ch);
size_t vc_channel_count(void);
uint16_t vc_channel_capabilities(uint8_t ch);
```

Each function is a static table lookup + vtable call. No runtime registration, no iterable section walk. The table is `const` after init, populated from DTS macros.

### Domain Pointer Resolution

The `domain_channel_consume_*` functions are called from the shared work queue thread (not the runtime worker), so they need access to the domain pointer. The channel table holds a `struct domain *` set once at `vc_channel_table_init()` (called by the runtime during init). The domain is a singleton — same as today's `domain_create_static()` pattern.

### DTS Composition (unchanged)

```dts
vc_controller: vc-controller {
    compatible = "jianwei,vc-controller";
    channels = <&vc_ch0 &vc_ch1>;
};
```

The `controller.c` macro expansion builds the `vc_channel_entry[]` table at compile time, same as today.

## Measurement Buffer

### Structure

```c
struct vc_measurement_buffer_entry {
    struct vc_measurement_snapshot snapshot;
};
```

No mutex. The driver writes directly; the runtime and adapters read directly. On a 32-bit MCU with aligned fields, a torn read seeing a half-updated snapshot has no safety consequence — protection runs again on the next measurement.

### Snapshot Fields

```c
struct vc_measurement_snapshot {
    uint8_t channel;
    uint32_t timestamp_ms;
    uint16_t present_mask;     /* which fields are valid this update */
    int32_t raw_voltage;       /* updated independently from current */
    uint32_t voltage_timestamp_ms;
    int32_t raw_current;       /* updated independently from voltage */
    uint32_t current_timestamp_ms;
    uint16_t provider_status;
    uint16_t provider_fault_cause;
};
```

Key change from current design: `voltage_timestamp_ms` and `current_timestamp_ms` are separate. Each ADC read updates its own value and timestamp independently. The `present_mask` indicates which field was just updated, so the domain knows whether to run voltage protection, current protection, or both.

## Interrupt-Driven ADC Sampling (HVB Variant)

### Per-Instance State

```c
struct hvb_vc_data {
    const struct device *dev;
    uint8_t channel;
    struct vc_measurement_buffer_entry *meas;

    /* k_work_poll for non-blocking DRDY wait */
    struct k_work_poll poll_work;
    struct k_poll_signal drdy_signal;
    struct k_poll_event drdy_event;
    struct gpio_callback drdy_cb;

    /* ADC phase state machine */
    enum { ADC_PHASE_VOLTAGE, ADC_PHASE_CURRENT } adc_phase;
    int32_t last_voltage_raw;   /* held across phases if needed */

    uint16_t provider_status;
    struct k_work_delayable next_cycle_work;
};
```

### State Machine

```
start_sampling()
  └→ select A0 for voltage input
  └→ arm DRDY interrupt (falling edge)
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))

DRDY ISR:
  └→ k_poll_signal_raise(&drdy_signal)
  └→ re-enable DRDY interrupt for next edge

k_work_poll handler fires (adc_phase == ADC_PHASE_VOLTAGE):
  └→ bit-bang 24 bits (~50µs) → raw_voltage
  └→ write raw_voltage + voltage_timestamp to meas->snapshot
  └→ domain_channel_consume_voltage(ch, raw_voltage)
       └→ calibration: measured_V = raw * K / 10000 + B
       └→ voltage protection check → fault + output action if exceeded
  └→ adc_phase = ADC_PHASE_CURRENT
  └→ select A0 for current input
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))

DRDY ISR: (same as above)

k_work_poll handler fires (adc_phase == ADC_PHASE_CURRENT):
  └→ bit-bang 24 bits (~50µs) → raw_current
  └→ write raw_current + current_timestamp to meas->snapshot
  └→ domain_channel_consume_current(ch, raw_current)
       └→ calibration: measured_I = raw * K / 10000 + B
       └→ current protection check → fault + output action if exceeded
  └→ adc_phase = ADC_PHASE_VOLTAGE
  └→ schedule next_cycle_work after sample_rate_ms

next_cycle_work handler:
  └→ select A0 for voltage input
  └→ arm DRDY interrupt
  └→ k_work_poll_submit(poll_work, drdy_event, K_MSEC(420))
```

### Timeout Handling

If DRDY never fires (hardware fault), k_work_poll times out after 420ms. The handler checks `poll_work.poll_result`:
- If timeout: set `provider_status |= VC_PROVIDER_STATUS_SAMPLE_ERROR`, set `provider_fault_cause = VC_FAULT_MEASUREMENT`, call `domain_channel_consume_fault(ch, VC_FAULT_MEASUREMENT)`, schedule retry after `sample_rate_ms`.
- Normal operation continues without separate watchdog logic.

### Shared Work Queue Scaling

All channels use one shared `k_work_q`. The work queue thread is occupied for ~60µs per ADC read (50µs bit-bang + 10µs domain processing). For 16 channels with voltage + current:
- 16 × 2 × 60µs = 1.92ms per full sample cycle
- Work queue utilization: <0.2% at 100ms sample rate

No per-channel threads needed.

## Domain Per-Channel Measurement Consumption

### New Domain Functions

```c
void domain_channel_consume_voltage(struct domain *domain,
                                    uint8_t channel, int32_t raw_voltage);
void domain_channel_consume_current(struct domain *domain,
                                    uint8_t channel, int32_t raw_current);
void domain_channel_consume_fault(struct domain *domain,
                                  uint8_t channel, uint16_t fault_cause);
```

Each function:
1. Applies calibration scaling (K/B coefficients)
2. Updates the channel's snapshot (`measured_voltage` or `measured_current`)
3. Runs the relevant protection check (voltage limit or current limit)
4. If protection triggers: calls `vc_channel_set_output(ch, 0)` / `vc_channel_set_enable(ch, false)` immediately
5. Updates channel status bits

These replace the current `domain_consume_measurement()` which takes a full snapshot struct and processes both voltage and current together.

### Lock-Free Per-Channel Processing

The domain's per-channel measurement state (measured values, fault bits, status bits) is only mutated by the measurement call chain (from the shared work queue thread). The runtime worker thread mutates other channel state (config, ramp target, operating mode) from the command path.

These two paths touch **different fields** of the channel state:
- Measurement path writes: `measured_voltage`, `measured_current`, `active_fault_cause`, `fault_history_cause`, `status_bits`, `last_fault_timestamp`
- Command path writes: `configured_target_voltage`, `operational_target_voltage`, `output_enabled`, `ramp_accum_ms`, calibration coefficients

No global domain lock needed for measurement consumption. The runtime worker's domain lock protects command processing and ramp tick only. If a command changes a protection threshold while a measurement is being processed, the new threshold takes effect on the next measurement — acceptable for a control loop running at ~1 Hz.

The one shared field is `operational_target_voltage`, which the ramp tick writes and the protection action reads. Since both the ramp tick and the protection check are called from different threads (runtime worker vs. shared workq), this single field needs an `atomic_set`/`atomic_get` or a lightweight per-channel spinlock. A `volatile int16_t` with aligned access is sufficient on Cortex-M4.

## Runtime Worker Changes

The runtime worker loop simplifies — no measurement draining:

```
vc_runtime_worker:
    while (!stop_requested):
        k_sem_take(&wake, K_MSEC(TICK_INTERVAL_MS))

        // Process commands from Modbus/host
        while (command_queue has items):
            dispatch_command(cmd)
            // may call vc_channel_set_output/set_enable directly

        // Periodic ramp tick (advances operational_target toward configured_target)
        if (tick timeout expired):
            for each channel:
                vc_tick_ramp(ch, dt_ms)
                if ramp changed output:
                    vc_channel_set_output(ch, new_code)

        // Refresh published snapshot for query path
        publish_snapshot()
```

Measurement processing is completely absent from this loop. It happens inline in the k_work_poll handlers on the shared workq thread.

## What Gets Deleted

| Module | Fate |
|---|---|
| `lib/voltage_control/provider_bus.c` | **Deleted** |
| `include/voltage_control/provider_bus.h` | **Deleted** |
| `vc_runtime_config_snapshot` struct | **Deleted** |
| `vc_runtime_config_slot` struct | **Deleted** |
| `vc_provider_msg` struct | **Deleted** |
| `vc_provider_binding` iterable section | **Deleted** |
| `evidence_msgq` (k_msgq) | **Deleted** |
| `provider_msgq` (k_msgq) | **Deleted** |
| Config slot mutexes | **Deleted** |
| Measurement buffer mutexes | **Deleted** |
| `vc_channel_api.apply_config` | **Deleted** |
| `vc_channel_api.notify_config_changed` | **Deleted** |
| `vc_channel_api.measure_voltage/current` | **Deleted** |
| `vc_channel_api.get_capabilities` | **Deleted** |
| `domain_consume_measurement()` (bulk) | **Replaced** by per-field functions |

## What Gets Added

| Module | Purpose |
|---|---|
| `lib/voltage_control/vc_channel_table.c` | Static dispatch: ch index → device + API (~60 lines) |
| `include/voltage_control/vc_channel_table.h` | Channel table API |
| `domain_channel_consume_voltage()` | Per-channel voltage processing (~30 lines) |
| `domain_channel_consume_current()` | Per-channel current processing (~30 lines) |
| `domain_channel_consume_fault()` | Per-channel fault injection (~15 lines) |
| DRDY GPIO interrupt setup in ADS1232 binding | DTS binding extension |
| k_work_poll + phase state machine in hvb_vc_channel | Replaces blocking poll loop (~80 lines) |

**Net**: ~350 lines deleted, ~220 lines added.

## DTS Binding Changes

### `ti,ads1232.yaml` — No changes needed

The `drdy-gpios` property already exists. The driver configures the GPIO interrupt at runtime via `gpio_pin_interrupt_configure_dt()`. No binding schema change required.

### `jianwei,vc-channel-base.yaml` — No changes

Capabilities and channel-index stay as-is.

### `jianwei,hvb-vc-channel.yaml` — No changes

DAC, ADC, enable-gpios, max-raw-dac, sample-rate-ms stay as-is.

### `jianwei,vc-controller.yaml` — No changes

Channel phandle list stays as-is.

## Test Impact

- **Domain unit tests** (`tests/voltage_control/domain/`): Add tests for `domain_channel_consume_voltage`, `domain_channel_consume_current`, `domain_channel_consume_fault`. Existing `domain_consume_measurement` tests are migrated.
- **VC channel stub**: Simplified — no longer needs `apply_config` or measurement callbacks. Just `set_output`, `set_enable`, `start_sampling` (no-op), `stop_sampling` (no-op).
- **Provider bus tests** (`tests/voltage_control/provider_bus/`): **Deleted** — provider bus no longer exists.
- **Runtime tests** (`tests/voltage_control/runtime/`): Updated to test command dispatch without measurement draining. Measurement consumption is tested via domain unit tests.

## Migration Notes

- The `vc_channel_api` rename to `vc_channel_hw_api` is a breaking change for the stub driver and any future variant drivers.
- The `vc_measurement_snapshot` struct gains per-field timestamps, which changes its layout. Modbus adapter register mapping for raw ADC values is unaffected (reads from domain snapshot, not measurement buffer).
- Calibration mode: `domain_calibration_set_raw_dac` currently writes to `snapshot.raw_dac_readback` and expects the provider bus to push it to hardware. In the new design, it calls `vc_channel_set_output(ch, code)` directly. The calibration flow becomes simpler.

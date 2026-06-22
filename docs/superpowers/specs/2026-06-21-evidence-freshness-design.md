# Evidence Freshness Design

## Problem

The domain has no way to know whether its measurement data is current. Measurements flow from channel drivers through the provider bus into the domain, but the domain discards timestamps after consumption and never checks measurement age. If a channel's measurement pipeline stalls (ADC failure, queue overflow), the domain continues operating on stale data with no indication to the operator.

## Decisions

- **Fault action:** flag only — no autonomous output change. Stale measurement is a diagnostic signal; the operator decides the response. Hardware-level failure modes are caught by pre-delivery stress testing.
- **Threshold:** system-wide `CONFIG_VC_MEASUREMENT_STALE_TIMEOUT_MS` (Kconfig). One value for all channels.
- **Scope:** only channels with `CH_CAP_VOLTAGE_MEASUREMENT` or `CH_CAP_CURRENT_MEASUREMENT`. Channels without measurement capability are never flagged stale.
- **Representation:** new `VC_FAULT_STALE` flag (separate from `VC_FAULT_MEASUREMENT` which means ADC sample error) and a new status bit.
- **Architecture:** event-driven only. No periodic tick in production code. The existing channel measurement timer (`k_work_delayable` in the driver) is the event source. Staleness is derived at snapshot publish time from buffer timestamps.

## Architecture

### Measurement Buffer (RAM Iterable Section)

Each channel gets a measurement buffer entry, registered as a Zephyr RAM iterable section with numeric sorting for O(1) indexed access by channel index.

```c
struct vc_measurement_buffer_entry {
    struct k_mutex lock;
    struct vc_measurement_snapshot snapshot;
};
```

**Registration (in driver macro):**

```c
STRUCT_SECTION_ITERABLE_NAMED(vc_measurement_buffer_entry,
    _##n##_, hvb_meas_buf_##n) = { /* zero-init */ };
```

The numeric suffix (`_0_`, `_1_`, ...) matches the channel index. `ITERABLE_SECTION_RAM_NUMERIC` in the linker snippet sorts entries numerically, so `STRUCT_SECTION_GET(vc_measurement_buffer_entry, channel_index, &entry)` gives O(1) access — no iteration loop.

**Linker snippet** (`provider_bus_sections.ld` or a new file):

```
ITERABLE_SECTION_RAM_NUMERIC(vc_measurement_buffer_entry, 4)
```

### Measurement Flow

1. Channel driver's measurement timer fires, reads ADC, publishes `vc_measurement_snapshot` on the provider bus (existing flow, unchanged).
2. Domain runtime receives the event, acquires the buffer entry's mutex, stores the snapshot, releases the mutex.
3. Domain runtime calls `domain_consume_measurement()` as before (calibration, protection, etc.).
4. When publishing a snapshot for frontends, the runtime reads the buffer entry's `timestamp_ms` and computes staleness.

### Staleness Detection

Computed at snapshot publish time — not via a timer or periodic tick.

When `vc_runtime_publish_snapshot()` runs (event-driven, on measurement arrival or command completion):

```
elapsed = k_uptime_get_32() - buffer_entry->snapshot.timestamp_ms
if (elapsed >= CONFIG_VC_MEASUREMENT_STALE_TIMEOUT_MS
    && channel has CH_CAP_VOLTAGE_MEASUREMENT or CH_CAP_CURRENT_MEASUREMENT
    && buffer_entry->snapshot.timestamp_ms != 0):
    set VC_FAULT_STALE in published snapshot fault flags
    set stale status bit
```

The `timestamp_ms != 0` check avoids false positives before the first measurement arrives.

This is a nanosecond integer comparison — no measurement trigger, no delay to frontend reads.

### Data Model Changes

**`domain.h`:**

```c
#define VC_FAULT_STALE  0x0080
```

New status bit `0x0040` in `status_bits` for "measurement stale".

**`vc_channel_snapshot`:** no new fields. Staleness is computed on-the-fly when publishing, not stored persistently.

**Kconfig:**

```kconfig
config VC_MEASUREMENT_STALE_TIMEOUT_MS
    int "Measurement staleness timeout in milliseconds"
    default 5000
    help
      Time after the last measurement arrival before the channel
      is flagged stale. Only applies to channels with voltage or
      current measurement capability.
```

## DTS-Derived Channel Count

### Motivation

`VC_MAX_CHANNELS` is currently a Kconfig value (`default 2, range 1 16`) that must be manually kept in sync with the devicetree. The channel count is already defined in DTS via the `vc-controller` node's `channels` phandle array.

### Change

Replace the Kconfig definition with a DTS-derived compile-time constant:

```c
/* domain.h */
#include <zephyr/devicetree.h>

#define VC_MAX_CHANNELS DT_PROP_LEN(DT_NODELABEL(vc_controller), channels)
```

Remove `config VC_MAX_CHANNELS` from `lib/voltage_control/Kconfig`.

All existing usages (static array sizing in `domain_state.c`, `domain_runtime.c`, `provider_bus.c`, bounds checks) continue to work — the macro is still a compile-time integer constant.

### Test DTS Binding

Tests run on `native_posix` which has no real hardware DTS. A minimal test-only binding provides the compile-time constant without requiring hardware phandles.

**New binding:** `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml`

```yaml
description: Stub VC channel for native_posix tests

compatible: "jianwei,vc-channel-stub"

properties:
  channel-index:
    type: int
    required: true

  capabilities:
    type: int
    required: true
```

No dac, adc, enable-gpios, or other hardware properties.

**Test overlay:** Each test suite gets `boards/native_posix.overlay`:

```dts
/ {
    vc_ch0: vc-channel-0 {
        compatible = "jianwei,vc-channel-stub";
        channel-index = <0>;
        capabilities = <0x000f>;
    };
    vc_ch1: vc-channel-1 {
        compatible = "jianwei,vc-channel-stub";
        channel-index = <1>;
        capabilities = <0x000f>;
    };
    vc_controller: vc-controller {
        compatible = "jianwei,vc-controller";
        channels = <&vc_ch0 &vc_ch1>;
        status = "okay";
    };
};
```

Tests continue to create domains with `domain_create(test_channels, N)` using hand-built channel entry arrays. The overlay only provides the compile-time `VC_MAX_CHANNELS` constant for static array sizing.

## Files Modified

| File | Change |
| --- | --- |
| `include/voltage_control/domain.h` | Replace `VC_MAX_CHANNELS` with DTS macro, add `VC_FAULT_STALE`, add stale status bit |
| `include/voltage_control/provider_bus.h` | Add `vc_measurement_buffer_entry` struct and access API |
| `lib/voltage_control/provider_bus.c` | Buffer init, store, and read functions |
| `lib/voltage_control/provider_bus_sections.ld` | Add `ITERABLE_SECTION_RAM_NUMERIC(vc_measurement_buffer_entry, 4)` |
| `lib/voltage_control/domain_runtime.c` | Store measurement in buffer before `domain_consume_measurement()`; compute staleness in `vc_runtime_publish_snapshot()` |
| `lib/voltage_control/Kconfig` | Remove `config VC_MAX_CHANNELS`, add `config VC_MEASUREMENT_STALE_TIMEOUT_MS` |
| `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c` | Register `vc_measurement_buffer_entry` per channel |
| `dts/bindings/voltage_control/jianwei,vc-channel-stub.yaml` | New test-only binding |
| `tests/voltage_control/*/boards/native_posix.overlay` | New test DTS overlays (domain, runtime, provider_bus, modbus_adapter) |

## Verification

1. All existing tests pass with DTS overlays on native_posix (domain 84, runtime 33, modbus_adapter 17, provider_bus 15).
2. New tests for staleness detection: measurement arrives → no stale flag; time exceeds threshold → stale flag set; fresh measurement → stale flag clears; channel without measurement capability → never stale.
3. `jw_hvb` board build clean — `VC_MAX_CHANNELS` resolves from real DTS.

# Virtual Voltage Channel Runtime Design

Date: 2026-06-19
Status: Deferred draft; not approved for current provider migration implementation

## Purpose

Define the runtime data pipeline between the Domain Runtime Library and Virtual Voltage Channel Providers. This covers the command path (domain → provider), the measurement path (provider → domain), transport primitives, concurrency ownership, and the two-clock model implementation.

## Two-Clock Model

The production runtime uses two independent timing sources:

- **Policy clock**: owned by the Domain Runtime Service. Advances ramp deadlines, recovery cooldowns, stale-data detection, and republishes Domain Snapshots on a domain-owned Zephyr timer or work item.
- **Evidence clock**: owned by each Virtual Voltage Channel Provider. The provider samples hardware at its configured cadence and publishes Measurement Snapshots as they become available.

The policy clock does not block on hardware sampling. The evidence clock does not drive product policy. They meet where the domain worker consumes a published Measurement Snapshot from a `k_msgq`.

## Data Pipeline

```text
Command Path (domain → provider):
  Domain Worker (policy step)
    → writes Runtime Config Snapshot to shared per-channel config
    → signals via k_event that new config is available
  Provider Worker (safe boundary, e.g. workqueue item)
    → reads latest Runtime Config Snapshot
    → calls device-level driver APIs (DAC, GPIO) to apply it
    → does NOT own ramp, protection, or recovery policy

Measurement Path (provider → domain):
  Provider Worker (sample cadence)
    → reads raw hardware (ADC, GPIO status)
    → publishes Measurement Snapshot into per-channel k_msgq
    → does NOT apply calibration or product policy
  Domain Worker (event loop)
    → receives Measurement Snapshot from k_msgq
    → applies calibration coefficients
    → evaluates protection rules
    → updates recovery state
    → republishes Domain Snapshot
```

## Data Structures

### Runtime Config Snapshot (Domain → Provider)

One per channel. The domain writes it; the provider reads the latest version at safe boundaries. This is a "latest wins" structure protected by a version counter and a wakeup event.

```c
struct vc_runtime_config {
    uint16_t raw_output_code;   // meaningful if CH_CAP_RAW_OUTPUT_DRIVE
    bool     output_enable;     // always meaningful (mandatory capability)
    uint32_t version;           // monotonic; incremented by domain on each write
};
```

The domain increments `version` after writing. The provider compares against its last-applied `version`. Only applies new config when `version` has changed.

### Measurement Snapshot (Provider → Domain)

Published by the provider into a `k_msgq`. The domain receives every published snapshot.

```c
struct vc_measurement_snapshot {
    uint8_t  channel;           // provider channel index
    int32_t  raw_voltage;       // meaningful if CH_CAP_VOLTAGE_MEASUREMENT
    int32_t  raw_current;       // meaningful if CH_CAP_CURRENT_MEASUREMENT
    uint32_t generation;        // monotonic publication counter per channel
    uint32_t timestamp;         // monotonic time of acquisition (ms or ticks)
};
```

`generation` increments on each publication even if values did not change. `timestamp` is the provider's monotonic time when the sample was acquired. The domain uses `generation` and `timestamp` for freshness/stale-data policy.

## Transport Choice

| Path | Mechanism | Rationale |
|---|---|---|
| Domain → Provider config | Shared `struct vc_runtime_config` + `k_event` signal | Latest-wins: only the most recent config matters. |
| Provider → Domain evidence | `k_msgq` (fixed-size queue, 2-4 slots) | Every measurement must be processed; discard-oldest if queue fills. |
| Domain timer wakeup | Zephyr system workqueue item or dedicated timer callback | Periodic policy step. |
| Provider sampling wakeup | Zephyr system workqueue item, resubmit on sample-rate-ms | Provider-owned cadence. |

## Provider Runtime Pattern

Each VC channel provider device has:

```c
struct vc_channel_provider_data {
    struct vc_runtime_config      config;        // latest domain intent
    uint32_t                      config_version; // last applied version
    struct k_event                config_event;  // signaled when config.version changes

    struct k_msgq                 evidence_q;    // measurement evidence → domain
    char __aligned(4)             evidence_buf[4 * sizeof(struct vc_measurement_snapshot)];

    struct k_work                 sample_work;   // deferred sample work item
    uint32_t                      sample_period_ms; // from DTS sample-rate-ms

    uint32_t                      generation;    // publication counter
};
```

The domain owns the per-channel `struct vc_runtime_config`. The provider sees it as shared readable state and signals `config_event` on version change. The provider does not write to the config.

### Sample Work Item

```c
// In the provider's init function:
k_work_init(&data->sample_work, provider_sample_work_handler);

// Handler:
static void provider_sample_work_handler(struct k_work *work)
{
    struct vc_channel_provider_data *data = ...;
    struct vc_measurement_snapshot snap = {0};
    int32_t raw;

    snap.channel = data->channel_index;
    snap.timestamp = k_uptime_get_32();
    snap.generation = ++data->generation;

    // Capture supported measurement paths
    if (cap_has(data->capabilities, CH_CAP_VOLTAGE_MEASUREMENT)) {
        adc_read(data->adc_dev, &raw);
        snap.raw_voltage = raw;
    }
    if (cap_has(data->capabilities, CH_CAP_CURRENT_MEASUREMENT)) {
        adc_read(data->adc_dev, &raw);
        snap.raw_current = raw;
    }

    // Publish; discard if queue is full (domain fell behind)
    k_msgq_put(&data->evidence_q, &snap, K_NO_WAIT);

    // Resubmit at sample period
    k_work_reschedule(&data->sample_work, K_MSEC(data->sample_period_ms));
}
```

## Domain Worker Pattern

The Domain Runtime Service is the single writer of `struct domain`. It runs in a dedicated thread or workqueue-delayed work item. It blocks on multiple inputs:

```c
void domain_worker_entry(void *p1, void *p2, void *p3)
{
    struct domain *d = (struct domain *)p1;

    while (true) {
        // Wait for: timer expiry OR evidence on any channel's msgq
        // Use k_poll or k_msgq_get with timeout

        // 1. Drain all available measurement snapshots
        for (int ch = 0; ch < d->channel_count; ch++) {
            struct vc_measurement_snapshot snap;
            while (k_msgq_get(&d->channels[ch].evidence_q, &snap,
                              K_NO_WAIT) == 0) {
                domain_consume_measurement(d, ch, &snap);
            }
        }

        // 2. Advance policy timers (ramp, cooldown, stale detection)
        domain_policy_step(d);

        // 3. Republish Domain Snapshot for adapters
        domain_publish_snapshot(d);
    }
}
```

The domain never calls `sensor_sample_fetch()`. The provider publishes evidence independently. The domain consumes what is available.

## Concurrent Access Rules

- `struct domain` is mutated only from the domain worker thread.
- Adapter reads (Modbus input/holding) read the published Domain Snapshot, not live domain internals.
- Adapter writes (Modbus holding writes) enqueue domain commands and return after domain acceptance (synchronous at the command boundary per the architecture spec).
- Provider sampling runs on the system workqueue. Providers read `struct vc_runtime_config` at the safe boundary and write to `k_msgq`.
- `k_msgq` is thread-safe for single-producer single-consumer. Each channel has one provider (producer) and one domain worker (consumer).

## Stale Measurement Handling

The domain worker tracks per-channel freshness in its policy step:

```c
// In domain_policy_step:
for (int ch = 0; ch < d->channel_count; ch++) {
    uint32_t age = now - d->channels[ch].last_measurement_timestamp;
    if (age > d->stale_timeout_ms) {
        // Mark per-source stale status in Domain Snapshot
        // Follow Protection Mode rules for stale evidence
    }
}
```

Stale timeout is configured per-board from DTS or Kconfig. Capability awareness: a channel without voltage/current measurement capability does not get marked stale for those sources.

## Provider `vc_channel_api` Callback Semantics

The callbacks in `vc_channel_api` are defined in `2026-06-19-vc-channel-provider-design.md`. Their runtime contract is:

- `set_output(code)`: writes raw DAC code through the underlying device driver. Called by the domain when publishing a Runtime Config Snapshot. Must be non-blocking (uses pre-calibrated code; no computation).
- `set_enable(enable)`: controls the output gate GPIO. Called by the domain. Must be non-blocking.
- `measure_voltage(value)`: synchronous read of the latest raw ADC value via the underlying device driver. Called by the provider sample work handler, not by the domain. Returns raw ADC code.
- `measure_current(value)`: same for current path.
- `get_capabilities()`: returns the static DTS-derived capability bitmask. Called once at domain init; result is cached.

The domain calls `set_output` and `set_enable` when publishing a Runtime Config Snapshot. The provider calls `measure_voltage` and `measure_current` from its sampling work item.

## Integration with Existing Design

| Concept | Defined Where |
|---|---|
| DTS bindings and provider API shape | `2026-06-19-vc-channel-provider-design.md` |
| Domain product behavior (ramp, protection, recovery, calibration) | `2026-06-15-voltage-control-domain-behavior.md` |
| Production runtime architecture (layers, data ownership, concurrency model) | `2026-06-18-zephyr-native-production-runtime-architecture.md` |
| Measurement/Config Snapshots, transport, provider runtime, domain worker | This document |

## Non-Goals

- Final choice of `k_msgq` vs `k_fifo` vs `zbus` for the evidence path. `k_msgq` is proposed as the preferred candidate; the implementation plan may confirm or replace it.
- Provider workqueue isolation. All providers may share the Zephyr system workqueue for V1.
- Settings/NVS persistence wiring.
- Domain worker thread priority and stack sizing.
- Calibration Mode raw control path integration with providers (deferred to a later slice).

## References

- `docs/superpowers/specs/2026-06-19-vc-channel-provider-design.md`
- `docs/superpowers/specs/2026-06-18-zephyr-native-production-runtime-architecture.md`
- `docs/superpowers/specs/2026-06-15-voltage-control-domain-behavior.md`
- Zephyr `k_msgq`: https://docs.zephyrproject.org/3.7.0/kernel/services/data_passing/message_queues.html
- Zephyr `k_event`: https://docs.zephyrproject.org/3.7.0/kernel/services/synchronization/events.html
- Zephyr workqueues: https://docs.zephyrproject.org/3.7.0/kernel/services/threads/workqueue.html

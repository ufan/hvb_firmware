# Production Runtime And Channel Provider Design

Date: 2026-06-18
Status: Draft for review

## Purpose

This document defines the production runtime interaction between the Zephyr-native Domain Runtime Library and Virtual Channel Providers.

It refines the runtime portion of `2026-06-18-zephyr-native-production-runtime-architecture.md` and replaces the deferred direction in `2026-06-19-vc-channel-runtime-design.md` once approved. It keeps the already-decided architecture boundaries from `UBIQUITOUS_LANGUAGE.md`, `CONTEXT.md`, and `AGENTS.md`:

- Product policy belongs in the Domain Runtime Library.
- Frontend Adapters submit domain commands and read Domain Snapshots only.
- Virtual Channel Providers expose static capabilities, apply raw runtime intent, and publish raw hardware evidence.
- Measurement Snapshots are provider-published evidence, not synchronous hardware reads requested by frontend or domain reads.
- Runtime Config Snapshots are complete, versioned domain-approved intent, not partial field updates.

## Design Decision

The production runtime uses a Provider-Worker Runtime model.

The Domain Runtime Worker is the single writer of domain state. It validates commands, advances product policy, consumes raw evidence, publishes Domain Snapshots, and publishes per-channel Runtime Config Snapshots.

Each Virtual Channel Provider owns hardware interaction. It applies the latest Runtime Config Snapshot at provider-owned safe boundaries, samples raw hardware on its own cadence, and publishes Measurement Snapshots back to the domain. Providers remain hardware-shaped and policy-free.

```text
Frontend Adapter
  -> Domain Command
  -> Domain Runtime Worker
  -> Runtime Config Snapshot
  -> Virtual Channel Provider Worker
  -> raw hardware

Virtual Channel Provider Worker
  -> Measurement Snapshot
  -> Domain Runtime Worker
  -> Domain Snapshot
  -> Frontend Adapter
```

Adapter reads never touch hardware. Domain reads never synchronously sample hardware. Physical progress, stale evidence, and provider faults are visible through Domain Snapshots.

## Domain Runtime Worker

The Domain Runtime Worker owns product state and product policy. It is the only execution context that mutates the domain state machine.

It owns:

- Domain command validation.
- Operating mode transitions.
- Configured Target Voltage and Operational Target Voltage state.
- Ramping, cooldown, retry, and derating timers.
- Calibration policy and coefficient application.
- Raw-to-calibrated unit conversion.
- Protection, recovery, Active Fault Block, and Fault History behavior.
- Per-source freshness and stale evidence policy.
- Domain Snapshot publication.
- Runtime Config Snapshot publication.

Its inputs are:

- Domain commands from Frontend Adapters.
- A periodic policy tick.
- Measurement Snapshots from Virtual Channel Providers.
- Provider apply/status evidence.

Its outputs are:

- Command acceptance or rejection.
- Latest coherent Domain Snapshot.
- Per-channel Runtime Config Snapshot.

Frontend command completion means the domain has accepted or rejected the command and, when accepted, has made the resulting runtime intent available to the provider path. It does not mean physical output has reached the requested target.

## Virtual Channel Provider Worker

Each Virtual Channel Provider owns its hardware interaction and timing. A provider may use Zephyr workqueues, delayed work, interrupts, GPIO, SPI, ADC, DAC, sensor, or bus APIs as appropriate for its hardware.

It owns:

- Applying latest raw output drive values.
- Applying latest output enable state.
- Applying calibration-mode raw output state.
- Enforcing provider-level raw bounds such as maximum DAC code.
- Sampling raw voltage/current/status at the provider cadence.
- Reporting provider availability, apply failures, ADC timeouts, hardware status, and interlock evidence.
- Publishing Measurement Snapshots.

It must not own:

- Calibrated voltage/current meaning.
- Ramping policy.
- Protection latching.
- Retry or derate policy.
- Modbus exception behavior.
- User-visible state.

The existing `struct vc_channel_api` callbacks are interpreted as provider-side hardware operations. Provider runtime code may call `set_output`, `set_enable`, `measure_voltage`, and `measure_current` while applying config or sampling. In this V1 runtime model, the domain does not call provider callbacks directly; it publishes Runtime Config Snapshots and consumes Measurement Snapshots.

## Runtime Config Snapshot

A Runtime Config Snapshot is domain-approved raw intent for one channel. It is complete, versioned, and latest-wins.

Minimum semantic fields:

```c
struct vc_runtime_config_snapshot {
	uint8_t channel;
	uint32_t version;
	uint16_t capability_flags;

	bool output_enable;
	uint16_t raw_output_drive;

	bool calibration_mode;
	bool calibration_output_enable;
	uint16_t calibration_raw_output_drive;

	bool force_safe_state;
};
```

Rules:

- The domain publishes a complete snapshot whenever runtime intent changes.
- Providers apply only the latest snapshot version.
- Providers do not infer product policy from field changes.
- `force_safe_state` means the provider must drive the physical channel to the safe raw state: raw drive zero and output disabled where supported.
- In Normal and Automatic modes, providers apply `output_enable` and `raw_output_drive`.
- In Calibration Mode, providers apply calibration raw output fields while still honoring provider bounds and hard safety evidence.
- Unsupported fields are ignored only when the static capability model says the channel lacks that capability; a provider that advertises a capability but cannot apply it is faulty.

## Measurement Snapshot

A Measurement Snapshot is raw provider evidence. It is not a product read model.

Minimum semantic fields:

```c
struct vc_measurement_snapshot {
	uint8_t channel;
	uint32_t generation;
	uint32_t timestamp_ms;

	uint16_t present_mask;
	int32_t raw_voltage;
	int32_t raw_current;
	uint16_t provider_status;
	uint16_t provider_fault_cause;
};
```

Rules:

- `generation` increments for every publication, even if raw values did not change.
- `timestamp_ms` is acquisition time, not domain-consumption time.
- `present_mask` identifies which raw evidence fields are meaningful.
- ADC timeout is status/fault evidence, not a calibrated measurement.
- A channel without voltage/current measurement capability does not publish fake measurement values and is not stale for missing unsupported paths.
- Providers publish hardware status and interlock evidence through the same raw evidence path.

## Domain Snapshot

The Domain Snapshot is the only host-visible read model for Frontend Adapters.

It includes:

- Configured Target Voltage.
- Operational Target Voltage.
- Calibrated measured voltage and current where supported and fresh.
- Output, ramping, cooldown, retry, and mode status.
- Per-source stale or invalid evidence status.
- Active Fault Block and Fault History.
- Last protection output action.
- Channel capability flags.
- Calibration raw readback fields when mode and capability allow them.
- Provider availability and hardware fault projection.

Modbus, shell, and future frontends read this snapshot. They do not access providers directly and do not trigger hardware sampling.

## Recommended Zephyr Transport

The V1 runtime uses simple Zephyr primitives rather than a full runtime bus.

| Path | Mechanism | Reason |
| --- | --- | --- |
| Frontend Adapter to domain command | `k_msgq` | Commands must be serialized and processed in order. |
| Domain policy tick | dedicated thread timeout or delayed work | Ramping, cooldown, retry, and stale checks need a policy clock. |
| Domain to provider config | shared per-channel snapshot plus version and signal | Runtime config is latest-wins; intermediate versions need not queue. |
| Provider to domain evidence | per-channel `k_msgq` | Evidence publications should be processed in order. |
| Domain Snapshot to adapters | coherent copied snapshot, protected by mutex or double-buffer versioning | Reads need coherence, not event history. |

`zbus` remains a possible later refactor if multiple services need typed pub/sub. It is not required for the first production runtime slice.

## Failure Semantics

Provider failures flow back as evidence. The domain decides product-visible behavior.

| Provider evidence | Domain behavior |
| --- | --- |
| DAC write failed | Record provider hardware evidence; apply protection/fault policy; force safe runtime config if blocking. |
| Output enable GPIO failed | Treat as hardware fault evidence; block output and expose Active Fault Block if unsafe. |
| ADC timeout | Mark source invalid/stale according to capability; apply stale measurement protection policy. |
| Interlock active | Treat as hard safety evidence; force safe config and block output until clear. |
| Provider unavailable at boot | Channel is unavailable/faulted; frontend must not see it as controllable. |
| Provider claims capability but lacks callback | Provider bug; channel is unavailable/faulted at runtime initialization. |

The provider may reject raw values that violate provider-level bounds, such as max DAC code. Rejection is reported as provider evidence. The provider must not decide whether to latch a fault, retry, derate, or map the failure to a Modbus exception.

## Startup Sequence

Production startup is ordered to keep outputs safe before frontend commands are accepted.

1. Zephyr initializes devices according to init priorities.
2. Providers initialize hardware into safe output state: raw drive zero where available and output disabled where available.
3. `hvb_controller` gathers the `vc_controller` channel table from devicetree.
4. The app verifies mandatory providers are ready.
5. The domain runtime initializes with static channel capabilities and variant defaults.
6. The domain publishes initial safe Runtime Config Snapshots.
7. Providers apply and confirm the initial safe state.
8. The domain publishes the initial Domain Snapshot.
9. Modbus and shell adapters start accepting commands.

Safe-state confirmation is required for enabled providers before a channel is presented as controllable. If confirmation fails, the app may still start frontends, but the affected channel must appear unavailable or faulted rather than controllable.

## Calibration Mode Interaction

Calibration Mode remains domain-owned product behavior.

The domain owns unlock, mode entry/exit, single-active-channel rule, coefficient write restrictions, sample command semantics, and Calibration Commit rules. Providers only apply raw calibration output intent and publish raw evidence.

Rules:

- Entering Calibration Mode publishes safe Runtime Config Snapshots for all channels: raw drive zero and output disabled.
- Calibration Output Enable is separate from normal Output Enable.
- Only the domain enforces the single-active calibration channel rule.
- Providers enforce raw hardware bounds and hard safety evidence.
- Calibration sample commands are domain commands that request provider sample work. Completion is observed through calibration sample status and later Domain Snapshots; adapter reads still do not synchronously sample hardware.
- Exiting Calibration Mode publishes safe Runtime Config Snapshots and clears calibration raw output state.

## Testing Strategy

Tests verify public behavior and stable seams.

Required tests:

- Domain command acceptance does not imply physical completion in Domain Snapshot.
- Adapter reads return the latest Domain Snapshot without provider calls.
- Runtime Config Snapshot is complete, versioned, and latest-wins.
- Fake providers apply config at safe boundaries and report apply failures as evidence.
- Domain consumes Measurement Snapshots and updates calibrated values, freshness, faults, and status.
- Missing capability is not stale evidence.
- Stale supported evidence follows protection policy.
- Provider unavailable at startup prevents controllable channel exposure.
- Calibration Mode publishes safe config on entry and exit.
- Calibration raw paths respect channel capabilities.
- Modbus and shell adapters depend on the domain facade, not provider APIs.

Hardware validation must separately prove boot-safe output state, DAC zero/write behavior, ADS1232 sampling behavior, enable GPIO polarity, interlock behavior, and RS-485 Modbus operation on the real HVB Variant.

## Non-Goals

This design does not define:

- Final C type names or file layout.
- Exact thread priority or stack size.
- Settings/NVS schema.
- Full persistence migration behavior.
- Formal promotion of the runtime into a Zephyr subsystem.
- A `zbus`-based runtime bus.
- Final host-tool UX behavior.

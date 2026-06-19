# Async Runtime And Domain State Machine Design

Date: 2026-06-19
Status: Draft for review

## Purpose

This document defines the next production runtime slice after the runtime/provider seam landed in `2026-06-18-production-runtime-channel-provider-design.md`.

The goal is to make voltage-control domain mutation asynchronous and serialized while introducing Zephyr's State Machine Framework (`smf`) where it clarifies domain behavior. This slice prepares for later active provider workers, settings persistence, freshness policy, and startup safe-state confirmation.

## Decision

Use a dedicated Domain Runtime Worker thread with Zephyr queues for incoming work. The runtime thread is the only production execution context that mutates `struct domain`.

Use Zephyr `smf` inside the domain layer for product behavior state transitions:

- System operating state: Normal, Automatic, Calibration.
- Per-channel lifecycle state: Disabled/Safe, Enabled/Holding, Ramping, Fault Latched, Retry Cooldown, Calibration Output, Unavailable.

Do not use `smf` as a message bus. Runtime transport remains `k_msgq`, semaphores, mutexes, and timer/work primitives. Do not put hardware calls in `smf` handlers. Providers remain hardware-shaped and apply domain-published Runtime Config Snapshots.

## Runtime Worker

`struct vc_runtime` owns:

- Borrowed `struct domain *domain`.
- Runtime thread and stack.
- Command queue for frontend/domain commands.
- Measurement evidence queue for provider-published evidence.
- Tick timing state for policy advancement.
- Mutex-protected read access for runtime config snapshots and, later, coherent domain snapshots.
- Stop signal for clean destruction.

The runtime loop processes work in a deterministic order:

1. Drain pending commands in FIFO order.
2. Drain pending measurement evidence in FIFO order.
3. Run policy tick when due.
4. Publish updated Runtime Config Snapshots through existing domain state.

Command completion means the domain accepted or rejected the command after serialized processing. It still does not mean physical output has reached the requested target.

## Command Model

Add a runtime command envelope:

```c
enum vc_runtime_command_type {
	VC_RUNTIME_CMD_SET_OPERATING_MODE,
	VC_RUNTIME_CMD_SET_SYSTEM_CONFIG,
	VC_RUNTIME_CMD_SET_CHANNEL_CONFIG,
	VC_RUNTIME_CMD_OUTPUT_ACTION,
	VC_RUNTIME_CMD_FAULT_COMMAND,
	VC_RUNTIME_CMD_CALIBRATION_UNLOCK,
	VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE,
	VC_RUNTIME_CMD_CALIBRATION_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_SAMPLE,
	VC_RUNTIME_CMD_CALIBRATION_COMMIT,
	VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC,
	VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
	VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
};
```

Each command carries the minimum payload needed to call the existing domain API. The first implementation may provide blocking submission helpers that enqueue a command and wait on a semaphore for the acceptance result. That preserves current Modbus-facing semantics while moving mutation into the runtime thread.

Queue overflow returns `VC_ERR_UNSAFE_STATE`. Null inputs return `VC_ERR_INVALID_VALUE`.

## Domain State Machines

The domain keeps product policy. `smf` is introduced as an implementation structure, not a public API.

System states:

- `Normal`: direct target/config/output commands follow normal policy.
- `Automatic`: policy tick may ramp operational target toward configured target.
- `Calibration`: normal output commands are rejected; calibration raw output commands are legal under existing unlock and single-active-channel rules.

Per-channel states:

- `DisabledSafe`: output disabled and raw output drive zero.
- `EnabledHolding`: output enabled and operational target stable.
- `Ramping`: operational target moving toward configured target.
- `FaultLatched`: active blocking fault; output intent forced safe.
- `RetryCooldown`: auto-retry delay active after a fault.
- `CalibrationOutput`: calibration output path active for one channel.
- `Unavailable`: provider/channel unavailable; controllability blocked.

Runtime events are translated into domain events before `smf_run_state()`:

- Command event.
- Measurement evidence event.
- Policy tick event.
- Stop/unavailable event, reserved for later startup safety work.

State handlers may update domain state, faults, operational targets, and Runtime Config Snapshot versioning. They must not call provider APIs or access hardware.

## Compatibility And Migration

Existing domain APIs remain for tests and pure domain use. Production app and frontend adapters should migrate to runtime command helpers so production mutation is serialized.

Existing `vc_runtime_submit_measurement()` and `vc_runtime_get_channel_config()` stay public. Measurement submission becomes queued. Runtime config read remains a copied read protected by the runtime lock until active provider workers are introduced.

The current `domain_tick()` behavior can be wrapped by the first runtime policy tick. Later work can move more tick behavior into per-channel `smf` handlers without changing the public runtime API.

## Testing Strategy

Native runtime tests must verify:

- Runtime starts and stops its thread cleanly.
- Blocking command submission returns the domain acceptance status.
- Commands are processed FIFO.
- Measurement evidence is serialized through the runtime and updates domain snapshots.
- Policy tick advances domain behavior only from the runtime context.
- Queue overflow and null inputs return explicit errors.
- Runtime config reads remain coherent while commands and measurements are processed.

Domain tests must verify public transition behavior, not private state handler internals:

- Calibration mode rejects normal output action through the runtime path.
- Faulted channel publishes safe runtime config.
- Ramping/tick behavior remains unchanged after introducing `smf`.

Board build verification remains:

- `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`

## Out Of Scope

This slice does not implement:

- Active provider worker loops.
- Provider sampling cadence.
- Settings persistence.
- Per-source stale evidence policy.
- Startup safe-state confirmation.
- `zbus` or a general runtime event bus.

Those are later slices in the agreed order: active providers, settings persistence, evidence freshness, startup safety confirmation.

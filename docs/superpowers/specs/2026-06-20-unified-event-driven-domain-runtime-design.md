# Unified Event-Driven Domain Runtime Library Design

Date: 2026-06-20
Status: design
Supersedes: 2026-06-19-async-runtime-smf-design.md (SMF scaffolding), 2026-06-19-vc-channel-runtime-design.md

## Problem

The current architecture splits the Domain Runtime Library into two separate types (`struct domain` in `domain.c` and `struct vc_runtime` in `runtime.c`) connected by a borrowing pointer. This contradicts UBIQUITOUS_LANGUAGE.md which defines a single "Domain Runtime Library." The split creates three concrete problems:

1. **NULL dereference crash.** `domain_tick()` at `runtime.c:136` passes `NULL` noise arrays to `vc_tick_measure()`, which dereferences them unconditionally. This is because `domain_tick` mixes simulation code (`vc_tick_measure`) with production policy.

2. **Modbus data race.** `modbus_adapter.c` calls `domain_*` functions directly, bypassing the runtime's serialized command queue. The Modbus server runs in the Zephyr Modbus thread while the runtime worker runs in its own thread — concurrent unsynchronized access to `struct domain`.

3. **Periodic tick is wrong.** `domain_tick` polls protection and recovery on a timer. Protection should fire immediately when a violating measurement arrives, not at the next tick interval. Recovery cooldown accumulates `dt_ms` — a timer-driven model fits better.

4. **SMF is scaffolded but unused.** All SMF state handlers are no-ops. The imperative if/switch chains in domain functions replicate state machine transitions without the framework.

## Design

### Unified type

Merge `struct domain` and `struct vc_runtime` into `struct vc_domain_runtime`. One logical unit. Two files for readability:

- `domain_state.c` — state machine, policy logic, validation, protection, calibration, internal tick-like helpers
- `domain_runtime.c` — thread, queues, timer scheduling, snapshot publication, command dispatch

The runtime thread owns all mutation. No external caller touches domain state directly. Internal policy functions remain `static` or internal-linkage so unit tests and the simulator can call them through a test-visible header or by linking `domain_state.c` directly.

```c
struct vc_domain_runtime {
    /* SMF */
    struct vc_domain_smf_ctx     smf;

    /* Identity */
    const struct vc_channel_entry *ch_entry;
    size_t                        channel_count;

    /* System state */
    enum vc_operating_mode        operating_mode;
    struct vc_system_config        sys_cfg;
    uint16_t                       system_fault_cause;
    uint8_t                        cal_unlock_step;
    bool                           cal_unlocked;

    /* Per-channel state */
    struct vc_channel_config       channels[VC_MAX_CHANNELS];
    struct vc_channel_snapshot     snapshots[VC_MAX_CHANNELS];
    struct vc_channel_runtime      ch_runtime[VC_MAX_CHANNELS];

    /* Runtime execution */
    struct k_thread                thread;
    struct k_mutex                 lock;
    struct k_msgq                  command_queue;
    struct k_sem                   wake;
    bool                           stop_requested;

    /* Published Domain Snapshot */
    struct vc_published_snapshot   published;
    struct k_mutex                 snapshot_lock;

    /* Inline buffers */
    char command_buffer[CONFIG_VC_RUNTIME_COMMAND_QUEUE_DEPTH *
                        sizeof(struct vc_runtime_work_item)];
};
```

### Event model

The runtime thread reacts to three event sources. No periodic tick.

| Event | Source | Trigger |
| --- | --- | --- |
| Command | Frontend adapters via `vc_runtime_submit_command()` | `k_sem_give(&wake)` after `k_msgq_put` |
| Measurement | Provider bus evidence queue | `k_sem_give(&wake)` after `vc_provider_bus_publish_measurement` |
| Timer | Kernel timer callback | `k_sem_give(&wake)` with timer-event flag |

The worker loop:

```
while (!stop_requested) {
    k_sem_take(&wake, K_FOREVER);
    drain_command_queue();       /* process all pending commands */
    drain_evidence_queue();      /* process all pending measurements */
    handle_timer_events();       /* process any expired timer flags */
    publish_snapshot();          /* update published domain snapshot */
}
```

Each drain/handle function runs under `k_mutex_lock(&lock)`. After all events in a cycle are processed, the published snapshot is updated once.

### Per-field command model

Replace full-struct commands (`VC_RUNTIME_CMD_SET_SYSTEM_CONFIG`, `VC_RUNTIME_CMD_SET_CHANNEL_CONFIG`) with per-field commands:

```c
enum vc_config_field {
    /* System fields */
    VC_FIELD_OPERATING_MODE,
    VC_FIELD_SLAVE_ADDRESS,
    VC_FIELD_BAUD_RATE_CODE,
    VC_FIELD_RECOVERY_POLICY_MODE,
    VC_FIELD_AUTO_RETRY_DELAY,
    VC_FIELD_AUTO_RETRY_MAX_COUNT,
    VC_FIELD_AUTO_RETRY_WINDOW,
    VC_FIELD_VOLTAGE_SAFE_BAND_PCT,
    VC_FIELD_CURRENT_SAFE_BAND_PCT,

    /* Channel fields */
    VC_FIELD_CONFIGURED_TARGET_VOLTAGE,
    VC_FIELD_RAMP_UP_STEP,
    VC_FIELD_RAMP_UP_INTERVAL,
    VC_FIELD_RAMP_DOWN_STEP,
    VC_FIELD_RAMP_DOWN_INTERVAL,
    VC_FIELD_VOLTAGE_PROTECTION_MODE,
    VC_FIELD_VOLTAGE_PROT_OUT_ACTION,
    VC_FIELD_VOLTAGE_LIMIT_THRESHOLD,
    VC_FIELD_CURRENT_PROTECTION_MODE,
    VC_FIELD_CURRENT_PROT_OUT_ACTION,
    VC_FIELD_CURRENT_LIMIT_THRESHOLD,
    VC_FIELD_AUTO_DERATE_STEP,
    VC_FIELD_SAVE_TARGET_POLICY,
    VC_FIELD_OUTPUT_CAL_K,
    VC_FIELD_OUTPUT_CAL_B,
    VC_FIELD_MEASURED_V_CAL_K,
    VC_FIELD_MEASURED_V_CAL_B,
    VC_FIELD_MEASURED_I_CAL_K,
    VC_FIELD_MEASURED_I_CAL_B,
};
```

New command types:

```c
VC_RUNTIME_CMD_SET_SYSTEM_FIELD,    /* payload: { field, value } */
VC_RUNTIME_CMD_SET_CHANNEL_FIELD,   /* payload: { field, value } */
```

Command payload for field writes:

```c
struct vc_field_write {
    enum vc_config_field field;
    uint16_t value;  /* cast to int16_t/enum as needed per field */
};
```

The runtime thread receives `{field_id, value}`, reads the current config internally, applies the single field, validates, and stores. The read-modify-write is serialized inside the runtime thread — no race by construction.

The existing action commands (`VC_RUNTIME_CMD_OUTPUT_ACTION`, `VC_RUNTIME_CMD_FAULT_COMMAND`, `VC_RUNTIME_CMD_CALIBRATION_*`, `VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION`, `VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION`) remain unchanged — they are already atomic operations.

The full-struct commands (`VC_RUNTIME_CMD_SET_SYSTEM_CONFIG`, `VC_RUNTIME_CMD_SET_CHANNEL_CONFIG`) are removed. Unit tests that need full-struct writes call the internal policy functions directly.

### SMF state definitions

#### Domain states

| State | Entry | Run | Exit |
| --- | --- | --- | --- |
| `Normal` | Clear cooldown timers | Process commands with normal policy | — |
| `Automatic` | — | Process commands; auto-recovery active | Clear cooldown timers |
| `Calibration` | Reset calibration outputs; clear unlock | Process calibration commands only | Reset calibration outputs; clear unlock |

#### Channel states

| State | Entry | Run (on event) | Exit |
| --- | --- | --- | --- |
| `DisabledSafe` | Force safe state to provider | Reject output unless enable action received | — |
| `EnabledHolding` | — | At target; monitor for protection | — |
| `Ramping` | Compute direction/interval, start k_timer | Each timer tick: advance one step, check protection. If target reached → transition to EnabledHolding. | Cancel timer |
| `FaultLatched` | Record fault timestamp, apply protection action | Reject enable; accept fault clear → DisabledSafe | — |
| `RetryCooldown` | Start cooldown timer | On timer expiry: check safe band → retry or re-latch | Cancel timer |
| `CalibrationOutput` | — | Accept raw DAC writes, calibration sample/commit | Force safe state |
| `Unavailable` | — | Reject all operations | — |

State transitions on events:

```
DisabledSafe  + OutputAction(Enable)     → Ramping (if target != current) or EnabledHolding
EnabledHolding + TargetChanged            → Ramping
EnabledHolding + ProtectionTriggered      → FaultLatched
Ramping       + TargetReached             → EnabledHolding
Ramping       + ProtectionTriggered       → FaultLatched
FaultLatched  + FaultClear (Normal mode)  → DisabledSafe
FaultLatched  + CooldownStart (Auto mode) → RetryCooldown
RetryCooldown + CooldownExpiry            → Ramping (retry) or FaultLatched (exhausted)
Any           + CalibrationModeEnter      → CalibrationOutput (if capable) or DisabledSafe
CalibrationOutput + CalibrationModeExit   → DisabledSafe
```

### Ramping

Ramping is a channel SMF state, not a periodic function.

1. On entering `Ramping`: compute ramp direction and interval from channel config. If step=0 or interval=0, jump directly to target and transition to `EnabledHolding`. Otherwise, start a `k_timer` with period = `interval * 100ms`.
2. On each timer expiry: the timer callback sets a flag and gives `wake`. The worker thread advances `operational_target_voltage` by one step toward `configured_target_voltage`. After advancing, call `check_protection()`. If target reached, cancel timer and transition to `EnabledHolding`.
3. If config changes target during ramping: re-evaluate direction. If already at new target, exit ramping.

### Protection

Protection fires as an internal `check_protection(channel)` call from three sites:

1. **Measurement event.** After `domain_consume_measurement()` applies calibration and updates `measured_voltage`/`measured_current`.
2. **Ramp step.** After advancing `operational_target_voltage` (the new value might cross a threshold if the threshold was lowered).
3. **Config change.** When a protection threshold is written that is now below the current measured value.

This replaces the periodic `vc_tick_protection` with immediate response at the state change that causes the fault.

### Recovery cooldown

In Automatic mode, when a fault is latched and recovery policy allows retry:

1. Enter `RetryCooldown` state.
2. Start a `k_timer` with one-shot delay = `auto_retry_delay * 1000ms`.
3. On timer expiry: check if measurement is within safe band. If yes, clear active fault and transition to `Ramping`. If no (or retry count exhausted), re-latch.

Same timer mechanism as ramping — consistent event-driven pattern.

### Status bits

Updated after every state-changing event in the worker cycle, not periodically. The `publish_snapshot()` call at the end of each worker cycle computes status bits from current state.

### Published Domain Snapshot

After each worker cycle (all commands, measurements, and timer events processed), the runtime publishes a complete snapshot:

```c
struct vc_published_snapshot {
    struct vc_system_snapshot   system;
    struct vc_channel_snapshot  channels[VC_MAX_CHANNELS];
    struct vc_channel_config   configs[VC_MAX_CHANNELS];
    struct vc_system_config    sys_config;
};
```

Protected by `snapshot_lock` (mutex). The runtime writes under lock after each cycle. Frontend adapters read under lock. The lock is held only for the duration of a `memcpy` — no computation under lock.

Frontend API:

```c
enum vc_status vc_runtime_get_system_snapshot(
    struct vc_domain_runtime *rt,
    struct vc_system_snapshot *snap);

enum vc_status vc_runtime_get_channel_snapshot(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    struct vc_channel_snapshot *snap);

enum vc_status vc_runtime_get_system_config(
    struct vc_domain_runtime *rt,
    struct vc_system_config *cfg);

enum vc_status vc_runtime_get_channel_config(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    struct vc_channel_config *cfg);
```

These read from the published snapshot, not from internal state. Frontend reads never touch the runtime thread's working state.

### Timer scheduling

The runtime owns a small set of `k_timer` instances:

```c
struct vc_channel_runtime {
    /* ... existing fields ... */
    struct k_timer ramp_timer;
    struct k_timer cooldown_timer;
    uint32_t       timer_event_flags;  /* bitfield: ramp_expired, cooldown_expired */
};
```

Timer callbacks set a flag in `timer_event_flags` and give `wake`. The worker thread checks flags and handles expired timers. This avoids running domain logic in ISR/timer callback context.

### Provider bus interaction

Unchanged. The provider bus config slots, evidence queue, and provider messages continue to work as they do now. The unified runtime publishes Runtime Config Snapshots to provider bus slots after each mutation cycle, just as the current `vc_runtime_publish_all_configs()` does.

### `domain_tick` removal

`domain_tick()` is removed from the public API. Its sub-functions are restructured:

| Current | New location |
| --- | --- |
| `vc_tick_ramp` | `Ramping` state handler, driven by `k_timer` |
| `vc_tick_measure` | Removed from production code. Stays in `demos/modbus_sim/` as direct snapshot manipulation. |
| `vc_tick_protection` | `check_protection()` called from measurement, ramp step, and config change handlers |
| `vc_tick_recovery` | `RetryCooldown` state handler, driven by `k_timer` |
| `vc_tick_status_bits` | `compute_status_bits()` called during `publish_snapshot()` |

### Uptime

The runtime reads `k_uptime_get()` internally when computing the published snapshot. `VC_RUNTIME_CMD_SET_UPTIME` and `domain_set_uptime()` are removed.

### Public API

```c
/* Creation */
struct vc_domain_runtime *vc_domain_runtime_create(
    const struct vc_channel_entry *channels, size_t count);
struct vc_domain_runtime *vc_domain_runtime_create_static(
    const struct vc_channel_entry *channels, size_t count);
void vc_domain_runtime_destroy(struct vc_domain_runtime *rt);

/* Commands (frontend adapters call these) */
enum vc_status vc_runtime_submit_command(
    struct vc_domain_runtime *rt,
    const struct vc_runtime_command *cmd,
    k_timeout_t timeout);

/* Convenience wrappers */
enum vc_status vc_runtime_set_system_field(
    struct vc_domain_runtime *rt,
    enum vc_config_field field, uint16_t value,
    k_timeout_t timeout);
enum vc_status vc_runtime_set_channel_field(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    enum vc_config_field field, uint16_t value,
    k_timeout_t timeout);
enum vc_status vc_runtime_output_action(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    enum vc_output_action action,
    k_timeout_t timeout);

/* Snapshot reads (frontend adapters call these) */
enum vc_status vc_runtime_get_system_snapshot(
    struct vc_domain_runtime *rt,
    struct vc_system_snapshot *snap);
enum vc_status vc_runtime_get_channel_snapshot(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    struct vc_channel_snapshot *snap);
enum vc_status vc_runtime_get_system_config(
    struct vc_domain_runtime *rt,
    struct vc_system_config *cfg);
enum vc_status vc_runtime_get_channel_config(
    struct vc_domain_runtime *rt,
    uint8_t channel,
    struct vc_channel_config *cfg);

/* Topology queries */
uint16_t vc_runtime_get_supported_channel_count(
    const struct vc_domain_runtime *rt);
uint16_t vc_runtime_get_variant_id(
    const struct vc_domain_runtime *rt);
```

### Modbus adapter changes

`vc_mb_adapter_create` takes `struct vc_domain_runtime *` instead of `struct domain *`.

**Writes:** Translate Modbus register address → `enum vc_config_field` + value. Call `vc_runtime_set_system_field()` or `vc_runtime_set_channel_field()`. Action registers map to existing action commands.

**Reads:** Call `vc_runtime_get_system_snapshot()` / `vc_runtime_get_channel_snapshot()` / config getters. No lock contention with runtime.

### Test strategy

**Unit tests** (link `domain_state.c` directly, no thread):
- Call internal policy functions directly
- Verify validation, config changes, protection, calibration, state transitions
- No queue or timer — pure logic testing

**Integration tests** (create full `vc_domain_runtime`):
- Submit commands through queue
- Verify published snapshot reflects changes
- Test per-field command round-trips
- Test timer-driven ramping end-to-end
- Test measurement → protection → fault → recovery lifecycle

**Modbus adapter tests** (new):
- Register address → field ID translation
- Capability gating
- Error mapping

### Simulator

`demos/modbus_sim/` calls internal policy functions directly for `measured_voltage`/`measured_current` simulation. It does not use `domain_tick`. It creates a `vc_domain_runtime` but manipulates snapshot fields through the policy API for simulation purposes.

## File changes

| Current file | Action | New file |
| --- | --- | --- |
| `lib/voltage_control/domain.c` | Rename, extract simulation code | `lib/voltage_control/domain_state.c` |
| `lib/voltage_control/runtime.c` | Rename, merge with domain | `lib/voltage_control/domain_runtime.c` |
| `include/voltage_control/domain.h` | Keep: types, field enum, internal policy API | same |
| `include/voltage_control/runtime.h` | Keep: public frontend API, updated command types | same |
| `lib/voltage_control/modbus_adapter.c` | Rewrite read/write paths | same |
| `include/voltage_control/modbus_adapter.h` | Update `vc_mb_adapter_create` signature | same |
| `applications/hvb_controller/src/main.c` | Update initialization | same |
| `demos/modbus_sim/src/main.c` | Absorb simulation code, remove `domain_tick` | same |

## Migration path

The plan implements this spec in sequential steps (Phase 3.1–3.7 of the remediation plan):

1. **3.1** Add `enum vc_config_field` and per-field command infrastructure
2. **3.2** Merge types, rename files, remove `domain_tick`
3. **3.3** Activate SMF state handlers
4. **3.4** Implement published domain snapshot
5. **3.5** Rewrite Modbus adapter
6. **3.6** Fix runtime_config_version on channel config changes
7. **3.7** Update all tests

Each step builds on the previous. Tests are updated incrementally — existing tests continue to compile at each step (with modifications as APIs change).

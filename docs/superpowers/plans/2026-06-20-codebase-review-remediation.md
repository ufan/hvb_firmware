# Codebase Review Remediation Plan

Date: 2026-06-20

## Context

Full codebase review against design specs, ubiquitous language, implementation plans, and current code. The review identified critical bugs, architectural violations, and gaps. Discussion resolved key design decisions that reshape the plan from bug-fix patches into an architectural evolution that fixes the bugs as a side effect.

## Key Design Decisions

These decisions were resolved through grilling before implementation:

| Decision | Resolution | Rationale |
| --- | --- | --- |
| Domain / runtime split | Merge into one unified Domain Runtime Library (two files for readability, one logical unit) | UBIQUITOUS_LANGUAGE.md defines a single "Domain Runtime Library". The split created artificial communication overhead and prevented the domain from scheduling timers. |
| `domain_tick` | Remove entirely. Event-driven architecture. | `domain_tick` mixes simulation code (`vc_tick_measure`) with production policy. Periodic ticking conflicts with the SMF state machine design. Production domain reacts to events: commands, measurements, timers. |
| Ramping model | SMF Ramping state with timer-driven steps. Entry decides if ramp is needed, starts timer at ramp interval. Each step computes next operational voltage. One-shot exit when target reached. | Clean event-driven model. Timer is a runtime concern; domain enters/exits states. |
| Protection triggers | Event-driven: fires on measurement arrival, after ramp step, and on config change (threshold lowered below current value). | Immediate response at the state change that causes the fault. Faster than periodic polling. |
| Recovery cooldown | Timer event from runtime, same mechanism as ramp. | Consistent event-driven pattern. |
| Status bits | Updated after every state-changing event, not periodic. | No stale status between ticks. |
| Frontend read path | Published Domain Snapshot. Runtime publishes complete snapshot after each mutation cycle. Frontends read without blocking. | Spec says "Frontend Adapters read Domain Snapshots only." Decouples read latency from runtime processing. |
| Frontend write path | Per-field commands `{field_id, value}` through command queue. Read-modify-write happens inside serialized runtime thread. | Race-free by construction. Smaller queue messages (~20 bytes vs ~52 bytes). ~4x less memcpy per Modbus write. Multi-frontend safe. |
| Field ID namespace | Separate domain field enum (`enum vc_config_field`). Protocol translation stays in adapters. | Domain Runtime Library stays protocol-neutral. Adapters translate their wire formats to domain field IDs. Supports variant boards with different protocol layouts. |
| Simulation code | Stays in `demos/modbus_sim/` as lightweight direct-domain path. No runtime stack in simulator. | Simulator is a dev tool, not a production integration test. Domain policy functions remain internally callable for simulator and unit tests. |
| Test strategy | Both unit tests (direct policy function calls) and integration tests (through command queue). | Unit tests verify policy logic fast. Integration tests verify serialization, event ordering, and snapshot publication. |

## Variant Context

The architecture serves a board family, not just the current HVB:

- **jw_hvb**: 1/2/4 channels, full capabilities (DAC, ADC, voltage/current measurement)
- **jw_lvb** (planned): 10 channels, voltage on/off only
- **Future**: up to 16 channels per board

Channel topology is composed from devicetree at build time. The Domain Runtime Library knows topology statically. UI/protocol adapters reflect the underlying channel topology, not the other way around.

---

## Phase 1 -- Independent Driver Fixes

Ship immediately as independent commits. Orthogonal to the architectural work.

### 1.1 AD5541 DAC byte order

**Problem:** AD5541 expects MSB-first SPI data. Driver sends native `uint16_t` on little-endian STM32F429 -- bytes are swapped. DAC outputs wrong values.

**Location:** `drivers/dac/ad5541/ad5541.c:48-59`

**Fix:** Apply `sys_cpu_to_be16()` to `tx_data` before writing. Include `<zephyr/sys/byteorder.h>`.

**Verification:** `jw_hvb` board build succeeds. Hardware verification deferred to bring-up.

---

### 1.2 Provider `SAMPLE_ERROR` never clears

**Problem:** `VC_PROVIDER_STATUS_SAMPLE_ERROR` is OR'd into `data->provider_status` on ADC failure but never cleared on success. A single transient error permanently latches the provider into fault state.

**Location:** `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c:109, 119, 123-126`

**Fix:** Clear `VC_PROVIDER_STATUS_SAMPLE_ERROR` at the start of each measurement cycle. Set it only if a measurement actually fails in the current cycle.

**Verification:** `jw_hvb` board build succeeds.

---

### 1.3 Move `dac_channel_setup` to init

**Problem:** `hvb_vc_set_output` calls `dac_channel_setup` on every invocation. This is a one-time setup call that adds unnecessary SPI overhead on every write. Return value ignored.

**Location:** `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c:35-36`

**Fix:** Move `dac_channel_setup` into `hvb_vc_init`, check return value. Remove from `hvb_vc_set_output`.

**Verification:** `jw_hvb` board build succeeds.

---

### 1.4 Fix `DT_INST_PROP(0, ...)` hardcoded in LOG_INF

**Problem:** `hvb_vc_init` prints `DT_INST_PROP(0, channel_index)` regardless of which DT instance is being initialized. All instances log channel 0's index.

**Location:** `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c:278`

**Fix:** Replace `DT_INST_PROP(0, channel_index)` with `cfg->channel_index`.

**Verification:** `jw_hvb` board build succeeds.

---

### 1.5 Store `dev` back-pointer in `hvb_vc_data`

**Problem:** `hvb_vc_work_handler` iterates all provider bindings via `STRUCT_SECTION_FOREACH` to find its own device pointer every sample period. O(N) per cycle. Silent permanent channel disable if lookup fails.

**Location:** `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c:186-216`

**Fix:** Add `const struct device *dev` to `hvb_vc_data`. Set during `hvb_vc_init`. Remove section iteration from work handler.

**Verification:** `jw_hvb` board build succeeds.

---

### 1.6 ADS1232 dedicated workqueue

**Problem:** DRDY polling blocks up to 420ms per channel read. Two sequential reads (voltage + current) block the system workqueue for up to 840ms.

**Location:** `drivers/sensor/ads1232/ads1232.c:92-99`, `drivers/voltage_control/hvb_vc_channel/hvb_vc_channel.c`

**Fix:** Run the HVB provider work handler on a dedicated workqueue instead of the system workqueue. Define a dedicated `K_THREAD_STACK_DEFINE` + `k_work_queue` in the HVB provider driver.

**Verification:** `jw_hvb` board build succeeds.

---

## Phase 2 -- Code Quality

Independent of the architecture. Can land before, during, or after Phase 3.

### 2.1 Fix `controller.c` ODR violation

**Problem:** Re-declares `struct vc_channel_entry` inline instead of using the definition from `domain.h`.

**Location:** `lib/voltage_control/controller.c:23-27`

**Fix:** Include `voltage_control/domain.h`, remove inline struct definition.

---

### 2.2 Deduplicate `CH_CAP_*` constants

**Problem:** Identical capability constants in `include/regmap/hvb_regs.h` and `dts/bindings/include/jianwei,vc-channel-capabilities.h`. Maintenance risk if they diverge.

**Fix:** Single source of truth. Remove duplicates from `hvb_regs.h`, include the capabilities header instead.

---

### 2.3 Fix mid-file include in `provider_bus.c`

**Problem:** `#include "voltage_control/vc_channel.h"` at line 118, between function definitions.

**Fix:** Move to top of file.

---

### 2.4 Replace C++ style comments with C89 comments

**Location:** `modbus_adapter.h:12`, `main.c:62,144,151`

---

### 2.5 Fix HVB channel base binding inheritance

**Problem:** `jianwei,hvb-vc-channel.yaml` re-declares all properties instead of inheriting from `jianwei,vc-channel-base.yaml`.

**Fix:** Add `include: jianwei,vc-channel-base.yaml`, remove redundant property declarations.

---

### 2.6 Remove or defer `interlock-gpios`

**Problem:** DTS binding defines optional `interlock-gpios` but driver never reads or reports it.

**Fix:** Remove from binding until hardware interlock design is done. Add note to project-status.md.

---

## Phase 3 -- Architectural Evolution: Unified Event-Driven Domain Runtime Library

This is the core work. It replaces the current `domain.c` + `runtime.c` split and `domain_tick` model with a unified, event-driven Domain Runtime Library. The critical bugs (NULL dereference, Modbus data race) cannot exist in the new architecture.

### 3.0 Design spec

Write a design spec for the unified event-driven Domain Runtime Library before implementation. The spec should cover:

- Unified `struct vc_domain_runtime` replacing separate `struct domain` + `struct vc_runtime`
- Event model: commands, measurements, timer events (no periodic tick)
- SMF state definitions with entry/run/exit handlers for domain and channel states
- Ramping: SMF Ramping state, timer-driven steps, one-shot exit
- Protection: fires on measurement, ramp step, and config change
- Recovery cooldown: timer-driven, same mechanism as ramp
- Status bits: updated after every state-changing event
- Published Domain Snapshot for frontend reads
- Per-field command model with `enum vc_config_field` and `{field_id, value}` payload
- Command queue and timer scheduling owned by runtime thread
- Internal policy functions remain callable for unit tests and simulator
- Provider bus interaction unchanged (config slots, evidence queue, provider messages)

### 3.1 Domain field enum and per-field command infrastructure

Define `enum vc_config_field` with domain-level field identifiers for all system and channel config fields. Add per-field command types:

```
VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
```

Payload: `{ enum vc_config_field field; uint16_t value; }`

Implement per-field dispatch: read current config internally, apply field, validate, store. All serialized inside the runtime thread.

**Files:**
- `include/voltage_control/domain.h` -- add `enum vc_config_field`
- `include/voltage_control/runtime.h` -- add new command types, shrink command union
- `lib/voltage_control/domain_state.c` -- per-field config setters with validation

---

### 3.2 Unified Domain Runtime Library structure

Merge `struct domain` and `struct vc_runtime` into `struct vc_domain_runtime`. One logical unit, two files for readability:

- `domain_state.c` -- state machine, policy logic, validation, protection, calibration
- `domain_runtime.c` -- thread, queues, timer scheduling, snapshot publication, command dispatch

Remove `domain_tick` and its simulation sub-functions (`vc_tick_measure`). Move simulation code to `demos/modbus_sim/`.

**Files:**
- `lib/voltage_control/domain_state.c` (rename from `domain.c`)
- `lib/voltage_control/domain_runtime.c` (rename from `runtime.c`)
- `include/voltage_control/domain.h` -- unified type, internal policy API
- `include/voltage_control/runtime.h` -- public frontend API (submit command, get snapshot)
- `demos/modbus_sim/src/main.c` -- absorb simulation-specific code

---

### 3.3 Event-driven SMF activation

Activate SMF state handlers, replacing imperative if/switch chains:

**Domain states:**
- `Normal` -- direct commands follow normal policy
- `Automatic` -- auto-start, recovery policy active
- `Calibration` -- raw output commands, normal output rejected

**Channel states:**
- `DisabledSafe` -- output off, safe state
- `EnabledHolding` -- output enabled, at target
- `Ramping` -- entry: compute interval, start timer. Run (on timer): advance one step, check protection. If target reached, one-shot exit to EnabledHolding.
- `FaultLatched` -- active fault block, awaiting clear
- `RetryCooldown` -- timer-driven cooldown, then retry
- `CalibrationOutput` -- raw DAC control active
- `Unavailable` -- channel not usable

Protection fires as an internal `check_protection(channel)` function called from:
- Measurement event handler (inside `domain_consume_measurement`)
- Ramp step handler (after advancing operational target)
- Config change handler (when thresholds change)

Status bits update after every state-changing event.

---

### 3.4 Published Domain Snapshot

Runtime publishes a complete `vc_system_snapshot` + `vc_channel_snapshot[]` after each mutation cycle (command processed, measurement consumed, timer event handled). Frontends read from the published snapshot without blocking the runtime thread.

**Mechanism:** Double-buffered or mutex-protected published snapshot. Runtime writes to the snapshot after each event. Frontend reads acquire a lightweight lock or read from the inactive buffer.

---

### 3.5 Modbus adapter integration

Rewrite Modbus adapter to use the unified Domain Runtime Library:

- `vc_mb_adapter_create` takes `struct vc_domain_runtime *`
- **Writes:** Translate Modbus register addresses to `enum vc_config_field` + value. Submit per-field commands through `vc_runtime_submit_command`. Action registers (`CH_OUTPUT_ACTION`, `CH_FAULT_CMD`, `CH_CAL_*`, `SYS_PARAM_ACTION`) map to existing action command types.
- **Reads:** Pull from published Domain Snapshot. No lock contention with runtime.
- Modbus adapter owns protocol-to-domain translation only. No domain state access.

**Files:**
- `include/voltage_control/modbus_adapter.h` -- change `vc_mb_adapter_create` signature
- `lib/voltage_control/modbus_adapter.c` -- rewrite read/write paths
- `applications/hvb_controller/src/main.c` -- update initialization

---

### 3.6 Bump `runtime_config_version` in channel config changes

**Problem:** `domain_set_channel_config` does not notify providers of config changes.

**Fix:** In the per-field config setter (3.1), after applying a field that affects runtime-visible state, bump `runtime_config_version` and publish updated Runtime Config Snapshot.

---

### 3.7 Update tests

**Unit tests** (`tests/voltage_control/domain/`):
- Continue testing policy functions directly (no thread, no queue)
- Remove `domain_tick`-dependent tests, replace with event-driven equivalents
- Add tests for per-field config setters
- Add tests for protection firing on measurement, ramp step, and config change

**Integration tests** (`tests/voltage_control/runtime/`):
- Test through command queue submission
- Test published Domain Snapshot reads
- Test per-field command round-trips
- Test timer-driven ramping end-to-end
- Test measurement → protection → fault lifecycle

**Modbus adapter tests** (new: `tests/voltage_control/modbus_adapter/`):
- Address decode for system, channel, and extension blocks
- Capability gating for channel input and holding registers
- Calibration mode register access restrictions
- Register write → per-field command → domain state verification
- Register read from published snapshot
- Error mapping from `vc_status` to `vc_mb_result`

**Provider bus dispatch tests** (new or extended):
- Fake iterable provider binding with mock `vc_channel_api`
- `vc_provider_bus_start_all`, `notify_channel`, `dispatch_one`

---

## Phase 4 -- Production Hardening

After the architectural evolution lands.

### 4.1 Modbus unit ID and baud rate from Kconfig/devicetree

**Problem:** Hardcoded `unit_id = 1` and `baud = 115200` in `main.c`.

**Fix:** Source from Kconfig defaults or devicetree. Domain system config provides runtime-changeable values; initial values come from build-time sources.

---

### 4.2 Heartbeat refactor

**Problem:** Heartbeat toggle and uptime update in main's `while(1)` loop.

**Fix:** Move heartbeat to `k_work_delayable`. Domain reads `k_uptime_get()` internally instead of receiving uptime externally. Remove `VC_RUNTIME_CMD_SET_UPTIME` and `domain_set_uptime`.

---

## Phase 5 -- Deferred Items

Tracked in `docs/superpowers/project-status.md`:

| Item | Status | Notes |
| --- | --- | --- |
| Settings persistence (NVS) | `deferred` | Design Zephyr settings/NVS behavior for save/load/factory-reset |
| Evidence freshness | `deferred` | Design stale measurement detection and capability-aware status policy |
| Startup safety confirmation | `deferred` | Design provider output-safe confirmation before command acceptance |
| Host-tools deferred validation | `deferred` | Validate factory GUI workflow and deployment scripts |

---

## Execution Order

```
Phase 1 (Driver fixes -- independent commits, ship immediately):
  1.1  AD5541 byte order fix
  1.2  Clear provider SAMPLE_ERROR on success
  1.3  Move dac_channel_setup to init
  1.4  Fix DT_INST_PROP(0) in LOG_INF
  1.5  Store dev back-pointer in hvb_vc_data
  1.6  ADS1232 dedicated workqueue

Phase 2 (Code quality -- independent, any order):
  2.1  Fix controller.c ODR
  2.2  Deduplicate CH_CAP_* constants
  2.3  Fix mid-file include
  2.4  C89 comments
  2.5  Fix base binding inheritance
  2.6  Remove/defer interlock-gpios

Phase 3 (Architectural evolution -- sequential):
  3.0  Design spec
  3.1  Domain field enum and per-field command infrastructure
  3.2  Unified Domain Runtime Library structure
  3.3  Event-driven SMF activation
  3.4  Published Domain Snapshot
  3.5  Modbus adapter integration
  3.6  Runtime config version on channel config changes
  3.7  Update tests (unit, integration, modbus adapter, provider bus)

Phase 4 (Production hardening -- after Phase 3):
  4.1  Modbus unit ID/baud from config
  4.2  Heartbeat refactor

Phase 5 (Deferred):
  Settings, freshness, startup safety, host-tools
```

## Verification Gate

After each phase, run:

```
# Domain unit tests
west twister -T tests/voltage_control/domain -p native_posix

# Runtime integration tests
west twister -T tests/voltage_control/runtime -p native_posix

# Modbus adapter tests (after Phase 3.5)
west twister -T tests/voltage_control/modbus_adapter -p native_posix

# Board build
west build -b jw_hvb applications/hvb_controller -- -DCONFIG_VC_MAX_CHANNELS=2
```

Update `docs/superpowers/project-status.md` with verification evidence after each phase.

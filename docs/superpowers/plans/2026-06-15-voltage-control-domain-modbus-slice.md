# Voltage Control Domain Modbus Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first vertical slice of the voltage-control architecture: a protocol-neutral domain core, an HVB variant profile, and a Modbus register adapter smoke path using the post-grill Modbus v2 register map.

**Architecture:** The domain core owns product concepts such as Configured Target Voltage, Operational Target Voltage, Output Drive Level, Output Enable, Active Fault Block, Fault History, operating mode, and recovery policy. The Modbus adapter owns register packing, address decoding, exception mapping, and self-clearing command readback. No Modbus adapter code may directly touch hardware.

**Tech Stack:** Zephyr RTOS 3.7.2, C, Zephyr ztest, Zephyr Modbus serial support for later transport integration, STM32F429 `jw_hvb` board.

---

## Prerequisites

Use these docs as authoritative inputs:

- `CONTEXT.md`
- `docs/superpowers/specs/2026-06-15-high-level-firmware-architecture-design.md`
- `ref/modbus_interface.md`

Do not use stale concepts from earlier drafts:

- Do not expose internal runtime enable intent as a Modbus register.
- Do not mix output actions and fault-clear operations into one register.
- Do not create separate, drifting enums for host output transitions and protection-triggered output transitions.
- Do not implement removed global fault-clear or command-result registers.

## File Structure

| File | Responsibility |
|---|---|
| `include/voltage_control/domain.h` | Protocol-neutral domain types and commands |
| `include/voltage_control/variant.h` | Board-family variant profile types |
| `include/voltage_control/modbus_adapter.h` | Register adapter API and Modbus-like result codes |
| `lib/CMakeLists.txt` | Adds repository shared libraries |
| `lib/voltage_control/CMakeLists.txt` | Builds the shared voltage-control library |
| `lib/voltage_control/domain.c` | Domain state, validation, output/fault commands, fault bookkeeping |
| `lib/voltage_control/hvb_variant.c` | Current HVB fixed two-channel variant profile |
| `lib/voltage_control/modbus_adapter.c` | Modbus v2 address decoding and register packing over domain API |
| `tests/voltage_control/domain/*` | ztest app for domain behavior |
| `tests/voltage_control/modbus_adapter/*` | ztest app for adapter mapping |
| `applications/hvb_controller/CMakeLists.txt` | Links shared library into app |
| `applications/hvb_controller/src/main.c` | Initializes HVB variant/domain smoke path |

## Canonical Domain Types

Implement these conceptual enums in `include/voltage_control/domain.h`.

```c
enum vc_status {
	VC_OK = 0,
	VC_ERR_UNSUPPORTED_CHANNEL = -1,
	VC_ERR_INVALID_VALUE = -2,
	VC_ERR_INVALID_COMMAND = -3,
	VC_ERR_UNSAFE_STATE = -4,
	VC_ERR_STORAGE = -5,
};

enum vc_operating_mode {
	VC_OPERATING_MODE_NORMAL = 0,
	VC_OPERATING_MODE_AUTOMATIC = 1,
};

enum vc_output_action {
	VC_OUTPUT_ACTION_NONE = 0,
	VC_OUTPUT_ACTION_ENABLE = 1,
	VC_OUTPUT_ACTION_DISABLE_GRACEFUL = 2,
	VC_OUTPUT_ACTION_DISABLE_IMMEDIATE = 3,
	VC_OUTPUT_ACTION_FORCE_OUTPUT_ZERO = 4,
	VC_OUTPUT_ACTION_CLAMP = 5,
};

enum vc_channel_fault_command {
	VC_CHANNEL_FAULT_COMMAND_NONE = 0,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_ACTIVE = 1,
	VC_CHANNEL_FAULT_COMMAND_CLEAR_HISTORY = 2,
};

enum vc_protection_mode {
	VC_PROTECTION_MODE_DISABLED = 0,
	VC_PROTECTION_MODE_FLAG_ONLY = 1,
	VC_PROTECTION_MODE_APPLY_OUTPUT_ACTION = 2,
};

enum vc_recovery_policy_mode {
	VC_RECOVERY_MANUAL_LATCH = 0,
	VC_RECOVERY_AUTO_RETRY = 1,
	VC_RECOVERY_AUTO_DERATE_RETRY = 2,
	VC_RECOVERY_NEVER_RETRY = 3,
};
```

Validation rules:

| Context | Valid output actions |
|---|---|
| Host Channel Output Action | None, Enable, Disable Graceful, Disable Immediate |
| Voltage Protection Output Action | None, Disable Graceful, Disable Immediate, Force Output Zero, Clamp |
| Current Protection Output Action | None, Disable Graceful, Disable Immediate, Force Output Zero |

## Canonical Register Maps

Implement `ref/modbus_interface.md` exactly for the first slice where fields are in scope.

System holding block:

| Offset | Name |
|---:|---|
| 0 | Operating Mode |
| 1 | Slave Address |
| 2 | Baud Rate Code |
| 3 | Recovery Policy Mode |
| 4 | Auto Retry Delay |
| 5 | Auto Retry Max Count |
| 6 | Auto Retry Window |
| 7 | Voltage Safe Band % |
| 8 | Current Safe Band % |
| 9-38 | Reserved |
| 39 | System Param Action |

Channel holding block:

| Offset | Name |
|---:|---|
| 0-1 | Configured Target Voltage |
| 2 | Channel Output Action |
| 3 | Channel Fault Command |
| 4 | Ramp Up Step |
| 5 | Ramp Up Interval |
| 6 | Ramp Down Step |
| 7 | Ramp Down Interval |
| 8 | Voltage Protection Mode |
| 9 | Voltage Protection Output Action |
| 10-11 | Voltage Limit Threshold |
| 12 | Current Protection Mode |
| 13 | Current Protection Output Action |
| 14-15 | Current Limit Threshold |
| 16 | Auto Derate Step |
| 17 | Save Target Policy |
| 18-23 | Calibration, K as UINT16 and B as INT16 |
| 24-38 | Reserved |
| 39 | Channel Param Action |

## Task 1: Domain And Variant Test Scaffold

**Files:**
- Create: `include/voltage_control/domain.h`
- Create: `include/voltage_control/variant.h`
- Create: `tests/voltage_control/domain/CMakeLists.txt`
- Create: `tests/voltage_control/domain/prj.conf`
- Create: `tests/voltage_control/domain/src/main.c`

- [ ] Create headers with the canonical types above plus structs for system config, channel config, channel snapshot, and domain state.
- [ ] Keep internal runtime enable intent inside domain state; do not expose it as a Modbus register.
- [ ] Add domain tests for HVB variant defaults: 2 channels, active mask `0x0003`, Normal mode, safe-band defaults `10`, recovery policy Manual Latch.
- [ ] Add domain tests rejecting unsupported channel 2 for current HVB.
- [ ] Add domain tests rejecting Configured Target Voltage above `20000` V x10.
- [ ] Add domain tests proving Fault History remains after clearing Active Fault Block.
- [ ] Add domain tests proving `Flag Only` sets Fault History but not Active Fault Block.
- [ ] Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`.
- [ ] Expected: initial run fails before implementation files exist.

## Task 2: Domain Core And HVB Variant

**Files:**
- Create: `lib/CMakeLists.txt`
- Create: `lib/voltage_control/CMakeLists.txt`
- Create: `lib/voltage_control/domain.c`
- Create: `lib/voltage_control/hvb_variant.c`
- Modify: `applications/hvb_controller/CMakeLists.txt`

- [ ] Implement HVB variant profile: variant id `1`, 2 channels, active mask `0x0003`, Automatic mode supported, environment sensor present, voltage range `0..20000` V x10.
- [ ] Implement domain initialization with Normal mode, Manual Latch recovery, safe-band defaults `10`, safe target `0`, calibration defaults `K=10000`, `B=0`.
- [ ] Implement `vc_domain_set_operating_mode`; switching modes must not change currently running channel state.
- [ ] Implement output action validation by context.
- [ ] Implement channel output actions: Enable, Disable Graceful, Disable Immediate. Force Output Zero and Clamp are rejected for host action context.
- [ ] Implement channel fault commands: clear active only when safe, clear history always if domain policy allows.
- [ ] Implement fault recording using calibrated-value semantics at the domain boundary; the first slice may accept already-calibrated values from tests.
- [ ] Run: `west build -b native_posix -d build/test-domain tests/voltage_control/domain`.
- [ ] Expected: domain tests pass.

## Task 3: Modbus Adapter Test Scaffold

**Files:**
- Create: `include/voltage_control/modbus_adapter.h`
- Create: `tests/voltage_control/modbus_adapter/CMakeLists.txt`
- Create: `tests/voltage_control/modbus_adapter/prj.conf`
- Create: `tests/voltage_control/modbus_adapter/src/main.c`

- [ ] Define `enum vc_modbus_result` with `OK=0`, `ILLEGAL_FUNCTION=1`, `ILLEGAL_ADDRESS=2`, `ILLEGAL_VALUE=3`, `DEVICE_FAILURE=4`.
- [ ] Add tests reading system input offsets 0-5: protocol `2.0`, variant id `1`, supported channel count `2`, active mask `0x0003`.
- [ ] Add tests for system holding writes: Operating Mode at offset 0, Recovery Policy Mode at offset 3, safe-band percentages at offsets 7-8.
- [ ] Add tests that safe-band values above `50` return `VC_MODBUS_ILLEGAL_VALUE`.
- [ ] Add tests that channel Configured Target Voltage is at channel offset 0 (single UINT16, no multi-register packing).
- [ ] Add tests that Channel Output Action is at channel offset 1, self-clears on read, and rejects Force Output Zero and Clamp from host context.
- [ ] Add tests that Channel Fault Command is at channel offset `3`, self-clears on read.
- [ ] Add tests that Current Protection Output Action rejects Clamp.
- [ ] Add tests that reserved writes return `VC_MODBUS_ILLEGAL_ADDRESS`.
- [ ] Run: `west build -b native_posix -d build/test-modbus-adapter tests/voltage_control/modbus_adapter`.
- [ ] Expected: initial run fails before adapter implementation exists.

## Task 4: Modbus Adapter Implementation

**Files:**
- Create: `lib/voltage_control/modbus_adapter.c`
- Modify: `lib/voltage_control/CMakeLists.txt`

- [ ] Implement register address decoding for system and channel blocks.
- [ ] Implement single-16-bit register reads and writes only; no INT32/UINT32 packing.
- [ ] Reject writes to reserved registers with `VC_MODBUS_ILLEGAL_ADDRESS`.
- [ ] Map unsupported channel to `VC_MODBUS_ILLEGAL_ADDRESS`.
- [ ] Map invalid enum/range/unsafe clear to `VC_MODBUS_ILLEGAL_VALUE`.
- [ ] Map storage/internal failures to `VC_MODBUS_DEVICE_FAILURE`.
- [ ] Ensure command registers read back as `0` after execution.
- [ ] Run adapter tests and domain tests.

## Task 5: HVB App Smoke Integration

**Files:**
- Modify: `applications/hvb_controller/src/main.c`
- Modify: `applications/hvb_controller/CMakeLists.txt`

- [ ] Initialize HVB variant profile and voltage-control domain before heartbeat loop.
- [ ] Initialize Modbus adapter over the domain without registering Zephyr RTU callbacks yet.
- [ ] Read protocol/variant/channel-count registers through the adapter and print one smoke line.
- [ ] Preserve existing SYS_RUN heartbeat behavior.
- [ ] Run: `west build -b jw_hvb -d build/hvb_controller applications/hvb_controller`.
- [ ] Expected: build succeeds.

## Task 6: Documentation And Verification

**Files:**
- Modify: `README.md`

- [ ] Add architecture status links to `CONTEXT.md`, the architecture spec, and `ref/modbus_interface.md`.
- [ ] Run final verification:

```sh
west build -b native_posix -d build/test-domain tests/voltage_control/domain
west build -b native_posix -d build/test-modbus-adapter tests/voltage_control/modbus_adapter
west build -b jw_hvb -d build/hvb_controller applications/hvb_controller
```

- [ ] Expected: all three commands exit 0.
- [ ] Inspect `git status --short` and stage only intended files.

## Later Slices

| Gap | Later slice |
|---|---|
| Zephyr Modbus RTU callback registration | Modbus transport integration |
| AD5541 output support | Output hardware support |
| ADS1232 measurement support | Measurement hardware support |
| Ramping worker/service | Channel service |
| Safety supervisor runtime loop | Safety supervisor |
| Zephyr settings/NVS persistence | Parameter store |
| Automatic retry execution | Recovery policy runtime |

## Execution Options

Plan complete and saved to `docs/superpowers/plans/2026-06-15-voltage-control-domain-modbus-slice.md`. Two execution options:

**1. Subagent-Driven (recommended)** - Dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints.

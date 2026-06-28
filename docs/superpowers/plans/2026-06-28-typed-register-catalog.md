# Typed Canonical Register Catalog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace copied VC snapshots and dense protocol register banks with a firmware-wide, protocol-neutral catalog over module-owned typed canonical state.

**Architecture:** Static ROM descriptors identify registers with a 32-bit module/instance/field ID and point to canonical module state. Frontends use `reg_read()` and owner-mediated `reg_write()`; module policy continues to use typed state directly. Shared X-macro schemas generate semantic IDs and the Modbus view while devicetree controls instantiated channels and capabilities.

**Tech Stack:** C11, Zephyr RTOS 3.7, devicetree, iterable sections, SMF, ztest, Twister

---

### Task 1: Catalog core

**Files:**
- Create: `include/reg_store/reg_catalog.h`
- Create: `lib/reg_store/reg_catalog.c`
- Modify: `lib/reg_store/CMakeLists.txt`
- Test: `tests/voltage_control/reg_store/src/main.c`

- [x] Add failing tests for structured IDs, fixed/mutable reads, access enforcement, owner-mediated writes, failed-write non-mutation, and missing IDs.
- [x] Run `west twister -p native_posix -s reg_store/voltage_control.reg_store` and confirm failures are caused by the missing catalog API.
- [x] Implement descriptor lookup, typed reads, generic status codes, and synchronous owner dispatch using statically declared descriptors.
- [x] Re-run the focused suite and retain the existing compact-store tests until adapter migration.

### Task 2: Static schemas and Modbus view

**Files:**
- Create: `include/reg_store/reg_schema.h`
- Create: `include/reg_store/system_regs.def`
- Create: `include/reg_store/vc_regs.def`
- Modify: `include/reg_store/reg_map.h`
- Test: `tests/voltage_control/reg_store/src/main.c`

- [x] Add failing tests proving stable system/channel semantic IDs and existing protocol-v3 wire addresses.
- [x] Define host-safe X-macro schemas and expand them into semantic field constants and Modbus mappings without Zephyr dependencies.
- [x] Add uniqueness and lookup tests, including absent channels and fixed 16-channel wire slots.

### Task 3: Canonical system-status registers

**Files:**
- Modify: `lib/sys_status/sys_status.c`
- Modify: `include/sys_status/sys_status.h`
- Test: `tests/voltage_control/ss_shell/src/main.c`

- [x] Add failing tests that read firmware version, uptime, temperature, and humidity through semantic catalog IDs.
- [x] Register fixed version values from ROM and mutable status fields from the existing atomic state.
- [x] Remove `sys_status` writes into the dense register store while preserving shell behavior.

### Task 4: Canonical VC state and owner writes

**Files:**
- Modify: `include/voltage_control/vc_channel.h`
- Modify: `include/voltage_control/vc_runtime.h`
- Modify: `lib/voltage_control/vc_channel.c`
- Modify: `lib/voltage_control/vc_runtime.c`
- Test: `tests/voltage_control/vc/src/main.c`
- Test: `tests/voltage_control/vc_controller/src/main.c`

- [x] Correct the stale baseline assertion that references removed system recovery state.
- [x] Add failing tests for catalog reads of fixed capabilities, raw evidence, derived measurements, runtime state, operational config, and calibration config.
- [x] Add failing tests showing config/command writes route through the VC worker, commit only after validation, and expose internal state-machine changes immediately.
- [x] Register channel descriptors from devicetree-composed instances and capabilities.
- [x] Replace copied config field updates with validated field-specific mutations where practical.
- [x] Remove `vc_published_snapshot`, snapshot mutex, and periodic full register publication; retain only private presentation helpers required during shell migration.

### Task 5: Mandatory frontend catalog facade

**Files:**
- Modify: `lib/modbus_adapter/modbus_register.c`
- Modify: `lib/modbus_adapter/modbus_adapter.c`
- Modify: `lib/voltage_control/vc_shell.c`
- Test: `tests/voltage_control/modbus_adapter/src/main.c`
- Test: `tests/voltage_control/vc_shell/src/main.c`

- [x] Add failing Modbus tests for semantic mapping, signed values, access errors, capability filtering, command dispatch, and adjacent HI/LO reads.
- [x] Generate/declare Modbus wire-to-semantic mappings from the shared schemas and route callbacks through catalog accessors.
- [x] Add one-value HI/LO latching for immediately adjacent reads; document that multi-field reads are not generation-coherent.
- [x] Route VC shell reads and writes through the same catalog facade while preserving command output and validation.

### Task 6: Remove mirrors and verify 16-channel composition

**Files:**
- Delete: `lib/reg_store/reg_store.c` legacy dense-bank implementation
- Reduce: `include/reg_store/reg_store.h` to compatibility-free catalog includes or remove it
- Add: `tests/voltage_control/reg_store/boards/native_posix_16.overlay`
- Modify: `UBIQUITOUS_LANGUAGE.md`
- Modify: relevant approved architecture specification

- [x] Add a 16-channel build/test configuration and verify every configured descriptor is reachable while unsupported capabilities reject access.
- [x] Remove legacy `_input`, `_holding`, publication helpers, and obsolete snapshot query plumbing after all callers migrate.
- [x] Update terminology to define Semantic Register, Register Catalog, and Register View; supersede coherent Domain Snapshot reads with per-value catalog coherence.
- [x] Run all voltage-control Twister suites, the production board build, architecture checks, and host-tool build/tests.
- [x] Compare the final map file against baseline and confirm dense banks and published snapshots are absent and RAM does not regress at equivalent channel count.

## Baseline Exception

At commit `567489a`, 9 of 10 voltage-control suites pass (191 test cases). `vc/voltage_control.vc` fails to compile because its `test_dispatch_system_field` still references the removed `vc_system_config.recovery_policy_mode`. The user approved correcting this stale assertion during Task 4.

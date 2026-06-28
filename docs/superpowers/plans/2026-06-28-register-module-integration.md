# Register Module Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate system status, voltage control, and the Modbus adapter behind owner-native static Semantic Registers and remove aggregate frontend state APIs.

**Architecture:** Module-owned schemas generate iterable-section descriptors over canonical scalar state. Adapter-specific views hold direct descriptor handles; owner callbacks provide synchronization and command routing while generic ID lookup remains an infrequent fallback.

**Tech Stack:** C11, Zephyr RTOS 3.7, devicetree, Kconfig, iterable sections, Settings, SMF, ztest, Twister

---

### Task 1: Owner-native schemas and descriptor handles

**Files:**
- Modify: `include/reg_store/reg_catalog.h`
- Modify: `include/reg_store/reg_schema.h`
- Create: `include/reg_store/sys_status_regs.def`
- Create: `include/reg_store/modbus_regs.def`
- Modify: `include/reg_store/vc_regs.def`
- Test: `tests/voltage_control/reg_store/src/main.c`

- [x] Add tests proving owner module IDs, VC global/channel IDs, descriptor-handle reads, and generic lookup compatibility.
- [x] Run the focused Register Catalog test and confirm the new API tests fail.
- [x] Add direct descriptor read/write helpers and owner-specific schema definitions without Modbus placement fields.
- [x] Generate statically addressable descriptor arrays and make handle access independent of linear `reg_describe()`.
- [x] Re-run the focused suite and commit the catalog slice.

### Task 2: System-status ownership

**Files:**
- Modify: `include/sys_status/sys_status.h`
- Modify: `lib/sys_status/sys_status.c`
- Modify: `lib/sys_status/sys_reset.c`
- Modify: `lib/sys_status/sys_status_shell.c`
- Test: `tests/voltage_control/ss_shell/src/main.c`

- [x] Add failing tests for system-status owner IDs, scalar shell reads, and catalog-routed delayed reset.
- [x] Bind fixed identity and atomic telemetry descriptors to the system-status owner.
- [x] Add the write-only reset descriptor and route it to the existing delayed-work service.
- [x] Remove `sys_status_snapshot` and `sys_status_get()` after migrating the shell.
- [x] Run the system-status suite and commit the slice.

### Task 3: Modbus-adapter canonical state and protocol view

**Files:**
- Modify: `include/modbus_adapter/modbus_adapter.h`
- Modify: `lib/modbus_adapter/modbus_adapter.c`
- Modify: `lib/modbus_adapter/modbus_register.[ch]`
- Create: `lib/modbus_adapter/modbus_view.def`
- Modify: `lib/modbus_adapter/modbus_adapter_shell.c`
- Test: `tests/voltage_control/modbus_adapter/src/main.c`
- Test: `tests/voltage_control/mb_shell/src/main.c`

- [x] Add failing tests for distinct active/next-boot semantic fields, catalog commands, unchanged wire addresses, and descriptor-handle views.
- [x] Move active and next-boot fields behind owner descriptors and one mutex; preserve rollback and source-specific persistence semantics.
- [x] Split Modbus placement into `modbus_view.def`, map catalog status directly to Modbus exceptions, and remove the VC context dependency.
- [x] Migrate the Modbus shell to scalar catalog access and remove aggregate configuration getters.
- [x] Run both Modbus suites, including settings-enabled coverage, and commit the slice.

### Task 4: VC direct bindings and field-wise mutation

**Files:**
- Modify: `include/voltage_control/vc.h`
- Modify: `include/voltage_control/vc_channel.h`
- Modify: `include/voltage_control/vc_controller.h`
- Modify: `lib/voltage_control/vc.c`
- Modify: `lib/voltage_control/vc_channel.c`
- Modify: `lib/voltage_control/vc_controller.c`
- Modify: `lib/voltage_control/vc_runtime.c`
- Test: `tests/voltage_control/vc/src/main.c`
- Test: `tests/voltage_control/vc_controller/src/main.c`

- [x] Add failing tests for VC global IDs, direct canonical reads, rejected-write non-mutation, worker serialization, and 16-channel handles.
- [x] Bind plain scalar descriptors to static controller/channel state; keep callbacks for computed, buffered, and enum values.
- [x] Replace copy-modify-validate single-field config writes with field-specific validation, mutation, and side effects.
- [x] Keep full-object operations only for settings load/save and factory defaults.
- [x] Remove frontend `vc_query` and `vc_dispatch`; retain on-demand controller snapshots as policy-test seams.
- [x] Run VC, controller, channel-state, and 16-channel suites and commit the slice.

### Task 5: Frontends, simulator, documentation, and verification

**Files:**
- Modify: `lib/voltage_control/vc_shell.c`
- Modify: `applications/hvb_controller/src/main.c`
- Modify: `demos/modbus_sim/src/main.c`
- Modify: `UBIQUITOUS_LANGUAGE.md`
- Test: `tests/voltage_control/vc_shell/src/main.c`
- Test: `tests/architecture/controller_split.sh`

- [x] Add failing shell tests showing reads and commands work without aggregate VC APIs.
- [x] Migrate the VC shell and application initialization to catalog-backed module services.
- [x] Repair the simulator against current DTS/provider APIs and remove stale legacy symbols.
- [x] Update terminology and architecture documentation, then search for all removed APIs.
- [ ] Run final Twister, architecture checks, host-tool tests, clean product and simulator builds, and `git diff --check`.
- [ ] Compare map files with baseline, document the delta, and commit the completed integration.

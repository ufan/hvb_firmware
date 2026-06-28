# Register Catalog Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the lifecycle, capability, synchronization, static-handle, composition, documentation, and efficiency gaps found in the owner-native Register Catalog review.

**Architecture:** Keep iterable-section descriptors and module-owned canonical state. Add a synchronized singleton lifecycle, publish a stable constant-time VC handle API, make frontends capability-aware, and complete removal of the obsolete VC command facade without dynamic registration or RAM mirrors.

**Tech Stack:** C11, Zephyr RTOS 3.7, devicetree, Kconfig, iterable sections, Settings, shell, ztest, Twister

---

### Task 1: Make VC catalog lifecycle safe

**Files:**
- Modify: `tests/voltage_control/reg_store/src/main.c`
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `lib/voltage_control/vc.c`

- [ ] Add tests that create and destroy VC, then require catalog reads and writes to return `REG_BUSY`. Run the post-destroy write in a helper thread so the pre-fix deadlock is bounded and observable. Add a second-create rejection test:

```c
ZTEST(reg_store, test_vc_catalog_is_unavailable_after_destroy)
{
	struct vc_ctx *ctx = vc_init();
	union reg_value value = {};

	zassert_not_null(ctx);
	vc_destroy(ctx);
	zassert_equal(reg_read(REG_VC_GLOBAL_SUPPORTED_CHANNELS_ID, &value), REG_BUSY);
}
```

The helper writer records its result and gives a completion semaphore. Require completion within 100 ms and abort the helper on timeout, so RED fails promptly instead of hanging Twister.

- [ ] Run `west twister -T tests/voltage_control/reg_store -p native_posix --outdir /tmp/twister-reg-hardening-lifecycle --clobber-output`; verify the new lifecycle test fails or hangs before the fix.
- [ ] Implement an atomic `STOPPED/STARTING/RUNNING/STOPPING` lifecycle. Initialize the queue before publishing `catalog_runtime`, reject a second create, unpublish before shutdown, fail queued commands safely, join the worker, then transition to `STOPPED`.
- [ ] Re-run the focused tests and confirm both Register Catalog scenarios pass without timeout.
- [ ] Commit with `git commit -m "fix(vc): guard register catalog runtime lifecycle"`.

### Task 2: Expose stable static channel handles

**Files:**
- Modify: `include/reg_store/reg_schema.h`
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `lib/modbus_adapter/modbus_register.c`
- Modify: `tests/voltage_control/reg_store/src/main.c`
- Modify: `tests/voltage_control/reg_store/testcase.yaml`

- [ ] Add 16-channel coverage that reads channel 15 capability flags and writes/reads its target through the Modbus view:

```c
zassert_equal(vc_mb_input_rd(mb,
	CH_BLOCK_BASE(15) + CH_CAPABILITY_FLAGS, &word), VC_MB_OK);
zassert_not_equal(word, 0U);
zassert_equal(vc_mb_holding_wr(mb,
	CH_BLOCK_BASE(15) + CH_CFG_TARGET_VOLTAGE, 1234U), VC_MB_OK);
zassert_equal(vc_mb_holding_rd(mb,
	CH_BLOCK_BASE(15) + CH_CFG_TARGET_VOLTAGE, &word), VC_MB_OK);
zassert_equal(word, 1234U);
```

- [ ] Run `west twister -T tests/voltage_control/reg_store -p native_posix -s voltage_control.reg_store_16ch --outdir /tmp/twister-reg-hardening-16ch --clobber-output`; verify RED because the stable handle API and adapter coverage are absent.
- [ ] Add bounds-checked `reg_vc_channel_handle(channel, ordinal)` and `reg_vc_global_handle(ordinal)` APIs. Add `BUILD_ASSERT` checks for no more than 16 children and contiguous `DT_REG_ADDR(node_id) == DT_NODE_CHILD_IDX(node_id)` topology.
- [ ] Replace Modbus's private descriptor-array declarations and arithmetic with the handle API.
- [ ] Run both Register Catalog scenarios and commit with `git commit -m "refactor(reg): expose stable static vc handles"`.

### Task 3: Make shell reads capability-aware

**Files:**
- Modify: `tests/voltage_control/vc_shell/boards/native_posix.overlay`
- Modify: `tests/voltage_control/vc_shell/src/main.c`
- Modify: `lib/voltage_control/vc_shell.c`

- [ ] Give channel 1 current measurement but no voltage measurement or raw output drive. Add tests for `vc status`, `vc ch 1 status`, `vc ch 1 config`, and `vc cal config 1`; supported current data must print and unsupported fields must be omitted.
- [ ] Run `west twister -T tests/voltage_control/vc_shell -p native_posix --outdir /tmp/twister-reg-hardening-shell --clobber-output`; verify the partial-capability case fails.
- [ ] Read capabilities first, fetch only supported registers, and check helper return values at every command call site:

```c
int ret = read_channel_snapshot(ch, &snap);
if (ret < 0) {
	shell_error(sh, "failed to read channel %u", ch);
	return ret;
}
```

- [ ] Re-run VC shell and Modbus adapter scenarios; keep presentation objects stack-local.
- [ ] Commit with `git commit -m "fix(vc-shell): honor channel capabilities when reading registers"`.

### Task 4: Serialize all Modbus persistence operations

**Files:**
- Modify: `tests/voltage_control/modbus_adapter/src/main.c`
- Modify: `lib/modbus_adapter/modbus_adapter.c`

- [ ] Add a `CONFIG_ZTEST` persistence observer invoked immediately before settings I/O. The test override blocks on a semaphore while a second thread attempts an owner write. Assert the second operation cannot complete until the observer is released; this deterministically proves settings I/O shares the adapter lock.
- [ ] Run `west twister -T tests/voltage_control/modbus_adapter -p native_posix -s voltage_control.modbus_adapter.settings --outdir /tmp/twister-reg-hardening-modbus --clobber-output`; verify RED because settings load currently enters the observer without holding the owner lock.
- [ ] Introduce internal `_locked` persistence/reconfiguration helpers. Each public owner operation acquires `mb->lock` exactly once; load, save, factory, settings access, state publication, and live reconfiguration occur inside that boundary.
- [ ] Run settings-disabled and settings-enabled Modbus scenarios.
- [ ] Commit with `git commit -m "fix(modbus): serialize register-owned persistence state"`.

### Task 5: Complete interface and build-composition cleanup

**Files:**
- Modify: `include/voltage_control/vc.h`
- Modify: `lib/voltage_control/vc_shell.c`
- Modify: `lib/modbus_adapter/Kconfig`
- Modify: `include/reg_store/modbus_regs.def`
- Modify: `include/reg_store/modbus_view.def`
- Modify: `include/reg_store/sys_status_regs.def`
- Modify: `include/reg_store/vc_global_regs.def`
- Modify: `docs/superpowers/diagrams/*.puml`
- Modify: `tests/architecture/controller_split.sh`

- [ ] Extend the architecture guard to reject `struct vc_cmd`, `vc_cmd_`, `vc_dispatch`, and `vc_query` in production code and to check the Modbus Kconfig dependencies.
- [ ] Run `bash tests/architecture/controller_split.sh`; verify it fails on obsolete command builders.
- [ ] Reduce `vc.h` to singleton lifecycle functions. Have the shell map parsed intent directly to register IDs/values. Set `depends on MODBUS && VC_RUNTIME && VC_CHANNEL_CONTROLLER` for `UI_MODBUS_RTU`.
- [ ] Add the required copyright header to the four new `.def` files and update tracked diagrams to show Frontend Adapter -> Register Catalog -> Owner Callback -> Canonical State.
- [ ] Run architecture and shell tests, configure the product build, then commit with `git commit -m "refactor: complete register catalog frontend boundary"`.

### Task 6: Bind fixed globals directly and finalize verification

**Files:**
- Modify: `lib/voltage_control/vc_runtime.c`
- Modify: `lib/modbus_adapter/modbus_register.c`
- Modify: `include/reg_store/modbus_view.def`
- Modify: `tests/voltage_control/reg_store/src/main.c`
- Modify: `docs/superpowers/plans/2026-06-28-register-catalog-hardening.md`

- [ ] Add tests asserting fixed VC global descriptors have non-NULL bound values and every mapped system/channel wire address resolves to the expected Semantic Register ID.
- [ ] Run Register Catalog Twister scenarios; verify RED because fixed VC globals remain callback-only.
- [ ] Bind variant, capability flags, channel count, and active mask to const scalar storage. Extend `modbus_view.def` with owner/field information and generate system input/holding handle tables instead of duplicating them manually.
- [ ] Run complete verification:

```sh
west twister -T tests/voltage_control -p native_posix \
  --outdir /tmp/twister-reg-hardening-final --clobber-output
bash tests/architecture/controller_split.sh
west build -b jw_hvb applications/hvb_controller \
  -d /tmp/build-hvb-reg-hardening-final -p always
west build -b jw_hvb demos/modbus_sim \
  -d /tmp/build-modbus-sim-reg-hardening-final -p always
/tmp/build-hvb-tools-register-integration/hvb_modbus_core/tests/hvb_tests
git diff --check 2c059b9...HEAD
```

- [ ] Compare the new product ELF with the `2c059b9` baseline using `arm-zephyr-eabi-size`; record text/data/BSS deltas and confirm no per-register RAM index exists.
- [ ] Commit with `git commit -m "refactor(reg): finalize catalog hardening"`.

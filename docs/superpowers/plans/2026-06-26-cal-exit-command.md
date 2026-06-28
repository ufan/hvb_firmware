# Calibration Exit Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the implicit cal-exit-by-mode-write with an explicit `CAL_EXIT` command that automatically restores the pre-calibration operating mode (NORMAL or AUTOMATIC).

**Architecture:** Add `pre_cal_mode` to `vc_controller` (saved on CAL entry). Add `vc_controller_cal_exit()` that restores it. Wire `VC_CAL_EXIT` through the existing cal command pipeline (`vc.h` → `vc.c` → `vc_runtime` → `vc_controller`). Expose as a dedicated `EXT_CAL_EXIT` holding register (offset 1 in EXT block). Update host tools to use the new register instead of writing `SYS_OPERATING_MODE`.

**Tech Stack:** C / Zephyr ztest (firmware); C++ / Catch2 / Qt (tools)

## Global Constraints

- No new public headers — extend existing ones only.
- `EXT_CAL_EXIT` is offset 1 in the EXT block (`EXT_BLOCK_BASE + 1 = 201`). Write any non-zero value to trigger.
- `vc_controller_cal_exit()` must reject with `VC_ERR_INVALID_COMMAND` if not currently in `VC_OPERATING_MODE_CALIBRATION`.
- Writing `SYS_OPERATING_MODE` (holding reg 0) still works as a fallback exit — do not remove that path.
- All existing tests must continue to pass.

---

### Task 1: Controller — store pre_cal_mode and implement cal_exit

**Files:**
- Modify: `include/voltage_control/vc_controller.h`
- Modify: `lib/voltage_control/vc_controller.c`
- Modify: `tests/voltage_control/vc_controller/src/main.c`

**Interfaces:**
- Produces: `enum vc_operating_mode vc_controller::pre_cal_mode` field
- Produces: `enum vc_status vc_controller_cal_exit(struct vc_controller *ctrl)`

- [ ] **Step 1: Write the failing tests**

Add to `tests/voltage_control/vc_controller/src/main.c` (after the last existing ZTEST):

```c
static void enter_cal(struct vc_controller *c)
{
	vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP1);
	vc_controller_calibration_unlock(c, CAL_UNLOCK_STEP2);
	vc_controller_set_operating_mode(c, VC_OPERATING_MODE_CALIBRATION);
}

ZTEST(vc_controller, test_cal_exit_rejected_when_not_in_cal)
{
	zassert_equal(vc_controller_cal_exit(ctrl), VC_ERR_INVALID_COMMAND);
}

ZTEST(vc_controller, test_cal_exit_from_normal_restores_normal)
{
	enter_cal(ctrl);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_NORMAL);
}

ZTEST(vc_controller, test_cal_exit_from_auto_restores_auto)
{
	vc_controller_set_operating_mode(ctrl, VC_OPERATING_MODE_AUTOMATIC);
	enter_cal(ctrl);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_CALIBRATION);

	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	zassert_equal(vc_controller_get_operating_mode(ctrl),
		      VC_OPERATING_MODE_AUTOMATIC);
}

ZTEST(vc_controller, test_cal_exit_resets_channels_to_disabled_safe)
{
	enter_cal(ctrl);
	zassert_equal(vc_controller_cal_exit(ctrl), VC_OK);
	/* After exit, no channel should have cal_output_enabled */
	struct vc_channel_snapshot snap;
	vc_controller_get_channel_snapshot(ctrl, 0, &snap);
	zassert_equal(snap.cal_output_enabled, 0);
	zassert_equal(snap.raw_dac_readback, 0);
}
```

- [ ] **Step 2: Build to verify tests fail to compile**

```bash
west build -d build_vc_state -b native_posix \
    tests/voltage_control/vc_controller 2>&1 | tail -20
```

Expected: compile error — `vc_controller_cal_exit` undeclared.

- [ ] **Step 3: Add `pre_cal_mode` field and declare `vc_controller_cal_exit`**

In `include/voltage_control/vc_controller.h`, add `pre_cal_mode` to the struct and the function declaration:

```c
struct vc_controller {
	struct vc_channel channels[VC_MAX_CHANNELS];
	size_t channel_count;
	struct vc_channel_buffer *meas_index[VC_MAX_CHANNELS];
	enum vc_operating_mode operating_mode;
	enum vc_operating_mode pre_cal_mode;   /* ← add this line */
	uint8_t cal_unlock_step;
	bool cal_unlocked;
	const struct vc_storage_backend *storage;
	struct vc_system_config sys_cfg;
};
```

And add the declaration after `vc_controller_calibration_unlock`:

```c
enum vc_status vc_controller_cal_exit(struct vc_controller *ctrl);
```

- [ ] **Step 4: Store `pre_cal_mode` on CAL entry**

In `lib/voltage_control/vc_controller.c`, inside `vc_controller_set_operating_mode`, add the store immediately before the `→ CAL` reset block (currently at line ~175):

```c
	/* → CAL: reset all channels into calibration state */
	if (mode == VC_OPERATING_MODE_CALIBRATION) {
		ctrl->pre_cal_mode = ctrl->operating_mode;   /* ← add before reset loop */
		for (size_t i = 0; i < ctrl->channel_count; i++) {
			vc_channel_reset_calibration(&ctrl->channels[i], true);
		}
	} else if (ctrl->operating_mode == VC_OPERATING_MODE_CALIBRATION) {
```

- [ ] **Step 5: Implement `vc_controller_cal_exit`**

Add after `vc_controller_calibration_unlock` in `lib/voltage_control/vc_controller.c`:

```c
enum vc_status vc_controller_cal_exit(struct vc_controller *ctrl)
{
	if (ctrl->operating_mode != VC_OPERATING_MODE_CALIBRATION) {
		return VC_ERR_INVALID_COMMAND;
	}
	return vc_controller_set_operating_mode(ctrl, ctrl->pre_cal_mode);
}
```

- [ ] **Step 6: Build and run tests**

```bash
west build -d build_vc_state -b native_posix \
    tests/voltage_control/vc_controller && \
    ./build_vc_state/zephyr/zephyr.exe
```

Expected: all `vc_controller` tests pass including the 4 new ones.

- [ ] **Step 7: Commit**

```bash
git add include/voltage_control/vc_controller.h \
        lib/voltage_control/vc_controller.c \
        tests/voltage_control/vc_controller/src/main.c
git commit -m "feat(controller): add pre_cal_mode + vc_controller_cal_exit"
```

---

### Task 2: Wire VC_CAL_EXIT through the command pipeline

**Files:**
- Modify: `include/voltage_control/vc.h`
- Modify: `lib/voltage_control/vc.c`
- Modify: `include/voltage_control/vc_runtime.h`
- Modify: `lib/voltage_control/vc_runtime.c`

**Interfaces:**
- Consumes: `vc_controller_cal_exit()` from Task 1
- Produces: `VC_CAL_EXIT` in `enum vc_cal_action`
- Produces: `vc_cmd_cal_exit()` builder in `vc.h`
- Produces: `VC_RUNTIME_CMD_CALIBRATION_EXIT` in `enum vc_runtime_command_type`

- [ ] **Step 1: Add `VC_CAL_EXIT` to `enum vc_cal_action` and add builder**

In `include/voltage_control/vc.h`, add `VC_CAL_EXIT` to the enum:

```c
enum vc_cal_action {
	VC_CAL_UNLOCK,
	VC_CAL_SET_OUTPUT_ENABLE,
	VC_CAL_SET_RAW_DAC,
	VC_CAL_SAMPLE,
	VC_CAL_COMMIT,
	VC_CAL_SET_MAX_RAW_DAC,
	VC_CAL_EXIT,                /* ← add */
};
```

Add the builder after `vc_cmd_cal_max_dac`:

```c
/* Build a calibration exit command — restores the pre-calibration operating mode. */
static inline struct vc_cmd vc_cmd_cal_exit(void)
{
	return (struct vc_cmd){
		.type = VC_CMD_CALIBRATION,
		.cal = { .action = VC_CAL_EXIT },
	};
}
```

- [ ] **Step 2: Add `VC_RUNTIME_CMD_CALIBRATION_EXIT` to the runtime enum**

In `include/voltage_control/vc_runtime.h`:

```c
enum vc_runtime_command_type {
	VC_RUNTIME_CMD_SET_OPERATING_MODE = 0,
	VC_RUNTIME_CMD_OUTPUT_ACTION,
	VC_RUNTIME_CMD_FAULT_COMMAND,
	VC_RUNTIME_CMD_CALIBRATION_UNLOCK,
	VC_RUNTIME_CMD_CALIBRATION_OUTPUT_ENABLE,
	VC_RUNTIME_CMD_CALIBRATION_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_SAMPLE,
	VC_RUNTIME_CMD_CALIBRATION_COMMIT,
	VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC,
	VC_RUNTIME_CMD_CALIBRATION_EXIT,          /* ← add */
	VC_RUNTIME_CMD_SYSTEM_PARAM_ACTION,
	VC_RUNTIME_CMD_CHANNEL_PARAM_ACTION,
	VC_RUNTIME_CMD_SET_SYSTEM_FIELD,
	VC_RUNTIME_CMD_SET_CHANNEL_FIELD,
	VC_RUNTIME_CMD_SET_CHANNEL_CAL_FIELD,
};
```

- [ ] **Step 3: Handle `VC_CAL_EXIT` in `dispatch_calibration` in `vc.c`**

In `lib/voltage_control/vc.c`, add a case to `dispatch_calibration` (after the `VC_CAL_SET_MAX_RAW_DAC` case):

```c
	case VC_CAL_SET_MAX_RAW_DAC:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC;
		cmd.payload.calibration_max_raw_dac = cal->value;
		break;
	case VC_CAL_EXIT:
		cmd.type = VC_RUNTIME_CMD_CALIBRATION_EXIT;
		break;
	default:
```

- [ ] **Step 4: Dispatch `VC_RUNTIME_CMD_CALIBRATION_EXIT` in `vc_runtime.c`**

In `lib/voltage_control/vc_runtime.c`, inside `vc_runtime_dispatch_command`, add after the `VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC` case:

```c
	case VC_RUNTIME_CMD_CALIBRATION_MAX_RAW_DAC:
		return vc_controller_channel_cal_max_raw_dac(ctrl, cmd->channel,
						cmd->payload.calibration_max_raw_dac);
	case VC_RUNTIME_CMD_CALIBRATION_EXIT:
		return vc_controller_cal_exit(ctrl);
```

- [ ] **Step 5: Build the vc_controller test suite to confirm no regressions**

```bash
west build -d build_vc_state -b native_posix \
    tests/voltage_control/vc_controller && \
    ./build_vc_state/zephyr/zephyr.exe
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/voltage_control/vc.h \
        lib/voltage_control/vc.c \
        include/voltage_control/vc_runtime.h \
        lib/voltage_control/vc_runtime.c
git commit -m "feat(pipeline): add VC_CAL_EXIT through cal command pipeline"
```

---

### Task 3: Regmap register + modbus adapter routing

**Files:**
- Modify: `include/regmap/vc_regs.h`
- Modify: `lib/modbus_adapter/modbus_register.c`
- Modify: `tests/voltage_control/modbus_adapter/src/main.c`

**Interfaces:**
- Consumes: `vc_cmd_cal_exit()` from Task 2
- Produces: `EXT_CAL_EXIT = 1` and `EXT_CAL_EXIT_ABS = EXT_BLOCK_BASE + 1` in regmap

- [ ] **Step 1: Add `EXT_CAL_EXIT` to the regmap**

In `include/regmap/vc_regs.h`, add after `EXT_CAL_UNLOCK_ABS`:

```c
#define EXT_CAL_UNLOCK                0
#define EXT_CAL_UNLOCK_ABS            (EXT_BLOCK_BASE + EXT_CAL_UNLOCK)

#define EXT_CAL_EXIT                  1
#define EXT_CAL_EXIT_ABS              (EXT_BLOCK_BASE + EXT_CAL_EXIT)
```

- [ ] **Step 2: Route EXT_CAL_EXIT in the modbus adapter**

In `lib/modbus_adapter/modbus_register.c`, update `vc_reg_write_ext`:

```c
enum vc_status vc_reg_write_ext(struct vc_ctx *ctx, uint16_t off,
				uint16_t val, k_timeout_t timeout)
{
	if (off == EXT_CAL_UNLOCK) {
		return vc_dispatch(ctx, vc_cmd_cal_unlock(val), timeout);
	}
	if (off == EXT_CAL_EXIT) {
		return vc_dispatch(ctx, vc_cmd_cal_exit(), timeout);
	}
	return VC_ERR_UNSUPPORTED_CAPABILITY;
}
```

Note: the existing check `off == EXT_CAL_UNLOCK_ABS - EXT_BLOCK_BASE` is equivalent to `off == EXT_CAL_UNLOCK` (both equal 0); either form is fine. Use the macro names for clarity.

- [ ] **Step 3: Update the modbus adapter test**

In `tests/voltage_control/modbus_adapter/src/main.c`:

Replace `test_extension_write_non_unlock_rejected` (which currently tests offset 1) with a test for an actually-unused offset, and add a new test for `EXT_CAL_EXIT`:

```c
/* Replace the old test body: */
ZTEST(modbus_adapter, test_extension_write_non_unlock_rejected)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);

	zassert_not_null(ctx);
	/* Offset 2 is not assigned — must be rejected */
	zassert_equal(vc_mb_holding_wr(mb, EXT_BLOCK_BASE + 2, 0),
		      VC_MB_ILLEGAL_ADDRESS);

	destroy_ctx(ctx);
}

/* Add new test: */
ZTEST(modbus_adapter, test_cal_exit_via_ext_register)
{
	struct vc_ctx *ctx = make_ctx();
	struct vc_mb_adapter *mb = vc_mb_adapter_create(ctx);
	uint16_t reg;

	zassert_not_null(ctx);

	/* Unlock + enter CAL */
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP1),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_UNLOCK_ABS, CAL_UNLOCK_STEP2),
		      VC_MB_OK);
	zassert_equal(vc_mb_holding_wr(mb, SYS_OPERATING_MODE,
				       VC_OPERATING_MODE_CALIBRATION), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, SYS_ACTIVE_OPERATING_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_OPERATING_MODE_CALIBRATION);

	/* Exit via EXT_CAL_EXIT — should restore to NORMAL (the pre-cal mode) */
	zassert_equal(vc_mb_holding_wr(mb, EXT_CAL_EXIT_ABS, 1), VC_MB_OK);
	k_msleep(50);
	zassert_equal(vc_mb_input_rd(mb, SYS_ACTIVE_OPERATING_MODE, &reg), VC_MB_OK);
	zassert_equal(reg, VC_OPERATING_MODE_NORMAL);

	destroy_ctx(ctx);
}
```

- [ ] **Step 4: Build and run the modbus adapter test suite**

```bash
west build -d build_modbus -b native_posix \
    tests/voltage_control/modbus_adapter && \
    ./build_modbus/zephyr/zephyr.exe
```

Expected: all tests pass including `test_cal_exit_via_ext_register`.

- [ ] **Step 5: Commit**

```bash
git add include/regmap/vc_regs.h \
        lib/modbus_adapter/modbus_register.c \
        tests/voltage_control/modbus_adapter/src/main.c
git commit -m "feat(regmap): add EXT_CAL_EXIT register, route in modbus adapter"
```

---

### Task 4: Host tools — hvb_modbus_core

**Files:**
- Modify: `tools/hvb_modbus_core/hvb_modbus_client.h`
- Modify: `tools/hvb_modbus_core/hvb_modbus_client.cpp`
- Modify: `tools/hvb_modbus_core/tests/test_calibration.cpp`

**Interfaces:**
- Consumes: `EXT_CAL_EXIT_ABS` from Task 3 (available via `#include "regmap/vc_regs.h"` which the tools already include)
- Produces: `exitCalibrationMode()` (no argument) — writes `1` to `EXT_CAL_EXIT_ABS`

- [ ] **Step 1: Update the test first (Catch2 / TDD)**

In `tools/hvb_modbus_core/tests/test_calibration.cpp`, replace the existing `exitCalibrationMode` test:

```cpp
TEST_CASE("exitCalibrationMode writes EXT_CAL_EXIT", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    // Exit without being in cal mode still sends the register write
    REQUIRE(client.exitCalibrationMode());
    REQUIRE(holding[reg::extAddr(EXT_CAL_EXIT)] == 1);

    client.detachTestArrays();
}
```

- [ ] **Step 2: Run tests to confirm the old test is gone and new one compiles but fails**

```bash
cmake --build tools/build/linux-debug --target hvb_tests && \
    ./tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests "[calibration]" 2>&1 | tail -20
```

Expected: compilation error — `exitCalibrationMode` still takes an argument.

- [ ] **Step 3: Update the header — remove `OpMode` param**

In `tools/hvb_modbus_core/hvb_modbus_client.h`, change:

```cpp
bool exitCalibrationMode(OpMode targetMode);
```

to:

```cpp
bool exitCalibrationMode();
```

- [ ] **Step 4: Update the implementation**

In `tools/hvb_modbus_core/hvb_modbus_client.cpp`, replace `exitCalibrationMode`:

```cpp
bool HvbModbusClient::exitCalibrationMode() {
    uint16_t v = 1;
    return writeRegsInternal(reg::extAddr(EXT_CAL_EXIT), 1, &v);
}
```

- [ ] **Step 5: Build and run Catch2 calibration tests**

```bash
cmake --build tools/build/linux-debug --target hvb_tests && \
    ./tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests "[calibration]"
```

Expected: all `[calibration]` tests pass.

- [ ] **Step 6: Commit**

```bash
git add tools/hvb_modbus_core/hvb_modbus_client.h \
        tools/hvb_modbus_core/hvb_modbus_client.cpp \
        tools/hvb_modbus_core/tests/test_calibration.cpp
git commit -m "feat(tools): exitCalibrationMode uses EXT_CAL_EXIT, drops targetMode param"
```

---

### Task 5: Factory tool — update CalibrationBackend

**Files:**
- Modify: `tools/hvb_factory_tool/gui/CalibrationBackend.h`
- Modify: `tools/hvb_factory_tool/gui/CalibrationBackend.cpp`

**Interfaces:**
- Consumes: `exitCalibrationMode()` (no argument) from Task 4

- [ ] **Step 1: Update the header**

In `tools/hvb_factory_tool/gui/CalibrationBackend.h`, change the slot signature:

```cpp
public slots:
    ...
    void exitCalibrationMode();   // was: void exitCalibrationMode(const QString& targetMode);
```

- [ ] **Step 2: Update the implementation**

In `tools/hvb_factory_tool/gui/CalibrationBackend.cpp`, replace `exitCalibrationMode`:

```cpp
void CalibrationBackend::exitCalibrationMode() {
    if (m_client.exitCalibrationMode()) {
        m_calActive = false;
        m_calUnlocked = false;
        m_statusMessage = "Exited calibration mode";
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}
```

- [ ] **Step 3: Build the factory tool**

```bash
cmake --build tools/build/linux-debug 2>&1 | grep -E "error:|warning:|Built"
```

Expected: builds cleanly. Any QML callers that pass `targetMode` will now be compile errors — fix them by removing the argument.

- [ ] **Step 4: Commit**

```bash
git add tools/hvb_factory_tool/gui/CalibrationBackend.h \
        tools/hvb_factory_tool/gui/CalibrationBackend.cpp
git commit -m "feat(factory-tool): CalibrationBackend.exitCalibrationMode no longer takes targetMode"
```

---

## Self-review

**Spec coverage:**
- [x] Explicit exit command (`EXT_CAL_EXIT` register, `vc_cmd_cal_exit()`)
- [x] Firmware remembers pre-cal mode (`pre_cal_mode` field, stored on CAL entry)
- [x] Auto-restore on exit (`vc_controller_cal_exit` calls `set_operating_mode(pre_cal_mode)`)
- [x] Reject exit when not in CAL
- [x] Channels reset to DISABLED_SAFE on exit (handled by existing `set_operating_mode` CAL→other path)
- [x] Firmware tests (vc_controller, modbus_adapter)
- [x] Host tool tests (Catch2)
- [x] Factory tool call site updated

**Fallback path preserved:** Writing `SYS_OPERATING_MODE` with a value of 0 or 1 still exits CAL (via `vc_controller_set_operating_mode`). No removal needed.

**No placeholders found.**

**Type consistency:** `vc_controller_cal_exit` is declared returning `enum vc_status` and called at every layer consistently.

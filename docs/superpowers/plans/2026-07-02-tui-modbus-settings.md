# TUI Modbus Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add staged device slave-address and baud-rate controls with an independent `Save Modbus` action to the TUI status-bar Setting dialog.

**Architecture:** Put validation, dirty comparison, and ordered register writes in a small header-only policy seam that can be tested without FTXUI or serial hardware. Bind the existing `ConfigInputs` fields to staged dialog controls in `main.cpp`, and submit saves through the existing serialized Modbus worker queue.

**Tech Stack:** C++17, FTXUI 5, Catch2 3, CMake

---

## File Structure

- Create `tools/hvb_demo_app/tui/modbus_settings.h`: pure validation and save sequencing for staged Modbus settings.
- Create `tools/hvb_modbus_core/tests/test_tui_modbus_settings.cpp`: host-side behavioral tests using callback fakes.
- Modify `tools/hvb_modbus_core/tests/CMakeLists.txt`: compile the new test file into `hvb_tests`.
- Modify `tools/hvb_demo_app/tui/main.cpp`: render staged controls and enqueue the dedicated save action.

### Task 1: Test and implement the Modbus settings policy

**Files:**
- Create: `tools/hvb_modbus_core/tests/test_tui_modbus_settings.cpp`
- Create: `tools/hvb_demo_app/tui/modbus_settings.h`
- Modify: `tools/hvb_modbus_core/tests/CMakeLists.txt`

- [ ] **Step 1: Add the failing policy tests**

Add `test_tui_modbus_settings.cpp` to `hvb_tests`, then write Catch2 cases using a `SystemConfig` baseline and callback vectors. Cover: unchanged values make no calls; a changed slave writes only the slave; a changed baud writes only the baud code; changing both writes slave then baud; malformed, trailing-character, zero, and 248 slave values return `InvalidSlave` without calls; a failed slave write stops before baud; and a failed baud write returns `BaudWriteFailed`.

```cpp
#include <catch2/catch_test_macros.hpp>
#include "../../hvb_demo_app/tui/modbus_settings.h"

TEST_CASE("changed Modbus fields are written in protocol order") {
    hvb::SystemConfig current{};
    current.slaveAddr = 1;
    current.baudRateCode = 0;
    std::vector<std::pair<std::string, uint16_t>> calls;

    auto result = hvb::tui::saveModbusSettings(
        "7", 1, current,
        [&](uint16_t value) { calls.emplace_back("slave", value); return true; },
        [&](uint16_t value) { calls.emplace_back("baud", value); return true; });

    CHECK(result == hvb::tui::ModbusSettingsSaveResult::Success);
    CHECK(calls == std::vector<std::pair<std::string, uint16_t>>{
        {"slave", 7}, {"baud", 1}});
}
```

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```bash
cmake --build build-tools --target hvb_tests -j
./build-tools/hvb_modbus_core/tests/hvb_tests "*Modbus*settings*"
```

Expected: compilation fails because `modbus_settings.h` and `saveModbusSettings` do not exist.

- [ ] **Step 3: Implement the minimal policy seam**

Create `modbus_settings.h` with SPDX/copyright headers, a result enum, strict `std::from_chars` parsing for slave IDs 1–247, baud-code validation for 0–1, dirty comparison against `SystemConfig`, and sequential callback invocation.

```cpp
enum class ModbusSettingsSaveResult {
    Success,
    InvalidSlave,
    InvalidBaud,
    SlaveWriteFailed,
    BaudWriteFailed,
};

inline ModbusSettingsSaveResult saveModbusSettings(
    std::string_view slaveText,
    int baudCode,
    const SystemConfig& current,
    const std::function<bool(uint16_t)>& writeSlave,
    const std::function<bool(uint16_t)>& writeBaud);
```

Also expose `parseModbusSlaveAddress(std::string_view, uint16_t&)` so the UI can
reject invalid text before queueing work. The save function must make no callback
for an unchanged field and must stop immediately when a callback returns false.

- [ ] **Step 4: Run the policy tests and verify GREEN**

Run:

```bash
cmake --build build-tools --target hvb_tests -j
./build-tools/hvb_modbus_core/tests/hvb_tests "*Modbus*settings*"
```

Expected: all Modbus settings policy cases pass.

- [ ] **Step 5: Commit the policy seam**

```bash
git add tools/hvb_demo_app/tui/modbus_settings.h \
  tools/hvb_modbus_core/tests/test_tui_modbus_settings.cpp \
  tools/hvb_modbus_core/tests/CMakeLists.txt
git commit -m "feat(tui): add staged modbus settings policy"
```

### Task 2: Add the dedicated Setting-dialog controls and save action

**Files:**
- Modify: `tools/hvb_demo_app/tui/main.cpp`

- [ ] **Step 1: Add staged controls without immediate writes**

Include `modbus_settings.h`. Add baud names `{"115200", "9600"}`, a plain FTXUI `Input` bound to `inputs.slaveAddr`, and an `InlineCycler` bound to `inputs.baudIdx` with a no-op commit callback and `autoCommit=false`. Add both components to `sysCfgForm` and render them under a `Modbus (next boot)` section in `sysCfgPopup`.

```cpp
auto scSlave = Input(&inputs.slaveAddr, "1-247");
auto scBaud = hvb::tui::InlineCycler(kBaudNames, &inputs.baudIdx, [] {});
```

When opening the dialog, call `syncDataToInputs(data, inputs)` before setting
`showSysCfg` so reopening discards abandoned edits and displays the last device
readback.

- [ ] **Step 2: Enqueue the dedicated save operation**

Add an `ActionButton("Save Modbus", ...)`. Validate before queueing with
`parseModbusSlaveAddress`. For valid input, snapshot the staged values and current
`data.sysCfg`, display `Writing Modbus config...`, then enqueue one worker item
that calls `saveModbusSettings` with `g_client.writeSlaveAddress()` and
`g_client.writeBaudRateCode()` callbacks.

On success, refresh `data.sysCfg`, call `syncDataToInputs`, and set:

```cpp
statusMsg = "OK: Modbus config saved — takes effect after reset";
```

Map `InvalidSlave` to `Error: slave address must be 1-247`, `InvalidBaud` to `Error: invalid baud rate`, and either write failure to `Error: ` plus `g_client.lastError()`. Always post `Event::Custom` after completion. Do not call `ParamAction::Save`, alter host connection values, close the dialog, or reset automatically.

- [ ] **Step 3: Build the TUI and run all host tests**

Run:

```bash
cmake --build build-tools --target hvb_tui hvb_tests -j
./build-tools/hvb_modbus_core/tests/hvb_tests
```

Expected: both targets build without warnings and all Catch2 cases pass.

- [ ] **Step 4: Manually verify dialog behavior**

Run `./build-tools/bin/hvb_tui`, connect to a device or simulator, open `Setting`, change each Modbus field without saving, and confirm no write occurs. Select `Save Modbus` and confirm the status bar shows the reset-required message. Confirm the existing system `Save` still invokes only the system parameter action and Reset remains manual.

- [ ] **Step 5: Commit the UI adapter**

```bash
git add tools/hvb_demo_app/tui/main.cpp
git commit -m "feat(tui): expose device modbus settings"
```

### Task 3: Final verification

**Files:**
- Verify: `docs/superpowers/specs/2026-07-02-tui-modbus-settings-design.md`
- Verify: all files changed by Tasks 1–2

- [ ] **Step 1: Check formatting and unintended changes**

Run:

```bash
git diff --check HEAD~2..HEAD
git status --short
```

Expected: no whitespace errors; only the user's pre-existing unrelated working-tree changes remain uncommitted.

- [ ] **Step 2: Re-run the complete verification set**

Run:

```bash
cmake --build build-tools --target hvb_tui hvb_tests -j
./build-tools/hvb_modbus_core/tests/hvb_tests
```

Expected: build succeeds and all tests pass.

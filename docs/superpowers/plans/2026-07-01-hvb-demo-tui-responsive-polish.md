# HVB Demo TUI Responsive Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make serial-port selection, Monitor status actions, and every TUI tab correct and responsive, with a balanced capability-aware Channel layout.

**Architecture:** Put device-independent UI decisions in a small header-only policy module and test them through the existing Catch2 host test target. Keep FTXUI components stable, use `Maybe` to remove unavailable controls from both rendering and focus, and use `flex`/`filler` rather than fixed outer geometry so the root and every tab consume resized terminal space.

**Tech Stack:** C++17, FTXUI 5, Catch2 3, CMake/CTest, existing `HvbModbusClient`.

---

## File structure

- Create `tools/hvb_demo_app/tui/tui_policy.h`: pure selection, status-click, and capability policies with no FTXUI dependency.
- Create `tools/hvb_modbus_core/tests/test_tui_policy.cpp`: Catch2 coverage for the pure policies.
- Modify `tools/hvb_modbus_core/tests/CMakeLists.txt`: compile the new policy tests.
- Modify `tools/hvb_demo_app/tui/main.cpp`: use cross-platform port discovery and responsive modal/root composition.
- Modify `tools/hvb_demo_app/tui/tab_monitor.h`: route status clicks through the tested policy and preserve flexible table sizing.
- Modify `tools/hvb_demo_app/tui/tab_channel.h`: balanced columns, aligned fields, and capability-aware rendering/focus.
- Modify `tools/hvb_demo_app/tui/tab_system.h`: equally expanding panels and vertical filler.

### Task 1: Add testable TUI policies

**Files:**
- Create: `tools/hvb_demo_app/tui/tui_policy.h`
- Create: `tools/hvb_modbus_core/tests/test_tui_policy.cpp`
- Modify: `tools/hvb_modbus_core/tests/CMakeLists.txt`

- [ ] **Step 1: Register a failing policy test source**

Add `test_tui_policy.cpp` to `add_executable(hvb_tests ...)` in `tools/hvb_modbus_core/tests/CMakeLists.txt`, then create the test with these cases:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "../../hvb_demo_app/tui/tui_policy.h"

using namespace hvb::tui;

TEST_CASE("port selection preserves an available port") {
    CHECK(selectedPortIndex({"A", "B"}, "B") == 1);
}

TEST_CASE("port selection falls back to the first available port") {
    CHECK(selectedPortIndex({"A", "B"}, "missing") == 0);
    CHECK(selectedPortIndex({}, "missing") == -1);
}

TEST_CASE("status click has no action for invalid, ramping, or zero-target off channels") {
    CHECK(statusClickAction(false, false, 10, 0, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, true, 10, 0, false) == StatusClickAction::None);
    CHECK(statusClickAction(true, false, 0, 0, false) == StatusClickAction::None);
}

TEST_CASE("status click enables a target and gracefully disables an active output") {
    CHECK(statusClickAction(true, false, 10, 0, false) == StatusClickAction::Enable);
    CHECK(statusClickAction(true, false, 10, 10, true) == StatusClickAction::DisableGraceful);
}

TEST_CASE("protection requires both measurement capabilities") {
    CHECK_FALSE(hasProtectionPolicy(0));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT));
    CHECK_FALSE(hasProtectionPolicy(CH_CAP_CURRENT_MEASUREMENT));
    CHECK(hasProtectionPolicy(CH_CAP_VOLTAGE_MEASUREMENT |
                              CH_CAP_CURRENT_MEASUREMENT));
}
```

- [ ] **Step 2: Run the test target and verify RED**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tests -j2
```

Expected: compilation fails because `tui_policy.h` does not exist.

- [ ] **Step 3: Add the minimal header-only policy**

Create `tools/hvb_demo_app/tui/tui_policy.h`:

```cpp
#pragma once

#include "register_map.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hvb::tui {

enum class StatusClickAction { None, Enable, DisableGraceful };

inline int selectedPortIndex(const std::vector<std::string>& ports,
                             const std::string& selected) {
    if (ports.empty()) return -1;
    const auto it = std::find(ports.begin(), ports.end(), selected);
    return it == ports.end() ? 0 : static_cast<int>(it - ports.begin());
}

inline StatusClickAction statusClickAction(bool valid, bool ramping,
                                           int16_t configuredTarget,
                                           int16_t operationalTarget,
                                           bool driveOn) {
    if (!valid || ramping) return StatusClickAction::None;
    if (!driveOn && configuredTarget == 0 && operationalTarget == 0)
        return StatusClickAction::None;
    return driveOn && operationalTarget != 0
        ? StatusClickAction::DisableGraceful
        : StatusClickAction::Enable;
}

inline bool hasProtectionPolicy(uint16_t caps) {
    constexpr uint16_t required = CH_CAP_VOLTAGE_MEASUREMENT |
                                  CH_CAP_CURRENT_MEASUREMENT;
    return (caps & required) == required;
}

} // namespace hvb::tui
```

- [ ] **Step 4: Build and run the policy tests**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tests -j2
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests "*port selection*,*status click*,*protection requires*"
```

Expected: build succeeds and all new assertions pass.

- [ ] **Step 5: Commit the policy seam**

```bash
git add tools/hvb_demo_app/tui/tui_policy.h \
        tools/hvb_modbus_core/tests/test_tui_policy.cpp \
        tools/hvb_modbus_core/tests/CMakeLists.txt
git commit -m "test(tui): cover interaction policies"
```

### Task 2: Make connection port selection scan-driven

**Files:**
- Modify: `tools/hvb_demo_app/tui/main.cpp`
- Test: `tools/hvb_modbus_core/tests/test_tui_policy.cpp`

- [ ] **Step 1: Verify the port-policy tests still pass before integration**

Run:

```bash
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests "*port selection*"
```

Expected: both port-selection cases pass.

- [ ] **Step 2: Replace local filesystem enumeration with the client API**

Remove the TUI-local `scanSerialPorts()` function and its `<filesystem>` dependency. Include `tui_policy.h`. Initialize and refresh the list with:

```cpp
auto portList = std::make_shared<std::vector<std::string>>();
int portIdx = -1;

auto doScanPorts = [&] {
    *portList = hvb::HvbModbusClient::scanPorts();
    portIdx = hvb::tui::selectedPortIndex(*portList, portVal);
    portVal = portIdx >= 0 ? (*portList)[portIdx] : std::string{};
    screen.PostEvent(Event::Custom);
};
```

Call `doScanPorts()` immediately before setting `showConnModal = true`. Do not scan at process startup unless the modal is opened.

- [ ] **Step 3: Render the selected port and Rescan inline**

Use one row for the label/list/button and preserve an explicit empty state:

```cpp
Element portChoice = portList->empty()
    ? text("(no ports found)") | dim | flex
    : portMenu->Render() | frame | flex;

hbox({
    text("Port  : "),
    portChoice,
    text(" "),
    bScan->Render(),
})
```

Keep Connect's existing `portVal.empty()` guard. Rename the button label from `Scan` to `Rescan`.

- [ ] **Step 4: Build the TUI**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tui -j2
```

Expected: `hvb_tui` builds with no new warnings.

- [ ] **Step 5: Commit the connection dialog**

```bash
git add tools/hvb_demo_app/tui/main.cpp
git commit -m "feat(tui): scan ports when opening connection settings"
```

### Task 3: Prevent invalid Monitor status actions

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_monitor.h`
- Test: `tools/hvb_modbus_core/tests/test_tui_policy.cpp`

- [ ] **Step 1: Run the status policy tests**

Run:

```bash
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests "*status click*"
```

Expected: both status-click cases pass.

- [ ] **Step 2: Route status clicks through the tested decision**

Include `tui_policy.h` and replace the inline status decision with:

```cpp
const uint16_t st = s.data.chInfo[ch].status;
const auto action = statusClickAction(
    s.data.valid,
    (st & ChStatus::RAMPING_ACTIVE) != 0,
    s.data.chCfg[ch].configuredTargetVRaw,
    s.data.chInfo[ch].operationalTargetVoltageRaw,
    (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0);
if (action == StatusClickAction::None) return;

const bool disabling = action == StatusClickAction::DisableGraceful;
const OutputAction outputAction = disabling
    ? OutputAction::DisableGraceful
    : OutputAction::Enable;
postWrite(s, inputs, disabling ? "Dis-Grace" : "Enable",
    [&s, ch, outputAction] {
        return s.client.sendOutputAction(ch, outputAction);
    }, refreshCh);
```

- [ ] **Step 3: Build and run all host tests**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tests hvb_tui -j2
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests
```

Expected: all tests pass and the TUI builds without new warnings.

- [ ] **Step 4: Commit the Monitor fix**

```bash
git add tools/hvb_demo_app/tui/tab_monitor.h
git commit -m "fix(tui): ignore off status clicks with zero target"
```

### Task 4: Reflow Channel and System tabs responsively

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_channel.h`
- Modify: `tools/hvb_demo_app/tui/tab_system.h`

- [ ] **Step 1: Add capability predicates and conditional focus**

In `makeChannelTab`, derive capability predicates from live data:

```cpp
auto hasOutput = [&s, ch] {
    return !s.data.valid ||
           (s.data.chInfo[ch].chCapFlags & CH_CAP_OUTPUT_ENABLE) != 0;
};
auto hasProtection = [&s, ch] {
    return !s.data.valid ||
           hasProtectionPolicy(s.data.chInfo[ch].chCapFlags);
};
```

Wrap protection controls as one `Container::Vertical` and apply `Maybe(protectionControls, hasProtection)`. Use the wrapped component in the main focus container so unavailable protection controls are neither rendered nor focusable. Apply the same pattern to output controls when output capability is absent.

- [ ] **Step 2: Build the balanced Channel layout**

Keep Live as a read-only full-width strip. Construct the body with equal flex columns:

```cpp
auto leftColumn = vbox({
    controlPanel | flex,
    settingPanel,
});
auto rightColumn = vbox({
    hasProtection() ? protectionPanel : emptyElement(),
    recoveryPanel | flex,
});

return vbox({
    livePanel,
    hbox({
        leftColumn | flex,
        rightColumn | flex,
    }) | flex,
    filler(),
});
```

Within Control, render aligned vertical rows with an eight-character label area: buttons first, then Vset, Ru, and Rd. Put Setting below Control. Put Protection above Recovery. Use `flex` on input renders instead of fixed outer panel widths.

- [ ] **Step 3: Make System panels expand equally**

Remove the left panel's fixed minimum width. Return equal flex panels and top alignment:

```cpp
return vbox({
    hbox({
        leftPanel | flex,
        rightPanel | flex,
    }) | flex,
    filler(),
});
```

Give editable values a readable minimum width, but do not constrain either panel's outer width.

- [ ] **Step 4: Build after layout changes**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tui -j2
```

Expected: build succeeds with no new warnings.

- [ ] **Step 5: Commit responsive tab layouts**

```bash
git add tools/hvb_demo_app/tui/tab_channel.h \
        tools/hvb_demo_app/tui/tab_system.h
git commit -m "feat(tui): make channel and system tabs responsive"
```

### Task 5: Verify all tabs under resize

**Files:**
- Modify: `tools/hvb_demo_app/tui/main.cpp` only if verification finds a fixed root constraint
- Modify: `tools/hvb_demo_app/tui/tab_monitor.h` only if verification finds a table expansion defect

- [ ] **Step 1: Audit outer geometry**

Run:

```bash
rg -n "size\((WIDTH|HEIGHT), (EQUAL|GREATER_THAN|LESS_THAN)" tools/hvb_demo_app/tui
```

Expected: fixed sizing is limited to small inputs and modal bounds; no tab or root content uses a fixed outer width/height.

- [ ] **Step 2: Keep the root and Monitor content flexible**

Confirm the root retains `tabContent->Render() | flex`, Monitor returns `table.Render()` followed by `filler()`, and each Monitor column is decorated with `flex`. If the audit finds a fixed outer constraint, remove it and apply `flex` at that container boundary.

- [ ] **Step 3: Run full automated verification**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tests hvb_tui -j2
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests
git diff --check
```

Expected: build succeeds, all Catch2 tests pass, and `git diff --check` produces no output.

- [ ] **Step 4: Perform focused manual QA**

Run `tools/build/linux-debug/bin/hvb_tui`, then verify:

1. Connection dialog scans before display and Rescan updates the selection-only list.
2. With no listed port, Connect has no effect.
3. Every tab fills 80×24 and larger terminal sizes after live resize.
4. Channel columns grow evenly and labels remain aligned.
5. A capability-deficient channel cannot focus hidden Protection controls.
6. A zero-target off channel does not send an action or display RAMP after Status click.

- [ ] **Step 5: Commit any verification-only geometry correction**

If Step 2 required a correction:

```bash
git add tools/hvb_demo_app/tui/main.cpp tools/hvb_demo_app/tui/tab_monitor.h
git commit -m "fix(tui): preserve flexible tab geometry"
```

If no correction was needed, do not create an empty commit.

### Task 6: Separate Live from the Channel panel body

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_channel.h`

- [ ] **Step 1: Add exactly one external blank row**

In the final Channel renderer, place one `emptyElement()` between `livePanel`
and the flexible two-column body:

```cpp
return vbox({
    livePanel,
    emptyElement(),
    hbox({ leftColumn | flex, rightColumn | flex }) | flex,
    filler(),
});
```

Do not add padding inside any panel and do not change the focus container.

- [ ] **Step 2: Build the TUI**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tui -j2
```

Expected: `hvb_tui` builds successfully with no new compiler warnings.

- [ ] **Step 3: Verify the host suite and whitespace**

Run:

```bash
cmake --build tools/build/linux-debug --target hvb_tests -j2
tools/build/linux-debug/hvb_modbus_core/tests/hvb_tests
git diff --check
```

Expected: all Catch2 tests pass and `git diff --check` produces no output.

- [ ] **Step 4: Commit the spacing adjustment**

```bash
git add tools/hvb_demo_app/tui/tab_channel.h
git commit -m "style(tui): separate channel live panel"
```

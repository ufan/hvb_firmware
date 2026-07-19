# Host-Tools Compat-Check & Variant Lookup Table Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PsbModbusClient::connect()` refuse to operate (not just warn)
on a protocol mismatch, and give every host tool a human-readable board
identity display (`jw_hvb (HVB family, rev A)`, `v1.2.0` firmware) instead
of raw register integers — the last piece of the version management
contract, per §4 and §6 of the design spec.

**Architecture:** A pure, unit-testable `psb::reg::protocolCompatible(major,
minor)` function (major must equal exactly, minor must be `>=`) gates
`PsbModbusClient::connect()`, replacing its previous loose 1-15 sanity-only
check. `readSystemInfo()`'s existing single-batch register read widens from
16 to 17 registers to also decode the already-added `BOARD_HW_REVISION`
register into a new `SystemInfo::boardHwRevision` field. A new header,
`board_catalog.h`, provides host-tool-side `variantName()`/`variantFamily()`/
`hwRevisionLabel()` lookup functions (the wire protocol stays numeric-only
by design — see spec §4) plus `formatFwVersion()` for the packed SemVer
register. All five host tools (CLI, TUI, factory REPL, demo GUI, factory
GUI) already call through the one shared `PsbModbusClient`/`SystemInfo`
path, so the compat-check applies to all of them automatically; only the
*display* wiring needs touching per tool.

**Tech Stack:** C++17 (`tools/psb_modbus_core`), Catch2 (unit tests, no
hardware needed), FTXUI (TUI), Qt6/QML (GUI, confirmed available in this
environment at `/home/yong/backup/Qt/6.8.5`).

## Global Constraints

- Design authority: `docs/superpowers/specs/2026-07-19-version-management-contract-design.md`
  §4 (wire registers, host-tool lookup table) and §6 (compat rule: exact
  major match, minor `>=`, refuse not warn, firmware version/hw revision
  are diagnostics-only and never gate behavior).
- **Verified live, not assumed**: a reserved/unmapped Modbus input register
  (e.g. `BOARD_HW_REVISION` on a pre-v3.3 board) reads back as `0` with
  `REG_OK` inside the same batch transaction (`lib/modbus_adapter/
  modbus_register.c:284-287`, `read_wire()`: `if (!wire->mapped) { *reg =
  0U; return REG_OK; }`) — it does **not** raise a Modbus exception that
  would fail the whole batch read. This is why widening `readSystemInfo()`'s
  batch from 16 to 17 registers is safe against older, already-deployed
  firmware: no version-gating logic is needed for `boardHwRevision`
  (unlike `currentUnitExp`, which needs gating because `0` is a
  misleadingly valid-looking value for that field specifically).
- `connect()` (the real serial-port path) has no unit-test coverage
  anywhere in this codebase (`attachTestArrays()` bypasses it entirely) —
  this is pre-existing, not something this plan fixes. The compat-check
  logic is therefore extracted into a pure, dependency-free function
  (`psb::reg::protocolCompatible`) specifically so it *can* be unit tested,
  with `connect()` reduced to a thin caller.
- `VC_PROTOCOL_MAJOR`/`VC_PROTOCOL_MINOR` (currently `3`/`3`) are already
  available as C macros to every file that includes `register_map.h` — it
  transitively includes `reg_store/reg_map.h` (`register_map.h:9`). No new
  constant needs to be defined; the client's own "expected" version is
  simply whatever this shared header declares it was built against.
- All five host tools were rebuilt successfully in this environment during
  plan authoring (`psb_demo_cli`, `psb_demo_tui`, `psb_factory_tui` via the
  `linux-release` preset; `psb_demo_gui`, `psb_factory_gui` via the
  `linux-gui` preset, Qt 6.8.5) — every step below reproduces changes that
  were already compiled and tested live, not speculative.
- `tools/build/linux-release` and `tools/build/linux-gui` already exist in
  this environment (incremental builds, not from-scratch).

---

## Task 1: Extract protocolCompatible() and formatFwVersion() into register_map.h

**Files:**
- Modify: `tools/psb_modbus_core/register_map.h`

**Interfaces:**
- Produces: `psb::reg::protocolCompatible(int major, int minor) -> bool`
  and `psb::reg::formatFwVersion(uint32_t packed) -> std::string`. Task 2
  consumes `protocolCompatible` in `connect()`; Tasks 4-8 consume
  `formatFwVersion` in every tool's display code.

- [ ] **Step 1: Add both functions**

Insert into `tools/psb_modbus_core/register_map.h`, immediately before the
existing `// Time — single-register UINT16 seconds` section:

```cpp
// Formats a packed FW_VERSION register (major:8 | minor:8 | patch:16, see
// docs/superpowers/specs/2026-07-19-version-management-contract-design.md
// §4) as "vMAJOR.MINOR.PATCH". All-zero ("v0.0.0") legitimately means "no
// real version known" (pre-v3.3 board, or an untagged firmware build) —
// not a read failure.
inline std::string formatFwVersion(uint32_t packed) {
    unsigned major = (packed >> 24) & 0xFFu;
    unsigned minor = (packed >> 16) & 0xFFu;
    unsigned patch = packed & 0xFFFFu;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "v%u.%u.%u", major, minor, patch);
    return buf;
}

// Protocol compatibility rule (design spec §6): exact major match required
// (a major bump means a breaking wire-format change this client literally
// cannot speak), minor must be at least what this client was built against
// (a newer firmware may add registers this client doesn't know about, which
// is fine — it simply won't request them).
inline bool protocolCompatible(int major, int minor) {
    return major == VC_PROTOCOL_MAJOR && minor >= VC_PROTOCOL_MINOR;
}
```

- [ ] **Step 2: Commit**

```bash
git add tools/psb_modbus_core/register_map.h
git commit -m "feat(psb_modbus_core): add protocolCompatible() and formatFwVersion() helpers"
```

(No standalone test/build step here — Task 3 exercises both functions via
`psb_tests`, since `register_map.h` is header-only with no library target
of its own to build in isolation.)

---

## Task 2: Add board_catalog.h lookup table

**Files:**
- Create: `tools/psb_modbus_core/board_catalog.h`

**Interfaces:**
- Produces: `psb::catalog::variantName(int) -> std::string`,
  `psb::catalog::variantFamily(int) -> std::string`,
  `psb::catalog::hwRevisionLabel(int) -> std::string`. Consumed by Tasks
  4-8 (all five tools' display code) and tested in Task 3.

- [ ] **Step 1: Create the file**

```cpp
#pragma once
// Copyright (c) 2026 Jianwei
// SPDX-License-Identifier: Apache-2.0
//
// Host-tool-side lookup table for board variant identity, per
// docs/superpowers/specs/2026-07-19-version-management-contract-design.md
// §4: the wire protocol stays numeric-only (VARIANT_ID, BOARD_HW_REVISION),
// so translating those into human-readable names/labels is a host-tool
// concern, not a protocol concern. Extend this table when a new board
// variant ships — this is the one place that needs updating.

#include <string>

namespace psb::catalog {

// VARIANT_ID -> board name. Matches VC_VARIANT_ID's Kconfig documentation
// (lib/voltage_control/Kconfig: "1 = HVB, 2 = LVB").
inline std::string variantName(int variantId) {
    switch (variantId) {
        case 1: return "jw_hvb";
        case 2: return "jw_lvb";
        default: return "unknown (id=" + std::to_string(variantId) + ")";
    }
}

// VARIANT_ID -> board family label — organizational grouping only (design
// spec §2: "never bumped — it's a label, not a version"), never gates
// behavior.
inline std::string variantFamily(int variantId) {
    switch (variantId) {
        case 1: return "HVB family";
        case 2: return "LVB family";
        default: return "unknown family";
    }
}

// BOARD_HW_REVISION -> human label. No board in this tree declares real
// board.yml revisions yet (every board defaults to index 0 — see
// CONFIG_VC_BOARD_HW_REVISION), so this is a generic 0=rev A/1=rev B/...
// scheme pending real per-variant revision names once board.yml revisions
// exist.
inline std::string hwRevisionLabel(int hwRevision) {
    if (hwRevision >= 0 && hwRevision < 26) {
        return std::string("rev ") + static_cast<char>('A' + hwRevision);
    }
    return "rev #" + std::to_string(hwRevision);
}

} // namespace psb::catalog
```

- [ ] **Step 2: Commit**

```bash
git add tools/psb_modbus_core/board_catalog.h
git commit -m "feat(psb_modbus_core): add board_catalog variant/revision lookup table"
```

---

## Task 3: Add BOARD_HW_REVISION to SystemInfo, widen readSystemInfo, add real compat-check to connect()

**Files:**
- Modify: `tools/psb_modbus_core/types.h`
- Modify: `tools/psb_modbus_core/psb_modbus_client.cpp`
- Modify: `tools/psb_modbus_core/tests/virtual_board.cpp`
- Modify: `tools/psb_modbus_core/tests/test_system_reads.cpp`

**Interfaces:**
- Consumes: `psb::reg::protocolCompatible`, `psb::reg::formatFwVersion`
  (Task 1), `psb::catalog::*` (Task 2, test-only in this task).
- Produces: `SystemInfo::boardHwRevision` (new field) — consumed by Tasks
  4-8's display wiring.

- [ ] **Step 1: Add the field to SystemInfo**

Modify `tools/psb_modbus_core/types.h`:

```cpp
    int protoMajor = 0;
    int protoMinor = 0;
    int variantId = 0;
    int boardHwRevision = 0;  // v3.3+; 0 on older firmware (reserved reg reads 0)
    uint16_t sysCapFlags = 0;
```

- [ ] **Step 2: Widen readSystemInfo()'s batch read and decode the new field**

Modify `tools/psb_modbus_core/psb_modbus_client.cpp`, in
`SystemInfo PsbModbusClient::readSystemInfo()`:

```cpp
    uint16_t buf[17] = {};
    if (!readRegsInternal(false, reg::sysAddr(0), 17, buf)) return info;

    info.protoMajor       = static_cast<int>(buf[SYS_PROTOCOL_MAJOR]);
    info.protoMinor       = static_cast<int>(buf[SYS_PROTOCOL_MINOR]);
    info.variantId        = static_cast<int>(buf[SYS_VARIANT_ID]);
    info.boardHwRevision  = static_cast<int>(buf[SYS_BOARD_HW_REVISION]);
```

(Only the array size, the read-count argument, and the one new decode line
change — every other line in the function is unchanged.)

- [ ] **Step 3: Replace connect()'s loose sanity check with a real compat-check**

Modify `tools/psb_modbus_core/psb_modbus_client.cpp`, in
`bool PsbModbusClient::connect(...)`:

```cpp
    // Probe: verify the board actually responds before claiming success.
    // Read PROTOCOL_MAJOR/MINOR (input offsets 0-1) in one transaction.
    uint16_t probe[2] = {0xFFFF, 0xFFFF};
    if (!readRegsInternal(false, reg::sysAddr(0), 2, probe)) {
        m_impl->errorText = "no response from board — check baud rate, slave ID, and cabling";
        m_impl->disconnect();
        return false;
    }
    int protoMajor = static_cast<int>(probe[0]);
    int protoMinor = static_cast<int>(probe[1]);
    if (protoMajor < 1 || protoMajor > 15) {
        m_impl->errorText = "unexpected protocol version " + std::to_string(protoMajor)
                            + " — check baud rate and slave ID";
        m_impl->disconnect();
        return false;
    }
    // Real compatibility gate (design spec §6): refuse to operate, not just
    // warn, on a protocol mismatch — an exact major mismatch means this
    // client cannot correctly speak the wire format at all, and a lower
    // minor means the firmware predates a register this client may rely on.
    if (!reg::protocolCompatible(protoMajor, protoMinor)) {
        m_impl->errorText = "firmware protocol v" + std::to_string(protoMajor) + "."
                            + std::to_string(protoMinor) + " is incompatible with this tool "
                            + "(requires v" + std::to_string(VC_PROTOCOL_MAJOR) + "."
                            + std::to_string(VC_PROTOCOL_MINOR) + " or newer, same major version)";
        m_impl->disconnect();
        return false;
    }

    return true;
}
```

(This replaces the old single-register `probe` read and its one `if`
block. The garbage-on-the-wire sanity check — catching e.g. baud/slave-ID
misconfiguration — is kept as a distinct, earlier check with its own
message; the new compat-check is a second, later gate with a distinct
message, so a user sees "wrong baud rate" vs. "wrong firmware version" as
genuinely different diagnoses.)

- [ ] **Step 4: Seed the new register in the shared test fixture**

Modify `tools/psb_modbus_core/tests/virtual_board.cpp`, in
`VirtualBoard::setVariantDefaults()`:

```cpp
    inputRegs[reg::sysAddr(0) + SYS_VARIANT_ID] = 1;
    inputRegs[reg::sysAddr(0) + SYS_BOARD_HW_REVISION] = 0;
```

- [ ] **Step 5: Add tests**

Modify `tools/psb_modbus_core/tests/test_system_reads.cpp`. First, add the
include:

```cpp
#include "psb_modbus_client.h"
#include "types.h"
#include "register_map.h"
#include "board_catalog.h"

#include <catch2/catch_test_macros.hpp>
```

Then in `fillDefaultInputRegs()`, add:

```cpp
    regs[2] = 1;    // Variant ID
    regs[16] = 0;   // Board HW revision
```

Then in the `"SystemInfo — defaults"` test case, add an assertion right
after the existing `variantId` check:

```cpp
    CHECK(info.variantId == 1);
    CHECK(info.boardHwRevision == 0);
    CHECK(info.supportedChannels == 2);
```

Then append these four new test cases at the end of the file:

```cpp
TEST_CASE("SystemInfo — board hardware revision decoded", "[system-reads]") {
    uint16_t inputRegs[280] = {};
    uint16_t holdingRegs[280] = {};
    fillDefaultInputRegs(inputRegs);
    inputRegs[16] = 2;  // Board HW revision (offset 16, "rev C")

    psb::PsbModbusClient client;
    client.attachTestArrays(inputRegs, holdingRegs, 280);

    auto info = client.readSystemInfo();
    CHECK(info.boardHwRevision == 2);
}

TEST_CASE("board_catalog — variant/family/revision lookup", "[system-reads]") {
    CHECK(psb::catalog::variantName(1) == "jw_hvb");
    CHECK(psb::catalog::variantName(2) == "jw_lvb");
    CHECK(psb::catalog::variantName(99) == "unknown (id=99)");

    CHECK(psb::catalog::variantFamily(1) == "HVB family");
    CHECK(psb::catalog::variantFamily(2) == "LVB family");
    CHECK(psb::catalog::variantFamily(99) == "unknown family");

    CHECK(psb::catalog::hwRevisionLabel(0) == "rev A");
    CHECK(psb::catalog::hwRevisionLabel(1) == "rev B");
    CHECK(psb::catalog::hwRevisionLabel(2) == "rev C");
}

TEST_CASE("reg::formatFwVersion — decodes packed major/minor/patch", "[system-reads]") {
    CHECK(psb::reg::formatFwVersion(0x00000000u) == "v0.0.0");
    // major=1, minor=2, patch=3: 0x01 << 24 | 0x02 << 16 | 0x0003
    CHECK(psb::reg::formatFwVersion(0x01020003u) == "v1.2.3");
}

TEST_CASE("reg::protocolCompatible — major must match, minor must be >=", "[system-reads]") {
    // Client built against VC_PROTOCOL_MAJOR.VC_PROTOCOL_MINOR (currently 3.3).
    CHECK(psb::reg::protocolCompatible(VC_PROTOCOL_MAJOR, VC_PROTOCOL_MINOR));
    CHECK(psb::reg::protocolCompatible(VC_PROTOCOL_MAJOR, VC_PROTOCOL_MINOR + 1));
    CHECK_FALSE(psb::reg::protocolCompatible(VC_PROTOCOL_MAJOR, VC_PROTOCOL_MINOR - 1));
    CHECK_FALSE(psb::reg::protocolCompatible(VC_PROTOCOL_MAJOR + 1, VC_PROTOCOL_MINOR));
    CHECK_FALSE(psb::reg::protocolCompatible(VC_PROTOCOL_MAJOR - 1, 99));
}
```

- [ ] **Step 6: Build and run the full test suite**

```sh
cd tools
cmake --build build/linux-release --target psb_tests
build/linux-release/psb_modbus_core/tests/psb_tests
```

Expected: `All tests passed (262 assertions in 68 test cases)` — 22 more
assertions and 4 more test cases than the pre-existing 240/64 baseline.

- [ ] **Step 7: Commit**

```bash
git add tools/psb_modbus_core/types.h tools/psb_modbus_core/psb_modbus_client.cpp \
        tools/psb_modbus_core/tests/virtual_board.cpp \
        tools/psb_modbus_core/tests/test_system_reads.cpp
git commit -m "feat(psb_modbus_core): add BOARD_HW_REVISION read + real protocol compat-check in connect()"
```

---

## Task 4: Wire lookup table + formatted display into CLI

**Files:**
- Modify: `tools/psb_demo_app/cli/main.cpp`

**Interfaces:** Consumes `psb::catalog::*` (Task 2), `psb::reg::formatFwVersion`
(Task 1), `SystemInfo::boardHwRevision` (Task 3).

- [ ] **Step 1: Add the include**

```cpp
#include "psb_modbus_client.h"
#include "config_manager.h"
#include "register_map.h"
#include "board_catalog.h"
```

- [ ] **Step 2: Replace the raw Variant ID line and FW Version line in cmdInfo()**

```cpp
    printSep("Protocol:", std::to_string(info.protoMajor) + "." + std::to_string(info.protoMinor));
    printSep("Variant:", psb::catalog::variantName(info.variantId) + " ("
        + psb::catalog::variantFamily(info.variantId) + ", "
        + psb::catalog::hwRevisionLabel(info.boardHwRevision) + ")");
```

and further down:

```cpp
    printSep("FW Version:", psb::reg::formatFwVersion(info.fwVersion));
```

- [ ] **Step 3: Build**

```sh
cd tools
cmake --build build/linux-release --target psb_demo_cli
```

Expected: links successfully.

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/cli/main.cpp
git commit -m "feat(psb_demo_cli): show variant name/family/revision and SemVer firmware version"
```

---

## Task 5: Wire lookup table + formatted display into TUI

**Files:**
- Modify: `tools/psb_demo_app/tui/tab_system.h`

**Interfaces:** same as Task 4.

- [ ] **Step 1: Add the include**

```cpp
#pragma once
#include "widgets.h"
#include "board_catalog.h"
#include <memory>
#include <string>
```

- [ ] **Step 2: Replace the Variant ID / FW Version rows**

```cpp
        auto leftPanel = vbox({
            hbox({ text("Protocol  : "), text(std::to_string(si.protoMajor)+"."+std::to_string(si.protoMinor)) }),
            hbox({ text("Variant   : "), text(psb::catalog::variantName(si.variantId) + " ("
                + psb::catalog::hwRevisionLabel(si.boardHwRevision) + ")") }),
            hbox({ text("FW Version: "), text(psb::reg::formatFwVersion(si.fwVersion)) }),
```

(This removes the now-unused `char fw[16]; snprintf(fw, ...)` block that
previously fed the FW Version row — the `tmp`/`hum` buffers just above it,
used by the Board Temp/Humidity rows, are untouched.)

- [ ] **Step 3: Build**

```sh
cd tools
cmake --build build/linux-release --target psb_demo_tui
```

- [ ] **Step 4: Commit**

```bash
git add tools/psb_demo_app/tui/tab_system.h
git commit -m "feat(psb_demo_tui): show variant name/revision and SemVer firmware version"
```

---

## Task 6: Wire lookup table + formatted display into Factory REPL

**Files:**
- Modify: `tools/psb_factory_tool/repl/FactoryCommands.cpp`

**Interfaces:** same as Task 4.

- [ ] **Step 1: Add the include**

```cpp
#include "FactoryCommands.h"
#include "register_map.h"
#include "board_catalog.h"
```

- [ ] **Step 2: Replace the "info" command's Variant/FW lines**

```cpp
                auto info = session.client().readSystemInfo();
                out << "Protocol: " << info.protoMajor << "." << info.protoMinor << "\n"
                    << "Variant:  " << psb::catalog::variantName(info.variantId) << " ("
                        << psb::catalog::variantFamily(info.variantId) << ", "
                        << psb::catalog::hwRevisionLabel(info.boardHwRevision) << ")\n"
                    << "FW:       " << psb::reg::formatFwVersion(info.fwVersion) << "\n"
```

(Removes the now-unused `char fw[12]; std::snprintf(fw, ...)` lines that
previously fed the `"FW:"` line.)

- [ ] **Step 3: Build**

```sh
cd tools
cmake --build build/linux-release --target psb_factory_tui
```

- [ ] **Step 4: Commit**

```bash
git add tools/psb_factory_tool/repl/FactoryCommands.cpp
git commit -m "feat(psb_factory_tui): show variant name/family/revision and SemVer firmware version"
```

---

## Task 7: Wire lookup table + formatted display into Demo GUI

**Files:**
- Modify: `tools/psb_demo_app/gui/modbus_worker.cpp`
- Modify: `tools/psb_demo_app/gui/qml/main.qml`

**Interfaces:** Consumes Tasks 1-3's C++ functions; produces new
`QVariantMap` keys (`variantName`, `variantFamily`, `boardHwRevision`,
`boardHwRevisionLabel`, `fwVersionStr`) consumed by `main.qml`.

- [ ] **Step 1: Add the include**

```cpp
#include "modbus_worker.h"
#include "register_map.h"
#include "board_catalog.h"
```

- [ ] **Step 2: Extend systemInfoToMap() with the new bridged fields**

```cpp
    m["variantId"] = info.variantId;
    m["variantName"] = QString::fromStdString(psb::catalog::variantName(info.variantId));
    m["variantFamily"] = QString::fromStdString(psb::catalog::variantFamily(info.variantId));
    m["boardHwRevision"] = info.boardHwRevision;
    m["boardHwRevisionLabel"] = QString::fromStdString(psb::catalog::hwRevisionLabel(info.boardHwRevision));
    m["supportedChannels"] = info.supportedChannels;
    m["activeChMask"] = info.activeChMask;
    m["boardTempC"] = static_cast<double>(info.boardTempRaw) * 0.1;  // raw LSB, estimate
    m["boardHumidityPct"] = static_cast<double>(info.boardHumidityRaw) * 0.1;
    m["uptimeSec"] = info.uptimeSec;
    m["fwVersion"] = info.fwVersion;
    m["fwVersionStr"] = QString::fromStdString(psb::reg::formatFwVersion(info.fwVersion));
```

(Existing keys are all kept as-is — this only inserts new keys alongside
them, so nothing else consuming `systemInfoToMap()`'s output breaks.)

- [ ] **Step 3: Update main.qml's toolbar display**

```qml
                LabeledValue {
                    label: "FW"
                    value: backend.connected ? (backend.sysInfo.fwVersionStr || "--") : "--"
                }
                LabeledValue {
                    label: "Proto"
                    value: backend.connected
                        ? (backend.sysInfo.protoMajor || 0) + "." + (backend.sysInfo.protoMinor || 0)
                        : "--"
                }
                LabeledValue {
                    label: "Variant"
                    value: backend.connected
                        ? (backend.sysInfo.variantName || "--") + " (" + (backend.sysInfo.boardHwRevisionLabel || "--") + ")"
                        : "--"
                }
```

- [ ] **Step 4: Build**

```sh
cd tools
cmake --build build/linux-gui --target psb_demo_gui
```

Expected: links successfully, including QML AOT compilation of the
modified `main.qml` (a QML syntax error would fail this build step, not
just show at runtime).

- [ ] **Step 5: Commit**

```bash
git add tools/psb_demo_app/gui/modbus_worker.cpp tools/psb_demo_app/gui/qml/main.qml
git commit -m "feat(psb_demo_gui): show variant name/revision and SemVer firmware version"
```

---

## Task 8: Wire lookup table + formatted display into Factory GUI

**Files:**
- Modify: `tools/psb_factory_tool/gui/CalibrationBackend.cpp`
- Modify: `tools/psb_factory_tool/gui/qml/components/DeviceInfoCard.qml`
- Modify: `tools/psb_factory_tool/gui/ReportEngine.cpp`

**Interfaces:** `CalibrationBackend::deviceInfo()` bridges a `QVariantMap`
(consumed by `DeviceInfoCard.qml`); `ReportEngine.cpp` reads
`ReportData::deviceInfo`, which is a plain `psb::SystemInfo` (see
`tools/psb_factory_tool/gui/ReportData.h:16`), so it already has
`boardHwRevision` once Task 3 lands — no separate bridging needed there.

- [ ] **Step 1: Add the include to CalibrationBackend.cpp**

```cpp
#include "CalibrationBackend.h"
#include "SweepData.h"
#include "register_map.h"
#include "board_catalog.h"
```

- [ ] **Step 2: Extend deviceInfo()'s QVariantMap**

```cpp
QVariantMap CalibrationBackend::deviceInfo() const {
    QVariantMap m;
    m["protoMajor"]        = m_sysInfo.protoMajor;
    m["protoMinor"]        = m_sysInfo.protoMinor;
    m["variantId"]         = m_sysInfo.variantId;
    m["variantName"]       = QString::fromStdString(psb::catalog::variantName(m_sysInfo.variantId));
    m["variantFamily"]     = QString::fromStdString(psb::catalog::variantFamily(m_sysInfo.variantId));
    m["boardHwRevision"]   = m_sysInfo.boardHwRevision;
    m["boardHwRevisionLabel"] = QString::fromStdString(psb::catalog::hwRevisionLabel(m_sysInfo.boardHwRevision));
    m["supportedChannels"] = m_sysInfo.supportedChannels;
    m["activeChMask"]      = (int)m_sysInfo.activeChMask;
    m["fwVersion"]         = QString::fromStdString(psb::reg::formatFwVersion(m_sysInfo.fwVersion));
    m["boardTemp"]         = m_sysInfo.boardTempRaw / 10.0;
    m["uptimeSec"]         = (int)m_sysInfo.uptimeSec;
    return m;
```

(`fwVersion`'s existing key changes from a raw `0x%1`-hex `QString` to the
`vX.Y.Z` format — `DeviceInfoCard.qml`'s `{label: "Firmware", value:
Backend.deviceInfo.fwVersion}` row picks up the new formatting
automatically with no QML change needed for that row.)

- [ ] **Step 3: Update DeviceInfoCard.qml's Variant row**

```qml
                {label: "Protocol",  value: Backend.deviceInfo.protoMajor + "." + Backend.deviceInfo.protoMinor},
                {label: "Firmware",  value: Backend.deviceInfo.fwVersion},
                {label: "Variant",   value: Backend.deviceInfo.variantName + " (" + Backend.deviceInfo.boardHwRevisionLabel + ")"},
```

- [ ] **Step 4: Add the include to ReportEngine.cpp**

```cpp
#include "ReportEngine.h"
#include "register_map.h"
#include "board_catalog.h"
```

- [ ] **Step 5: Update the report's Firmware/Variant rows**

```cpp
    kv("Protocol",     QString("%1.%2").arg(data.deviceInfo.protoMajor).arg(data.deviceInfo.protoMinor));
    kv("Firmware",     QString::fromStdString(psb::reg::formatFwVersion(data.deviceInfo.fwVersion)));
    kv("Variant",      QString::fromStdString(psb::catalog::variantName(data.deviceInfo.variantId))
                            + " (" + QString::fromStdString(psb::catalog::hwRevisionLabel(data.deviceInfo.boardHwRevision)) + ")");
```

(This is `data.deviceInfo`, a `psb::SystemInfo` struct member per
`ReportData.h:16` — not the `CalibrationBackend::deviceInfo()`
`QVariantMap` from Steps 1-2 — so `data.deviceInfo.boardHwRevision` is
directly available once Task 3's `SystemInfo` field exists, with no
further bridging required here.)

- [ ] **Step 6: Build**

```sh
cd tools
cmake --build build/linux-gui --target psb_factory_gui
```

Expected: links successfully, including QML AOT compilation of the
modified `DeviceInfoCard.qml`.

- [ ] **Step 7: Commit**

```bash
git add tools/psb_factory_tool/gui/CalibrationBackend.cpp \
        tools/psb_factory_tool/gui/qml/components/DeviceInfoCard.qml \
        tools/psb_factory_tool/gui/ReportEngine.cpp
git commit -m "feat(psb_factory_gui): show variant name/revision and SemVer firmware version"
```

---

## Task 9: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Run the full unit test suite one more time**

```sh
cd tools
build/linux-release/psb_modbus_core/tests/psb_tests
```

Expected: `All tests passed (262 assertions in 68 test cases)`.

- [ ] **Step 2: Rebuild every affected binary from a clean incremental state**

```sh
cmake --build build/linux-release --target psb_demo_tui psb_demo_cli psb_factory_tui
cmake --build build/linux-gui --target psb_demo_gui psb_factory_gui
```

Expected: all five link successfully with no errors.

- [ ] **Step 3: No further commit needed**

This task is verification-only. If any step above surfaces a regression,
fix it as part of the task that introduced it and re-commit there.

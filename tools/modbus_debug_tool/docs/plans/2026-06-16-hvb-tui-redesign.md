# HVB TUI Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the read-only TUI skeleton with a fully interactive operator console: channel table with row selection, action panel for writing target/ramp/protection config, system and channel detail tabs.

**Architecture:** Split `tui/main.cpp` into header-only tab factories (`tab_monitor.h`, `tab_system.h`, `tab_channel.h`) plus shared helpers (`tui_format.h` for pure formatting, `widgets.h` for FTXUI component builders). Each tab factory closes over an `AppState&` that bundles the client, connection flag, polled data, status string, and screen reference. All writes dispatch on a detached thread via `writeAsync()`.

**Tech Stack:** FTXUI v5.0.0 (`Container::Vertical`, `Renderer`, `CatchEvent`, `Button`, `Input`, `ButtonOption`), C++17, ModbusLib (serhmarch), doctest.

---

## File Structure

| Path | Action | Responsibility |
|---|---|---|
| `tui/tui_format.h` | **Create** | `ScannedData`, pure format helpers (`fmtVoltage`, `statusBadge`, `faultStr`, `protCompact`) — no FTXUI |
| `tui/widgets.h` | **Create** | `AppState`, `writeAsync`, `CommitInput`, `InlineCycler`, `ActionButton` |
| `tui/tab_monitor.h` | **Create** | `makeMonitorTab()` — table + action panel + keyboard nav |
| `tui/tab_system.h` | **Create** | `makeSystemTab()` — left info + right config |
| `tui/tab_channel.h` | **Create** | `makeChannelTab()` — live bar + all config sections |
| `tui/main.cpp` | **Modify** | Remove old render functions; wire AppState + new tab factories |
| `tui/CMakeLists.txt` | **No change** | All new files are headers |
| `tests/CMakeLists.txt` | **Modify** | Add `test_tui_format.cpp` |
| `tests/test_tui_format.cpp` | **Create** | Unit tests for pure format helpers |

---

## Task 1: tui_format.h — pure format helpers

**Files:**
- Create: `tui/tui_format.h`
- Create: `tests/test_tui_format.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_tui_format.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "../tui/tui_format.h"

using namespace hvb::tui;
using namespace hvb;

TEST_CASE("statusBadge") {
    CHECK(statusBadge(0x0000) == "OFF");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO) == "ON");
    CHECK(statusBadge(ChStatus::RAMPING_ACTIVE) == "RAMP");
    CHECK(statusBadge(ChStatus::OUTPUT_DRIVE_NONZERO | ChStatus::RAMPING_ACTIVE) == "ON RAMP");
    CHECK(statusBadge(ChStatus::ACTIVE_FAULT) == "FAULT");
    CHECK(statusBadge(ChStatus::COOLDOWN_ACTIVE) == "COOL");
    CHECK(statusBadge(ChStatus::UNSUPPORTED) == "UNSUP");
}

TEST_CASE("faultStr") {
    CHECK(faultStr(0) == "--");
    CHECK(faultStr(FaultCause::VOLTAGE_LIMIT) == "VL");
    CHECK(faultStr(FaultCause::VOLTAGE_LIMIT | FaultCause::CURRENT_LIMIT) == "VL CL");
    CHECK(faultStr(FaultCause::OUTPUT_HW_FAULT) == "HW");
}

TEST_CASE("protCompact") {
    CHECK(protCompact(ProtectionMode::Disabled,          OutputAction::None)             == "Disabled");
    CHECK(protCompact(ProtectionMode::FlagOnly,          OutputAction::None)             == "FlagOnly");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::DisableImmediate) == "Apply/Dis-Im");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::ForceOutputZero)  == "Apply/Force0");
    CHECK(protCompact(ProtectionMode::ApplyOutputAction, OutputAction::Clamp)            == "Apply/Clamp");
}

TEST_CASE("fmtVoltage") {
    CHECK(fmtVoltage(5000)  == "+500.0 V");
    CHECK(fmtVoltage(0)     == "+0.0 V");
    CHECK(fmtVoltage(-1000) == "-100.0 V");
}

TEST_CASE("fmtCurrentUA") {
    // 1 LSB = 1 nA; 32767 LSB = 32.767 µA
    CHECK(fmtCurrentUA(32767) == "+32.767 uA");
    CHECK(fmtCurrentUA(0)     == "+0.000 uA");
    CHECK(fmtCurrentUA(-1000) == "-1.000 uA");
}
```

- [ ] **Step 2: Add test to CMakeLists**

In `tests/CMakeLists.txt`, add alongside existing test sources:
```cmake
target_sources(hvb_tests PRIVATE test_tui_format.cpp)
```

- [ ] **Step 3: Run tests — verify they fail**

```bash
cd tools/modbus_debug_tool
cmake --preset linux-debug
cmake --build build/linux-debug --target hvb_tests 2>&1 | tail -5
# Expected: error: 'hvb::tui' has not been declared  (or file not found)
```

- [ ] **Step 4: Create tui/tui_format.h**

```cpp
#pragma once
#include "types.h"
#include "register_map.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace hvb::tui {

struct ScannedData {
    hvb::SystemInfo   sysInfo{};
    hvb::ChannelInfo  chInfo[2]{};
    hvb::SystemConfig sysCfg{};
    hvb::ChannelConfig chCfg[2]{};
    bool valid = false;
};

inline std::string fmtVoltage(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f V", hvb::reg::voltageToV(raw));
    return buf;
}

inline std::string fmtCurrentUA(int16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.3f uA", hvb::reg::currentToA(raw) * 1e6);
    return buf;
}

inline std::string fmtInterval(uint16_t raw) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f s", hvb::reg::intervalToS(raw));
    return buf;
}

inline std::string statusBadge(uint16_t status) {
    using namespace hvb::ChStatus;
    if (status & UNSUPPORTED)     return "UNSUP";
    if (status & ACTIVE_FAULT)    return "FAULT";
    if (status & COOLDOWN_ACTIVE) return "COOL";
    if (status & RETRY_EXHAUSTED) return "RETRY-X";
    bool on   = (status & OUTPUT_DRIVE_NONZERO) != 0;
    bool ramp = (status & RAMPING_ACTIVE) != 0;
    if (on && ramp) return "ON RAMP";
    if (on)         return "ON";
    if (ramp)       return "RAMP";
    return "OFF";
}

inline std::string faultStr(uint16_t fault) {
    if (!fault) return "--";
    std::string s;
    if (fault & hvb::FaultCause::VOLTAGE_LIMIT)        s += "VL ";
    if (fault & hvb::FaultCause::CURRENT_LIMIT)        s += "CL ";
    if (fault & hvb::FaultCause::MEASUREMENT_INVALID)  s += "MI ";
    if (fault & hvb::FaultCause::OUTPUT_HW_FAULT)      s += "HW ";
    if (fault & hvb::FaultCause::VARIANT_INTERLOCK)    s += "IL ";
    if (fault & hvb::FaultCause::AUTO_RETRY_EXHAUSTED) s += "RE ";
    if (fault & hvb::FaultCause::CONFIG_INVALID_AUTO)  s += "CI ";
    if (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string protCompact(ProtectionMode mode, OutputAction action) {
    switch (mode) {
    case ProtectionMode::Disabled:          return "Disabled";
    case ProtectionMode::FlagOnly:          return "FlagOnly";
    case ProtectionMode::ApplyOutputAction: break;
    }
    switch (action) {
    case OutputAction::DisableGraceful:  return "Apply/Dis-Gr";
    case OutputAction::DisableImmediate: return "Apply/Dis-Im";
    case OutputAction::ForceOutputZero:  return "Apply/Force0";
    case OutputAction::Clamp:            return "Apply/Clamp";
    default:                             return "Apply/None";
    }
}

} // namespace hvb::tui
```

- [ ] **Step 5: Run tests — verify they pass**

```bash
cmake --build build/linux-debug --target hvb_tests
./build/linux-debug/tests/hvb_tests -tc="statusBadge,faultStr,protCompact,fmtVoltage,fmtCurrentUA"
# Expected: all 5 test cases PASS
```

- [ ] **Step 6: Commit**

```bash
git add tui/tui_format.h tests/test_tui_format.cpp tests/CMakeLists.txt
git commit -m "tui: add tui_format.h with pure format helpers and tests"
```

---

## Task 2: widgets.h — FTXUI component builders and AppState

**Files:**
- Create: `tui/widgets.h`

- [ ] **Step 1: Create tui/widgets.h**

```cpp
#pragma once
#include "tui_format.h"
#include "hvb_modbus_client.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

namespace hvb::tui {

struct AppState {
    hvb::HvbModbusClient&     client;
    std::atomic<bool>&        connected;
    ScannedData&              data;
    std::string&              statusMsg;
    ftxui::ScreenInteractive& screen;
};

// Dispatch fn on a background thread; update statusMsg on completion.
// fn must not capture any stack locals that may be destroyed before it runs.
inline void writeAsync(AppState& s, const std::string& label, std::function<bool()> fn) {
    s.statusMsg = "Writing " + label + "...";
    s.screen.PostEvent(Event::Custom);
    std::thread([&s, label, fn = std::move(fn)]() mutable {
        bool ok = fn();
        s.statusMsg = ok ? ("OK: " + label) : ("Error: " + s.client.lastError());
        s.screen.PostEvent(Event::Custom);
    }).detach();
}

// Input that calls onCommit when Enter is pressed (instead of inserting newline).
inline Component CommitInput(std::string* val,
                             const std::string& placeholder,
                             std::function<void()> onCommit) {
    auto inp = Input(val, placeholder);
    return CatchEvent(inp, [onCommit](Event e) {
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Button-backed inline dropdown.
// Renders as "[current ▾]". Space/←/→ cycles; Enter commits.
// Uses Button so it participates in FTXUI's Tab-focus chain.
inline Component InlineCycler(std::vector<std::string> opts,
                               int* sel,
                               std::function<void()> onCommit) {
    auto optsPtr = std::make_shared<std::vector<std::string>>(std::move(opts));
    auto bopt    = ButtonOption{};
    bopt.transform = [sel, optsPtr](const EntryState& es) -> Element {
        std::string lbl = "[" + optsPtr->at(*sel) + " \xe2\x96\xbe]"; // UTF-8 ▾
        auto e = text(lbl);
        if (es.focused) e = e | inverted;
        return e;
    };
    // onClick: cycle forward (mouse-click support)
    auto btn = Button("", [sel, optsPtr] { *sel = (*sel + 1) % (int)optsPtr->size(); }, bopt);
    return CatchEvent(btn, [sel, optsPtr, onCommit](Event e) {
        int n = (int)optsPtr->size();
        if (e == Event::Character(' ') || e == Event::ArrowRight) {
            *sel = (*sel + 1) % n; return true;
        }
        if (e == Event::ArrowLeft) {
            *sel = (*sel - 1 + n) % n; return true;
        }
        if (e == Event::Return) { onCommit(); return true; }
        return false;
    });
}

// Styled action button: "[ label ]", inverted when focused.
inline Component ActionButton(const std::string& label, std::function<void()> onClick) {
    auto bopt = ButtonOption{};
    bopt.transform = [](const EntryState& es) -> Element {
        auto e = text("[ " + es.label + " ]");
        if (es.focused) e = e | inverted;
        return e;
    };
    return Button(label, std::move(onClick), bopt);
}

} // namespace hvb::tui
```

- [ ] **Step 2: Verify it compiles (included by main.cpp compilation)**

```bash
cmake --build build/linux-debug --target hvb_tui 2>&1 | tail -10
# main.cpp doesn't include widgets.h yet, so no error yet — that's fine.
# The header itself will be compiled when included in Task 3+.
```

- [ ] **Step 3: Commit**

```bash
git add tui/widgets.h
git commit -m "tui: add widgets.h — AppState, writeAsync, CommitInput, InlineCycler, ActionButton"
```

---

## Task 3: tab_monitor.h — Monitor tab

**Files:**
- Create: `tui/tab_monitor.h`

The monitor tab renders a system summary bar, a 2-row channel table with ▶ row selection, and an action panel below. Keyboard contract: ↑/↓ moves selected row, Tab enters action panel, Escape leaves it.

The action panel is built per-channel; `Container::Tab({panel0, panel1}, &st->selectedRow)` switches between them automatically so the interactive inputs always match the selected row.

- [ ] **Step 1: Create tui/tab_monitor.h**

```cpp
#pragma once
#include "widgets.h"
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeMonitorTab(AppState& s) {
    // ---- Dropdown tables ----
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kVActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero","Clamp"};
    static const std::vector<OutputAction> kVActVals  = {
        OutputAction::None, OutputAction::DisableGraceful, OutputAction::DisableImmediate,
        OutputAction::ForceOutputZero, OutputAction::Clamp
    };
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };

    // ---- Tab-local state ----
    struct St {
        int  selectedRow  = 0;
        bool panelFocused = false;
        std::string targetV[2];
        std::string ruStep[2], ruInt[2];
        std::string rdStep[2], rdInt[2];
        std::string vThr[2],   iThr[2];
        int vModeIdx[2]{}, vActIdx[2]{};
        int iModeIdx[2]{}, iActIdx[2]{};
    };
    auto st = std::make_shared<St>();

    // ---- Read-only table (called from Renderer every frame) ----
    auto drawTable = [&s, st]() -> Element {
        if (!s.data.valid)
            return text(" Not connected — press 'c' to connect ") | center | bold;

        const auto& si = s.data.sysInfo;
        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        auto sysbar = hbox({
            text("Proto: " + std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor)),
            text("  Variant: " + std::to_string(si.variantId)),
            text("  Uptime: " + std::to_string(si.uptimeSec) + " s"),
            text("  Mode: " + std::string(opModeName(si.activeOpMode))),
            text("  Temp: " + std::string(tmp) + " \xc2\xb0\x43"), // °C
            text("  Humid: " + std::string(hum) + " %"),
            text("  Fault: " + faultStr(si.faultCause)),
        });

        std::vector<std::vector<std::string>> rows;
        rows.push_back({"CH","Vmeas","Imeas","Status","Ramp\xe2\x86\x91","Ramp\xe2\x86\x93","I-Prot","Target V","Fault"});
        for (int ch = 0; ch < 2; ++ch) {
            const auto& ci = s.data.chInfo[ch];
            const auto& cc = s.data.chCfg[ch];
            std::string sel = (ch == st->selectedRow) ? "\xe2\x96\xb6" : " ";
            if (ci.status & ChStatus::UNSUPPORTED) {
                rows.push_back({sel + std::to_string(ch), "(unsupported)", "", "", "", "", "", "", ""});
                continue;
            }
            rows.push_back({
                sel + std::to_string(ch),
                fmtVoltage(ci.voltageRaw),
                fmtCurrentUA(ci.currentRaw),
                statusBadge(ci.status),
                std::to_string(cc.rampUpStepRaw),
                std::to_string(cc.rampDownStepRaw),
                protCompact(cc.iProtMode, cc.iProtOutputAction),
                fmtVoltage(cc.configuredTargetVRaw),
                faultStr(ci.activeFault),
            });
        }
        auto tbl = Table(rows);
        tbl.SelectAll().Border(LIGHT);
        tbl.SelectAll().Separator(LIGHT);
        tbl.SelectRows(0, 0).Decorate(bold);
        tbl.SelectRows(0, 0).Separator(HEAVY);
        if (s.data.valid && st->selectedRow < 2)
            tbl.SelectRows(st->selectedRow + 1, st->selectedRow + 1)
               .Decorate(bold | color(Color::Cyan));
        return vbox({ sysbar, separator(), tbl.Render() });
    };

    // ---- Action panel factory (one per channel, both built at startup) ----
    auto makePanel = [&s, st, &kProtModes, &kVActNames, &kVActVals,
                                           &kIActNames, &kIActVals](int ch) -> Component {
        auto onTarget = [&s, st, ch] {
            try {
                auto raw = hvb::reg::voltageFromV(std::stod(st->targetV[ch]));
                writeAsync(s, "Target V", [&s, ch, raw] {
                    return s.client.writeConfiguredTargetVoltage(ch, raw);
                });
            } catch (...) { s.statusMsg = "Error: invalid voltage"; }
        };
        auto onRampUp = [&s, st, ch] {
            try {
                auto step = (uint16_t)std::stoul(st->ruStep[ch]);
                auto iv   = (uint16_t)std::stoul(st->ruInt[ch]);
                writeAsync(s, "Ramp Up", [&s, ch, step, iv] {
                    return s.client.writeRampUp(ch, step, iv);
                });
            } catch (...) { s.statusMsg = "Error: invalid ramp-up value"; }
        };
        auto onRampDown = [&s, st, ch] {
            try {
                auto step = (uint16_t)std::stoul(st->rdStep[ch]);
                auto iv   = (uint16_t)std::stoul(st->rdInt[ch]);
                writeAsync(s, "Ramp Down", [&s, ch, step, iv] {
                    return s.client.writeRampDown(ch, step, iv);
                });
            } catch (...) { s.statusMsg = "Error: invalid ramp-down value"; }
        };
        auto onVProt = [&s, st, ch, &kVActVals] {
            try {
                auto mode   = static_cast<ProtectionMode>(st->vModeIdx[ch]);
                auto action = kVActVals.at(st->vActIdx[ch]);
                auto raw    = hvb::reg::voltageFromV(std::stod(st->vThr[ch]));
                writeAsync(s, "V Limit", [&s, ch, mode, action, raw] {
                    return s.client.writeVoltageProtection(ch, mode, action, raw);
                });
            } catch (...) { s.statusMsg = "Error: invalid V-limit value"; }
        };
        auto onIProt = [&s, st, ch, &kIActVals] {
            try {
                auto mode   = static_cast<ProtectionMode>(st->iModeIdx[ch]);
                auto action = kIActVals.at(st->iActIdx[ch]);
                // User types µA; 1 LSB = 1 nA → multiply by 1000
                auto raw    = static_cast<int16_t>(std::stod(st->iThr[ch]) * 1000.0 + 0.5);
                writeAsync(s, "I Limit", [&s, ch, mode, action, raw] {
                    return s.client.writeCurrentProtection(ch, mode, action, raw);
                });
            } catch (...) { s.statusMsg = "Error: invalid I-limit value"; }
        };

        auto tgtInp = CommitInput(&st->targetV[ch],  "+0.0",   onTarget);
        auto ruStepInp = CommitInput(&st->ruStep[ch], "0",     onRampUp);
        auto ruIntInp  = CommitInput(&st->ruInt[ch],  "0",     onRampUp);
        auto rdStepInp = CommitInput(&st->rdStep[ch], "0",     onRampDown);
        auto rdIntInp  = CommitInput(&st->rdInt[ch],  "0",     onRampDown);
        auto vModeC = InlineCycler(kProtModes, &st->vModeIdx[ch], onVProt);
        auto vActC  = InlineCycler(kVActNames, &st->vActIdx[ch],  onVProt);
        auto vThrInp = CommitInput(&st->vThr[ch],    "+0.0",   onVProt);
        auto iModeC = InlineCycler(kProtModes, &st->iModeIdx[ch], onIProt);
        auto iActC  = InlineCycler(kIActNames, &st->iActIdx[ch],  onIProt);
        auto iThrInp = CommitInput(&st->iThr[ch],    "0.000",  onIProt);

        auto bEnable  = ActionButton("Enable",    [&s,ch]{ writeAsync(s,"Enable",    [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }); });
        auto bDisImm  = ActionButton("Dis-Immed", [&s,ch]{ writeAsync(s,"Dis-Immed", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }); });
        auto bDisGra  = ActionButton("Dis-Grace", [&s,ch]{ writeAsync(s,"Dis-Grace", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }); });
        auto bClrAct  = ActionButton("ClrActive", [&s,ch]{ writeAsync(s,"ClrActive", [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }); });
        auto bClrHist = ActionButton("ClrHist",   [&s,ch]{ writeAsync(s,"ClrHist",   [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }); });

        // Flat Vertical container: Tab order matches visual top-to-bottom order
        auto container = Container::Vertical({
            tgtInp,
            ruStepInp, ruIntInp,
            rdStepInp, rdIntInp,
            vModeC, vActC, vThrInp,
            iModeC, iActC, iThrInp,
            bEnable, bDisImm, bDisGra, bClrAct, bClrHist,
        });

        // Custom renderer: lays out inputs with label text; Container handles focus/events
        return Renderer(container, [=, ch]() {
            std::string title = " CH" + std::to_string(ch) + " ";
            return window(text(title) | bold, vbox({
                hbox({ text("  Target    : "), tgtInp->Render(), text(" V") }),
                hbox({ text("  Ramp Up   : step "), ruStepInp->Render(),
                       text(" LSB  interval "), ruIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  Ramp Down : step "), rdStepInp->Render(),
                       text(" LSB  interval "), rdIntInp->Render(), text(" \xc3\x970.1 s") }),
                hbox({ text("  V Limit   : "), vModeC->Render(), text("  "), vActC->Render(),
                       text("  threshold "), vThrInp->Render(), text(" V") }),
                hbox({ text("  I Limit   : "), iModeC->Render(), text("  "), iActC->Render(),
                       text("  threshold "), iThrInp->Render(), text(" \xc2\xb5\x41") }), // µA
                separator(),
                hbox({ text("  "), bEnable->Render(), text("  "), bDisImm->Render(),
                       text("  "), bDisGra->Render(), text("    "),
                       bClrAct->Render(), text("  "), bClrHist->Render() }),
            }));
        });
    };

    auto panel0 = makePanel(0);
    auto panel1 = makePanel(1);

    // Container::Tab switches the active (event-routing) panel when selectedRow changes
    auto panelTab = Container::Tab({panel0, panel1}, &st->selectedRow);

    // Full tab: render table + panel; route keyboard between them
    return Renderer(panelTab, [=, &s, st, drawTable]() {
        return vbox({ drawTable(), separator(), panelTab->Render() });
    }) | CatchEvent([st](Event e) {
        if (!st->panelFocused) {
            if (e == Event::ArrowUp)   { st->selectedRow = std::max(0, st->selectedRow - 1); return true; }
            if (e == Event::ArrowDown) { st->selectedRow = std::min(1, st->selectedRow + 1); return true; }
            if (e == Event::Tab)       { st->panelFocused = true; return true; }
            return false;
        }
        if (e == Event::Escape) { st->panelFocused = false; return true; }
        return false;
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Commit**

```bash
git add tui/tab_monitor.h
git commit -m "tui: add tab_monitor.h — table, action panel, row selection nav"
```

---

## Task 4: tab_system.h — System tab

**Files:**
- Create: `tui/tab_system.h`

- [ ] **Step 1: Create tui/tab_system.h**

```cpp
#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeSystemTab(AppState& s) {
    static const std::vector<std::string> kOpModes    = {"Normal", "Automatic"};
    static const std::vector<std::string> kBaudNames  = {"115200", "9600"};
    static const std::vector<std::string> kRecovNames = {"ManualLatch","AutoRetry","AutoDerate","NeverRetry"};
    static const std::vector<std::string> kSaveNames  = {"No", "Yes"};

    struct St {
        std::string slaveAddr, retryDelay, retryMax, retryWindow, vBand, iBand;
        int opModeIdx = 0, baudIdx = 0, recovIdx = 0;
    };
    auto st = std::make_shared<St>();

    auto onOpMode  = [&s, st] { writeAsync(s, "OpMode",   [&s, st] { return s.client.writeOperatingMode(static_cast<OpMode>(st->opModeIdx)); }); };
    auto onBaud    = [&s, st] { writeAsync(s, "BaudRate", [&s, st] { return s.client.writeBaudRateCode((uint16_t)st->baudIdx); }); };
    auto onSlave   = [&s, st] {
        try { uint16_t a = (uint16_t)std::stoul(st->slaveAddr);
              writeAsync(s, "SlaveAddr", [&s, a] { return s.client.writeSlaveAddress(a); }); }
        catch (...) { s.statusMsg = "Error: invalid slave address"; }
    };
    auto onRecov   = [&s, st] {
        try {
            auto pol = static_cast<RecoveryPolicy>(st->recovIdx);
            int d = std::stoi(st->retryDelay), m = std::stoi(st->retryMax),
                w = std::stoi(st->retryWindow);
            writeAsync(s, "Recovery", [&s, pol, d, m, w] {
                return s.client.writeSystemRecoveryPolicy(pol, d, m, w);
            });
        } catch (...) { s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onBands   = [&s, st] {
        try {
            uint16_t v = (uint16_t)std::stoul(st->vBand), i = (uint16_t)std::stoul(st->iBand);
            writeAsync(s, "SafeBands", [&s, v, i] { return s.client.writeSafeBands(v, i); });
        } catch (...) { s.statusMsg = "Error: invalid band value"; }
    };

    auto opModeC   = InlineCycler(kOpModes,   &st->opModeIdx, onOpMode);
    auto baudC     = InlineCycler(kBaudNames, &st->baudIdx,   onBaud);
    auto recovC    = InlineCycler(kRecovNames,&st->recovIdx,  onRecov);
    auto slaveInp  = CommitInput(&st->slaveAddr,   "1",   onSlave);
    auto delayInp  = CommitInput(&st->retryDelay,  "0",   onRecov);
    auto maxInp    = CommitInput(&st->retryMax,    "3",   onRecov);
    auto winInp    = CommitInput(&st->retryWindow, "60",  onRecov);
    auto vBandInp  = CommitInput(&st->vBand,       "10",  onBands);
    auto iBandInp  = CommitInput(&st->iBand,       "10",  onBands);

    auto bSave    = ActionButton("Save",    [&s]{ writeAsync(s,"Save",    [&s]{ return s.client.sendParamAction(-1, ParamAction::Save); }); });
    auto bLoad    = ActionButton("Load",    [&s]{ writeAsync(s,"Load",    [&s]{ return s.client.sendParamAction(-1, ParamAction::Load); }); });
    auto bFactory = ActionButton("Factory", [&s]{ writeAsync(s,"Factory", [&s]{ return s.client.sendParamAction(-1, ParamAction::FactoryReset); }); });
    auto bReset   = ActionButton("Reset",   [&s]{ writeAsync(s,"Reset",   [&s]{ return s.client.sendParamAction(-1, ParamAction::SoftwareReset); }); });

    auto cfgContainer = Container::Vertical({
        opModeC, baudC, slaveInp, recovC,
        delayInp, maxInp, winInp,
        vBandInp, iBandInp,
        bSave, bLoad, bFactory, bReset,
    });

    return Renderer(cfgContainer, [=, &s]() {
        if (!s.data.valid)
            return text(" Not connected ") | center | border;

        const auto& si = s.data.sysInfo;
        const auto& sc = s.data.sysCfg;

        // Left: read-only info
        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        char fw[16];
        snprintf(fw, sizeof(fw), "0x%04X", si.fwVersion);
        auto leftPanel = vbox({
            hbox({ text("Protocol  : "), text(std::to_string(si.protoMajor)+"."+std::to_string(si.protoMinor)) }),
            hbox({ text("Variant ID: "), text(std::to_string(si.variantId)) }),
            hbox({ text("FW Version: "), text(fw) }),
            hbox({ text("Channels  : "), text(std::to_string(si.supportedChannels)) }),
            hbox({ text("Uptime    : "), text(std::to_string(si.uptimeSec) + " s") }),
            hbox({ text("Board Temp: "), text(std::string(tmp) + " \xc2\xb0\x43") }),
            hbox({ text("Humidity  : "), text(std::string(hum) + " %RH") }),
            hbox({ text("Op Mode   : "), text(opModeName(si.activeOpMode)) }),
            hbox({ text("Fault     : "), text(faultStr(si.faultCause)) }),
        }) | border | size(WIDTH, GREATER_THAN, 38);

        // Right: writable config
        auto rightPanel = vbox({
            hbox({ text("Op Mode    : "), opModeC->Render() }),
            hbox({ text("Slave Addr : "), slaveInp->Render(), text("  (0-247)") }),
            hbox({ text("Baud Rate  : "), baudC->Render() }),
            hbox({ text("Recovery   : "), recovC->Render() }),
            hbox({ text("  Delay    : "), delayInp->Render(), text(" s") }),
            hbox({ text("  Max Retry: "), maxInp->Render() }),
            hbox({ text("  Window   : "), winInp->Render(), text(" s") }),
            hbox({ text("V Safe Band: "), vBandInp->Render(), text(" %  (0-50)") }),
            hbox({ text("I Safe Band: "), iBandInp->Render(), text(" %  (0-50)") }),
            separator(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "),
                   bFactory->Render(), text("  "), bReset->Render() }),
        }) | border;

        return hbox({ leftPanel, rightPanel });
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Commit**

```bash
git add tui/tab_system.h
git commit -m "tui: add tab_system.h — system info + writable config"
```

---

## Task 5: tab_channel.h — Channel detail tab

**Files:**
- Create: `tui/tab_channel.h`

- [ ] **Step 1: Create tui/tab_channel.h**

```cpp
#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeChannelTab(AppState& s, int ch) {
    static const std::vector<std::string> kProtModes = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kVActNames = {"None","Dis-Graceful","Dis-Immed","ForceZero","Clamp"};
    static const std::vector<OutputAction> kVActVals = {
        OutputAction::None, OutputAction::DisableGraceful, OutputAction::DisableImmediate,
        OutputAction::ForceOutputZero, OutputAction::Clamp
    };
    static const std::vector<std::string> kIActNames = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kSaveTarget = {"No","Yes"};

    struct St {
        std::string targetV, vThr, iThr;
        std::string ruStep, ruInt, rdStep, rdInt, derateStep;
        std::string outK, outB, measVK, measVB, measIK, measIB;
        int vModeIdx = 0, vActIdx = 0;
        int iModeIdx = 0, iActIdx = 0;
        int saveTargetIdx = 0;
    };
    auto st = std::make_shared<St>();

    auto onTarget = [&s, st, ch] {
        try {
            auto raw = hvb::reg::voltageFromV(std::stod(st->targetV));
            writeAsync(s, "Target V", [&s, ch, raw] {
                return s.client.writeConfiguredTargetVoltage(ch, raw);
            });
        } catch (...) { s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->ruStep);
            auto iv   = (uint16_t)std::stoul(st->ruInt);
            writeAsync(s, "Ramp Up", [&s, ch, step, iv] { return s.client.writeRampUp(ch, step, iv); });
        } catch (...) { s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->rdStep);
            auto iv   = (uint16_t)std::stoul(st->rdInt);
            writeAsync(s, "Ramp Down", [&s, ch, step, iv] { return s.client.writeRampDown(ch, step, iv); });
        } catch (...) { s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onDerate = [&s, st, ch] {
        try {
            auto step = (uint16_t)std::stoul(st->derateStep);
            writeAsync(s, "Derate", [&s, ch, step] { return s.client.writeDerateStep(ch, step); });
        } catch (...) { s.statusMsg = "Error: invalid derate step"; }
    };
    auto onVProt = [&s, st, ch, &kVActVals] {
        try {
            auto mode   = static_cast<ProtectionMode>(st->vModeIdx);
            auto action = kVActVals.at(st->vActIdx);
            auto raw    = hvb::reg::voltageFromV(std::stod(st->vThr));
            writeAsync(s, "V Limit", [&s, ch, mode, action, raw] {
                return s.client.writeVoltageProtection(ch, mode, action, raw);
            });
        } catch (...) { s.statusMsg = "Error: invalid V-limit value"; }
    };
    auto onIProt = [&s, st, ch, &kIActVals] {
        try {
            auto mode   = static_cast<ProtectionMode>(st->iModeIdx);
            auto action = kIActVals.at(st->iActIdx);
            auto raw    = static_cast<int16_t>(std::stod(st->iThr) * 1000.0 + 0.5);
            writeAsync(s, "I Limit", [&s, ch, mode, action, raw] {
                return s.client.writeCurrentProtection(ch, mode, action, raw);
            });
        } catch (...) { s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto onCal = [&s, st, ch] {
        try {
            auto ok  = s.client.writeCalibrationOutput(ch, (uint16_t)std::stoul(st->outK),  (int16_t)std::stoi(st->outB));
            ok &= s.client.writeCalibrationMeasV(ch, (uint16_t)std::stoul(st->measVK), (int16_t)std::stoi(st->measVB));
            ok &= s.client.writeCalibrationMeasI(ch, (uint16_t)std::stoul(st->measIK), (int16_t)std::stoi(st->measIB));
            s.statusMsg = ok ? "OK: Calibration" : "Error: " + s.client.lastError();
            s.screen.PostEvent(Event::Custom);
        } catch (...) { s.statusMsg = "Error: invalid calibration value"; }
    };
    auto onSaveTarget = [&s, st, ch] {
        bool save = st->saveTargetIdx != 0;
        writeAsync(s, "SaveTarget", [&s, ch, save] { return s.client.writeSaveTargetPolicy(ch, save); });
    };

    auto tgtInp    = CommitInput(&st->targetV,  "+0.0",  onTarget);
    auto ruStepInp = CommitInput(&st->ruStep,   "0",     onRampUp);
    auto ruIntInp  = CommitInput(&st->ruInt,    "0",     onRampUp);
    auto rdStepInp = CommitInput(&st->rdStep,   "0",     onRampDown);
    auto rdIntInp  = CommitInput(&st->rdInt,    "0",     onRampDown);
    auto derInp    = CommitInput(&st->derateStep,"0",    onDerate);
    auto vModeC    = InlineCycler(kProtModes, &st->vModeIdx, onVProt);
    auto vActC     = InlineCycler(kVActNames, &st->vActIdx,  onVProt);
    auto vThrInp   = CommitInput(&st->vThr,    "+0.0",  onVProt);
    auto iModeC    = InlineCycler(kProtModes, &st->iModeIdx, onIProt);
    auto iActC     = InlineCycler(kIActNames, &st->iActIdx,  onIProt);
    auto iThrInp   = CommitInput(&st->iThr,    "0.000", onIProt);
    auto outKInp   = CommitInput(&st->outK,    "10000", onCal);
    auto outBInp   = CommitInput(&st->outB,    "0",     onCal);
    auto measVKInp = CommitInput(&st->measVK,  "10000", onCal);
    auto measVBInp = CommitInput(&st->measVB,  "0",     onCal);
    auto measIKInp = CommitInput(&st->measIK,  "10000", onCal);
    auto measIBInp = CommitInput(&st->measIB,  "0",     onCal);
    auto saveTgtC  = InlineCycler(kSaveTarget, &st->saveTargetIdx, onSaveTarget);

    auto bEnable  = ActionButton("Enable",    [&s,ch]{ writeAsync(s,"Enable",    [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }); });
    auto bDisImm  = ActionButton("Dis-Immed", [&s,ch]{ writeAsync(s,"Dis-Immed", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }); });
    auto bDisGra  = ActionButton("Dis-Grace", [&s,ch]{ writeAsync(s,"Dis-Grace", [&s,ch]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }); });
    auto bSave    = ActionButton("Save",      [&s,ch]{ writeAsync(s,"Save",      [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::Save); }); });
    auto bLoad    = ActionButton("Load",      [&s,ch]{ writeAsync(s,"Load",      [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::Load); }); });
    auto bFactory = ActionButton("Factory",   [&s,ch]{ writeAsync(s,"Factory",   [&s,ch]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }); });
    auto bClrAct  = ActionButton("ClrActive", [&s,ch]{ writeAsync(s,"ClrActive", [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }); });
    auto bClrHist = ActionButton("ClrHist",   [&s,ch]{ writeAsync(s,"ClrHist",   [&s,ch]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }); });

    auto container = Container::Vertical({
        tgtInp, bEnable, bDisImm, bDisGra,
        ruStepInp, ruIntInp, rdStepInp, rdIntInp, derInp,
        vModeC, vActC, vThrInp,
        iModeC, iActC, iThrInp,
        outKInp, outBInp, measVKInp, measVBInp, measIKInp, measIBInp,
        saveTgtC, bSave, bLoad, bFactory, bClrAct, bClrHist,
    });

    return Renderer(container, [=, &s, ch]() {
        // Live readings bar
        Element liveBar = text(" Not connected ") | dim;
        if (s.data.valid) {
            const auto& ci = s.data.chInfo[ch];
            if (ci.status & ChStatus::UNSUPPORTED) {
                liveBar = text("  Channel " + std::to_string(ch) + " unsupported") | bold;
            } else {
                char lastFault[24] = "--";
                if (ci.lastFaultTimestamp > 0)
                    snprintf(lastFault, sizeof(lastFault), "%u s ago", (unsigned)ci.lastFaultTimestamp);
                liveBar = hbox({
                    text("  Vmeas: "),  text(fmtVoltage(ci.voltageRaw))  | bold,
                    text("   Imeas: "), text(fmtCurrentUA(ci.currentRaw)) | bold,
                    text("   Op Target: "), text(fmtVoltage(ci.operationalTargetVoltageRaw)),
                    text("   Status: "), text(statusBadge(ci.status)) | bold,
                    text("   Retries: "), text(std::to_string(ci.retryCount)),
                    text("\n  Active Fault: "), text(faultStr(ci.activeFault)),
                    text("   Fault History: "), text(faultStr(ci.faultHistory)),
                    text("   Cooldown: "), text(std::to_string(ci.cooldownSec) + " s"),
                    text("   Last Fault: "), text(lastFault),
                });
            }
        }

        auto outputPanel = window(text(" Output "), vbox({
            hbox({ text("Target V : "), tgtInp->Render(), text(" V") }),
            hbox({ bEnable->Render(), text("  "), bDisImm->Render(), text("  "), bDisGra->Render() }),
        }));

        auto rampPanel = window(text(" Ramping "), vbox({
            hbox({ text("Ramp Up   : step "), ruStepInp->Render(), text(" LSB  int "), ruIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Ramp Down : step "), rdStepInp->Render(), text(" LSB  int "), rdIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Derate Step:      "), derInp->Render(),   text(" LSB") }),
        }));

        auto vProtPanel = window(text(" Voltage Protection "), hbox({
            text("Mode : "), vModeC->Render(),
            text("   Action : "), vActC->Render(),
            text("   Threshold: "), vThrInp->Render(), text(" V"),
        }));

        auto iProtPanel = window(text(" Current Protection "), hbox({
            text("Mode : "), iModeC->Render(),
            text("   Action : "), iActC->Render(),
            text("   Threshold: "), iThrInp->Render(), text(" \xc2\xb5\x41"), // µA
        }));

        auto calPanel = window(text(" Calibration "), vbox({
            hbox({ text("Output  K: "), outKInp->Render(), text("  B: "), outBInp->Render() }),
            hbox({ text("Meas V  K: "), measVKInp->Render(), text("  B: "), measVBInp->Render() }),
            hbox({ text("Meas I  K: "), measIKInp->Render(), text("  B: "), measIBInp->Render() }),
        }));

        auto persistPanel = window(text(" Persistence "), vbox({
            hbox({ text("Save Target: "), saveTgtC->Render() }),
            separator(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
            hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
        }));

        return vbox({
            window(text(" CH" + std::to_string(ch) + " Live "), liveBar),
            hbox({ outputPanel, rampPanel }),
            vProtPanel,
            iProtPanel,
            hbox({ calPanel, persistPanel }),
        });
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Commit**

```bash
git add tui/tab_channel.h
git commit -m "tui: add tab_channel.h — live bar, output/ramp/protection/cal/persist panels"
```

---

## Task 6: Refactor main.cpp

**Files:**
- Modify: `tui/main.cpp`

Replace old `renderMonitor/renderSystemInfo/renderChannelDetail` and the `ScannedData` struct with the new headers. Move tab switching to the outer CatchEvent (number keys) so arrow keys always reach the tab content.

- [ ] **Step 1: Rewrite tui/main.cpp**

```cpp
#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "tab_monitor.h"
#include "tab_system.h"
#include "tab_channel.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace ftxui;

static hvb::HvbModbusClient g_client;
static hvb::ConfigManager   g_cfg;
static std::atomic<bool>    g_connected{false};
static int g_pollInterval = 2;

int main(int argc, char** argv) {
    g_cfg.load();

    std::string portArg;
    int baudArg = 115200, slaveArg = 1, timeoutArg = 500;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-p" && i+1 < argc) portArg    = argv[++i];
        else if (a == "-b" && i+1 < argc) baudArg    = std::stoi(argv[++i]);
        else if (a == "-i" && i+1 < argc) slaveArg   = std::stoi(argv[++i]);
        else if (a == "-t" && i+1 < argc) timeoutArg = std::stoi(argv[++i]);
        else if (a == "-s" && i+1 < argc) g_pollInterval = std::stoi(argv[++i]);
    }

    std::string modalPort    = portArg.empty() ? (g_cfg.port.empty() ? "/dev/ttyUSB0" : g_cfg.port) : portArg;
    std::string modalBaud    = std::to_string(baudArg != 115200 ? baudArg : g_cfg.baudRate);
    std::string modalSlaveId = std::to_string(slaveArg != 1     ? slaveArg : g_cfg.slaveId);

    auto screen = ScreenInteractive::Fullscreen();
    int  activeTab   = 0;
    bool showModal   = false;
    bool connecting  = false;
    std::string statusMsg;
    hvb::tui::ScannedData data;
    std::atomic<bool> running{true};

    // AppState bundles everything the tab factories need
    hvb::tui::AppState appState{g_client, g_connected, data, statusMsg, screen};

    // ---- Poll thread ----
    std::thread pollThread([&] {
        while (running) {
            if (g_connected) {
                data.sysInfo = g_client.readSystemInfo();
                for (int ch = 0; ch < 2; ++ch) data.chInfo[ch] = g_client.readChannelInfo(ch);
                data.sysCfg  = g_client.readSystemConfig();
                for (int ch = 0; ch < 2; ++ch) data.chCfg[ch]  = g_client.readChannelConfig(ch);
                data.valid = g_client.isConnected();
                if (running) screen.PostEvent(Event::Custom);
            }
            for (int i = 0; i < g_pollInterval * 10 && running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // ---- Connection modal ----
    auto portInput  = Input(&modalPort,    "e.g. /dev/ttyUSB0");
    auto baudInput  = Input(&modalBaud,    "115200");
    auto slaveInput = Input(&modalSlaveId, "1");

    auto doConnect = [&] {
        if (modalPort.empty() || connecting) return;
        connecting = true;
        statusMsg  = "Connecting to " + modalPort + "...";
        screen.PostEvent(Event::Custom);
        std::thread([&] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(modalBaud);    } catch (...) {}
            try { slave = std::stoi(modalSlaveId); } catch (...) {}
            bool ok = g_client.connect(modalPort, baud, slave, timeoutArg);
            g_connected = ok;
            if (ok) {
                data.sysInfo = g_client.readSystemInfo();
                for (int ch = 0; ch < 2; ++ch) data.chInfo[ch] = g_client.readChannelInfo(ch);
                data.sysCfg  = g_client.readSystemConfig();
                for (int ch = 0; ch < 2; ++ch) data.chCfg[ch]  = g_client.readChannelConfig(ch);
                data.valid = true;
            }
            statusMsg  = ok ? "Connected " + modalPort : "Connect failed: " + g_client.lastError();
            connecting = false;
            showModal  = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };

    auto modalButtons = Container::Horizontal({
        Button("Connect", doConnect),
        Button("Cancel",  [&] { showModal = false; }),
    });
    auto modalForm = Container::Vertical({portInput, baudInput, slaveInput, modalButtons});
    auto modalRenderer = Renderer(modalForm, [&] {
        return vbox({
            text(" Connect to HVB ") | bold | center, separator(),
            hbox({ text("Port    : "), portInput->Render()  }),
            hbox({ text("Baud    : "), baudInput->Render()  }),
            hbox({ text("Slave ID: "), slaveInput->Render() }),
            separator(),
            modalButtons->Render() | center,
            text(connecting ? "Connecting..." : "") | dim | center,
        }) | border | size(WIDTH, EQUAL, 50);
    });

    // ---- Tab content (tab factories) ----
    std::vector<std::string> tabTitles = {"Mon", "Sys", "CH0", "CH1"};
    auto tabSelector = Toggle(&tabTitles, &activeTab);

    auto tabContent = Container::Tab({
        hvb::tui::makeMonitorTab(appState),
        hvb::tui::makeSystemTab(appState),
        hvb::tui::makeChannelTab(appState, 0),
        hvb::tui::makeChannelTab(appState, 1),
    }, &activeTab);

    // topBar: visual only — tabSelector rendered here but NOT in focus chain
    auto topBar = Renderer([&] {
        std::string connStr = g_connected
            ? ("\xe2\x97\x8f Connected " + modalPort)   // ● Connected
            : (statusMsg.empty() ? "\xe2\x97\x8b Disconnected" : statusMsg);
        return hbox({
            text(" HVB TUI ") | bold,
            separator(),
            text(" " + connStr + " ") | (g_connected ? color(Color::Green) : color(Color::Red)),
            separator(),
            tabSelector->Render(),
            filler(),
            text(" q:quit  r:poll  c:connect  d:disconnect  1-4:tabs ") | dim,
        });
    });

    // mainContainer: topBar (visual) + tabContent (interactive only)
    // tabSelector is NOT added here so ↑/↓ always reach the active tab
    auto mainContainer = Container::Vertical({topBar, tabContent});

    auto root = mainContainer
        | Modal(modalRenderer, &showModal)
        | CatchEvent([&](Event e) {
            if (showModal) {
                if (e == Event::Escape) { showModal = false; return true; }
                return false;
            }
            if (e == Event::Character('q')) { running = false; screen.ExitLoopClosure()(); return true; }
            if (e == Event::Character('r')) {
                if (g_connected) {
                    data.sysInfo = g_client.readSystemInfo();
                    for (int i = 0; i < 2; ++i) data.chInfo[i] = g_client.readChannelInfo(i);
                    data.sysCfg  = g_client.readSystemConfig();
                    for (int i = 0; i < 2; ++i) data.chCfg[i]  = g_client.readChannelConfig(i);
                    data.valid = true;
                }
                return true;
            }
            if (e == Event::Character('c')) { if (!g_connected && !connecting) showModal = true; return true; }
            if (e == Event::Character('d')) {
                g_client.disconnect(); g_connected = false; data.valid = false;
                statusMsg = "Disconnected"; return true;
            }
            if (e == Event::Character('1')) { activeTab = 0; return true; }
            if (e == Event::Character('2')) { activeTab = 1; return true; }
            if (e == Event::Character('3')) { activeTab = 2; return true; }
            if (e == Event::Character('4')) { activeTab = 3; return true; }
            return false;
        });

    // Auto-connect if port given on command line
    if (!portArg.empty()) {
        std::thread([&] {
            bool ok = g_client.connect(portArg, baudArg, slaveArg, timeoutArg);
            g_connected = ok;
            if (ok) {
                data.sysInfo = g_client.readSystemInfo();
                for (int i = 0; i < 2; ++i) data.chInfo[i] = g_client.readChannelInfo(i);
                data.sysCfg  = g_client.readSystemConfig();
                for (int i = 0; i < 2; ++i) data.chCfg[i]  = g_client.readChannelConfig(i);
                data.valid = true;
            }
            statusMsg = ok ? "" : "Connect failed: " + g_client.lastError();
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    screen.Loop(root);
    running = false;
    if (pollThread.joinable()) pollThread.join();
    g_client.disconnect();
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tui/main.cpp
git commit -m "tui: refactor main.cpp — use AppState and new tab factories, number keys for tab switch"
```

---

## Task 7: Build and verify

- [ ] **Step 1: Run unit tests**

```bash
cd tools/modbus_debug_tool
cmake --preset linux-debug
cmake --build build/linux-debug --target hvb_tests
./build/linux-debug/tests/hvb_tests
# Expected: all tests PASS including the 5 new tui_format tests
```

- [ ] **Step 2: Build release binaries**

```bash
cmake --preset linux-release
cmake --build build/linux-release --target hvb_tui hvbctrl
# Expected: build succeeds with no errors
ls -lh bin/hvb_tui bin/hvbctrl
```

- [ ] **Step 3: Smoke test — verify TUI starts**

```bash
./bin/hvb_tui --help 2>&1 || true
./bin/hvb_tui &
# Expected: fullscreen TUI appears showing "Disconnected" status bar
# Press 1/2/3/4: tab titles highlight in the top bar
# Press 'c': connection modal opens, Port field accepts typing including 'd'
# Press Escape: modal closes
# Press 'q': exits cleanly
kill %1 2>/dev/null || true
```

- [ ] **Step 4: Smoke test with hardware (if PORT available)**

```bash
PORT=/dev/ttyUSB0
./bin/hvb_tui -p $PORT
# Expected:
#   - Connected status turns green after connection
#   - Monitor tab shows system bar + 2-row channel table with real values
#   - ↑/↓ moves ▶ between rows
#   - Tab enters action panel (border highlights)
#   - Typing in Target V field and pressing Enter issues write
#   - Status bar shows "OK: Target V" or error message
#   - Press 2: System tab shows left info + right config
#   - Press 3/4: Channel tabs show live bar + all panels
```

- [ ] **Step 5: Commit test results note and final polish**

```bash
git add -A
git commit -m "tui: full redesign — interactive monitor/system/channel tabs with writes"
```

---

## Self-Review Checklist

**Spec coverage:**
- ✅ Monitor tab: system summary bar, channel table (all 9 columns), action panel
- ✅ Action panel: Target V, Ramp Up/Down (step+interval), V Limit (mode/action/threshold), I Limit, buttons
- ✅ System tab: left info (all fields), right config (all fields), 4 action buttons
- ✅ Channel tab: live bar (all fields), Output, Ramping (+ derate), VProt, IProt, Calibration, Persistence
- ✅ Keyboard nav: ↑/↓ for rows, Tab enters panel, Escape returns, Enter commits, 1-4 switch tabs
- ✅ Write-on-thread pattern via `writeAsync`
- ✅ Pure format helpers unit-tested

**Architecture notes:**
- `tabSelector` Toggle is rendered visually in `topBar` but NOT added to `mainContainer` as a focusable child. This ensures ↑/↓ always reach the active tab's CatchEvent (the Toggle would otherwise intercept ←/→ before the panel's InlineCycler could see them). Tab switching uses 1-4 number keys.
- `InlineCycler` uses ←/→ for cycling. This works correctly because the outer CatchEvent only handles 1-4/q/r/c/d — it does not intercept ←/→.
- `writeAsync` captures `AppState& s` by reference. The AppState outlives all background threads (it lives in `main()` and threads complete before `screen.Loop` returns).
- I-threshold unit: user types µA; multiply by 1000 to get nA/LSB (1 LSB = 1 nA). Max representable: 32.767 µA (int16_t max = 32767 LSB = 32767 nA).

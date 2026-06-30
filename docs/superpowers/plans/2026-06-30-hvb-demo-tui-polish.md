# HVB Demo TUI Polish — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Polish hvb_demo_tui: embed interactive editing into Monitor table rows, remove async writes, fix config-value display and cross-tab sync bugs.

**Architecture:** Single shared `ConfigInputs` struct replaces per-tab `St` copies. Blocking `writeSync()` replaces detached-thread `writeAsync()`. Monitor tab replaces Menu + editing panel with a `Container::Vertical` of per-row `Container::Horizontal` components containing real FTXUI Buttons and Inputs.

**Tech Stack:** C++17, FTXUI (screen/dom/component), Modbus RTU via hvb_modbus_core

---

## File Structure

| File | Role |
|------|------|
| `tools/hvb_demo_app/tui/widgets.h` | `ConfigInputs` struct, `writeSync()`, `syncDataToInputs()` |
| `tools/hvb_demo_app/tui/tab_system.h` | System tab — remove `St`, use `ConfigInputs` |
| `tools/hvb_demo_app/tui/tab_channel.h` | Channel tab — remove `St`, use `ConfigInputs` |
| `tools/hvb_demo_app/tui/tab_monitor.h` | Monitor tab – interactive table (complete rewrite) |
| `tools/hvb_demo_app/tui/main.cpp` | Wire `ConfigInputs`, dynamic lifecycle, blocking writes |

No new files. All 5 files are modified.

---

### Task 1: widgets.h — ConfigInputs, writeSync, syncDataToInputs

**Files:**
- Modify: `tools/hvb_demo_app/tui/widgets.h`

This task adds the shared state struct, the blocking write helper, and the data-to-input sync function. The old `writeAsync()` is removed at the end.

- [ ] **Step 1: Define ConfigInputs struct**

In `tools/hvb_demo_app/tui/widgets.h`, add after the `AppState` struct (after line 28) and before `writeAsync`:

```cpp
// Shared config-input state — single source of truth for all tabs.
// Populated from ScannedData on connect/refresh/write-success.
// Each tab's Input/InlineCycler widgets bind to fields in this struct.
struct ConfigInputs {
    // Monitor table editable columns
    std::string targetV [MAX_CHANNELS];  // Vset  — configured target voltage in V
    std::string ruStep  [MAX_CHANNELS];  // Ramp↑ — ramp-up step raw LSB
    std::string rdStep  [MAX_CHANNELS];  // Ramp↓ — ramp-down step raw LSB
    std::string iThr    [MAX_CHANNELS];  // I-limit — current threshold in nA

    // System tab
    std::string slaveAddr;
    int opModeIdx       = 0;
    int baudIdx         = 0;
    int startupIdx      = 0;

    // Channel tab extra fields
    std::string ruInt      [MAX_CHANNELS];
    std::string rdInt      [MAX_CHANNELS];
    std::string derateStep [MAX_CHANNELS];
    int iModeIdx           [MAX_CHANNELS]{};
    int iActIdx            [MAX_CHANNELS]{};
    int recovIdx           [MAX_CHANNELS]{};
    std::string retryDelay  [MAX_CHANNELS];
    std::string retryMax    [MAX_CHANNELS];
    std::string retryWindow [MAX_CHANNELS];
    std::string iBand       [MAX_CHANNELS];
};
```

- [ ] **Step 2: Implement syncDataToInputs()**

Add after `ConfigInputs`:

```cpp
inline void syncDataToInputs(const ScannedData& data, ConfigInputs& cfg) {
    if (!data.valid) return;

    // System fields
    const auto& sc = data.sysCfg;
    cfg.slaveAddr  = std::to_string(sc.slaveAddr);
    cfg.opModeIdx  = static_cast<int>(sc.operatingMode);
    cfg.baudIdx    = static_cast<int>(sc.baudRateCode);
    cfg.startupIdx = static_cast<int>(sc.startupChannelPolicy);

    // Per-channel fields
    int n = data.numChannels();
    for (int ch = 0; ch < n; ++ch) {
        const auto& cc = data.chCfg[ch];

        // Monitor editable
        {
            double v = reg::voltageToV(cc.configuredTargetVRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%+.1f", v);
            cfg.targetV[ch] = buf;
        }
        cfg.ruStep[ch] = std::to_string(cc.rampUpStepRaw);
        cfg.rdStep[ch] = std::to_string(cc.rampDownStepRaw);
        {
            double a = reg::currentToA(cc.iLimitThresholdRaw);
            char buf[16];
            snprintf(buf, sizeof(buf), "%.3f", a * 1e9);
            cfg.iThr[ch] = buf;
        }

        // Channel tab extras
        cfg.ruInt[ch]      = std::to_string(cc.rampUpInterval);
        cfg.rdInt[ch]      = std::to_string(cc.rampDownInterval);
        cfg.derateStep[ch] = std::to_string(cc.derateStepRaw);
        cfg.iModeIdx[ch]   = static_cast<int>(cc.iProtMode);
        cfg.iActIdx[ch]    = static_cast<int>(cc.iProtOutputAction);
        cfg.recovIdx[ch]   = static_cast<int>(cc.recoveryPolicyMode);
        cfg.retryDelay[ch]  = std::to_string(cc.autoRetryDelay);
        cfg.retryMax[ch]    = std::to_string(cc.autoRetryMaxCount);
        cfg.retryWindow[ch] = std::to_string(cc.autoRetryWindow);
        cfg.iBand[ch]       = std::to_string(cc.currentSafeBandPct);
    }
}
```

- [ ] **Step 3: Implement writeSync()**

Add after `syncDataToInputs`:

```cpp
// Blocking write — acquires scanMutex, runs writeFn, reads back config on success.
inline void writeSync(AppState& s, ConfigInputs& inputs,
                      const std::string& label,
                      std::function<bool()> writeFn,
                      std::function<void()> refreshFn) {
    { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Writing " + label + "..."; }
    s.screen.PostEvent(Event::Custom);

    bool ok;
    { std::lock_guard<std::mutex> lk(s.scanMutex); ok = writeFn(); }

    if (ok) {
        refreshFn();
        syncDataToInputs(s.data, inputs);
        { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "OK: " + label; }
    } else {
        { std::lock_guard<std::mutex> lk(s.statusMutex);
          s.statusMsg = "Error: " + s.client.lastError(); }
    }
    s.screen.PostEvent(Event::Custom);
}
```

- [ ] **Step 4: Remove writeAsync()**

Delete the entire `writeAsync` function (lines 32–42 of the current `widgets.h`).

- [ ] **Step 5: Build check**

```bash
cd tools/hvb_demo_app && mkdir -p build && cd build && cmake .. && make hvb_demo_tui 2>&1
```

Expected: Compiles without errors (link errors are OK at this stage since callers still reference `writeAsync` and don't pass `ConfigInputs`).

- [ ] **Step 6: Commit**

```bash
git add tools/hvb_demo_app/tui/widgets.h
git commit -m "feat(tui): add ConfigInputs, writeSync, syncDataToInputs"
```

---

### Task 2: tab_system.h — wire to ConfigInputs

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_system.h`

Remove local `St` struct, accept `ConfigInputs&` as parameter, use its fields directly for widget state.

- [ ] **Step 1: Rewrite makeSystemTab signature and replace St with ConfigInputs**

Replace the entire content of `tools/hvb_demo_app/tui/tab_system.h`:

```cpp
#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeSystemTab(AppState& s, ConfigInputs& inputs) {
    static const std::vector<std::string> kOpModes       = {"Normal", "Automatic"};
    static const std::vector<std::string> kBaudNames     = {"115200", "9600"};
    static const std::vector<std::string> kStartupPolicy = {"Load NVS Config", "Factory Default"};

    auto onOpMode  = [&s, &inputs] {
        writeSync(s, inputs, "OpMode",
            [&s, &inputs] { return s.client.writeOperatingMode(static_cast<OpMode>(inputs.opModeIdx)); },
            [&s, &inputs] {
                s.data.sysCfg = s.client.readSystemConfig();
                s.data.sysCfg.operatingMode = static_cast<OpMode>(inputs.opModeIdx);
            });
    };
    auto onBaud    = [&s, &inputs] {
        writeSync(s, inputs, "BaudRate",
            [&s, &inputs] { return s.client.writeBaudRateCode((uint16_t)inputs.baudIdx); },
            [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
    };
    auto onSlave   = [&s, &inputs] {
        try { uint16_t a = (uint16_t)std::stoul(inputs.slaveAddr);
              writeSync(s, inputs, "SlaveAddr",
                  [&s, a] { return s.client.writeSlaveAddress(a); },
                  [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid slave address"; }
    };
    auto onStartup = [&s, &inputs] {
        writeSync(s, inputs, "StartupPolicy",
            [&s, &inputs] { return s.client.writeStartupChannelPolicy((uint16_t)inputs.startupIdx); },
            [&s, &inputs] { s.data.sysCfg = s.client.readSystemConfig(); });
    };

    auto opModeC      = InlineCycler(kOpModes,       &inputs.opModeIdx,  onOpMode);
    auto baudC        = InlineCycler(kBaudNames,     &inputs.baudIdx,    onBaud);
    auto startupC     = InlineCycler(kStartupPolicy, &inputs.startupIdx, onStartup);
    auto slaveInp     = CommitInput(&inputs.slaveAddr, "1", onSlave);

    auto bSave    = ActionButton("Save",    [&s, &inputs]{
        writeSync(s, inputs, "Save", [&s]{ return s.client.sendParamAction(-1, ParamAction::Save); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bLoad    = ActionButton("Load",    [&s, &inputs]{
        writeSync(s, inputs, "Load", [&s]{ return s.client.sendParamAction(-1, ParamAction::Load); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bFactory = ActionButton("Factory", [&s, &inputs]{
        writeSync(s, inputs, "Factory", [&s]{ return s.client.sendParamAction(-1, ParamAction::FactoryReset); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bCalExit = ActionButton("Exit Cal", [&s, &inputs]{
        writeSync(s, inputs, "Exit Cal", [&s]{ return s.client.exitCalibrationMode(); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });
    auto bReset   = ActionButton("Reset",   [&s, &inputs]{
        writeSync(s, inputs, "Reset", [&s]{ return s.client.sendParamAction(-1, ParamAction::SoftwareReset); },
                  [&s, &inputs]{ s.data.sysCfg = s.client.readSystemConfig(); syncDataToInputs(s.data, inputs); });
    });

    auto cfgContainer = Container::Vertical({
        opModeC, baudC, slaveInp, startupC,
        bSave, bLoad, bFactory, bCalExit, bReset,
    });

    return Renderer(cfgContainer, [=, &s]() {
        if (!s.data.valid)
            return text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | border;

        const auto& si = s.data.sysInfo;
        const auto& sc = s.data.sysCfg;

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

        auto rightPanel = vbox({
            hbox({ text("Op Mode      : "), opModeC->Render() }),
            hbox({ text("Slave Addr   : "), slaveInp->Render(), text("  (0-247)") }),
            hbox({ text("Baud Rate    : "), baudC->Render() }),
            hbox({ text("Startup Pol  : "), startupC->Render() }),
            separator(),
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "),
                   bFactory->Render(), text("  "), bCalExit->Render(), text("  "),
                   bReset->Render() }),
        }) | border;

        (void)sc;
        return hbox({ leftPanel, rightPanel });
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Build check**

```bash
cd tools/hvb_demo_app/build && make hvb_demo_tui 2>&1
```

Expected: Compiles. May have link errors if main.cpp hasn't been updated yet (will be fixed in Task 5).

- [ ] **Step 3: Commit**

```bash
git add tools/hvb_demo_app/tui/tab_system.h
git commit -m "feat(tui): wire tab_system to ConfigInputs, blocking writes"
```

---

### Task 3: tab_channel.h — wire to ConfigInputs

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_channel.h`

Remove local `St` struct, accept `ConfigInputs&` and `ch` index. All input widgets bind to `inputs.xxx[ch]`.

- [ ] **Step 1: Rewrite makeChannelTab signature and replace St**

Replace the entire content of `tools/hvb_demo_app/tui/tab_channel.h`:

```cpp
#pragma once
#include "widgets.h"
#include <memory>
#include <string>

namespace hvb::tui {

inline Component makeChannelTab(AppState& s, ConfigInputs& inputs, int ch) {
    static const std::vector<std::string> kProtModes  = {"Disabled","FlagOnly","Apply-Action"};
    static const std::vector<std::string> kIActNames  = {"None","Dis-Graceful","Dis-Immed","ForceZero"};
    static const std::vector<OutputAction> kIActVals  = {
        OutputAction::None, OutputAction::DisableGraceful,
        OutputAction::DisableImmediate, OutputAction::ForceOutputZero
    };
    static const std::vector<std::string> kRecovNames = {"ManualLatch","AutoRetry","AutoDerate","NeverRetry"};

    auto refreshCh = [&s, &inputs]() {
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, s.data.chInfo[ch].chCapFlags);
        syncDataToInputs(s.data, inputs);
    };

    auto onTarget = [&s, &inputs, refreshCh] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            writeSync(s, inputs, "Target V",
                [&s, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto onRampUp = [&s, &inputs, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.ruStep[ch]);
            auto iv   = (uint16_t)std::stoul(inputs.ruInt[ch]);
            writeSync(s, inputs, "Ramp Up",
                [&s, step, iv] { return s.client.writeRampUp(ch, step, iv); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onRampDown = [&s, &inputs, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.rdStep[ch]);
            auto iv   = (uint16_t)std::stoul(inputs.rdInt[ch]);
            writeSync(s, inputs, "Ramp Down",
                [&s, step, iv] { return s.client.writeRampDown(ch, step, iv); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp value"; }
    };
    auto onDerate = [&s, &inputs, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.derateStep[ch]);
            writeSync(s, inputs, "Derate",
                [&s, step] { return s.client.writeDerateStep(ch, step); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid derate step"; }
    };
    auto onIProt = [&s, &inputs, &kIActVals, refreshCh] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = kIActVals.at(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) * 1000.0 + 0.5);
            writeSync(s, inputs, "I Limit",
                [&s, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto onRecov = [&s, &inputs, refreshCh] {
        try {
            auto pol = static_cast<RecoveryPolicy>(inputs.recovIdx[ch]);
            int d = std::stoi(inputs.retryDelay[ch]), m = std::stoi(inputs.retryMax[ch]),
                w = std::stoi(inputs.retryWindow[ch]);
            writeSync(s, inputs, "Recovery",
                [&s, pol, d, m, w] { return s.client.writeChannelRecovery(ch, pol, d, m, w); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid recovery value"; }
    };
    auto onBand = [&s, &inputs, refreshCh] {
        try {
            uint16_t pct = (uint16_t)std::stoul(inputs.iBand[ch]);
            writeSync(s, inputs, "SafeBand",
                [&s, pct] { return s.client.writeChannelSafeBand(ch, pct); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid safe-band value"; }
    };

    auto tgtInp    = CommitInput(&inputs.targetV[ch],   "+0.0",  onTarget);
    auto ruStepInp = CommitInput(&inputs.ruStep[ch],    "0",     onRampUp);
    auto ruIntInp  = CommitInput(&inputs.ruInt[ch],     "0",     onRampUp);
    auto rdStepInp = CommitInput(&inputs.rdStep[ch],    "0",     onRampDown);
    auto rdIntInp  = CommitInput(&inputs.rdInt[ch],     "0",     onRampDown);
    auto derInp    = CommitInput(&inputs.derateStep[ch],"0",     onDerate);
    auto iModeC    = InlineCycler(kProtModes,  &inputs.iModeIdx[ch], onIProt);
    auto iActC     = InlineCycler(kIActNames,  &inputs.iActIdx[ch],  onIProt);
    auto iThrInp   = CommitInput(&inputs.iThr[ch],    "0.000", onIProt);
    auto recovC    = InlineCycler(kRecovNames, &inputs.recovIdx[ch], onRecov);
    auto delayInp  = CommitInput(&inputs.retryDelay[ch],  "0",  onRecov);
    auto maxInp    = CommitInput(&inputs.retryMax[ch],    "3",  onRecov);
    auto winInp    = CommitInput(&inputs.retryWindow[ch], "60", onRecov);
    auto iBandInp  = CommitInput(&inputs.iBand[ch],       "10", onBand);

    auto bEnable  = ActionButton("Enable",    [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Enable", [&s]{ return s.client.sendOutputAction(ch, OutputAction::Enable); }, refreshCh); });
    auto bDisImm  = ActionButton("Dis-Immed", [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Dis-Immed", [&s]{ return s.client.sendOutputAction(ch, OutputAction::DisableImmediate); }, refreshCh); });
    auto bDisGra  = ActionButton("Dis-Grace", [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Dis-Grace", [&s]{ return s.client.sendOutputAction(ch, OutputAction::DisableGraceful); }, refreshCh); });
    auto bSave    = ActionButton("Save",      [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Save", [&s]{ return s.client.sendParamAction(ch, ParamAction::Save); }, refreshCh); });
    auto bLoad    = ActionButton("Load",      [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Load", [&s]{ return s.client.sendParamAction(ch, ParamAction::Load); }, refreshCh); });
    auto bFactory = ActionButton("Factory",   [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "Factory", [&s]{ return s.client.sendParamAction(ch, ParamAction::FactoryReset); }, refreshCh); });
    auto bClrAct  = ActionButton("ClrActive", [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "ClrActive", [&s]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearActiveFaultBlock); }, refreshCh); });
    auto bClrHist = ActionButton("ClrHist",   [&s, &inputs, refreshCh]{
        writeSync(s, inputs, "ClrHist", [&s]{ return s.client.sendChannelFaultCommand(ch, ChannelFaultCommand::ClearFaultHistory); }, refreshCh); });

    auto container = Container::Vertical({
        tgtInp, bEnable, bDisImm, bDisGra,
        ruStepInp, ruIntInp, rdStepInp, rdIntInp, derInp,
        iModeC, iActC, iThrInp,
        recovC, delayInp, maxInp, winInp, iBandInp,
        bSave, bLoad, bFactory, bClrAct, bClrHist,
    });

    return Renderer(container, [=, &s]() {
        if (s.data.valid && ch >= s.data.numChannels())
            return text(" CH" + std::to_string(ch) + " not present on this device ") | dim | center;

        const uint16_t caps = s.data.valid ? s.data.chInfo[ch].chCapFlags : 0xFFFFu;
        const bool hasOutEn = (caps & CH_CAP_OUTPUT_ENABLE) != 0;
        const bool hasVolts = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        const bool hasCurr  = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;

        Element liveBar = text(" Not connected ") | dim;
        if (s.data.valid) {
            const auto& ci = s.data.chInfo[ch];
            char lastFault[24] = "--";
            if (ci.lastFaultTimestamp > 0)
                snprintf(lastFault, sizeof(lastFault), "%u s ago", (unsigned)ci.lastFaultTimestamp);
            Elements liveParts;
            if (hasVolts) { liveParts.push_back(text("  Vmeas: ")); liveParts.push_back(text(fmtVoltage(ci.voltageRaw)) | bold); }
            if (hasCurr)  { liveParts.push_back(text("   Imeas: ")); liveParts.push_back(text(fmtCurrentUA(ci.currentRaw)) | bold); }
            liveParts.push_back(text("   Op Target: "));
            liveParts.push_back(text(fmtVoltage(ci.operationalTargetVoltageRaw)));
            liveParts.push_back(text("   Status: "));
            liveParts.push_back(text(statusBadge(ci.status)) | bold);
            liveParts.push_back(text("   Retries: "));
            liveParts.push_back(text(std::to_string(ci.retryCount)));
            liveBar = hbox(std::move(liveParts));
        }

        auto outputPanel = window(text(" Output "), vbox({
            hbox({ text("Target V : "), tgtInp->Render(), text(" V") }),
            hasOutEn
                ? hbox({ bEnable->Render(), text("  "), bDisImm->Render(), text("  "), bDisGra->Render() })
                : hbox({ text("  (output control not supported) ") | dim }),
        }));

        auto rampPanel = window(text(" Ramping "), vbox({
            hbox({ text("Ramp Up   : step "), ruStepInp->Render(), text(" LSB  int "), ruIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Ramp Down : step "), rdStepInp->Render(), text(" LSB  int "), rdIntInp->Render(), text(" \xc3\x970.1s") }),
            hbox({ text("Derate Step:      "), derInp->Render(),   text(" LSB") }),
        }));

        auto recovPanel = window(text(" Recovery "), vbox({
            hbox({ text("Policy    : "), recovC->Render() }),
            hbox({ text("Delay     : "), delayInp->Render(), text(" s"),
                   text("  Max: "), maxInp->Render(),
                   text("  Window: "), winInp->Render(), text(" s") }),
            hbox({ text("I Safe Band: "), iBandInp->Render(), text(" % (0-50)") }),
        }));

        auto persistPanel = window(text(" Persistence "), vbox({
            hbox({ bSave->Render(), text("  "), bLoad->Render(), text("  "), bFactory->Render() }),
            hbox({ bClrAct->Render(), text("  "), bClrHist->Render() }),
        }));

        Element calInfo = text(" No data ") | dim;
        if (s.data.valid) {
            const auto& cc = s.data.chCalCfg[ch];
            Elements calRows;
            calRows.push_back(hbox({ text("Output  K: "), text(std::to_string(cc.outCalK))  | bold, text("  B: "), text(std::to_string(cc.outCalB))  | bold }));
            if (hasVolts) calRows.push_back(hbox({ text("Meas V  K: "), text(std::to_string(cc.measVCalK)) | bold, text("  B: "), text(std::to_string(cc.measVCalB)) | bold }));
            if (hasCurr)  calRows.push_back(hbox({ text("Meas I  K: "), text(std::to_string(cc.measICalK)) | bold, text("  B: "), text(std::to_string(cc.measICalB)) | bold }));
            calInfo = vbox(std::move(calRows));
        }
        auto calPanel = window(text(" Calibration (read-only) "), calInfo);

        Elements rows;
        rows.push_back(window(text(" CH" + std::to_string(ch) + " Live "), liveBar));
        rows.push_back(hbox({ outputPanel, rampPanel }));
        rows.push_back(recovPanel);
        if (hasCurr) {
            rows.push_back(window(text(" Current Protection "), hbox({
                text("Mode : "), iModeC->Render(),
                text("   Action : "), iActC->Render(),
                text("   Threshold: "), iThrInp->Render(), text(" \xc2\xb5\x41"),
            })));
        }
        rows.push_back(hbox({ calPanel, persistPanel }));
        return vbox(std::move(rows));
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Build check**

```bash
cd tools/hvb_demo_app/build && make hvb_demo_tui 2>&1
```

Expected: Compiles. Link errors from main.cpp expected (will be fixed in Task 5).

- [ ] **Step 3: Commit**

```bash
git add tools/hvb_demo_app/tui/tab_channel.h
git commit -m "feat(tui): wire tab_channel to ConfigInputs, blocking writes"
```

---

### Task 4: tab_monitor.h — interactive table

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_monitor.h`

Complete rewrite. Replace Menu + per-channel editing panel with a Container::Vertical of per-channel Container::Horizontal rows, each containing real widgets.

- [ ] **Step 1: Write new tab_monitor.h**

Replace the entire content of `tools/hvb_demo_app/tui/tab_monitor.h`:

```cpp
#pragma once
#include "widgets.h"
#include <algorithm>
#include <memory>
#include <string>

namespace hvb::tui {

// Build one row of the Monitor table for channel `ch`.
// Returns a Renderer wrapping a Container::Horizontal of interactive widgets.
// Invisible rows (ch >= numChannels) catch all events and render as empty.
inline Component makeMonitorRow(AppState& s, ConfigInputs& inputs, int ch) {
    auto refreshCh = [&s, &inputs]() {
        uint16_t caps = s.data.chInfo[ch].chCapFlags;
        s.data.chCfg[ch] = s.client.readChannelConfig(ch, caps);
        s.data.chCalCfg[ch] = s.client.readChannelCalConfig(ch, caps);
        syncDataToInputs(s.data, inputs);
    };

    // ---- Status toggle button ----
    auto bopt = ButtonOption{};
    bopt.transform = [&s, ch](const EntryState& es) -> Element {
        uint16_t st = s.data.valid ? s.data.chInfo[ch].status : 0;
        bool on = (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0;
        std::string lbl = on ? "[ ON ]" : "[ OFF ]";
        auto e = text(lbl);
        if (on)      e = e | color(Color::Green) | bold;
        else         e = e | dim;
        if (es.focused) e = e | inverted;
        return e;
    };
    auto statusBtn = Button("", [&s, &inputs, ch, refreshCh] {
        if (!s.data.valid) return;
        uint16_t st = s.data.chInfo[ch].status;
        bool on = (st & ChStatus::OUTPUT_DRIVE_NONZERO) != 0;
        OutputAction act = on ? OutputAction::DisableImmediate : OutputAction::Enable;
        std::string lbl = on ? "Disable" : "Enable";
        writeSync(s, inputs, lbl,
            [&s, ch, act] { return s.client.sendOutputAction(ch, act); },
            refreshCh);
    }, bopt);

    // ---- Vset Input ----
    auto onVset = [&s, &inputs, ch, refreshCh] {
        try {
            auto raw = reg::voltageFromV(std::stod(inputs.targetV[ch]));
            writeSync(s, inputs, "Target V",
                [&s, ch, raw] { return s.client.writeConfiguredTargetVoltage(ch, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid voltage"; }
    };
    auto vsetInp = CommitInput(&inputs.targetV[ch], "+0.0", onVset);

    // ---- Ramp Up Input ----
    auto onRampUp = [&s, &inputs, ch, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.ruStep[ch]);
            writeSync(s, inputs, "Ramp Up",
                [&s, ch, step] { return s.client.writeRampUp(ch, step, s.data.chCfg[ch].rampUpInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-up value"; }
    };
    auto rampUpInp = CommitInput(&inputs.ruStep[ch], "0", onRampUp);

    // ---- Ramp Down Input ----
    auto onRampDown = [&s, &inputs, ch, refreshCh] {
        try {
            auto step = (uint16_t)std::stoul(inputs.rdStep[ch]);
            writeSync(s, inputs, "Ramp Down",
                [&s, ch, step] { return s.client.writeRampDown(ch, step, s.data.chCfg[ch].rampDownInterval); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid ramp-down value"; }
    };
    auto rampDownInp = CommitInput(&inputs.rdStep[ch], "0", onRampDown);

    // ---- I-limit Input ----
    auto onILimit = [&s, &inputs, ch, refreshCh] {
        try {
            auto mode   = static_cast<ProtectionMode>(inputs.iModeIdx[ch]);
            auto action = static_cast<OutputAction>(inputs.iActIdx[ch]);
            auto raw    = static_cast<int16_t>(std::stod(inputs.iThr[ch]) * 1000.0 + 0.5);
            writeSync(s, inputs, "I Limit",
                [&s, ch, mode, action, raw] { return s.client.writeCurrentProtection(ch, mode, action, raw); },
                refreshCh);
        } catch (...) { std::lock_guard<std::mutex> lk(s.statusMutex); s.statusMsg = "Error: invalid I-limit value"; }
    };
    auto iLimitInp = CommitInput(&inputs.iThr[ch], "0.000", onILimit);

    // ---- Horizontal container of all interactive widgets ----
    auto rowWidgets = Container::Horizontal({
        statusBtn, vsetInp, rampUpInp, rampDownInp, iLimitInp,
    });

    return Renderer(rowWidgets, [=, &s, ch]() mutable {
        // Return empty element for invisible rows (ch >= numChannels or not connected)
        bool show = s.data.valid && ch < s.data.numChannels();
        if (!show) return emptyElement();

        const auto& ci = s.data.chInfo[ch];
        const uint16_t caps = ci.chCapFlags;
        bool hasV = (caps & CH_CAP_VOLTAGE_MEASUREMENT) != 0;
        bool hasI = (caps & CH_CAP_CURRENT_MEASUREMENT) != 0;
        bool hasOut = (caps & CH_CAP_OUTPUT_ENABLE) != 0;

        char chLabel[8];
        snprintf(chLabel, sizeof(chLabel), "CH%-2d", ch);

        auto chT  = text(chLabel)                               | size(WIDTH, EQUAL, 4);
        auto vmT  = hasV ? text(fmtVoltage(ci.voltageRaw))      | size(WIDTH, EQUAL, 13) : text("--") | size(WIDTH, EQUAL, 13) | dim;
        auto imT  = hasI ? text(fmtCurrentUA(ci.currentRaw))    | size(WIDTH, EQUAL, 16) : text("--") | size(WIDTH, EQUAL, 16) | dim;
        auto vtT  = hasV ? text(fmtVoltage(ci.operationalTargetVoltageRaw)) | size(WIDTH, EQUAL, 13) : text("--") | size(WIDTH, EQUAL, 13) | dim;
        auto fltT = text(faultStr(ci.activeFault));

        Elements parts;
        parts.push_back(chT);
        parts.push_back(vmT);
        parts.push_back(imT);
        if (hasOut) parts.push_back(statusBtn->Render());
        else        parts.push_back(text(" -- ") | size(WIDTH, EQUAL, 8) | dim);
        parts.push_back(vsetInp->Render()     | size(WIDTH, EQUAL, 13));
        parts.push_back(vtT);
        parts.push_back(rampUpInp->Render()   | size(WIDTH, EQUAL, 8));
        parts.push_back(rampDownInp->Render() | size(WIDTH, EQUAL, 8));
        if (hasI) parts.push_back(iLimitInp->Render() | size(WIDTH, EQUAL, 13));
        else      parts.push_back(text(" -- ") | size(WIDTH, EQUAL, 13) | dim);
        parts.push_back(fltT);

        return hbox(std::move(parts));
    }) | CatchEvent([&s, ch](Event e) {
        // Swallow all events for invisible rows so focus never lands here.
        if (!s.data.valid || ch >= s.data.numChannels()) return true;
        return false;
    });
}

inline Component makeMonitorTab(AppState& s, ConfigInputs& inputs) {
    // Build all 16 rows upfront. Invisible rows catch events + render empty.
    Components rows;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        rows.push_back(makeMonitorRow(s, inputs, ch));
    }
    auto tableContainer = Container::Vertical(rows);

    return Renderer(tableContainer, [=, &s]() {
        int n = s.data.numChannels();

        if (!s.data.valid) {
            return vbox({
                text(" Not connected \xe2\x80\x94 click [ Connect ] in the toolbar ") | center | bold,
                filler(),
            });
        }

        const auto& si = s.data.sysInfo;
        char tmp[16], hum[16];
        snprintf(tmp, sizeof(tmp), "%.1f", si.boardTempRaw * 0.1);
        snprintf(hum, sizeof(hum), "%.1f", si.boardHumidityRaw * 0.1);
        auto sysbar = hbox({
            text(" Proto: " + std::to_string(si.protoMajor) + "." + std::to_string(si.protoMinor)),
            text("  Variant: " + std::to_string(si.variantId)),
            text("  Ch: " + std::to_string(n)),
            text("  Uptime: " + std::to_string(si.uptimeSec) + " s"),
            text("  Mode: " + std::string(opModeName(si.activeOpMode))),
            text("  Temp: " + std::string(tmp) + " \xc2\xb0\x43"),
            text("  Humid: " + std::string(hum) + " %"),
            text("  Fault: " + faultStr(si.faultCause)),
        });

        if (n == 0)
            return vbox({ sysbar, text(" Discovering channels... ") | dim | center });

        // Column headers
        auto colHdr = hbox({
            text(" CH ")      | size(WIDTH, EQUAL, 4)  | bold,
            text(" Vm (V)     ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Im (nA)        ") | size(WIDTH, EQUAL, 16) | bold,
            text(" Status  ") | size(WIDTH, EQUAL, 8)  | bold,
            text(" Vset (V)   ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Vt (V)     ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Ramp\xe2\x86\x91") | size(WIDTH, EQUAL, 8)  | bold,
            text(" Ramp\xe2\x86\x93") | size(WIDTH, EQUAL, 8)  | bold,
            text(" I-lim (nA)  ") | size(WIDTH, EQUAL, 13) | bold,
            text(" Fault") | bold,
        });

        return vbox({
            sysbar,
            separator(),
            colHdr,
            separator(),
            tableContainer->Render(),
        });
    });
}

} // namespace hvb::tui
```

- [ ] **Step 2: Build check**

```bash
cd tools/hvb_demo_app/build && make hvb_demo_tui 2>&1
```

Expected: Compiles with warnings (unused variables OK). Link errors from main.cpp expected.

- [ ] **Step 3: Commit**

```bash
git add tools/hvb_demo_app/tui/tab_monitor.h
git commit -m "feat(tui): rewrite Monitor tab as interactive table"
```

---

### Task 5: main.cpp — wire ConfigInputs, dynamic lifecycle

**Files:**
- Modify: `tools/hvb_demo_app/tui/main.cpp`

Add `ConfigInputs` instance, update all tab factory calls to include it, add `syncDataToInputs` calls after connect/refresh, implement dynamic component lifecycle (build on connect, clear on disconnect).

- [ ] **Step 1: Add ConfigInputs instance and update connect/refresh logic**

In `tools/hvb_demo_app/tui/main.cpp`, after line 84 (`AppState appState...`), add the `ConfigInputs` instance:

```cpp
    hvb::tui::ConfigInputs inputs;
```

- [ ] **Step 2: Update connect lambda**

Replace the connect lambda (lines 109-135). `Container::Tab` copies children on construction, so `tabComponents.swap()` is ineffective. Instead, all 18 components are stable — the per-channel tabs render a placeholder when `ch >= numChannels()`. The tab bar title vector is resized to control what the user sees:

```cpp
    auto doConnect = [&] {
        if (modalPort.empty() || connecting) return;
        connecting = true;
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Connecting to " + modalPort + "..."; }
        screen.PostEvent(Event::Custom);
        std::thread([&] {
            int baud = 115200, slave = 1;
            try { baud  = std::stoi(modalBaud);    } catch (...) {}
            try { slave = std::stoi(modalSlaveId); } catch (...) {}
            bool ok = g_client.connect(modalPort, baud, slave, timeoutArg);
            g_connected = ok;
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                syncDataToInputs(data, inputs);
                rebuildChannelTitles(tabTitles, data.numChannels());
                int maxTab = static_cast<int>(tabTitles.size()) - 1;
                if (activeTab > maxTab) activeTab = maxTab;
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok
                ? ("Connected " + modalPort + "  (" + std::to_string(data.numChannels()) + " ch)")
                : ("Error: " + g_client.lastError()); }
            connecting = false;
            showModal  = false;
            screen.PostEvent(Event::Custom);
        }).detach();
    };
```

- [ ] **Step 3: Update disconnect button**

Replace the disconnect lambda (lines 158-163). Tab components are stable (Container::Tab copies children on construction). The tab bar resizes to hide channel tabs; per-channel renderers naturally show placeholder when `!data.valid`:

```cpp
    auto bDisconnect = hvb::tui::ActionButton("Disconnect", [&] {
        g_connected = false;
        data.valid  = false;
        tabTitles   = {"Monitor", "System"};
        activeTab   = std::min(activeTab, 1);
        { std::lock_guard<std::mutex> lk(statusMutex); statusMsg = "Disconnected"; }
    });
```

- [ ] **Step 4: Update Refresh button**

Replace the refresh lambda (lines 165-171):

```cpp
    auto bRefresh = hvb::tui::ActionButton("Refresh", [&] {
        if (g_connected) {
            { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
            syncDataToInputs(data, inputs);
            data.valid = true;
        }
    });
```

- [ ] **Step 5: Update tab component initialization**

Replace lines 190-195. All `2 + MAX_CHANNELS` components are built once upfront and never swapped. Per-channel tabs render a placeholder when `ch >= numChannels()` or `!data.valid` — effectively "deleted" from the user's perspective:

```cpp
    // Tab content — built upfront with all 18 slots (Monitor + System + 16 channels).
    // Per-channel tabs render a placeholder for out-of-range channels or when disconnected.
    // The tab bar (tabTitles) controls visibility by resizing the title list.
    Components tabComponents = {
        hvb::tui::makeMonitorTab(appState, inputs),
        hvb::tui::makeSystemTab(appState, inputs),
    };
    for (int ch = 0; ch < hvb::tui::MAX_CHANNELS; ++ch)
        tabComponents.push_back(hvb::tui::makeChannelTab(appState, inputs, ch));
    auto tabContent = Container::Tab(tabComponents, &activeTab);
```

- [ ] **Step 6: Update auto-connect path**

Replace lines 242-255 (auto-connect when -p given on command line). Same pattern as Step 2 — no component rebuild, just sync + resize titles:

```cpp
    // Auto-connect if port given on command line
    if (!portArg.empty()) {
        std::thread([&] {
            bool ok = g_client.connect(portArg, baudArg, slaveArg, timeoutArg);
            g_connected = ok;
            if (ok) {
                { std::lock_guard<std::mutex> lk(g_scanMutex); doFullScan(data); }
                data.valid = true;
                syncDataToInputs(data, inputs);
                rebuildChannelTitles(tabTitles, data.numChannels());
            }
            { std::lock_guard<std::mutex> lk(statusMutex);
              statusMsg = ok ? "" : "Error: " + g_client.lastError(); }
            screen.PostEvent(Event::Custom);
        }).detach();
    }
```

- [ ] **Step 7: Build check**

```bash
cd tools/hvb_demo_app/build && make hvb_demo_tui 2>&1
```

Expected: Full compile + link success. No errors.

- [ ] **Step 8: Commit**

```bash
git add tools/hvb_demo_app/tui/main.cpp
git commit -m "feat(tui): wire ConfigInputs, dynamic component lifecycle"
```

---

### Task 6: Build and Verify

**Files:**
- None (verification only)

- [ ] **Step 1: Clean rebuild**

```bash
cd tools/hvb_demo_app && rm -rf build && mkdir build && cd build && cmake .. && make hvb_demo_tui -j$(nproc) 2>&1
```

Expected: Zero errors, zero warnings.

- [ ] **Step 2: Verify binary exists and is usable**

```bash
ls -la tools/hvb_demo_app/bin/hvb_demo_tui
./tools/hvb_demo_app/bin/hvb_demo_tui --help 2>&1 || true
```

- [ ] **Step 3: Verify with hardware (if board connected)**

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null && echo "Board present" || echo "No board"
./tools/hvb_demo_app/bin/hvb_demo_tui -p /dev/ttyUSB0 -s 1 2>&1 &
# Wait 3s for auto-connect, then kill
sleep 3 && kill %1 2>/dev/null
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore(tui): verify build and binary output"
```

---

## Verification Checklist (Post-Implementation)

1. Connect to board — Monitor Input fields show correct device values (not empty defaults)
2. Edit Vset in Monitor table, Enter — write succeeds, Input string updates to committed value
3. Switch to Channel tab — Vset Input shows same value as Monitor tab
4. Edit Vset in Channel tab — Monitor tab reflects same value after switching back
5. Click Status [ OFF ] button — output enables, button label changes to [ ON ] (green)
6. Click Status [ ON ] button — output disables, button label changes to [ OFF ] (dim)
7. Poll runs 30 s — readonly columns update, no flicker or jitter
8. Disconnect — channel tabs removed, Monitor table shows "Not connected"
9. Reconnect — widgets rebuilt, values loaded from current board config
10. InlineCycler changes (op mode, baud, recovery policy) — write succeeds, UI updates

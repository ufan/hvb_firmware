# Host Tools Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the monolithic `tools/modbus_debug_tool` into a shared core library, an end-user demo app, and a factory calibration tool — while adding protocol v2.1 calibration register support throughout.

**Architecture:** Three directories under `tools/`: `hvb_modbus_core` (shared static lib), `hvb_demo_app` (CLI+TUI+GUI for operators), `hvb_factory_tool` (REPL+GUI for calibration). A `shared_qml` module provides theming for both GUIs. The core library gains calibration unlock, raw DAC/ADC, sample, commit, and snapshot APIs. The demo app loses calibration write controls (read-only only). The factory tool provides the full calibration workflow.

**Tech Stack:** C++17, CMake 3.20+, ModbusLib v0.4.8, CLI11 v2.4.2, FTXUI v5.0.0, toml++ v3.4.0, daniele77/cli v2.2.0 (new), Catch2 v3.7.1, Qt 6.8+ (optional GUI)

**Worktree:** `.worktrees/host-tools-architecture` on branch `feature/host-tools-architecture`

**Spec:** `docs/superpowers/specs/2026-06-16-host-tools-architecture.md`

**Register reference:** `ref/modbus_interface.md`, `include/regmap/hvb_regs.h`

---

## Phase 1: Core Library v2.1 Updates

### Task 1: Core types.h — Add v2.1 Enums, Structs, and Helpers

**Files:**
- Modify: `tools/modbus_debug_tool/core/types.h`

- [ ] **Step 1: Add CalibrationSampleStatus enum and Calibration OpMode**

Add `Calibration = 2` to `OpMode`, the new `CalibrationSampleStatus` enum, `SysCap::CALIBRATION_MODE`, and `CalibrationSnapshot` struct. Update `opModeName()`.

```cpp
// In OpMode enum, add after Automatic:
    Calibration = 2,

// New enum after OpMode:
enum class CalibrationSampleStatus : uint16_t {
    NoSample = 0,
    Valid    = 1,
    Busy     = 2,
    Error    = 3,
};

// In SysCap namespace, add:
    inline constexpr uint16_t CALIBRATION_MODE = 0x0004;
```

- [ ] **Step 2: Add calibration fields to ChannelInfo**

```cpp
// At end of ChannelInfo struct, add:
    int32_t rawAdcVoltage = 0;
    int32_t rawAdcCurrent = 0;
    CalibrationSampleStatus sampleStatus = CalibrationSampleStatus::NoSample;
    uint16_t rawDacReadback = 0;
```

- [ ] **Step 3: Add calibration fields to ChannelConfig**

```cpp
// At end of ChannelConfig struct, add:
    bool calOutputEnabled = false;
    uint16_t rawDacCode = 0;
    uint16_t maxRawDacLimit = 4095;
```

- [ ] **Step 4: Add CalibrationSnapshot struct**

```cpp
struct CalibrationSnapshot {
    bool outputEnabled = false;
    uint16_t rawDacCode = 0;
    uint16_t maxRawDacLimit = 0;
    uint16_t rawDacReadback = 0;
    CalibrationSampleStatus sampleStatus = CalibrationSampleStatus::NoSample;
    int32_t rawAdcVoltage = 0;
    int32_t rawAdcCurrent = 0;
};
```

- [ ] **Step 5: Update opModeName() and add calSampleStatusName()**

```cpp
inline const char* opModeName(OpMode m) {
    switch (m) {
    case OpMode::Normal:      return "Normal";
    case OpMode::Automatic:   return "Automatic";
    case OpMode::Calibration: return "Calibration";
    }
    return "?";
}

inline const char* calSampleStatusName(CalibrationSampleStatus s) {
    switch (s) {
    case CalibrationSampleStatus::NoSample: return "NoSample";
    case CalibrationSampleStatus::Valid:    return "Valid";
    case CalibrationSampleStatus::Busy:     return "Busy";
    case CalibrationSampleStatus::Error:    return "Error";
    }
    return "?";
}
```

- [ ] **Step 6: Verify build**

Run: `cd tools/modbus_debug_tool && cmake --preset linux-debug && cmake --build build/linux-debug 2>&1 | tail -5`
Expected: Build succeeds (no errors)

- [ ] **Step 7: Commit**

```bash
git add tools/modbus_debug_tool/core/types.h
git commit -m "feat(core): add protocol v2.1 calibration types (OpMode::Calibration, CalibrationSnapshot)"
```

---

### Task 2: Core register_map.h — Add INT32 Signed Helper and Extension Address

**Files:**
- Modify: `tools/modbus_debug_tool/core/register_map.h`

- [ ] **Step 1: Add signed int32 helper and extension address function**

```cpp
// After uint32FromRegs, add:
inline int32_t int32FromRegs(uint16_t hi, uint16_t lo) {
    uint32_t u = (static_cast<uint32_t>(hi) << 16) | lo;
    return static_cast<int32_t>(u);
}

// After chAddr, add:
inline constexpr uint16_t extAddr(uint16_t off) {
    return EXT_BLOCK_BASE + off;
}
```

- [ ] **Step 2: Verify build**

Run: `cd tools/modbus_debug_tool && cmake --build build/linux-debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add tools/modbus_debug_tool/core/register_map.h
git commit -m "feat(core): add int32FromRegs signed helper and extAddr for extension block"
```

---

### Task 3: Core register_meta.cpp — Add v2.1 Calibration Registers to Catalog

**Files:**
- Modify: `tools/modbus_debug_tool/core/register_meta.cpp`

- [ ] **Step 1: Update SYSTEM_INPUT mode enum labels to include Calibration**

Change the Active Operating Mode entry's enumLabels from `{"Normal", "Automatic"}` to `{"Normal", "Automatic", "Calibration"}`.

```cpp
    {12, "Active Operating Mode", "uint16", "enum",    "Current domain operating mode", 1.0, false, false, -1,
        {"Normal", "Automatic", "Calibration"}},
```

- [ ] **Step 2: Update SYSTEM_HOLDING mode enum labels to include Calibration**

```cpp
    {0,  "Operating Mode",        "uint16", "enum",    "Normal, Automatic, or Calibration", 1.0, true, false, -1,
        {"Normal", "Automatic", "Calibration"}},
```

- [ ] **Step 3: Add channel input calibration tail offsets 12-17 to CHANNEL_INPUT**

Add after the channel core/discovery registers. `CH_CAPABILITY_FLAGS` is offset 9; measured voltage/current are offsets 10-11; calibration tail starts at offset 12:

```cpp
    {12, "Raw ADC Voltage HI",    "int32_hi","lsb",    "Calibration Mode — raw ADC voltage (high)", 1.0},
    {13, "Raw ADC Voltage LO",    "int32_lo","lsb",    "Calibration Mode — raw ADC voltage (low)", 1.0},
    {14, "Raw ADC Current HI",    "int32_hi","lsb",    "Calibration Mode — raw ADC current (high)", 1.0},
    {15, "Raw ADC Current LO",    "int32_lo","lsb",    "Calibration Mode — raw ADC current (low)", 1.0},
    {16, "Cal Sample Status",     "uint16", "enum",    "Calibration Mode — sample status", 1.0, false, false, -1,
        {"NoSample", "Valid", "Busy", "Error"}},
    {17, "Raw DAC Readback",      "uint16", "lsb",     "Calibration Mode — last written DAC code", 1.0},
```

- [ ] **Step 4: Add channel holding calibration tail offsets 22-26 to CHANNEL_HOLDING**

Add after the `Meas I Calibration B` entry (offset 21):

```cpp
    {22, "Cal Output Enable",     "uint16", "bool",    "Calibration Mode — raw output gate", 1.0, true},
    {23, "Raw DAC Code",          "uint16", "lsb",     "Calibration Mode — native DAC code", 1.0, true},
    {24, "Cal Sample Command",    "uint16", "enum",    "Calibration Mode — write 1 to capture ADC", 1.0, true, true, -1,
        {"None", "Execute"}},
    {25, "Cal Commit Command",    "uint16", "enum",    "Calibration Mode — write 1 to persist", 1.0, true, true, -1,
        {"None", "Execute"}},
    {26, "Cal Max Raw DAC Limit", "uint16", "lsb",     "Calibration Mode — temporary max DAC code", 1.0, true},
```

- [ ] **Step 5: Update catalog heading strings for v2.1 offsets**

In `formatRegisterCatalog()`:

```cpp
    ss << "\n=== Channel Input Registers (FC04, per-channel offsets 0..17) ===\n";
    // ...
    ss << "\n=== Channel Holding Registers (FC03/06, per-channel offsets 0..26) ===\n";
```

- [ ] **Step 6: Verify build**

Run: `cd tools/modbus_debug_tool && cmake --build build/linux-debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 7: Commit**

```bash
git add tools/modbus_debug_tool/core/register_meta.cpp
git commit -m "feat(core): add v2.1 calibration registers to metadata catalog"
```

---

### Task 4: Core hvb_modbus_client — Add Calibration APIs

**Files:**
- Modify: `tools/modbus_debug_tool/core/hvb_modbus_client.h`
- Modify: `tools/modbus_debug_tool/core/hvb_modbus_client.cpp`

- [ ] **Step 1: Add calibration method declarations to hvb_modbus_client.h**

Add after the existing `writeCalibrationMeasI` declaration:

```cpp
    // Calibration Mode operations (v2.1)
    bool unlockCalibrationStep(uint16_t value);
    bool enterCalibrationMode();
    bool exitCalibrationMode(OpMode targetMode);

    bool writeCalibrationOutputEnable(int ch, bool enable);
    bool writeRawDacCode(int ch, uint16_t code);
    bool sendCalibrationSampleCommand(int ch);
    bool sendCalibrationCommitCommand(int ch);
    bool writeCalibrationMaxDacLimit(int ch, uint16_t limit);
    CalibrationSnapshot readCalibrationSnapshot(int ch);
```

- [ ] **Step 2: Extend readChannelInfo to read 18 registers (offsets 0..17)**

In `hvb_modbus_client.cpp`, replace the `readChannelInfo` implementation:

```cpp
ChannelInfo HvbModbusClient::readChannelInfo(int ch) {
    ChannelInfo info;
    if (!checkConnected()) return info;

    uint16_t base = reg::chAddr(ch, 0);
    uint16_t buf[18] = {};  // offsets 0..17
    if (!readRegsInternal(false, base, 18, buf)) return info;

    info.voltageRaw                   = static_cast<int16_t>(buf[CH_MEASURED_VOLTAGE]);
    info.currentRaw                   = static_cast<int16_t>(buf[CH_MEASURED_CURRENT]);
    info.operationalTargetVoltageRaw  = static_cast<int16_t>(buf[CH_OPER_TARGET_VOLTAGE]);
    info.status                       = buf[CH_STATUS_BITS];
    info.activeFault                  = buf[CH_ACTIVE_FAULT_CAUSE];
    info.faultHistory                 = buf[CH_FAULT_HISTORY_CAUSE];
    info.lastProtOutputAction         = buf[CH_LAST_PROT_OUT_ACTION];
    info.retryCount                   = static_cast<int>(buf[CH_AUTO_RETRY_COUNT]);
    info.cooldownSec                  = static_cast<int>(buf[CH_AUTO_COOLDOWN_REMAINING]);
    info.lastFaultTimestamp           = reg::uint32FromRegs(buf[CH_LAST_FAULT_TIMESTAMP_HI],
                                                            buf[CH_LAST_FAULT_TIMESTAMP_LO]);
    info.chCapFlags                   = buf[CH_CAPABILITY_FLAGS];
    info.rawAdcVoltage               = reg::int32FromRegs(buf[CH_RAW_ADC_VOLTAGE_HI],
                                                           buf[CH_RAW_ADC_VOLTAGE_LO]);
    info.rawAdcCurrent               = reg::int32FromRegs(buf[CH_RAW_ADC_CURRENT_HI],
                                                           buf[CH_RAW_ADC_CURRENT_LO]);
    info.sampleStatus                = static_cast<CalibrationSampleStatus>(buf[CH_CAL_SAMPLE_STATUS]);
    info.rawDacReadback              = buf[CH_RAW_DAC_READBACK];
    return info;
}
```

- [ ] **Step 3: Extend readChannelConfig to read offsets 0..25**

Replace the `readChannelConfig` implementation. The board limits single-read count to 12, so use three batches:

```cpp
ChannelConfig HvbModbusClient::readChannelConfig(int ch) {
    ChannelConfig cfg;
    if (!checkConnected()) return cfg;

    uint16_t base = reg::chAddr(ch, 0);
    // Batch 1: offsets 0..11
    uint16_t buf[12] = {};
    if (!readRegsInternal(true, base, 12, buf)) return cfg;

    cfg.configuredTargetVRaw = static_cast<int16_t>(buf[CH_CFG_TARGET_VOLTAGE]);
    cfg.outputAction         = static_cast<OutputAction>(buf[CH_OUTPUT_ACTION]);
    cfg.faultCommand         = static_cast<ChannelFaultCommand>(buf[CH_FAULT_CMD]);
    cfg.rampUpStepRaw        = buf[CH_RAMP_UP_STEP];
    cfg.rampUpInterval       = buf[CH_RAMP_UP_INTERVAL];
    cfg.rampDownStepRaw      = buf[CH_RAMP_DOWN_STEP];
    cfg.rampDownInterval     = buf[CH_RAMP_DOWN_INTERVAL];
    cfg.vProtMode            = static_cast<ProtectionMode>(buf[CH_VOLTAGE_PROTECTION_MODE]);
    cfg.vProtOutputAction    = static_cast<OutputAction>(buf[CH_VOLTAGE_PROT_OUT_ACTION]);
    cfg.vLimitThresholdRaw   = static_cast<int16_t>(buf[CH_VOLTAGE_LIMIT_THRESHOLD]);
    cfg.iProtMode            = static_cast<ProtectionMode>(buf[CH_CURRENT_PROTECTION_MODE]);
    cfg.iProtOutputAction    = static_cast<OutputAction>(buf[CH_CURRENT_PROT_OUT_ACTION]);

    // Batch 2: offsets 12..23
    uint16_t buf2[12] = {};
    if (!readRegsInternal(true, base + 12, 12, buf2)) return cfg;

    cfg.iLimitThresholdRaw   = static_cast<int16_t>(buf2[CH_CURRENT_LIMIT_THRESHOLD - 12]);
    cfg.derateStepRaw        = buf2[CH_AUTO_DERATE_STEP - 12];
    cfg.saveTargetPolicy     = buf2[CH_SAVE_TARGET_POLICY - 12] != 0;
    cfg.outCalK              = buf2[CH_OUTPUT_CAL_K - 12];
    cfg.outCalB              = static_cast<int16_t>(buf2[CH_OUTPUT_CAL_B - 12]);
    cfg.measVCalK            = buf2[CH_MEASURED_V_CAL_K - 12];
    cfg.measVCalB            = static_cast<int16_t>(buf2[CH_MEASURED_V_CAL_B - 12]);
    cfg.measICalK            = buf2[CH_MEASURED_I_CAL_K - 12];
    cfg.measICalB            = static_cast<int16_t>(buf2[CH_MEASURED_I_CAL_B - 12]);
    cfg.calOutputEnabled     = buf2[CH_CAL_OUTPUT_ENABLE - 12] != 0;
    cfg.rawDacCode           = buf2[CH_CAL_DAC_CODE - 12];
    // buf2[11] = offset 23 (sample cmd, reads as 0)

    // Batch 3: offsets 24..25
    uint16_t buf3[2] = {};
    if (!readRegsInternal(true, base + 24, 2, buf3)) return cfg;

    // buf3[0] = offset 24 (commit cmd, reads as 0)
    cfg.maxRawDacLimit       = buf3[CH_CAL_MAX_RAW_DAC_LIMIT - 24];
    return cfg;
}
```

- [ ] **Step 4: Add calibration operation implementations**

Add at end of channel writes section, before `#undef CHW`:

```cpp
bool HvbModbusClient::unlockCalibrationStep(uint16_t value) {
    return writeRegsInternal(reg::extAddr(EXT_CAL_UNLOCK), 1, &value);
}
bool HvbModbusClient::enterCalibrationMode() {
    return writeOperatingMode(OpMode::Calibration);
}
bool HvbModbusClient::exitCalibrationMode(OpMode targetMode) {
    if (targetMode == OpMode::Calibration) {
        m_impl->errorText = "target mode cannot be Calibration";
        return false;
    }
    return writeOperatingMode(targetMode);
}
```

Note: these three do not use the CHW macro — add them *after* the `#undef CHW` / `#undef CHWN` lines:

```cpp
bool HvbModbusClient::writeCalibrationOutputEnable(int ch, bool enable) {
    uint16_t v = enable ? 1 : 0;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 1, &v);
}
bool HvbModbusClient::writeRawDacCode(int ch, uint16_t code) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_DAC_CODE), 1, &code);
}
bool HvbModbusClient::sendCalibrationSampleCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_SAMPLE_CMD), 1, &v);
}
bool HvbModbusClient::sendCalibrationCommitCommand(int ch) {
    uint16_t v = CAL_COMMAND_EXECUTE;
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_COMMIT_CMD), 1, &v);
}
bool HvbModbusClient::writeCalibrationMaxDacLimit(int ch, uint16_t limit) {
    return writeRegsInternal(reg::chAddr(ch, CH_CAL_MAX_RAW_DAC_LIMIT), 1, &limit);
}

CalibrationSnapshot HvbModbusClient::readCalibrationSnapshot(int ch) {
    CalibrationSnapshot snap;
    if (!checkConnected()) return snap;

    // Read channel input offsets 12..17
    uint16_t ibuf[6] = {};
    if (!readRegsInternal(false, reg::chAddr(ch, CH_RAW_ADC_VOLTAGE_HI), 6, ibuf)) return snap;

    snap.rawAdcVoltage  = reg::int32FromRegs(ibuf[0], ibuf[1]);
    snap.rawAdcCurrent  = reg::int32FromRegs(ibuf[2], ibuf[3]);
    snap.sampleStatus   = static_cast<CalibrationSampleStatus>(ibuf[4]);
    snap.rawDacReadback = ibuf[5];

    // Read channel holding offsets 21..25
    uint16_t hbuf[5] = {};
    if (!readRegsInternal(true, reg::chAddr(ch, CH_CAL_OUTPUT_ENABLE), 5, hbuf)) return snap;

    snap.outputEnabled  = hbuf[0] != 0;
    snap.rawDacCode     = hbuf[1];
    // hbuf[2] = sample cmd (reads 0), hbuf[3] = commit cmd (reads 0)
    snap.maxRawDacLimit = hbuf[4];
    return snap;
}
```

- [ ] **Step 5: Verify build**

Run: `cd tools/modbus_debug_tool && cmake --build build/linux-debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add tools/modbus_debug_tool/core/hvb_modbus_client.h tools/modbus_debug_tool/core/hvb_modbus_client.cpp
git commit -m "feat(core): add v2.1 calibration APIs (unlock, raw DAC/ADC, sample, commit, snapshot)"
```

---

### Task 5: Virtual Board v2.1 Defaults

**Files:**
- Modify: `tools/modbus_debug_tool/tests/virtual_board.cpp`

- [ ] **Step 1: Update setVariantDefaults for protocol minor 1 and calibration capability**

```cpp
// Change:
    inputRegs[reg::sysAddr(0) + SYS_PROTOCOL_MINOR] = 0;
// To:
    inputRegs[reg::sysAddr(0) + SYS_PROTOCOL_MINOR] = 1;

// Change:
    inputRegs[reg::sysAddr(0) + SYS_CAPABILITY_FLAGS] = 0x0003; // Auto + Env
// To:
    inputRegs[reg::sysAddr(0) + SYS_CAPABILITY_FLAGS] = 0x0007; // Auto + Env + Calibration

// Add after channel defaults loop, for each channel:
        holdingRegs[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 4095;
```

- [ ] **Step 2: Verify build**

Run: `cd tools/modbus_debug_tool && cmake --build build/linux-debug 2>&1 | tail -5`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add tools/modbus_debug_tool/tests/virtual_board.cpp
git commit -m "test: update virtual board defaults to protocol v2.1 (minor=1, calibration cap)"
```

---

### Task 6: Core Tests — v2.1 Protocol and Calibration

**Files:**
- Create: `tools/modbus_debug_tool/tests/test_calibration.cpp`
- Modify: `tools/modbus_debug_tool/tests/CMakeLists.txt`

- [ ] **Step 1: Write calibration test file**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "hvb_modbus_client.h"
#include "register_map.h"
#include "types.h"
#include <cstring>

using namespace hvb;

static constexpr int MAX_ADDR = 280;

static void initBoard(uint16_t* input, uint16_t* holding) {
    memset(input, 0, MAX_ADDR * sizeof(uint16_t));
    memset(holding, 0, MAX_ADDR * sizeof(uint16_t));
    input[SYS_PROTOCOL_MAJOR] = 2;
    input[SYS_PROTOCOL_MINOR] = 1;
    input[SYS_CAPABILITY_FLAGS] = 0x0007;
    input[SYS_SUPPORTED_CHANNELS] = 2;
    input[SYS_ACTIVE_CHANNEL_MASK] = 0x0003;
    for (int ch = 0; ch < 2; ++ch) {
        uint16_t base = reg::chAddr(ch, 0);
        holding[base + CH_OUTPUT_CAL_K] = 10000;
        holding[base + CH_MEASURED_V_CAL_K] = 10000;
        holding[base + CH_MEASURED_I_CAL_K] = 10000;
        holding[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 4095;
    }
}

TEST_CASE("Protocol v2.1 system info", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto info = client.readSystemInfo();
    REQUIRE(info.protoMajor == 2);
    REQUIRE(info.protoMinor == 1);
    REQUIRE((info.sysCapFlags & SysCap::CALIBRATION_MODE) != 0);

    client.detachTestArrays();
}

TEST_CASE("CalibrationSampleStatus enum names", "[calibration]") {
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::NoSample)) == "NoSample");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Valid)) == "Valid");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Busy)) == "Busy");
    REQUIRE(std::string(calSampleStatusName(CalibrationSampleStatus::Error)) == "Error");
}

TEST_CASE("OpMode Calibration name", "[calibration]") {
    REQUIRE(std::string(opModeName(OpMode::Calibration)) == "Calibration");
}

TEST_CASE("Raw ADC signed 32-bit decode", "[calibration]") {
    REQUIRE(reg::int32FromRegs(0x0000, 0x0001) == 1);
    REQUIRE(reg::int32FromRegs(0xFFFF, 0xFFFF) == -1);
    REQUIRE(reg::int32FromRegs(0x7FFF, 0xFFFF) == 2147483647);
    REQUIRE(reg::int32FromRegs(0x8000, 0x0000) == -2147483648);
}

TEST_CASE("Calibration unlock write", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.unlockCalibrationStep(CAL_UNLOCK_STEP1));
    REQUIRE(holding[reg::extAddr(EXT_CAL_UNLOCK)] == 0xCA1B);

    REQUIRE(client.unlockCalibrationStep(CAL_UNLOCK_STEP2));
    REQUIRE(holding[reg::extAddr(EXT_CAL_UNLOCK)] == 0xA11B);

    client.detachTestArrays();
}

TEST_CASE("Calibration output enable and raw DAC code", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.writeCalibrationOutputEnable(0, true));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_OUTPUT_ENABLE)] == 1);

    REQUIRE(client.writeRawDacCode(0, 2048));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_DAC_CODE)] == 2048);

    REQUIRE(client.writeCalibrationOutputEnable(0, false));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_OUTPUT_ENABLE)] == 0);

    client.detachTestArrays();
}

TEST_CASE("Calibration sample and commit commands", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.sendCalibrationSampleCommand(0));
    REQUIRE(holding[reg::chAddr(0, CH_CAL_SAMPLE_CMD)] == CAL_COMMAND_EXECUTE);

    REQUIRE(client.sendCalibrationCommitCommand(1));
    REQUIRE(holding[reg::chAddr(1, CH_CAL_COMMIT_CMD)] == CAL_COMMAND_EXECUTE);

    client.detachTestArrays();
}

TEST_CASE("CalibrationSnapshot round-trip", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base_in = reg::chAddr(0, 0);
    uint16_t base_hold = reg::chAddr(0, 0);
    // Set up input registers (raw ADC values)
    input[base_in + CH_RAW_ADC_VOLTAGE_HI] = 0x0001;
    input[base_in + CH_RAW_ADC_VOLTAGE_LO] = 0x2345;
    input[base_in + CH_RAW_ADC_CURRENT_HI] = 0xFFFF;
    input[base_in + CH_RAW_ADC_CURRENT_LO] = 0xFE00;
    input[base_in + CH_CAL_SAMPLE_STATUS] = 1; // Valid
    input[base_in + CH_RAW_DAC_READBACK] = 2048;
    // Set up holding registers
    holding[base_hold + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base_hold + CH_CAL_DAC_CODE] = 2048;
    holding[base_hold + CH_CAL_MAX_RAW_DAC_LIMIT] = 3000;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto snap = client.readCalibrationSnapshot(0);
    REQUIRE(snap.rawAdcVoltage == 0x00012345);
    REQUIRE(snap.rawAdcCurrent == -512); // 0xFFFFFE00
    REQUIRE(snap.sampleStatus == CalibrationSampleStatus::Valid);
    REQUIRE(snap.rawDacReadback == 2048);
    REQUIRE(snap.outputEnabled == true);
    REQUIRE(snap.rawDacCode == 2048);
    REQUIRE(snap.maxRawDacLimit == 3000);

    client.detachTestArrays();
}

TEST_CASE("readChannelInfo includes v2.1 calibration fields", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    input[base + CH_RAW_ADC_VOLTAGE_HI] = 0x0000;
    input[base + CH_RAW_ADC_VOLTAGE_LO] = 0x1234;
    input[base + CH_RAW_ADC_CURRENT_HI] = 0x0000;
    input[base + CH_RAW_ADC_CURRENT_LO] = 0x5678;
    input[base + CH_CAL_SAMPLE_STATUS] = 2; // Busy
    input[base + CH_RAW_DAC_READBACK] = 1024;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto info = client.readChannelInfo(0);
    REQUIRE(info.rawAdcVoltage == 0x1234);
    REQUIRE(info.rawAdcCurrent == 0x5678);
    REQUIRE(info.sampleStatus == CalibrationSampleStatus::Busy);
    REQUIRE(info.rawDacReadback == 1024);

    client.detachTestArrays();
}

TEST_CASE("readChannelConfig includes v2.1 calibration fields", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    uint16_t base = reg::chAddr(0, 0);
    holding[base + CH_CAL_OUTPUT_ENABLE] = 1;
    holding[base + CH_CAL_DAC_CODE] = 3000;
    holding[base + CH_CAL_MAX_RAW_DAC_LIMIT] = 3500;

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    auto cfg = client.readChannelConfig(0);
    REQUIRE(cfg.calOutputEnabled == true);
    REQUIRE(cfg.rawDacCode == 3000);
    REQUIRE(cfg.maxRawDacLimit == 3500);

    client.detachTestArrays();
}

TEST_CASE("exitCalibrationMode rejects Calibration target", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE_FALSE(client.exitCalibrationMode(OpMode::Calibration));
    REQUIRE(client.exitCalibrationMode(OpMode::Normal));
    REQUIRE(holding[reg::sysAddr(SYS_OPERATING_MODE)] == 0);

    client.detachTestArrays();
}

TEST_CASE("writeCalibrationMaxDacLimit", "[calibration]") {
    uint16_t input[MAX_ADDR], holding[MAX_ADDR];
    initBoard(input, holding);

    HvbModbusClient client;
    client.attachTestArrays(input, holding, MAX_ADDR);

    REQUIRE(client.writeCalibrationMaxDacLimit(1, 2000));
    REQUIRE(holding[reg::chAddr(1, CH_CAL_MAX_RAW_DAC_LIMIT)] == 2000);

    client.detachTestArrays();
}
```

- [ ] **Step 2: Add test_calibration.cpp to CMakeLists.txt**

Add `test_calibration.cpp` to the `add_executable(hvb_tests ...)` source list.

- [ ] **Step 3: Build and run tests**

Run: `cd tools/modbus_debug_tool && cmake --build build/linux-debug 2>&1 | tail -5 && build/linux-debug/tests/hvb_tests "[calibration]" -v`
Expected: All calibration tests PASS

- [ ] **Step 4: Run full test suite to check for regressions**

Run: `cd tools/modbus_debug_tool && build/linux-debug/tests/hvb_tests -v 2>&1 | tail -20`
Expected: All tests PASS (some existing tests may need the 18-register read adjustment — see Step 5)

- [ ] **Step 5: Fix any regressions from extended readChannelInfo**

The `readChannelInfo` now reads 18 registers instead of 12. The `BoardDevice::readInputRegisters` in `virtual_board.cpp` has no count limit on input reads (only holding has `count > 12` check), so existing tests should pass. If any test fixture uses a smaller `testMaxAddr`, increase it.

- [ ] **Step 6: Commit**

```bash
git add tools/modbus_debug_tool/tests/test_calibration.cpp tools/modbus_debug_tool/tests/CMakeLists.txt
git commit -m "test: add v2.1 calibration core tests (types, APIs, snapshot round-trip)"
```

---

## Phase 2: Directory Restructure

### Task 7: Restructure tools/ Directory Layout

This is the big structural move. We restructure the directory while keeping everything building.

**Files:**
- Move: `tools/modbus_debug_tool/core/` → `tools/hvb_modbus_core/`
- Move: `tools/modbus_debug_tool/tests/` → `tools/hvb_modbus_core/tests/`
- Move: `tools/modbus_debug_tool/cli/` → `tools/hvb_demo_app/cli/`
- Move: `tools/modbus_debug_tool/tui/` → `tools/hvb_demo_app/tui/`
- Move: `tools/modbus_debug_tool/gui/` → `tools/hvb_demo_app/gui/`
- Move: `tools/modbus_debug_tool/CMakeLists.txt` → restructured
- Move: `tools/modbus_debug_tool/CMakePresets.json` → `tools/CMakePresets.json`
- Create: `tools/CMakeLists.txt` (top-level)
- Create: `tools/hvb_factory_tool/` (skeleton)
- Create: `tools/shared_qml/` (skeleton)

- [ ] **Step 1: Create new directory structure**

```bash
mkdir -p tools/hvb_modbus_core
mkdir -p tools/hvb_modbus_core/tests
mkdir -p tools/hvb_demo_app/cli
mkdir -p tools/hvb_demo_app/tui
mkdir -p tools/hvb_demo_app/gui
mkdir -p tools/hvb_demo_app/gui/qml/components
mkdir -p tools/hvb_factory_tool/repl
mkdir -p tools/hvb_factory_tool/gui/qml
mkdir -p tools/shared_qml
```

- [ ] **Step 2: Move core files**

```bash
git mv tools/modbus_debug_tool/core/types.h tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/register_map.h tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/register_meta.h tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/register_meta.cpp tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/config_manager.h tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/config_manager.cpp tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/hvb_modbus_client.h tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/hvb_modbus_client.cpp tools/hvb_modbus_core/
git mv tools/modbus_debug_tool/core/monitor_render.cpp tools/hvb_modbus_core/
```

- [ ] **Step 3: Move test files**

```bash
git mv tools/modbus_debug_tool/tests/virtual_board.h tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/virtual_board.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_connection.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_system_reads.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_channel_reads.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_writes.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_enum_validation.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_error_handling.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_monitor_output.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_tui_format.cpp tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/tests/test_calibration.cpp tools/hvb_modbus_core/tests/
```

- [ ] **Step 4: Move demo app files**

```bash
git mv tools/modbus_debug_tool/cli/main.cpp tools/hvb_demo_app/cli/
git mv tools/modbus_debug_tool/tui/main.cpp tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/tui/tab_monitor.h tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/tui/tab_system.h tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/tui/tab_channel.h tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/tui/widgets.h tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/tui/tui_format.h tools/hvb_demo_app/tui/
git mv tools/modbus_debug_tool/gui/main.cpp tools/hvb_demo_app/gui/
git mv tools/modbus_debug_tool/gui/modbus_backend.h tools/hvb_demo_app/gui/
git mv tools/modbus_debug_tool/gui/modbus_backend.cpp tools/hvb_demo_app/gui/
git mv tools/modbus_debug_tool/gui/modbus_worker.h tools/hvb_demo_app/gui/
git mv tools/modbus_debug_tool/gui/modbus_worker.cpp tools/hvb_demo_app/gui/
git mv tools/modbus_debug_tool/gui/qml/ tools/hvb_demo_app/gui/qml/
```

- [ ] **Step 5: Move deploy scripts and config**

```bash
git mv tools/modbus_debug_tool/CMakePresets.json tools/CMakePresets.json
git mv tools/modbus_debug_tool/deploy_linux.sh tools/hvb_demo_app/
git mv tools/modbus_debug_tool/deploy_windows.sh tools/hvb_demo_app/
git mv tools/modbus_debug_tool/run_tests.sh tools/hvb_modbus_core/tests/
git mv tools/modbus_debug_tool/README.md tools/hvb_demo_app/
```

- [ ] **Step 6: Write tools/CMakeLists.txt (new top-level)**

```cmake
cmake_minimum_required(VERSION 3.20)
project(hvb_tools LANGUAGES CXX)

option(BUILD_GUI "Build Qt Quick GUIs (requires Qt 6.x)" OFF)
option(BUILD_FACTORY "Build factory calibration tool" ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries (default OFF → static ModbusLib)")

include(FetchContent)

# ModbusLib — cross-platform C++ Modbus library (RTU client)
FetchContent_Declare(modbuslib
    GIT_REPOSITORY https://github.com/serhmarch/ModbusLib.git
    GIT_TAG v0.4.8
)
set(MB_QT_ENABLED OFF CACHE BOOL "" FORCE)
set(MB_EXAMPLES_ENABLED OFF CACHE BOOL "" FORCE)
set(MB_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(modbuslib)
set(MODBUSLIB_INCLUDE_DIR ${modbuslib_SOURCE_DIR}/src)

# CLI11 — header-only argument parser
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(cli11)

# toml++ — header-only TOML parser
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# ftxui — C++ TUI library
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# Catch2 — test framework
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_subdirectory(hvb_modbus_core)
add_subdirectory(hvb_demo_app)

if(BUILD_FACTORY)
    # daniele77/cli — header-only Cisco-style REPL
    FetchContent_Declare(daniele77cli
        GIT_REPOSITORY https://github.com/daniele77/cli.git
        GIT_TAG v2.2.0
    )
    FetchContent_MakeAvailable(daniele77cli)
    add_subdirectory(hvb_factory_tool)
endif()

if(BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core SerialPort Quick QuickControls2)
endif()
```

- [ ] **Step 7: Write tools/hvb_modbus_core/CMakeLists.txt**

```cmake
add_library(hvb_modbus_core STATIC
    config_manager.cpp
    hvb_modbus_client.cpp
    monitor_render.cpp
    register_meta.cpp
)
target_include_directories(hvb_modbus_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC ${MODBUSLIB_INCLUDE_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(hvb_modbus_core PUBLIC modbus)
target_link_libraries(hvb_modbus_core PUBLIC tomlplusplus::tomlplusplus)
target_compile_features(hvb_modbus_core PUBLIC cxx_std_17)
target_compile_options(hvb_modbus_core PRIVATE -Wall -Wextra)
target_compile_options(hvb_modbus_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>)

add_subdirectory(tests)
```

- [ ] **Step 8: Write tools/hvb_modbus_core/tests/CMakeLists.txt**

```cmake
add_executable(hvb_tests
    test_connection.cpp
    test_system_reads.cpp
    test_channel_reads.cpp
    test_writes.cpp
    test_enum_validation.cpp
    test_error_handling.cpp
    test_monitor_output.cpp
    test_tui_format.cpp
    test_calibration.cpp
)

target_include_directories(hvb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${MODBUSLIB_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

target_link_libraries(hvb_tests PRIVATE
    hvb_modbus_core
    Catch2::Catch2WithMain
)

target_compile_features(hvb_tests PRIVATE cxx_std_17)
target_compile_options(hvb_tests PRIVATE -Wall -Wextra)
```

- [ ] **Step 9: Write tools/hvb_demo_app/CMakeLists.txt**

```cmake
add_subdirectory(cli)
add_subdirectory(tui)

if(BUILD_GUI)
    add_subdirectory(gui)
endif()
```

- [ ] **Step 10: Write tools/hvb_demo_app/cli/CMakeLists.txt**

```cmake
add_executable(hvb_demo_cli main.cpp)
target_link_libraries(hvb_demo_cli PRIVATE hvb_modbus_core CLI11::CLI11)
target_compile_features(hvb_demo_cli PRIVATE cxx_std_17)
target_compile_options(hvb_demo_cli PRIVATE -Wall -Wextra)

set_target_properties(hvb_demo_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 11: Write tools/hvb_demo_app/tui/CMakeLists.txt**

```cmake
add_executable(hvb_demo_tui main.cpp)
target_include_directories(hvb_demo_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(hvb_demo_tui PRIVATE hvb_modbus_core ftxui::screen ftxui::dom ftxui::component)
target_compile_features(hvb_demo_tui PRIVATE cxx_std_17)
target_compile_options(hvb_demo_tui PRIVATE -Wall -Wextra)

set_target_properties(hvb_demo_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 12: Write tools/hvb_demo_app/gui/CMakeLists.txt**

```cmake
set(CMAKE_AUTOMOC ON)

qt_add_executable(hvb_demo_gui
    main.cpp
    modbus_backend.cpp modbus_backend.h
    modbus_worker.cpp modbus_worker.h
)

qt_add_qml_module(hvb_demo_gui
    URI HvbTool
    NO_PLUGIN
    QML_FILES
        qml/Theme.qml
        qml/main.qml
        qml/ConnectionBar.qml
        qml/SystemInfoTab.qml
        qml/SystemConfigTab.qml
        qml/ChannelTab.qml
        qml/RawDebugDialog.qml
        qml/components/ReadOnlyField.qml
        qml/components/EditableField.qml
        qml/components/EnumCombo.qml
        qml/components/StatusBadge.qml
    SOURCES
        main.cpp
        modbus_backend.cpp modbus_backend.h
        modbus_worker.cpp modbus_worker.h
)

target_link_libraries(hvb_demo_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_features(hvb_demo_gui PRIVATE cxx_std_17)
target_compile_options(hvb_demo_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_demo_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 13: Write tools/hvb_factory_tool/CMakeLists.txt (skeleton)**

```cmake
add_subdirectory(repl)

if(BUILD_GUI)
    add_subdirectory(gui)
endif()
```

- [ ] **Step 14: Write tools/hvb_factory_tool/repl/CMakeLists.txt (skeleton with placeholder main)**

```cmake
add_executable(hvb_factory_tui main.cpp)
target_include_directories(hvb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(hvb_factory_tui PRIVATE
    hvb_modbus_core
    cli::cli
)
target_compile_features(hvb_factory_tui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_tui PRIVATE -Wall -Wextra)

set_target_properties(hvb_factory_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

Create a minimal `tools/hvb_factory_tool/repl/main.cpp`:

```cpp
#include <cli/cli.h>
#include <cli/loopscheduler.h>
#include <cli/clilocalsession.h>
#include <iostream>

int main() {
    auto rootMenu = std::make_unique<cli::Menu>("factory");
    rootMenu->Insert("info", [](std::ostream& out) {
        out << "HVB Factory Calibration Tool v0.1\n";
    }, "Show tool version");

    cli::Cli cli(std::move(rootMenu));
    cli::LoopScheduler sched;
    cli::CliLocalTerminalSession session(cli, sched, std::cout);
    session.ExitAction([&sched](auto& out) {
        out << "Goodbye.\n";
        sched.Stop();
    });
    sched.Run();
    return 0;
}
```

- [ ] **Step 15: Update CMakePresets.json paths**

Update `tools/CMakePresets.json` so `sourceDir` points to `tools/` instead of `tools/modbus_debug_tool/`. Update build directory paths from `build/` to `build/` (relative to tools/).

- [ ] **Step 16: Update tui_format.h include path**

The TUI files include `"widgets.h"` and other local headers — since they moved together, relative includes still work. But `tui_format.h` may reference core headers by relative path. Check and fix any broken includes (e.g., `"../core/types.h"` → `"types.h"` since hvb_modbus_core is on the include path).

- [ ] **Step 17: Verify build**

```bash
cd tools && cmake -B build/linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build/linux-debug 2>&1 | tail -20
```
Expected: All targets build (hvb_modbus_core, hvb_demo_cli, hvb_demo_tui, hvb_factory_tui, hvb_tests)

- [ ] **Step 18: Run tests**

```bash
cd tools && build/linux-debug/hvb_modbus_core/tests/hvb_tests -v 2>&1 | tail -20
```
Expected: All tests PASS

- [ ] **Step 19: Remove old modbus_debug_tool directory**

```bash
git rm -r tools/modbus_debug_tool/core/CMakeLists.txt
git rm -r tools/modbus_debug_tool/cli/CMakeLists.txt
git rm -r tools/modbus_debug_tool/tui/CMakeLists.txt
git rm -r tools/modbus_debug_tool/gui/CMakeLists.txt
git rm -r tools/modbus_debug_tool/tests/CMakeLists.txt
git rm -r tools/modbus_debug_tool/tests/cli_crosscheck.sh
git rm -r tools/modbus_debug_tool/tests/cli_run_all.sh
git rm tools/modbus_debug_tool/CMakeLists.txt
```

Remove any remaining empty directories.

- [ ] **Step 20: Commit**

```bash
git add tools/
git commit -m "refactor: restructure tools/ into hvb_modbus_core + hvb_demo_app + hvb_factory_tool"
```

---

## Phase 3: Demo App Cleanup

### Task 8: Demo CLI — Remove Calibration Writes, Add v2.1 Display

**Files:**
- Modify: `tools/hvb_demo_app/cli/main.cpp`

- [ ] **Step 1: Remove calibration write commands**

Remove these subcommand definitions and their variable declarations:
- `cal-out` subcommand (lines ~443-446)
- `cal-meas-v` subcommand (lines ~448-451)
- `cal-meas-i` subcommand (lines ~453-456)
- Variables: `ch_cal_out_k`, `ch_cal_out_b`, `ch_cal_mv_k`, `ch_cal_mv_b`, `ch_cal_mi_k`, `ch_cal_mi_b`

- [ ] **Step 2: Update cmdInfo to show protocol v2.1 and calibration capability**

In `cmdInfo()`, update the capability flags display:

```cpp
    printSep("Cap Flags:", std::to_string(info.sysCapFlags) + " "
        + (info.sysCapFlags & hvb::SysCap::AUTO_MODE_SUPPORTED ? "[Auto]" : "")
        + (info.sysCapFlags & hvb::SysCap::ENV_SENSOR_PRESENT ? "[Env]" : "")
        + (info.sysCapFlags & hvb::SysCap::CALIBRATION_MODE ? "[Cal]" : ""));
```

- [ ] **Step 3: Update app description**

Change `CLI::App app{"HVB Modbus Debug Tool"}` to `CLI::App app{"HVB Demo App"}`.

- [ ] **Step 4: Verify build and run**

```bash
cd tools && cmake --build build/linux-debug --target hvb_demo_cli 2>&1 | tail -5
bin/hvb_demo_cli --help
```
Expected: Build succeeds, help shows no cal-out/cal-meas-v/cal-meas-i subcommands

- [ ] **Step 5: Commit**

```bash
git add tools/hvb_demo_app/cli/main.cpp
git commit -m "feat(demo): remove calibration writes from CLI, add v2.1 capability display"
```

---

### Task 9: Demo TUI — Remove Calibration Write Panel

**Files:**
- Modify: `tools/hvb_demo_app/tui/tab_channel.h`

- [ ] **Step 1: Remove calibration write controls from makeChannelTab**

Remove from `St` struct:
```cpp
        std::string outK, outB, measVK, measVB, measIK, measIB;
```

Remove the `onCal` lambda entirely.

Remove these widget declarations:
```cpp
    auto outKInp   = CommitInput(&st->outK,    "10000", onCal);
    auto outBInp   = CommitInput(&st->outB,    "0",     onCal);
    auto measVKInp = CommitInput(&st->measVK,  "10000", onCal);
    auto measVBInp = CommitInput(&st->measVB,  "0",     onCal);
    auto measIKInp = CommitInput(&st->measIK,  "10000", onCal);
    auto measIBInp = CommitInput(&st->measIB,  "0",     onCal);
```

Remove these from the container:
```cpp
        outKInp, outBInp, measVKInp, measVBInp, measIKInp, measIBInp,
```

- [ ] **Step 2: Replace writable calPanel with read-only display**

Replace the `calPanel` render block with a read-only version that reads from `s.data.chCfg[ch]`:

```cpp
        Element calInfo = text(" No data ") | dim;
        if (s.data.valid) {
            const auto& cc = s.data.chCfg[ch];
            calInfo = vbox({
                hbox({ text("Output  K: "), text(std::to_string(cc.outCalK)) | bold, text("  B: "), text(std::to_string(cc.outCalB)) | bold }),
                hbox({ text("Meas V  K: "), text(std::to_string(cc.measVCalK)) | bold, text("  B: "), text(std::to_string(cc.measVCalB)) | bold }),
                hbox({ text("Meas I  K: "), text(std::to_string(cc.measICalK)) | bold, text("  B: "), text(std::to_string(cc.measICalB)) | bold }),
            });
        }
        auto calPanel = window(text(" Calibration (read-only) "), calInfo);
```

- [ ] **Step 3: Verify build**

```bash
cd tools && cmake --build build/linux-debug --target hvb_demo_tui 2>&1 | tail -5
```
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add tools/hvb_demo_app/tui/tab_channel.h
git commit -m "feat(demo): replace TUI calibration write panel with read-only display"
```

---

### Task 10: Config Manager — Update Default Path

**Files:**
- Modify: `tools/hvb_modbus_core/config_manager.cpp`

- [ ] **Step 1: Update default config file path**

Change the config filename from `.hvb_modbus_tool.toml` to `.hvb_demo_app.toml`:

Find the `defaultPath()` implementation and change the filename string.

- [ ] **Step 2: Verify build**

```bash
cd tools && cmake --build build/linux-debug 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add tools/hvb_modbus_core/config_manager.cpp
git commit -m "feat(core): update config file path to ~/.hvb_demo_app.toml"
```

---

## Phase 4: Factory Tool REPL

### Task 11: FactorySession — Connection and State Management

**Files:**
- Create: `tools/hvb_factory_tool/repl/FactorySession.h`
- Create: `tools/hvb_factory_tool/repl/FactorySession.cpp`

- [ ] **Step 1: Write FactorySession header**

```cpp
#pragma once

#include "hvb_modbus_client.h"
#include "config_manager.h"
#include "types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hvb::factory {

enum class WatchMode { Off, Adc, Measure, Status, All };

class FactorySession {
public:
    FactorySession();
    ~FactorySession();

    bool connect(const std::string& port, int baud = 115200, int slaveId = 1);
    void disconnect();
    bool isConnected() const;
    HvbModbusClient& client();

    int activeChannel() const;
    void setActiveChannel(int ch);

    WatchMode watchMode() const;
    void startWatch(WatchMode mode, int intervalMs, std::ostream& out);
    void stopWatch();

    std::string lastError() const;

private:
    HvbModbusClient m_client;
    ConfigManager m_config;
    int m_activeChannel = -1;
    std::atomic<WatchMode> m_watchMode{WatchMode::Off};
    std::atomic<bool> m_watchRunning{false};
    std::thread m_watchThread;
    std::mutex m_ioMutex;

    void watchLoop(WatchMode mode, int intervalMs, std::ostream& out);
};

} // namespace hvb::factory
```

- [ ] **Step 2: Write FactorySession implementation**

```cpp
#include "FactorySession.h"
#include "register_map.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace hvb::factory {

FactorySession::FactorySession() = default;

FactorySession::~FactorySession() {
    stopWatch();
    disconnect();
}

bool FactorySession::connect(const std::string& port, int baud, int slaveId) {
    return m_client.connect(port, baud, slaveId);
}

void FactorySession::disconnect() {
    stopWatch();
    m_client.disconnect();
    m_activeChannel = -1;
}

bool FactorySession::isConnected() const { return m_client.isConnected(); }
HvbModbusClient& FactorySession::client() { return m_client; }
int FactorySession::activeChannel() const { return m_activeChannel; }
void FactorySession::setActiveChannel(int ch) { m_activeChannel = ch; }
WatchMode FactorySession::watchMode() const { return m_watchMode.load(); }
std::string FactorySession::lastError() const { return m_client.lastError(); }

void FactorySession::startWatch(WatchMode mode, int intervalMs, std::ostream& out) {
    stopWatch();
    m_watchMode = mode;
    m_watchRunning = true;
    m_watchThread = std::thread(&FactorySession::watchLoop, this, mode, intervalMs, std::ref(out));
}

void FactorySession::stopWatch() {
    m_watchRunning = false;
    if (m_watchThread.joinable()) m_watchThread.join();
    m_watchMode = WatchMode::Off;
}

void FactorySession::watchLoop(WatchMode mode, int intervalMs, std::ostream& out) {
    int ch = m_activeChannel;
    while (m_watchRunning && m_client.isConnected()) {
        std::lock_guard<std::mutex> lk(m_ioMutex);
        std::ostringstream ss;

        if (mode == WatchMode::Adc || mode == WatchMode::All) {
            m_client.sendCalibrationSampleCommand(ch);
            auto snap = m_client.readCalibrationSnapshot(ch);
            ss << "  ADC V=" << snap.rawAdcVoltage
               << " I=" << snap.rawAdcCurrent
               << " status=" << calSampleStatusName(snap.sampleStatus)
               << " DAC=" << snap.rawDacReadback
               << " out=" << (snap.outputEnabled ? "ON" : "OFF");
        }
        if (mode == WatchMode::Measure || mode == WatchMode::All) {
            auto ci = m_client.readChannelInfo(ch);
            ss << "  V=" << std::fixed << std::setprecision(1)
               << reg::voltageToV(ci.voltageRaw) << "V"
               << " I=" << reg::currentToA(ci.currentRaw) * 1e6 << "uA";
        }
        if (mode == WatchMode::Status || mode == WatchMode::All) {
            auto si = m_client.readSystemInfo();
            ss << "  mode=" << opModeName(si.activeOpMode)
               << " uptime=" << si.uptimeSec << "s";
        }

        out << "\r" << ss.str() << std::flush;

        for (int i = 0; i < intervalMs / 100 && m_watchRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    out << "\n";
}

} // namespace hvb::factory
```

- [ ] **Step 3: Add to CMakeLists.txt**

Update `tools/hvb_factory_tool/repl/CMakeLists.txt`:

```cmake
add_executable(hvb_factory_tui
    main.cpp
    FactorySession.cpp
)
target_include_directories(hvb_factory_tui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)
target_link_libraries(hvb_factory_tui PRIVATE
    hvb_modbus_core
    cli::cli
)
target_compile_features(hvb_factory_tui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_tui PRIVATE -Wall -Wextra)

set_target_properties(hvb_factory_tui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 4: Verify build**

```bash
cd tools && cmake --build build/linux-debug --target hvb_factory_tui 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add tools/hvb_factory_tool/repl/
git commit -m "feat(factory): add FactorySession with connection, channel state, and watch modes"
```

---

### Task 12: Factory REPL — Full Menu Tree and Command Handlers

**Files:**
- Create: `tools/hvb_factory_tool/repl/FactoryCommands.h`
- Create: `tools/hvb_factory_tool/repl/FactoryCommands.cpp`
- Modify: `tools/hvb_factory_tool/repl/main.cpp`

- [ ] **Step 1: Write FactoryCommands header**

```cpp
#pragma once

#include "FactorySession.h"
#include <cli/cli.h>
#include <memory>

namespace hvb::factory {

std::unique_ptr<cli::Menu> buildRootMenu(FactorySession& session);

} // namespace hvb::factory
```

- [ ] **Step 2: Write FactoryCommands implementation**

```cpp
#include "FactoryCommands.h"
#include "register_map.h"
#include <iomanip>
#include <sstream>

namespace hvb::factory {

static void requireConnected(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    fn();
}

static void requireCalChannel(FactorySession& s, std::ostream& out, std::function<void()> fn) {
    if (!s.isConnected()) { out << "Error: not connected\n"; return; }
    if (s.activeChannel() < 0) { out << "Error: no active channel (use 'ch 0' or 'ch 1')\n"; return; }
    fn();
}

static int parseIntervalMs(const std::string& str) {
    if (str.empty()) return 1000;
    if (str.back() == 's') {
        auto num = str.substr(0, str.size() - 1);
        if (num.find("ms") != std::string::npos) return 0;
        return static_cast<int>(std::stod(num) * 1000);
    }
    if (str.size() > 2 && str.substr(str.size()-2) == "ms") {
        return std::stoi(str.substr(0, str.size()-2));
    }
    return static_cast<int>(std::stod(str) * 1000);
}

std::unique_ptr<cli::Menu> buildRootMenu(FactorySession& session) {
    auto root = std::make_unique<cli::Menu>("factory");

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port) {
            if (session.connect(port)) out << "Connected to " << port << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect to device", {"port"});

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port, int baud) {
            if (session.connect(port, baud)) out << "Connected to " << port << " @ " << baud << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect to device", {"port", "baud"});

    root->Insert("connect",
        [&session](std::ostream& out, const std::string& port, int baud, int id) {
            if (session.connect(port, baud, id)) out << "Connected to " << port << " @ " << baud << " id=" << id << "\n";
            else out << "Error: " << session.lastError() << "\n";
        },
        "Connect to device", {"port", "baud", "id"});

    root->Insert("disconnect",
        [&session](std::ostream& out) {
            session.disconnect();
            out << "Disconnected\n";
        },
        "Disconnect from device");

    root->Insert("info",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto info = session.client().readSystemInfo();
                out << "Protocol: " << info.protoMajor << "." << info.protoMinor << "\n"
                    << "Variant:  " << info.variantId << "\n"
                    << "Mode:     " << opModeName(info.activeOpMode) << "\n"
                    << "Uptime:   " << info.uptimeSec << " s\n"
                    << "Cap:      " << info.sysCapFlags
                    << (info.sysCapFlags & SysCap::CALIBRATION_MODE ? " [Cal]" : " [No Cal]") << "\n";
            });
        },
        "System info dump");

    // ---- Calibration submenu ----
    auto calMenu = std::make_unique<cli::Menu>("cal");

    calMenu->Insert("unlock",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                static int step = 0;
                uint16_t val = (step == 0) ? CAL_UNLOCK_STEP1 : CAL_UNLOCK_STEP2;
                if (session.client().unlockCalibrationStep(val)) {
                    out << "Unlock step " << (step + 1) << " OK (0x"
                        << std::hex << val << std::dec << ")\n";
                    step = (step + 1) % 2;
                } else {
                    out << "Error: " << session.lastError() << "\n";
                    step = 0;
                }
            });
        },
        "Send unlock step (alternates step 1/2)");

    calMenu->Insert("enter",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                if (session.client().enterCalibrationMode())
                    out << "Entered Calibration Mode\n";
                else
                    out << "Error: " << session.lastError() << "\n";
            });
        },
        "Enter Calibration Mode");

    calMenu->Insert("exit",
        [&session](std::ostream& out, const std::string& mode) {
            requireConnected(session, out, [&] {
                OpMode target = OpMode::Normal;
                if (mode == "auto" || mode == "automatic") target = OpMode::Automatic;
                session.stopWatch();
                if (session.client().exitCalibrationMode(target))
                    out << "Exited to " << opModeName(target) << "\n";
                else
                    out << "Error: " << session.lastError() << "\n";
            });
        },
        "Exit Calibration Mode", {"normal|auto"});

    calMenu->Insert("status",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                auto si = session.client().readSystemInfo();
                out << "Mode: " << opModeName(si.activeOpMode) << "\n";
                for (int ch = 0; ch < 2; ++ch) {
                    auto snap = session.client().readCalibrationSnapshot(ch);
                    out << "  CH" << ch
                        << " out=" << (snap.outputEnabled ? "ON" : "OFF")
                        << " dac=" << snap.rawDacCode
                        << " readback=" << snap.rawDacReadback
                        << " sample=" << calSampleStatusName(snap.sampleStatus)
                        << " adc_v=" << snap.rawAdcVoltage
                        << " adc_i=" << snap.rawAdcCurrent
                        << " limit=" << snap.maxRawDacLimit << "\n";
                }
            });
        },
        "Show calibration status");

    calMenu->Insert("safe",
        [&session](std::ostream& out) {
            requireConnected(session, out, [&] {
                for (int ch = 0; ch < 2; ++ch) {
                    session.client().writeRawDacCode(ch, 0);
                    session.client().writeCalibrationOutputEnable(ch, false);
                }
                session.stopWatch();
                out << "All calibration outputs disabled, DAC zeroed\n";
            });
        },
        "Disable all cal outputs and zero DAC");

    // ---- Channel submenu (nested inside cal) ----
    calMenu->Insert("ch",
        [&session](std::ostream& out, int ch) {
            if (ch < 0 || ch > 1) { out << "Error: channel must be 0 or 1\n"; return; }
            session.setActiveChannel(ch);
            out << "Active channel: " << ch << "\n";
        },
        "Select active channel", {"0|1"});

    calMenu->Insert("enable",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().writeCalibrationOutputEnable(ch, true))
                    out << "CH" << ch << " calibration output enabled\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Enable calibration output on active channel");

    calMenu->Insert("disable",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                session.client().writeRawDacCode(ch, 0);
                session.client().writeCalibrationOutputEnable(ch, false);
                out << "CH" << ch << " calibration output disabled, DAC zeroed\n";
            });
        },
        "Disable calibration output, zero DAC");

    calMenu->Insert("dac",
        [&session](std::ostream& out, int code) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (code < 0) { out << "Error: DAC code must be >= 0\n"; return; }
                if (session.client().writeRawDacCode(ch, static_cast<uint16_t>(code)))
                    out << "CH" << ch << " DAC = " << code << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Write raw DAC code", {"code"});

    calMenu->Insert("sample",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().sendCalibrationSampleCommand(ch))
                    out << "CH" << ch << " sample triggered\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Trigger raw ADC sample");

    calMenu->Insert("read",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto snap = session.client().readCalibrationSnapshot(ch);
                out << "CH" << ch << " snapshot:\n"
                    << "  Output:   " << (snap.outputEnabled ? "ON" : "OFF") << "\n"
                    << "  DAC code: " << snap.rawDacCode << "  readback: " << snap.rawDacReadback << "\n"
                    << "  Max limit:" << snap.maxRawDacLimit << "\n"
                    << "  Sample:   " << calSampleStatusName(snap.sampleStatus) << "\n"
                    << "  ADC V:    " << snap.rawAdcVoltage << "\n"
                    << "  ADC I:    " << snap.rawAdcCurrent << "\n";
            });
        },
        "Read calibration snapshot");

    calMenu->Insert("limit",
        [&session](std::ostream& out, int max) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().writeCalibrationMaxDacLimit(ch, static_cast<uint16_t>(max)))
                    out << "CH" << ch << " max DAC limit = " << max << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Set max raw DAC limit", {"max"});

    calMenu->Insert("commit",
        [&session](std::ostream& out) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                if (session.client().sendCalibrationCommitCommand(ch))
                    out << "CH" << ch << " coefficients committed\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Commit calibration coefficients for active channel");

    // Coefficient commands
    calMenu->Insert("coeff",
        [&session](std::ostream& out, const std::string& type, double k, double b) {
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto kRaw = static_cast<uint16_t>(k);
                auto bRaw = static_cast<int16_t>(b);
                bool ok = false;
                if (type == "out")
                    ok = session.client().writeCalibrationOutput(ch, kRaw, bRaw);
                else if (type == "meas-v")
                    ok = session.client().writeCalibrationMeasV(ch, kRaw, bRaw);
                else if (type == "meas-i")
                    ok = session.client().writeCalibrationMeasI(ch, kRaw, bRaw);
                else { out << "Error: type must be out|meas-v|meas-i\n"; return; }
                if (ok) out << "CH" << ch << " " << type << " K=" << kRaw << " B=" << bRaw << "\n";
                else out << "Error: " << session.lastError() << "\n";
            });
        },
        "Write calibration coefficients", {"out|meas-v|meas-i", "k", "b"});

    calMenu->Insert("coeff",
        [&session](std::ostream& out, const std::string& subcmd) {
            if (subcmd != "show") { out << "Usage: coeff show | coeff <type> <k> <b>\n"; return; }
            requireCalChannel(session, out, [&] {
                int ch = session.activeChannel();
                auto cfg = session.client().readChannelConfig(ch);
                out << "CH" << ch << " coefficients:\n"
                    << "  Output:  K=" << cfg.outCalK << " B=" << cfg.outCalB << "\n"
                    << "  Meas V:  K=" << cfg.measVCalK << " B=" << cfg.measVCalB << "\n"
                    << "  Meas I:  K=" << cfg.measICalK << " B=" << cfg.measICalB << "\n";
            });
        },
        "Show current coefficients", {"show"});

    // Watch commands
    calMenu->Insert("watch",
        [&session](std::ostream& out, const std::string& mode) {
            if (mode == "off") { session.stopWatch(); out << "Watch stopped\n"; return; }
            requireCalChannel(session, out, [&] {
                WatchMode wm = WatchMode::Off;
                if (mode == "adc") wm = WatchMode::Adc;
                else if (mode == "measure") wm = WatchMode::Measure;
                else if (mode == "status") wm = WatchMode::Status;
                else if (mode == "all") wm = WatchMode::All;
                else { out << "Error: mode must be adc|measure|status|all|off\n"; return; }
                session.startWatch(wm, 1000, out);
                out << "Watch " << mode << " started (1s)\n";
            });
        },
        "Start/stop periodic watch", {"adc|measure|status|all|off"});

    calMenu->Insert("watch",
        [&session](std::ostream& out, const std::string& mode, const std::string& interval) {
            if (mode == "off") { session.stopWatch(); out << "Watch stopped\n"; return; }
            requireCalChannel(session, out, [&] {
                WatchMode wm = WatchMode::Off;
                if (mode == "adc") wm = WatchMode::Adc;
                else if (mode == "measure") wm = WatchMode::Measure;
                else if (mode == "status") wm = WatchMode::Status;
                else if (mode == "all") wm = WatchMode::All;
                else { out << "Error: mode must be adc|measure|status|all|off\n"; return; }
                int ms = parseIntervalMs(interval);
                if (ms <= 0) { out << "Error: invalid interval\n"; return; }
                session.startWatch(wm, ms, out);
                out << "Watch " << mode << " started (" << interval << ")\n";
            });
        },
        "Start periodic watch with interval", {"adc|measure|status|all|off", "interval"});

    root->Insert(std::move(calMenu));
    return root;
}

} // namespace hvb::factory
```

- [ ] **Step 3: Update main.cpp to use buildRootMenu**

```cpp
#include "FactorySession.h"
#include "FactoryCommands.h"
#include <cli/cli.h>
#include <cli/loopscheduler.h>
#include <cli/clilocalsession.h>
#include <iostream>

int main() {
    hvb::factory::FactorySession session;
    auto rootMenu = hvb::factory::buildRootMenu(session);

    cli::Cli cli(std::move(rootMenu));
    cli::LoopScheduler sched;
    cli::CliLocalTerminalSession localSession(cli, sched, std::cout);
    localSession.ExitAction([&sched, &session](auto& out) {
        session.stopWatch();
        session.disconnect();
        out << "Goodbye.\n";
        sched.Stop();
    });
    sched.Run();
    return 0;
}
```

- [ ] **Step 4: Update CMakeLists.txt to include new sources**

```cmake
add_executable(hvb_factory_tui
    main.cpp
    FactorySession.cpp
    FactoryCommands.cpp
)
```

- [ ] **Step 5: Verify build**

```bash
cd tools && cmake --build build/linux-debug --target hvb_factory_tui 2>&1 | tail -10
```

- [ ] **Step 6: Commit**

```bash
git add tools/hvb_factory_tool/repl/
git commit -m "feat(factory): implement full REPL menu tree with calibration commands and watch modes"
```

---

## Phase 5: Shared QML Theme

### Task 13: Shared QML Theme Singleton

**Files:**
- Create: `tools/shared_qml/HvbTheme.qml`
- Create: `tools/shared_qml/qmldir`

- [ ] **Step 1: Write HvbTheme.qml**

```qml
pragma Singleton
import QtQuick

QtObject {
    enum Theme { Light, Dark }
    property int current: Theme.Dark

    readonly property color background:     current === Theme.Dark ? "#1e1e2e" : "#eff1f5"
    readonly property color surface:        current === Theme.Dark ? "#313244" : "#ccd0da"
    readonly property color surfaceAlt:     current === Theme.Dark ? "#45475a" : "#bcc0cc"
    readonly property color text:           current === Theme.Dark ? "#cdd6f4" : "#4c4f69"
    readonly property color textSubtle:     current === Theme.Dark ? "#a6adc8" : "#6c6f85"
    readonly property color primary:        current === Theme.Dark ? "#89b4fa" : "#1e66f5"
    readonly property color success:        current === Theme.Dark ? "#a6e3a1" : "#40a02b"
    readonly property color warning:        current === Theme.Dark ? "#f9e2af" : "#df8e1d"
    readonly property color error:          current === Theme.Dark ? "#f38ba8" : "#d20f39"
    readonly property color border:         current === Theme.Dark ? "#585b70" : "#9ca0b0"

    readonly property int fontSizeSmall:   11
    readonly property int fontSizeNormal:  13
    readonly property int fontSizeLarge:   16
    readonly property int fontSizeTitle:   20

    readonly property int spacingSmall:    4
    readonly property int spacingNormal:   8
    readonly property int spacingLarge:    16

    readonly property int radiusSmall:     4
    readonly property int radiusNormal:    8

    function toggle() {
        current = (current === Theme.Dark) ? Theme.Light : Theme.Dark
    }
}
```

- [ ] **Step 2: Write qmldir**

```
module HvbTheme
singleton HvbTheme 1.0 HvbTheme.qml
```

- [ ] **Step 3: Commit**

```bash
git add tools/shared_qml/
git commit -m "feat(shared): add HvbTheme.qml singleton with light/dark palettes"
```

---

## Phase 6: Factory GUI (Deferred — Structure Only)

### Task 14: Factory GUI Skeleton

**Files:**
- Create: `tools/hvb_factory_tool/gui/CMakeLists.txt`
- Create: `tools/hvb_factory_tool/gui/CalibrationBackend.h`
- Create: `tools/hvb_factory_tool/gui/CalibrationBackend.cpp`
- Create: `tools/hvb_factory_tool/gui/main.cpp`
- Create: `tools/hvb_factory_tool/gui/qml/MainWindow.qml`

This task creates the structural skeleton for the factory GUI. The full QML workflow steps (UnlockStep, EnterStep, ChannelControl, CoefficientsStep, CommitExitStep) are deferred to a follow-up plan since they require interactive Qt testing.

- [ ] **Step 1: Write CalibrationBackend.h**

```cpp
#pragma once

#include <QObject>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include "hvb_modbus_client.h"

namespace hvb::factory {

class CalibrationBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool calUnlocked READ calUnlocked NOTIFY calStateChanged)
    Q_PROPERTY(bool calActive READ calActive NOTIFY calStateChanged)
    Q_PROPERTY(int activeChannel READ activeChannel WRITE setActiveChannel NOTIFY activeChannelChanged)
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit CalibrationBackend(QObject* parent = nullptr);
    ~CalibrationBackend() override;

    bool connected() const;
    bool calUnlocked() const;
    bool calActive() const;
    int activeChannel() const;
    void setActiveChannel(int ch);
    QStringList ports() const;
    QString statusMessage() const;

public slots:
    void connectToDevice(const QString& port, int baud, int slaveId);
    void disconnectFromDevice();
    void scanPorts();
    void unlockStep1();
    void unlockStep2();
    void enterCalibrationMode();
    void exitCalibrationMode(const QString& targetMode);
    void enableOutput(bool enable);
    void writeRawDac(int code);
    void triggerSample();
    void commitChannel();
    void safeAll();
    void writeCoefficients(const QString& type, double k, double b);
    void refreshSnapshot();

signals:
    void connectedChanged();
    void calStateChanged();
    void activeChannelChanged();
    void portsChanged();
    void statusMessageChanged();
    void snapshotUpdated(QVariantMap snapshot);

private:
    HvbModbusClient m_client;
    bool m_calUnlocked = false;
    bool m_calActive = false;
    int m_activeChannel = 0;
    QStringList m_ports;
    QString m_statusMessage;
};

} // namespace hvb::factory
```

- [ ] **Step 2: Write CalibrationBackend.cpp (minimal implementation)**

```cpp
#include "CalibrationBackend.h"
#include "types.h"

namespace hvb::factory {

CalibrationBackend::CalibrationBackend(QObject* parent) : QObject(parent) {}
CalibrationBackend::~CalibrationBackend() = default;

bool CalibrationBackend::connected() const { return m_client.isConnected(); }
bool CalibrationBackend::calUnlocked() const { return m_calUnlocked; }
bool CalibrationBackend::calActive() const { return m_calActive; }
int CalibrationBackend::activeChannel() const { return m_activeChannel; }
QStringList CalibrationBackend::ports() const { return m_ports; }
QString CalibrationBackend::statusMessage() const { return m_statusMessage; }

void CalibrationBackend::setActiveChannel(int ch) {
    if (m_activeChannel != ch) { m_activeChannel = ch; emit activeChannelChanged(); }
}

void CalibrationBackend::scanPorts() {
    m_ports.clear();
    for (const auto& p : HvbModbusClient::scanPorts())
        m_ports.append(QString::fromStdString(p));
    emit portsChanged();
}

void CalibrationBackend::connectToDevice(const QString& port, int baud, int slaveId) {
    if (m_client.connect(port.toStdString(), baud, slaveId)) {
        m_statusMessage = "Connected";
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit connectedChanged();
    emit statusMessageChanged();
}

void CalibrationBackend::disconnectFromDevice() {
    m_client.disconnect();
    m_calUnlocked = false;
    m_calActive = false;
    emit connectedChanged();
    emit calStateChanged();
}

void CalibrationBackend::unlockStep1() {
    if (m_client.unlockCalibrationStep(CAL_UNLOCK_STEP1))
        m_statusMessage = "Unlock step 1 OK";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::unlockStep2() {
    if (m_client.unlockCalibrationStep(CAL_UNLOCK_STEP2)) {
        m_statusMessage = "Unlock step 2 OK";
        m_calUnlocked = true;
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::enterCalibrationMode() {
    if (m_client.enterCalibrationMode()) {
        m_calActive = true;
        m_statusMessage = "Calibration Mode active";
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::exitCalibrationMode(const QString& targetMode) {
    OpMode mode = (targetMode == "auto") ? OpMode::Automatic : OpMode::Normal;
    if (m_client.exitCalibrationMode(mode)) {
        m_calActive = false;
        m_calUnlocked = false;
        m_statusMessage = "Exited to " + targetMode;
        emit calStateChanged();
    } else {
        m_statusMessage = QString::fromStdString(m_client.lastError());
    }
    emit statusMessageChanged();
}

void CalibrationBackend::enableOutput(bool enable) {
    if (m_client.writeCalibrationOutputEnable(m_activeChannel, enable))
        m_statusMessage = enable ? "Output enabled" : "Output disabled";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::writeRawDac(int code) {
    if (m_client.writeRawDacCode(m_activeChannel, static_cast<uint16_t>(code)))
        m_statusMessage = "DAC = " + QString::number(code);
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::triggerSample() {
    if (m_client.sendCalibrationSampleCommand(m_activeChannel))
        m_statusMessage = "Sample triggered";
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::commitChannel() {
    if (m_client.sendCalibrationCommitCommand(m_activeChannel))
        m_statusMessage = "Committed CH" + QString::number(m_activeChannel);
    else
        m_statusMessage = QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::safeAll() {
    for (int ch = 0; ch < 2; ++ch) {
        m_client.writeRawDacCode(ch, 0);
        m_client.writeCalibrationOutputEnable(ch, false);
    }
    m_statusMessage = "All outputs safe";
    emit statusMessageChanged();
}

void CalibrationBackend::writeCoefficients(const QString& type, double k, double b) {
    auto kRaw = static_cast<uint16_t>(k);
    auto bRaw = static_cast<int16_t>(b);
    bool ok = false;
    if (type == "out") ok = m_client.writeCalibrationOutput(m_activeChannel, kRaw, bRaw);
    else if (type == "meas-v") ok = m_client.writeCalibrationMeasV(m_activeChannel, kRaw, bRaw);
    else if (type == "meas-i") ok = m_client.writeCalibrationMeasI(m_activeChannel, kRaw, bRaw);
    m_statusMessage = ok ? "Coefficients written" : QString::fromStdString(m_client.lastError());
    emit statusMessageChanged();
}

void CalibrationBackend::refreshSnapshot() {
    auto snap = m_client.readCalibrationSnapshot(m_activeChannel);
    QVariantMap map;
    map["outputEnabled"] = snap.outputEnabled;
    map["rawDacCode"] = snap.rawDacCode;
    map["maxRawDacLimit"] = snap.maxRawDacLimit;
    map["rawDacReadback"] = snap.rawDacReadback;
    map["sampleStatus"] = static_cast<int>(snap.sampleStatus);
    map["rawAdcVoltage"] = snap.rawAdcVoltage;
    map["rawAdcCurrent"] = snap.rawAdcCurrent;
    emit snapshotUpdated(map);
}

} // namespace hvb::factory
```

- [ ] **Step 3: Write factory GUI CMakeLists.txt**

```cmake
set(CMAKE_AUTOMOC ON)

qt_add_executable(hvb_factory_gui
    main.cpp
    CalibrationBackend.cpp CalibrationBackend.h
)

qt_add_qml_module(hvb_factory_gui
    URI HvbFactory
    NO_PLUGIN
    QML_FILES
        qml/MainWindow.qml
    SOURCES
        main.cpp
        CalibrationBackend.cpp CalibrationBackend.h
)

target_include_directories(hvb_factory_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../hvb_modbus_core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${MODBUSLIB_INCLUDE_DIR}
)

target_link_libraries(hvb_factory_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_features(hvb_factory_gui PRIVATE cxx_std_17)
target_compile_options(hvb_factory_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_factory_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 4: Write factory GUI main.cpp**

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include "CalibrationBackend.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    hvb::factory::CalibrationBackend backend;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.loadFromModule("HvbFactory", "MainWindow");

    return app.exec();
}
```

- [ ] **Step 5: Write minimal MainWindow.qml**

```qml
import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 800; height: 600
    visible: true
    title: "HVB Factory Calibration Tool"
    Material.theme: Material.Dark

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16

        Label {
            text: "HVB Factory Calibration Tool"
            font.pixelSize: 20
            font.bold: true
        }

        Label {
            text: backend.connected ? "Connected" : "Not connected"
            color: backend.connected ? "green" : "gray"
        }

        Label {
            text: backend.statusMessage
            font.italic: true
        }

        Item { Layout.fillHeight: true }

        Label {
            text: "Full GUI workflow steps are implemented in a follow-up plan."
            opacity: 0.5
        }
    }
}
```

- [ ] **Step 6: Commit**

```bash
git add tools/hvb_factory_tool/gui/ tools/shared_qml/
git commit -m "feat(factory): add factory GUI skeleton (CalibrationBackend + MainWindow)"
```

---

## Phase 7: CLI Shell Test Updates

### Task 15: Update CLI Shell Tests for New Binary Name

**Files:**
- Modify: `tools/hvb_modbus_core/tests/cli_crosscheck.sh` (if it exists after move)
- Modify: `tools/hvb_modbus_core/tests/cli_run_all.sh` (if it exists after move)

- [ ] **Step 1: Update binary name references**

In any shell test scripts, replace `hvbctrl` with `hvb_demo_cli` and update paths from `../bin/` to the new output location.

- [ ] **Step 2: Run full test suite**

```bash
cd tools && cmake --build build/linux-debug && build/linux-debug/hvb_modbus_core/tests/hvb_tests -v
```
Expected: All tests PASS

- [ ] **Step 3: Commit**

```bash
git add tools/
git commit -m "test: update shell tests for hvb_demo_cli binary name"
```

---

## Summary

| Phase | Tasks | Key Deliverables |
|-------|-------|-----------------|
| 1: Core v2.1 | 1-6 | CalibrationSampleStatus, CalibrationSnapshot, calibration client APIs, tests |
| 2: Restructure | 7 | `hvb_modbus_core/` + `hvb_demo_app/` + `hvb_factory_tool/` layout |
| 3: Demo cleanup | 8-10 | Remove cal writes from CLI/TUI, read-only cal display, new config path |
| 4: Factory REPL | 11-12 | Full Cisco-style REPL with unlock, raw DAC/ADC, coefficients, watch modes |
| 5: Shared QML | 13 | HvbTheme singleton with light/dark palettes |
| 6: Factory GUI | 14 | CalibrationBackend skeleton, MainWindow placeholder |
| 7: Test updates | 15 | Shell tests updated for new binary names |

**Out of scope (deferred to follow-up plans):**
- Full factory GUI QML workflow steps (UnlockStep, EnterStep, ChannelControl, CoefficientsStep, CommitExitStep)
- Windows deploy script update
- Migration/backward compat with old layout

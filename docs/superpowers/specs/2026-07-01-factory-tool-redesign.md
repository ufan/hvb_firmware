# Factory Tool Redesign — HVB Calibration & QA Tool

**Date:** 2026-07-01  
**Status:** Approved for implementation planning  
**Scope:** `tools/hvb_factory_tool/` — both `gui/` and `repl/`

---

## 1. Problem Statement

The existing factory tool (`hvb_factory_tool`) is a minimal proof-of-concept:

- The GUI (`MainWindow.qml`) is a flat manual panel with no workflow structure. There is no enforcement of step ordering, no guided calibration flow, and no output.
- The REPL provides low-level calibration commands but no automated sweep, no test suite, and no report generation.
- Three confirmed data-contract bugs exist at the C++/QML boundary (see §10).
- The tool provides no functional verification or stress testing after calibration, leaving boards unverified before shipment.

This redesign adds an automatic DAC-sweep calibration flow (with DMM-prompted reference values and linear regression), functional and stress testing, PDF report generation, and a structured GUI architecture that enforces the correct factory workflow order while allowing flexible testing.

---

## 2. Top-Level Architecture

### 2.1 Work Modes

The application has three top-level modes selectable from a persistent left sidebar. Each is independently accessible once a device is connected.

| Mode | Description | Step ordering |
|------|-------------|---------------|
| **Calibration** | Guided wizard for raw-DAC calibration workflow | Strict — forward progress gated on step completion |
| **Testing** | Functional test and stress test | Free-form — any order, any number of runs |
| **Report** | Metadata entry + PDF generation | Always reachable once any data exists |

During an active calibration session (cal mode unlocked on device), navigation to Testing is blocked. Testing operates in Normal or Automatic operating mode; entering Testing forces exit from cal mode if still active.

### 2.2 C++ Backend Split

| Class | Responsibility | Thread |
|-------|---------------|--------|
| `CalibrationBackend` | Connection, device info, cal session state, per-channel committed status, QSettings persistence | Main (Qt main thread) |
| `CalibrationWorker` | Auto DAC sweep loop: steps DAC, waits settlement, reads ADC snapshot, pauses for DMM input | Worker thread (QThread) |
| `TestEngine` | Functional test and stress test execution | Worker thread (QThread) |
| `ReportEngine` | PDF generation from `ReportData` struct | Called synchronously from main thread on demand |

`CalibrationBackend` is the singleton exposed to QML (via `QML_ELEMENT` + `qmlRegisterSingletonInstance` — replaces the deprecated `setContextProperty`). `CalibrationWorker` and `TestEngine` are owned by `CalibrationBackend`, moved to worker threads, and communicated with via queued signals.

### 2.3 QML File Structure

```
gui/qml/
  MainWindow.qml               # top-level: sidebar + StackLayout
  WorkModeSidebar.qml          # mode icons + connection status badge
  cal/
    CalibrationWorkspace.qml   # step indicator + StackLayout of pages
    CalConnectPage.qml         # step 1: connect & identify
    CalUnlockPage.qml          # step 2: unlock + enter cal mode
    CalChannelPage.qml         # step 3: per-channel sweep/manual + config
    CalCommitPage.qml          # step 4: commit & exit
  test/
    TestingWorkspace.qml       # tab: Functional | Stress
    FunctionalTestPage.qml
    StressTestPage.qml
  ReportPage.qml
  components/
    DeviceInfoCard.qml
    StepIndicator.qml
    SweepTable.qml
    FitResultRow.qml
    LiveChart.qml
    ResultsTable.qml
```

---

## 3. Calibration Workspace

### 3.1 Step Ordering

Navigation forward is gated on step completion (see completion criteria per step below). Navigation backward is always permitted and triggers roll-back (§3.6). The step indicator in `CalibrationWorkspace.qml` shows: `1 Connect → 2 Unlock → 3 Calibrate → 4 Commit`.

### 3.2 Step 1 — Connect & Identify

**Content:** Port combo (auto-populated on page load via `scanPorts()`), Baud Rate, Slave ID, Connect/Disconnect button. On successful connection: `DeviceInfoCard` showing protocol version, firmware version, variant ID, channel count, capability flags, board temperature.

**Completion criteria:** Connected + `protoMinor >= 1` (calibration mode supported, `SysCap::CALIBRATION_MODE` set).

**Roll-back target:** Disconnects device, clears all session data.

### 3.3 Step 2 — Unlock & Enter Cal Mode

**Content:** Single "Unlock + Enter Calibration Mode" button. Performs the two-step unlock sequence (write 0xCA1B → write 0xA11B → write operating mode 2) atomically via `CalibrationBackend::unlockAndEnter()`. Status badge: Locked / Unlocking / Cal Active / Error (with last error message).

**Completion criteria:** Device confirms operating mode = Calibration (2).

**Roll-back target:** If cal mode is active, calls `exitCalibrationMode()`. Status reset to Locked.

### 3.4 Step 3 — Per-Channel Calibration

One tab per channel detected (e.g. CH0, CH1). Channels are independent — CH0 can be Done while CH1 is Pending. "Next" is gated on all channels marked Done.

#### 3.4.1 Capability-Driven Axis Selection

Which calibration axes are required for a channel is determined entirely by the channel's capability flags (read via `readChannelInfo` at connect time). This is not user-configurable.

| Axis | Formula | Required capability |
|------|---------|---------------------|
| `out` (V → DAC) | `dac_code = V × K/10000 + B` | `CH_CAP_RAW_OUTPUT_DRIVE` |
| `meas-V` (ADC → V) | `V = adc_V × K/10000 + B` | `CH_CAP_VOLTAGE_MEASUREMENT` |
| `meas-I` (ADC → I) | `I = adc_I × K/10000 + B` | `CH_CAP_CURRENT_MEASUREMENT` |

A channel may require any subset — including none (skip calibration for that channel entirely), any single axis, any pair, or all three. The sweep UI shows only the DMM input columns relevant to the active axes: DMM-V is shown when `out` or `meas-V` is active; DMM-I is shown when `meas-I` is active.

Channels with no calibration capabilities are shown in the channel tab list as "No cal required — skip" and are automatically marked Done.

#### 3.4.2 Sweep Config (collapsible, persisted to QSettings)

| Parameter | Default | Description |
|-----------|---------|-------------|
| DAC min | 0 | Lowest DAC code in sweep |
| DAC max | 4000 | Highest DAC code in sweep |
| Step size | 800 | DAC increment per point |
| Settlement time | 200 ms | Wait after DAC write before sampling |
| Cooldown before read | 100 ms | Additional wait before reading ADC snapshot |

Number of sweep points is derived: `floor((max - min) / step) + 1`. Minimum 2 points enforced.

#### 3.4.3 Auto Sweep Mode (default)

1. Operator presses **Start Sweep**.
2. Backend enables cal output for the channel.
3. `CalibrationWorker` steps through DAC codes: write DAC → wait settlement ms → send sample command → wait cooldown ms → read snapshot → emit `sweepStepReady(step, total, dacCode, adcV, adcI)`.
4. UI displays the live sweep table (DAC code, ADC-V, ADC-I, DMM V input field, DMM I input field). Each row enables after its step completes.
5. Operator enters DMM readings into the input fields as each row populates. DMM fields are not required to proceed to the next step — operator can fill them after the sweep completes.
6. After all steps: **Compute Fit** button (or auto-compute if all DMM fields filled). Linear regression is computed per axis:
   - `out`: `(dac_code, dmm_V)` pairs → K, B such that `dac_code = dmm_V × K/10000 + B`
   - `meas-V`: `(adc_V, dmm_V)` pairs → K, B such that `dmm_V = adc_V × K/10000 + B`
   - `meas-I`: `(adc_I, dmm_I)` pairs → K, B such that `dmm_I = adc_I × K/10000 + B`
7. Fit results displayed: K, B (editable override), R² (read-only quality indicator). R² ≥ 0.999 is shown in green (good); 0.99–0.999 in orange (warning, operator should inspect points); < 0.99 in red (poor fit, re-run recommended). These thresholds are visual guides only — they do not block accepting the fit. Operator accepts each axis independently.
8. **Write & Verify**: writes accepted coefficients via `writeCalibrationOutput/MeasV/MeasI`, reads them back via `readChannelCalConfig`, confirms match.
9. **Disable cal output**: calls `writeCalibrationOutputEnable(ch, false)` and `writeRawDacCode(ch, 0)`. Required by firmware before commit (commit is rejected while output is enabled or DAC ≠ 0). Channel tab marked Done.

Sweep can be aborted at any time. Partial sweep data is retained; operator can re-run or switch to Manual mode.

#### 3.4.4 Manual Mode

Replaces the sweep table with an editable grid: (DAC code, DMM-V, DMM-I) entry rows. Operator adds/removes rows freely (minimum 2). **Compute Fit** triggers the same regression. Alternatively, each axis has a **Direct K/B override** row — operator types K and B directly, bypassing regression entirely (no R² shown). **Write & Verify** same as auto mode.

#### 3.4.5 Channel State

Each channel tab displays a status badge: `○ Pending` / `⟳ In Progress` / `● Done` / `↩ Rolled Back`.

### 3.5 Step 4 — Commit & Exit

**Content:** Per-channel summary table: channel, axis, old K (from NVS readback at session start), new K, old B, new B — changes highlighted. Per-channel **Commit CH*n*** button and **Commit All** button. After all commits: **Exit Calibration Mode** button (calls `exitCalibrationMode()`).

**Completion criteria:** All channels committed + cal mode exited. Leads to sidebar (operator navigates to Testing or Report).

**Roll-back target (from this step → step 3):** Calls `exitCalibrationMode()`, then `sendParamAction(ch, ParamAction::Load)` per channel (reloads NVS coefficients into RAM), clears all sweep data and channel Done states.

### 3.6 Roll-Back Summary

| From step | To step | Actions |
|-----------|---------|---------|
| 4 (Commit) | 3 (Calibrate) | Exit cal mode; param load per channel (NVS reload); clear sweep data and Done flags |
| 3 (Calibrate) | 2 (Unlock) | Disable cal output, zero DAC, exit cal mode; param load per channel; clear sweep data |
| 2 (Unlock) | 1 (Connect) | Exit cal mode if active; unlock state reset |
| 1 (Connect) | — | Disconnect |

---

## 4. Testing Workspace

Two peer panels accessed via a tab bar: **Functional Test** and **Stress Test**. No ordering between them. Device must be connected. Operating mode set to Normal before any test run (backend does this automatically).

### 4.1 Channel Selection

Both panels share the same channel selector pattern:

```
Channel: [CH0]  [CH1]  [All]
```

**All Channels** always means sequential: complete full test on CH0, then CH1, etc. (never simultaneous). This holds for both test types.

### 4.2 Functional Test

**Config (persisted to QSettings):**
- Test points: list of (target voltage in V, tolerance %). Editable rows, add/remove. Can be **Shared** (same points for all channels) or **Per-channel** (independent lists per channel).
- Settle time (ms): wait after output enable before reading measurement.
- Retries on fault: how many times to retry a faulted point before marking FAIL.

**Execution per point per channel:**
1. Set `CFG_TARGET_VOLTAGE` for channel.
2. Send `OutputAction::Enable`.
3. Wait settle time ms.
4. Read `ChannelInfo` (voltageRaw, currentRaw, status, activeFault).
5. Convert voltageRaw to V via calibration formula. Compare to target ± tolerance.
6. Send `OutputAction::DisableGraceful`.
7. Record result.

**Result columns:** `#`, `Ch`, `Target V`, `Tolerance`, `Result` (PASS/FAIL), `Measured V`, `Err%`.

**Overall verdict:** PASS only if all points on all channels pass. Last run's verdict is what the report uses. History log (collapsible) retains all previous runs.

### 4.3 Stress Test

**Config (persisted to QSettings):**
- Target voltage per channel (individual fields when All selected).
- Duration (seconds).
- Poll interval (ms).
- Fault tolerance: number of faults before abort (0 = stop on first).

**Execution (per channel when All selected, sequential):**
1. Set target voltage, enable output.
2. `TestEngine` polls `ChannelInfo` at poll interval for full duration.
3. Each poll: read voltageRaw, currentRaw, status, activeFault. Accumulate: min/max/sum for stats.
4. Fault detected → increment fault counter; if > tolerance, abort.
5. After duration (or abort): disable output, compute stats (avg, σ, min, max).

**Live display (during run):** scrolling voltage-vs-time chart (one line per channel for All mode, one line for single), current active channel indicator ("Running CH1 of 2…"), elapsed/total timer.

**Result:** per-channel stats table + overall PASS/FAIL verdict. PASS requires: zero faults (or within fault tolerance), voltage within ±tolerance% of target throughout duration.

---

## 5. Report Page

Always reachable from sidebar. Does not require calibration or testing to have been run — sections without data are marked "Not run — excluded from report."

**Metadata fields (operator-filled):**
- Board serial number
- Operator ID
- Free-text notes

**Report sections (auto-populated from session data):**
1. Cover: tool name, board serial, operator ID, timestamp, overall verdict (PASS / FAIL / PARTIAL)
2. Device Info: protocol version, firmware, variant, channels, capability flags
3. Calibration Results (per channel): old vs new K/B per axis, R², sweep point table, DMM readings
4. Functional Test: config, per-point result table, overall verdict
5. Stress Test: config, per-channel stats, fault events, overall verdict
6. Notes
7. Operator signature line

**Generate PDF:** opens file-save dialog. Uses `QPrinter` + `QTextDocument` (no external dependencies). Output path default from QSettings.

---

## 6. Configuration (QSettings)

Accessible via a ⚙ icon in the toolbar (settings dialog, not a dedicated page).

| Group | Keys |
|-------|------|
| `sweep/` | `dacMin`, `dacMax`, `stepSize`, `settlementMs`, `cooldownMs` (axes derived from channel caps, not stored) |
| `functest/` | `defaultTolerancePct`, `defaultSettleMs`, `defaultPoints` (JSON array) |
| `stress/` | `defaultDurationSec`, `defaultPollMs`, `defaultFaultTolerance` |
| `report/` | `outputDir`, `companyName`, `logoPath` |
| `connection/` | `defaultPort`, `defaultBaud`, `defaultSlaveId` |

---

## 7. REPL Parity

New commands added to `FactoryCommands.cpp` to mirror GUI capabilities:

| Command | Description |
|---------|-------------|
| `cal sweep <ch>` | Auto sweep with current config; pauses at each point for DMM input |
| `cal sweep <ch> --manual` | Guided manual point entry |
| `cal sweep <ch> --config` | Print current sweep config |
| `cal sweep <ch> --set <key> <val>` | Override a sweep config parameter |
| `cal fit <ch>` | Show computed K/B + R² from last sweep without writing |
| `test func <ch\|all> [v1 v2…] [--tol PCT] [--settle MS]` | Functional test |
| `test stress <ch\|all> [--dur S] [--poll MS] [--v VOLTS]` | Stress test |
| `report [path]` | Generate Markdown report to file |

Existing commands unchanged.

---

## 8. Bug Fixes (from QML Review)

These are fixed as part of the refactor, not deferred:

| ID | File | Bug | Fix |
|----|------|-----|-----|
| B-1 | `CalibrationBackend.cpp:145` | `rawDacReadback` key populated from `rawDacCode`. `CalibrationSnapshot` v3 has no `rawDacReadback` field. | Remove the key; update QML snapshot display to show only `outputEnabled`, `rawDacCode`, `rawAdcVoltage`, `rawAdcCurrent` |
| B-2 | `CalibrationBackend.cpp` (`refreshSnapshot`) | `maxRawDacLimit` key never set but QML reads it (shows "undefined"). Field is firmware-internal, not in `CalibrationSnapshot`. | Remove QML reference; display is removed in redesigned UI |
| B-3 | `CalibrationBackend.cpp:121` | `safeAll()` hardcodes 2 channels | Replace with `readSystemInfo().supportedChannels` |
| B-4 | `MainWindow.qml:36` | Status label `elide: Text.ElideRight` broken — preceding spacer `Item { Layout.fillWidth: true }` absorbs all space | Remove spacer; give label `Layout.fillWidth: true` and keep `Layout.maximumWidth: 300` |
| B-5 | `MainWindow.qml:76,115` | `enabled:` property appears after `Layout.fillWidth:` (attached property) — ORD-1 violation | Move `enabled:` before `Layout.fillWidth:` |
| B-6 | `main.cpp:13` | `setContextProperty()` deprecated in Qt6 | Register `CalibrationBackend` as `QML_ELEMENT` singleton; expose via `qmlRegisterSingletonInstance` |

---

## 9. Implementation Phases

This spec is large enough to warrant phased implementation plans:

| Phase | Scope |
|-------|-------|
| 1 | Bug fixes (§8) + backend refactor (CalibrationBackend, QML_ELEMENT, safeAll fix) |
| 2 | Calibration workspace: CalibrationWorker + 4 wizard pages + sweep UI |
| 3 | Testing workspace: TestEngine + FunctionalTestPage + StressTestPage |
| 4 | ReportEngine (PDF) + ReportPage + QSettings config dialog |
| 5 | REPL additions (§7) |

Each phase produces a working, committable state. Phases 2–5 depend on Phase 1 completing first. Phases 3–5 are independent of each other.

---

## 10. Out of Scope

- DMM SCPI auto-read (DMM values are always entered manually by operator)
- Parallel multi-channel test execution
- Network/remote operation
- Firmware flashing
- Cryptographic audit log or tamper-evident signing
- Factory handoff enforcement (deferred per calibration-mode PRD §Out of Scope)
- Calibration-complete enforcement before Normal/Automatic output enable

---

## 11. File Inventory (new / changed)

**New files:**

```
gui/
  CalibrationWorker.h/.cpp
  TestEngine.h/.cpp
  ReportEngine.h/.cpp
  ReportData.h
  SweepData.h
  TestResult.h
  qml/
    WorkModeSidebar.qml
    ReportPage.qml
    cal/
      CalibrationWorkspace.qml
      CalConnectPage.qml
      CalUnlockPage.qml
      CalChannelPage.qml
      CalCommitPage.qml
    test/
      TestingWorkspace.qml
      FunctionalTestPage.qml
      StressTestPage.qml
    components/
      DeviceInfoCard.qml
      StepIndicator.qml
      SweepTable.qml
      FitResultRow.qml
      LiveChart.qml
      ResultsTable.qml
```

**Changed files:**

```
gui/CalibrationBackend.h/.cpp   — refactored; bugs B-1 through B-6 fixed
gui/main.cpp                    — QML_ELEMENT singleton registration
gui/CMakeLists.txt              — new sources, new QML files, Qt6::PrintSupport
repl/FactoryCommands.cpp        — new commands (§7)
repl/FactoryCommands.h          — updated declaration
```

**Deleted files:**

```
gui/qml/MainWindow.qml          — replaced by new QML structure
```

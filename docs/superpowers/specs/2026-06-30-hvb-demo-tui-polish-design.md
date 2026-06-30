# HVB Demo TUI Polish — Design Spec

**Status**: Approved
**Date**: 2026-06-30

---

## 1. Overview

Polish and bugfix the `hvb_demo_tui` (tools/hvb_demo_app/tui/) host-side Modbus terminal UI. Two goals:

1. **Correct register metadata usage**: drive UI behavior from `modbus_view.def` poll categories (REALTIME, FIXED, CONFIG, COMMAND). Only poll REALTIME registers. CONFIG/COMMAND are write-then-readback, never polled.
2. **Inline editing in Monitor tab**: remove the per-channel editing panel; replace the read-only Menu table with a full interactive table where each row has clickable buttons and editable inputs directly in the cells.
3. **Bugfixes**: config values not displayed after connect; register values not synced between Monitor and Channel tabs.

---

## 2. Register Category-Driven Polling

### 2.1 Categories (from `include/reg_store/modbus_view.def`)

| Category | Meaning | When read | UI role |
|----------|---------|-----------|---------|
| REALTIME | Live measurements, status, faults | Every poll interval (~1 s) | Readonly columns (Vm, Im, Vt, Fault, Status label) |
| FIXED | Version, variant, capabilities, FW version | Connect only | System tab info, capability gate for widget visibility |
| CONFIG | RW operational parameters | Connect + after successful write | Input boxes (Vset, Ramp↑↓, I-limit), Channel tab fields |
| COMMAND | WO triggers (output action, fault cmd, param action) | Never polled | Write only |

### 2.2 Poll thread behavior

`doPollScan()` already reads only dynamic registers (system input offsets 6-14, channel input offsets 0-8). No change in the Modbus layer.

### 2.3 Config refresh on write

After a blocking CONFIG write succeeds, immediately read back the affected channel's config block to update `s.data.chCfg[ch]`, then sync UI strings:

```
writeSync → write → OK → readChannelConfig(ch) → syncDataToInputs()
                      → FAIL → statusMsg = error, Input string unchanged
```

### 2.4 No poll of fixed/config/cmd

The poll thread stays as-is. Config values in `s.data.chCfg[ch]` are stale between writes — this is correct and intended. Config does not change spontaneously on the hardware.

---

## 3. Interactive Monitor Table

### 3.1 Structure

Replace the current `Menu`-based table + per-channel editing panel with a `Container::Vertical` of per-channel `Container::Horizontal` rows. Each row contains real FTXUI widgets. The per-channel panel at the bottom is removed entirely.

16 rows are pre-built. Rows for `ch >= numChannels()` are empty/hidden.

### 3.2 Row layout

| # | Column | Widget type | Data source | Unit | Update trigger |
|---|--------|------------|-------------|------|----------------|
| 1 | CH# | `text` | channel index | — | static |
| 2 | Vm | `text` (readonly) | `ci.voltageRaw` → `fmtVoltage()` | V | poll (REALTIME) |
| 3 | Im | `text` (readonly) | `ci.currentRaw` → `fmtCurrentUA()` | nA | poll (REALTIME) |
| 4 | **Status** | **`Button`** (clickable toggle) | `ci.status` bits | — | poll (REALTIME, label only) |
| 5 | **Vset** | **`Input`** (editable, Enter=commit) | `cc.configuredTargetVRaw` | V | connect + write result |
| 6 | Vt | `text` (readonly) | `ci.operationalTargetVoltageRaw` | V | poll (REALTIME) |
| 7 | **Ramp↑** | **`Input`** (editable, Enter=commit) | `cc.rampUpStepRaw` | V/s | connect + write result |
| 8 | **Ramp↓** | **`Input`** (editable, Enter=commit) | `cc.rampDownStepRaw` | V/s | connect + write result |
| 9 | **I-limit** | **`Input`** (editable, Enter=commit) | `cc.iLimitThresholdRaw` | nA | connect + write result |
| 10 | Fault | `text` (readonly) | `ci.activeFault` → `faultStr()` | — | poll (REALTIME) |

### 3.3 Widget behavior

**Readonly columns** (Vm, Im, Vt, Fault): Plain `text()`. No state held — reads directly from `s.data` every render frame. Poll-driven updates cause no flicker; only the character grid changes.

**Status Button**: Renders as `[ ON ]` (green, bold) when `OUTPUT_DRIVE_NONZERO` bit is set; `[ OFF ]` (dim) otherwise. Click fires Enable or DisableImmediate via blocking `writeSync`.

**Editable Inputs** (Vset, Ramp↑, Ramp↓, I-limit): Show current device value from `ConfigInputs` struct. Click to focus, type, Enter commits. Write is blocking (~10 ms). On success, the Input string is reset to the just-read-back value. On error, the Input string stays as-typed and status bar shows the error.

**Capability gating**: Im (column 3) and I-limit (column 9) are hidden if the channel lacks `CH_CAP_CURRENT_MEASUREMENT`. Vm (column 2) hidden if no `CH_CAP_VOLTAGE_MEASUREMENT`. Status button hidden if no `CH_CAP_OUTPUT_ENABLE`.

### 3.4 No-flash guarantee

- Readonly columns: just `text()` from `s.data`. No component state to rebuild. Poll re-render is a simple grid refresh.
- Editable columns: pulled from `ConfigInputs` strings, synced only on connect and after write success. Poll never touches them.
- Status Button: stable FTXUI component. Only the label string changes, not the widget identity.

### 3.5 Focus navigation

- **Up/Down arrows**: move between rows.
- **Tab / Shift+Tab**: cycle through editable cells within the selected row (Status → Vset → Ramp↑ → Ramp↓ → I-limit).
- **Mouse click**: directly focuses the clicked widget.

---

## 4. Write Model — Synchronous Blocking

### 4.1 Current `writeAsync()` — removed

Background-thread writes via `writeAsync()` are replaced. All config and command writes become blocking calls from the UI thread.

### 4.2 New `writeSync()`

```cpp
void writeSync(AppState& s, ConfigInputs& inputs, const std::string& label,
               std::function<bool()> writeFn,
               std::function<void()> refreshFn);  // reads back config, syncs inputs
```

Flow:
1. Set `statusMsg = "Writing <label>..."`
2. Post `Event::Custom` (shows "Writing..." in status bar)
3. Acquire `scanMutex` (blocks poll thread for duration of write, ~7 ms)
4. Call `writeFn()` — the actual Modbus write
5. If OK: call `refreshFn()` to read back config into `s.data`, then `syncDataToInputs()`
6. Release `scanMutex`
7. Set `statusMsg = "OK: <label>"` or `"Error: ..."`
8. Post `Event::Custom` (refresh UI)

### 4.3 Impact on poll thread

While `writeSync` holds `scanMutex`, the poll thread blocks on acquiring it and skips one poll cycle. At 1 s poll interval and ~10 ms write duration, this is invisible.

### 4.4 Poll → config staleness

Poll never reads CONFIG registers. Between writes, `s.data.chCfg[ch].configuredTargetVRaw` and similar fields are stale — this is fine. The Input boxes hold the last-known value from connect or last write. If the device-side value changes externally, it will be out of sync until the next manual Refresh or write.

---

## 5. Shared State — ConfigInputs

### 5.1 Single struct for all tabs

A `ConfigInputs` struct is shared across Monitor, System, and Channel tabs. It holds the string/int representations of all CONFIG input fields:

```cpp
struct ConfigInputs {
    // Per-channel fields
    std::string targetV   [MAX_CHANNELS];  // Vset
    std::string ruStep    [MAX_CHANNELS];  // Ramp↑
    std::string rdStep    [MAX_CHANNELS];  // Ramp↓
    std::string iThr      [MAX_CHANNELS];  // I-limit

    // System fields
    std::string slaveAddr;
    int  opModeIdx;
    int  baudIdx;
    int  startupIdx;

    // Channel tab extra fields
    std::string ruInt     [MAX_CHANNELS];
    std::string rdInt     [MAX_CHANNELS];
    std::string derateStep[MAX_CHANNELS];
    int  iModeIdx         [MAX_CHANNELS];
    int  iActIdx          [MAX_CHANNELS];
    int  recovIdx         [MAX_CHANNELS];
    std::string retryDelay [MAX_CHANNELS];
    std::string retryMax   [MAX_CHANNELS];
    std::string retryWindow[MAX_CHANNELS];
    std::string iBand      [MAX_CHANNELS];
};
```

### 5.2 Sync function

```cpp
void syncDataToInputs(const ScannedData& data, ConfigInputs& inputs);
```

Called:
- After `doFullScan()` completes (connect time)
- After each `writeSync()` success
- After manual Refresh

This resolves both bugs:
- **Bug 1** (config not displayed after connect): `syncDataToInputs` populates all strings from `s.data`.
- **Bug 2** (values not synced between tabs): all tabs read from the same `ConfigInputs` instance.

### 5.3 Value conversion

| Field | Source | Conversion |
|-------|--------|------------|
| `targetV[ch]` | `cc.configuredTargetVRaw` | `voltageToV(raw)` → `"+X.X"` |
| `ruStep[ch]` | `cc.rampUpStepRaw` | `std::to_string(raw)` |
| `rdStep[ch]` | `cc.rampDownStepRaw` | `std::to_string(raw)` |
| `iThr[ch]` | `cc.iLimitThresholdRaw` | `currentToA(raw) * 1e9` → `"X.XXX"` |
| `slaveAddr` | `sc.slaveAddr` | `std::to_string(addr)` |
| `opModeIdx` | `sc.operatingMode` | cast to int index |
| Index fields | respective enums | cast to int index |

---

## 6. Dynamic Component Lifecycle

### 6.1 Connect

1. `doFullScan(data)` → reads `numChannels()`, per-channel `chCapFlags`
2. Resize `tabComponents` to `2 + numChannels()` (Monitor + System + CH0..CHn-1)
3. Build Monitor rows: `Container::Vertical` with exactly `numChannels()` row components, each gated on capabilities
4. Build Channel tabs: one `makeChannelTab(appState, ch)` per channel
5. Call `syncDataToInputs(data, inputs)` to populate all Inputs

### 6.2 Refresh

No component rebuild. `doFullScan(data)` + `syncDataToInputs(data, inputs)`. Widgets stay alive. `numChannels()` and `chCapFlags` are re-read but if channel count changed, the next disconnect/reconnect cycle handles it.

### 6.3 Disconnect

1. Empty `tabComponents` → resize to 2
2. Clear Monitor row containers
3. Clear per-channel tab entries
4. Reset `activeTab` to 0 if it referenced a removed channel tab

### 6.4 Next Connect

Clean start — new `doFullScan`, new widgets built from fresh hardware config.

---

## 7. Files Modified

| File | Changes |
|------|---------|
| `tui/widgets.h` | Add `ConfigInputs` struct, `writeSync()`, `syncDataToInputs()`. Remove `writeAsync()`. |
| `tui/tab_monitor.h` | Replace Menu + per-channel panel with full interactive table. Remove `St`, use `ConfigInputs`. |
| `tui/tab_system.h` | Remove `St`, use `ConfigInputs`. |
| `tui/tab_channel.h` | Remove `St`, use `ConfigInputs`. |
| `tui/main.cpp` | Wire `ConfigInputs` to all tabs. Call `syncDataToInputs()` after connect. Dynamic component lifecycle. |
| `tui/tui_format.h` | Add `fmtVoltageShort()`, `fmtCurrentShort()` compact formatters for table cells if needed. |

---

## 8. Non-Goals

- No changes to `hvb_modbus_client` or `hvb_modbus_core`. The client already has all necessary read/write methods.
- No changes to the poll thread architecture. Poll strategy stays as-is.
- No changes to the System tab layout (only wiring to shared `ConfigInputs`).
- No unit tests for TUI rendering (FTXUI is inherently visual; testing is via manual QA).

---

## 9. Verification

1. Connect to HVB board — verify all Monitor Input fields show correct device values
2. Edit Vset in Monitor tab, Enter — verify write succeeds, Input shows committed value
3. Switch to Channel tab — verify Vset Input shows same value as Monitor tab
4. Edit Vset in Channel tab — verify Monitor tab reflects it after switching back
5. Click Status button — verify output toggles, button label updates
6. Poll runs 30 s — verify no flicker or jitter in readonly columns
7. Disconnect — verify channel tabs removed, Monitor table cleared
8. Reconnect — verify widgets rebuilt with current board config

# HVB TUI Redesign

**Date:** 2026-06-16  
**Status:** Approved

## Context

The current TUI (`tui/main.cpp`) is a read-only skeleton: four tabs (Monitor/System/CH0/CH1) that display data but have no writable fields and no user interaction beyond connection and tab switching. The redesign adds full operator control — output enable/disable, target voltage, ramping, protection config, calibration, and persistence — while keeping the Monitor tab as the primary day-to-day view.

---

## Tab Structure

```
[Mon]  [Sys]  [CH0]  [CH1]
```

Four tabs, static. CH0 and CH1 are independent; both always present (UNSUPPORTED shown if the channel is inactive).

---

## Monitor Tab (primary view)

### System summary bar

Single line above the table, read-only, polled:

```
Proto: 2.0  Variant: 1  Uptime: 1234 s  Mode: Normal  Temp: 25.3 °C  Humid: 48 %  Fault: none
```

### Channel table (read-only display)

Columns: **CH | Vmeas | Imeas | Status | Ramp↑ | Ramp↓ | I-Prot | Target V | Fault**

- **Status**: composite badge — `ON`, `OFF`, `RAMP`, `FAULT`, `COOLDOWN` derived from `ChStatus` bits
- **Ramp↑ / Ramp↓**: step in LSB only (interval lives in the action panel)
- **I-Prot**: compact — `Disabled`, `FlagOnly`, or `Apply/<action>` (e.g., `Apply/Dis-Im`)
- **Target V**: read-only display of `configuredTargetVRaw` converted to volts
- **Fault**: decoded active fault causes (`VL`, `CL`, `MI`, `HW`, `IL`, `RE`, `CI`), `--` if none

### Keyboard navigation

- **↑ / ↓**: move selected channel row
- **Tab**: enter action panel / advance to next field inside panel
- **Shift-Tab**: previous field inside panel
- **Enter**: write the current input field value to the board; activate a button
- **Escape**: return focus from action panel back to table rows

### Action panel (appears below table, tracks selected row)

```
┌─ CH0 ─────────────────────────────────────────────────────────────────────────────┐
│  Target    : [ +500.0 ] V                                                         │
│  Ramp Up   : step [  10  ] LSB   interval [  10  ] ×0.1 s                        │
│  Ramp Down : step [   5  ] LSB   interval [  10  ] ×0.1 s                        │
│  V Limit   : [ Apply-Action ▾]  [ Clamp         ▾]  threshold [ +490.0 ] V       │
│  I Limit   : [ Disabled     ▾]  [ None          ▾]  threshold [  32767 ] uA      │
│                                                                                   │
│  [ Enable ]  [ Disable-Immediate ]  [ Disable-Graceful ]                         │
│  [ Clear Active Fault ]  [ Clear History ]                                        │
└───────────────────────────────────────────────────────────────────────────────────┘
```

**Fields (all writable on Enter):**

| Field | Unit | API call |
|---|---|---|
| Target V | volts → LSB (100 mV/LSB) | `writeConfiguredTargetVoltage` |
| Ramp Up step | LSB | `writeRampUp` |
| Ramp Up interval | ×0.1 s | `writeRampUp` (same call) |
| Ramp Down step | LSB | `writeRampDown` |
| Ramp Down interval | ×0.1 s | `writeRampDown` |
| V Limit mode | dropdown (Disabled / FlagOnly / Apply-Action) | `writeVoltageProtection` |
| V Limit action | dropdown (None / Dis-Graceful / Dis-Immediate / ForceZero / Clamp) | `writeVoltageProtection` |
| V Limit threshold | volts → LSB | `writeVoltageProtection` |
| I Limit mode | dropdown (Disabled / FlagOnly / Apply-Action) | `writeCurrentProtection` |
| I Limit action | dropdown (None / Dis-Graceful / Dis-Immediate / ForceZero) | `writeCurrentProtection` |
| I Limit threshold | µA → LSB (1 nA/LSB, so ×1000) | `writeCurrentProtection` |

**Buttons:**

| Button | API call |
|---|---|
| Enable | `sendOutputAction(ch, Enable)` |
| Disable-Immediate | `sendOutputAction(ch, DisableImmediate)` |
| Disable-Graceful | `sendOutputAction(ch, DisableGraceful)` |
| Clear Active Fault | `sendChannelFaultCommand(ch, ClearActiveFaultBlock)` |
| Clear History | `sendChannelFaultCommand(ch, ClearFaultHistory)` |

---

## System Tab

Two-column layout. Left panel is read-only `SystemInfo`; right panel has writable `SystemConfig` fields plus system-scope action buttons.

### Left — System Info (read-only)

Protocol, Variant ID, Cap Flags, Channels (count + mask), FW Version, Uptime, Board Temp (°C), Humidity (%RH), Active Op Mode, Sys Status (hex), Fault Cause (decoded or hex).

### Right — System Config (writable)

| Field | Widget | API call |
|---|---|---|
| Op Mode | dropdown (Normal / Automatic) | `writeOperatingMode` |
| Slave Addr | integer input 0–247 | `writeSlaveAddress` |
| Baud Rate | dropdown (115200 / 9600) | `writeBaudRateCode` |
| Recovery Policy | dropdown (ManualLatch / AutoRetry / AutoDerateRetry / NeverRetry) | `writeSystemRecoveryPolicy` |
| Retry Delay | integer input (s) | `writeSystemRecoveryPolicy` |
| Retry Max | integer input | `writeSystemRecoveryPolicy` |
| Retry Window | integer input (s) | `writeSystemRecoveryPolicy` |
| V Safe Band | integer input 0–50 (%) | `writeSafeBands` |
| I Safe Band | integer input 0–50 (%) | `writeSafeBands` |

**Buttons:** Save · Load · Factory Reset · Software Reset (`sendParamAction(-1, …)`)

---

## Channel Tab (CH0 / CH1)

### Live readings bar (top, read-only, polled)

```
Vmeas: +500.0 V   Imeas: +123.4 uA   Op Target: +500.0 V   Status: ON RAMP   Retries: 2
Active Fault: VL   Fault History: VL   Cooldown: 0 s   Last Fault: 1230 s ago
```

### Panels below (writable, Tab navigation)

**Output**
- Target V input (volts) + Enable / Disable-Immediate / Disable-Graceful buttons

**Ramping**
- Ramp Up: step (LSB) + interval (×0.1 s)
- Ramp Down: step (LSB) + interval (×0.1 s)
- Derate Step (LSB)

**Voltage Protection**
- Mode dropdown / Action dropdown / Threshold (volts)

**Current Protection**
- Mode dropdown / Action dropdown / Threshold (µA)

**Calibration**
- Output K (×10000) + B (×1000)
- Meas V K + B
- Meas I K + B

**Persistence**
- Save Target Policy toggle (Yes / No)
- Buttons: Save · Load · Factory Reset
- Buttons: Clear Active Fault · Clear History

---

## Architecture

The current `tui/main.cpp` (single 320-line file) is replaced by a split file structure:

```
tui/
├── main.cpp              — app entry, screen loop, poll thread, connection modal (unchanged design)
├── tab_monitor.h/.cpp    — Monitor tab: renderMonitorTable(), renderActionPanel()
├── tab_system.h/.cpp     — System tab: renderSystemInfo(), renderSystemConfig()
├── tab_channel.h/.cpp    — Channel tab: renderChannelLive(), all config panels
└── widgets.h             — shared: makeDropdown(), makeIntInput(), makeButton() helpers
```

`HvbModbusClient` is used directly — no new abstraction layer needed. All writes happen on a detached thread (same pattern as the connect flow) to avoid blocking the FTXUI event loop. A brief `statusMsg` string shown in the top bar confirms write success or error.

### Poll behaviour (unchanged)
Background thread polls every `g_pollInterval` seconds, posts `Event::Custom` to trigger re-render. Manual `r` key triggers an immediate poll. Writing a field does not wait for the next poll — the write result is reflected in `statusMsg`; data refreshes on the next poll cycle.

---

## Write interaction model

Every writable field follows the same pattern:
1. User edits field (text input or dropdown selection)
2. Press **Enter** to commit
3. Write dispatched on detached thread; `statusMsg` set to `"Writing…"`
4. On completion: `statusMsg` ← `"OK"` or error string; `screen.PostEvent(Event::Custom)` triggers re-render

Dropdowns cycle through valid enum values with **Space** or **←/→**; Enter commits.

---

## Out of scope

- Raw register debug view (already exists in CLI via `raw fc03/fc04/fc06`)
- Modbus frame log / traffic monitor
- Multi-board / multi-port support
- Windows TUI (build target exists but not tested in this redesign)

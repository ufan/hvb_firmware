# GUI Refactor Design — HVB Demo App

**Date:** 2026-07-01  
**Scope:** `tools/hvb_demo_app/gui/` — QML layer only. C++ backend (`modbus_backend.*`, `modbus_worker.*`) is unchanged except one default value.  
**Reference:** `tools/ui_scheam.yml` (layout spec), `tools/hvb_demo_app/tui/` (reference implementation)

---

## Strategy

Clean rewrite of all QML files. The existing tab structure (SystemInfo + SystemConfig tabs) is structurally incompatible with the schema (Monitor tab + per-channel tabs). Patching would leave tangled dead code; replacing is the same effort with a cleaner result.

The C++ backend already exposes all required data and write slots. The only backend change is the default poll interval: `m_pollInterval` 2000 → 1000 ms.

---

## Third-party / OSS

- **Qt Quick Controls 2 Material dark style** — built-in Qt 6.8.5, zero extra deps. Applied via `QT_QUICK_CONTROLS_STYLE=Material` + `QT_QUICK_CONTROLS_MATERIAL_THEME=Dark` in `main.cpp`, plus `Theme.qml` for token overrides.
- **QtCharts** (`ChartView` + `LineSeries` + `DateTimeAxis`) — built-in Qt 6.8.5. Used for rolling time-series graphs in each channel tab.
- No external table library. `Repeater + GridLayout` is sufficient for ≤16 interactive rows.

---

## File Structure

```
qml/
  Theme.qml               # Material dark token overrides + shared column-width constants
  main.qml                # Root ApplicationWindow
  ConnectionModal.qml     # Popup: Port / Baud / SlaveAddr / Poll / Connect
  SysConfigDialog.qml     # Popup: WorkingMode / StartupPolicy / Save/Load/Factory
  MonitorTab.qml          # Channel summary table
  ChannelTab.qml          # Per-channel: Live + Control + Protection + Recovery + Graphs
  RawDebugDialog.qml      # Unchanged
  components/
    BreathingIndicator.qml  # Animated status dot
    LabeledValue.qml        # Compact "Key: value" chip for status bar
    ChannelGraph.qml        # Reusable Qt Charts rolling time-series panel
    ReadOnlyField.qml       # Unchanged
    EditableField.qml       # Unchanged
    EnumCombo.qml           # Unchanged
    StatusBadge.qml         # Unchanged
```

Deleted: `SystemInfoTab.qml`, `SystemConfigTab.qml`, `ConnectionBar.qml`.

---

## Main Layout (`main.qml`)

```
ApplicationWindow (Material dark)
└── ColumnLayout
    ├── Menu bar (RowLayout)
    │     "HVB" bold
    │     BreathingIndicator
    │     SysMode toggle (Normal/Auto, connected-only)
    │     Uptime label (connected-only)
    │     ── filler ──
    │     Port@Baud#Slave label (dim when offline)
    │     [Connect / Disconnect] Button
    │     [Quit] Button
    ├── TabBar  — Monitor | CH0 | CH1 | … (dynamic after connect)
    ├── StackLayout (fillHeight)
    │     MonitorTab
    │     Repeater → ChannelTab × channelCount
    └── Status bar (RowLayout)
          [⚙ Config] → SysConfigDialog
          FW / Proto / Variant strip
          T / H strip
          ── filler ──
          [■ Debug] toggle → RawLogPanel visible
          Status/error label
└── RawLogPanel (ScrollView, collapsed by default, auto-scroll)
```

- `ConnectionModal` and `SysConfigDialog` are `Popup` overlays anchored to window center.
- Menu bar SysMode toggle writes `backend.writeOperatingMode` immediately.
- Connect button opens `ConnectionModal` when offline; calls `backend.disconnectFromDevice` when online.
- **Channel tabs lifecycle**: `Repeater { model: backend.channelCount }` — tabs are created after connect when `channelCount` is populated from the board capability register, and destroyed on disconnect when `channelCount` returns to 0. TabBar resets to Monitor tab on disconnect.
- **Monitor columns lifecycle**: `activeColumns` computed array is populated after connect from the union of channel capability flags, and cleared to `[]` on disconnect. Both header and data rows are `Repeater`-driven from this array.

---

## Monitor Tab (`MonitorTab.qml`)

### Dynamic column model

The column set is **not fixed**. After connect, a JS computed property `activeColumns` is derived from the union of all channel `chCapFlags` values read from the board capability register. `activeColumns` is an ordered array of column-descriptor objects:

```js
// always present
{ key: "ch",    label: "CH",       width: Theme.colCh,    cap: 0 }
// present if any channel has CH_CAP_OUTPUT_ENABLE (0x0001)
{ key: "vset",  label: "Vset (V)", width: Theme.colVset,  cap: 0x0001 }
{ key: "status",label: "Status",   width: Theme.colStatus, cap: 0x0001 }
// always present (operational target is always meaningful)
{ key: "vop",   label: "Vop (V)",  width: Theme.colVop,   cap: 0 }
// present if any channel has CH_CAP_VOLTAGE_MEASUREMENT (0x0004)
{ key: "v",     label: "V (V)",    width: Theme.colV,     cap: 0x0004 }
// present if any channel has CH_CAP_CURRENT_MEASUREMENT (0x0008)
{ key: "i",     label: "I (nA)",   width: Theme.colI,     cap: 0x0008 }
// present if any channel has CH_CAP_OUTPUT_ENABLE
{ key: "ru",    label: "Ru",       width: Theme.colRamp,  cap: 0x0001 }
{ key: "rd",    label: "Rd",       width: Theme.colRamp,  cap: 0x0001 }
// present if any channel has CH_CAP_CURRENT_MEASUREMENT
{ key: "limit", label: "Limit(nA)",width: Theme.colLimit, cap: 0x0008 }
// always present
{ key: "fault", label: "Fault",    width: Theme.colFault, cap: 0 }
```

`activeColumns` is recomputed whenever `backend.channelDataChanged` fires and `backend.channelCount > 0`. It is set to `[]` on disconnect (which clears both the header and all rows).

### Rendering

- **Header row**: `Repeater { model: activeColumns }` → one `Label` per entry, width from `col.width`
- **Data rows**: outer `Repeater { model: backend.channelCount }`, inner `Repeater { model: activeColumns }` → each cell rendered by a `switch(col.key)` delegate
- Individual cells show `--` (dim label) when the specific channel's own `chCapFlags` lacks the column's `cap` (handles mixed-capability boards where not all channels share the same flags)
- Offline / `activeColumns` empty: centred "Not connected — click Connect" message

### Cell behaviours

| key | Type | Write call |
|-----|------|------------|
| vset | TextField (V) | `backend.writeTargetVoltage(ch, rawFromV(v))` |
| status | Button (ON/RAMP/OFF) | toggle Enable / DisableGraceful |
| vop | read-only | `channelInfoList[i].operationalTargetV` |
| v | read-only | `channelInfoList[i].voltageV` |
| i | read-only | `channelInfoList[i].currentRaw` |
| ru | TextField (V/s) | `backend.writeRampUp(ch, rawFromV(v), existingInterval)` |
| rd | TextField (V/s) | `backend.writeRampDown(ch, rawFromV(v), existingInterval)` |
| limit | TextField (nA) | `backend.writeCurrentProtection(ch, existingMode, existingAction, nA)` |
| fault | read-only label | active fault code, red when non-zero |

---

## Channel Tab (`ChannelTab.qml`)

```
ScrollView
└── ColumnLayout
    ├── Live panel (RowLayout, always visible)
    │     Vset input | Vop | V | I | StatusBadge | Retries
    ├── Control + Protection (RowLayout)
    │   ├── Control GroupBox
    │   │     [Enable] [Disable] [Kill]   (hidden: no CH_CAP_OUTPUT_ENABLE)
    │   │     Ru: TextField V/s
    │   │     Rd: TextField V/s
    │   └── Protection GroupBox           (hidden: no CH_CAP_CURRENT_MEASUREMENT)
    │         I-Limit: TextField nA
    │         Mode: EnumCombo
    │         Action: EnumCombo
    │         Safe Band: SpinBox %
    │         [Clear Active Fault] [Clear Fault History]
    ├── Recovery + Persistence (RowLayout)
    │   ├── Recovery GroupBox
    │   │     Policy: EnumCombo
    │   │     Delay / Max / Window SpinBoxes
    │   │     Derate step TextField
    │   └── Persistence GroupBox
    │         [Save] [Load] [Factory Reset]
    └── Graphs (RowLayout, equal width)
        ├── ChannelGraph "Voltage"
        │     series: Vset (blue) / Vop (cyan) / V meas (green)
        │     Y-axis: V   X-axis: time (rolling)
        └── ChannelGraph "Current"      (hidden: no CH_CAP_CURRENT_MEASUREMENT)
              series: I meas (orange) / I-limit (red dashed)
              Y-axis: nA   X-axis: time (rolling)
```

Both graphs share the same `windowMinutes` property at the `ChannelTab` level.

---

## ChannelGraph Component (`components/ChannelGraph.qml`)

**Props:**
- `title: string`
- `seriesConfigs: array` — `[{ name, color, unit }]`
- `windowMinutes: int` — bound from parent ChannelTab (default 5)
- `channelIndex: int`

**Internals:**
- `ChartView` + one `LineSeries` per config entry + `DateTimeAxis` (X) + `ValueAxis` Y (auto-range over visible series)
- Per-series `CheckBox` in a header row above the chart
- JS ring-buffer arrays accumulate `{ t: Date.now(), v: value }` entries
- `Connections { target: backend; function onChannelDataChanged() { appendSample(); trimOld(); } }`
- `trimOld()` removes entries where `Date.now() - t > windowMinutes * 60000`
- Window combobox options: 1 min / 5 min / 10 min / 30 min; default 5 min

---

## ConnectionModal (`ConnectionModal.qml`)

Popup fields: Port (editable ComboBox ← `backend.ports`), Baud (ComboBox: 9600/115200), Slave (SpinBox 0–247), Poll (ComboBox: 0.5s/1s/2s/5s/10s, default 1s).

On open: `backend.scanPorts()`. On Connect: sets all backend properties then `backend.connectToDevice()`. Closes on success, Cancel, or Escape.

---

## SysConfigDialog (`SysConfigDialog.qml`)

Popup fields: Working Mode (ComboBox: Normal/Automatic → `backend.writeOperatingMode` on change), Startup Policy (ComboBox → `backend.writeStartupChannelPolicy` on change), plus Save / Load / Factory Reset buttons. Close button / Escape dismisses.

---

## BreathingIndicator (`components/BreathingIndicator.qml`)

`Rectangle` (12×12, radius 6) with `SequentialAnimation` on `opacity`:
- **Connected:** green, 1 s cosine breathe (opacity 0.3 → 1.0 → 0.3)
- **Connecting:** yellow, fast blink (400 ms)
- **Offline:** grey, static opacity 0.4

---

## Threading and Communication Model

### Register classes

All Modbus I/O is divided into three distinct classes that must not be mixed in the same polling cycle:

| Class | Registers | Trigger | Frequency |
|-------|-----------|---------|-----------|
| **Realtime** | ch status, voltage, current, op-target, fault flags, retry count, cooldown; sys uptime, activeOpMode, sysStatus, faultCause, temp, humidity, activeChMask | Automatic periodic poll | Every poll interval (default 1 s) |
| **Config** | sys config (opMode, startup policy, baud, slave), ch config (ramp, protection, recovery, cal), ch cap flags | On connect (full scan) and after each successful write that modifies config | On demand |
| **Command** | output action, fault command, param action (save/load/factory/reset) | User action | On demand |

Realtime registers are the only ones polled automatically. Config registers are read once on connect and refreshed after writes. Calibration registers are part of the config class.

### Worker thread

`ModbusWorker` runs in a dedicated `QThread`. **All** Modbus serial I/O executes on this thread. The main (GUI) thread never calls blocking I/O.

```
Main thread                     Worker thread (ModbusWorker)
─────────────────               ──────────────────────────────
QTimer (poll)   ──invoke──►  doPollStatus()     # realtime regs only
User write cmd  ──invoke──►  doWriteXxx()       # blocking, emits operationComplete
Connect         ──invoke──►  doConnect()
                               └─► doFullScan() # config + realtime regs, one-time
```

`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` is used for all cross-thread calls, so the GUI thread is never blocked.

### Poll separation

The existing `doRefreshSystemInfo` / `doRefreshChannelInfo` slots (which read all registers including config) are **not** called by the periodic poll timer. They are kept for explicit on-demand refreshes only.

A new `doPollStatus()` slot is added to `ModbusWorker` that reads **only** realtime registers:
- `readSystemStatus()` — uptime, mode, sys status, fault cause, temp, humidity, activeChMask
- `readChannelStatus(ch, capFlags)` — per active channel: voltage, current, op-target, status, fault flags, retries, cooldown

This mirrors `doPollScan` in the TUI reference implementation.

### Write / command serialisation

Write and command operations are queued to the worker thread via `QueuedConnection`. They execute sequentially; the poll timer's next tick is deferred until any in-flight write completes (Qt event loop serialises queued invocations on the worker thread).

### Result notification

Every write and command operation emits `operationComplete(bool ok, QString msg)`. The backend's `onOperationComplete` slot:
1. Sets `m_statusMessage` to `"✓ <op>" ` (green) or `"✗ <op>: <error>"` (red)
2. Starts a 4-second auto-clear timer (success only — errors persist until next operation)
3. Emits `statusMessageChanged`

The QML `ErrorBox` in the status bar binds to `backend.statusMessage` and colours it green/red based on the `✓`/`✗` prefix. This gives the user explicit, always-visible confirmation of every write result without a modal dialog.

---

## Implementation Notes

### Unit conversion for voltage inputs
`writeTargetVoltage`, `writeRampUp`, and `writeRampDown` all take raw LSB integers. The backend's `channelInfoList` already exposes calibrated float values (`voltageV`, `operationalTargetV`) so there is a known LSB→V factor in play. Rather than adding new backend slots, add a small JS helper in `Theme.qml` (or a shared `Utils.js`) that converts V → raw LSB using the same linear factor the TUI uses (`reg::voltageFromV`): `raw = Math.round(v * 3276.7)` (16-bit full-scale at 10 V → 32767 LSB, adjust if variant differs). The plan will confirm the exact factor from the modbus core headers.

### Monitor tab Limit field
`writeCurrentProtection(ch, mode, action, thresholdRaw)` must not clobber existing mode/action when the user only edits the threshold. The row delegate reads `backend.channelConfigList[index].iProtMode` and `iProtOutputAction` before calling the slot.

---

## Backend Changes

The C++ backend requires targeted changes — the QML layer cannot be rewritten cleanly without these:

1. **`modbus_backend.h`** — `int m_pollInterval = 1000;` (was 2000)
2. **`modbus_backend.cpp`** — `pollTick()` invokes `doWorker->doPollStatus()` only, not `doRefreshSystemInfo` / `doRefreshChannelInfo`
3. **`modbus_backend.cpp`** — `onOperationComplete()` adds auto-clear timer for success messages (4 s)
4. **`modbus_worker.h`** — add `doPollStatus()` slot (realtime registers only)
5. **`modbus_worker.cpp`** — implement `doPollStatus()`: calls `readSystemStatus()` then per-active-channel `readChannelStatus()`; emits `systemInfoReady` + `channelInfoReady` signals as before

---

## Deleted Files

- `qml/ConnectionBar.qml`
- `qml/SystemInfoTab.qml`
- `qml/SystemConfigTab.qml`

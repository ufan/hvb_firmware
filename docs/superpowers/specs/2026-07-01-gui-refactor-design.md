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

---

## Monitor Tab (`MonitorTab.qml`)

Table header + `Repeater` over `backend.channelCount`, both using `GridLayout` with 10 identical column widths defined in `Theme.qml`.

| Column | Type | Behavior |
|--------|------|----------|
| CH | label | CH0…CH15 |
| Vset (V) | TextField | Enter → `backend.writeTargetVoltage` |
| Status | Button | ON / RAMP / OFF → toggle Enable / DisableGraceful |
| Vop (V) | read-only | `channelInfoList[i].operationalTargetV` |
| V (V) | read-only | `channelInfoList[i].voltageV` |
| I (nA) | read-only | `channelInfoList[i].currentRaw` |
| Ru | TextField | Enter → `backend.writeRampUp` (V/s) |
| Rd | TextField | Enter → `backend.writeRampDown` (V/s) |
| Limit (nA) | TextField | Enter → `backend.writeCurrentProtection` threshold |
| Fault | read-only | active fault code, red when non-zero |

- Columns show `--` when capability flag absent (`CH_CAP_OUTPUT_ENABLE` / `CH_CAP_CURRENT_MEASUREMENT`).
- Offline: centred "Not connected — click Connect" message replaces table.

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

## Implementation Notes

### Unit conversion for voltage inputs
`writeTargetVoltage`, `writeRampUp`, and `writeRampDown` all take raw LSB integers. The backend's `channelInfoList` already exposes calibrated float values (`voltageV`, `operationalTargetV`) so there is a known LSB→V factor in play. Rather than adding new backend slots, add a small JS helper in `Theme.qml` (or a shared `Utils.js`) that converts V → raw LSB using the same linear factor the TUI uses (`reg::voltageFromV`): `raw = Math.round(v * 3276.7)` (16-bit full-scale at 10 V → 32767 LSB, adjust if variant differs). The plan will confirm the exact factor from the modbus core headers.

### Monitor tab Limit field
`writeCurrentProtection(ch, mode, action, thresholdRaw)` must not clobber existing mode/action when the user only edits the threshold. The row delegate reads `backend.channelConfigList[index].iProtMode` and `iProtOutputAction` before calling the slot.

---

## Backend Change

`modbus_backend.h` line ~148: `int m_pollInterval = 1000;` (was 2000).

---

## Deleted Files

- `qml/ConnectionBar.qml`
- `qml/SystemInfoTab.qml`
- `qml/SystemConfigTab.qml`

# GUI Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the `tools/hvb_demo_app/gui/` QML layer to match `tools/ui_scheam.yml` — menu bar + Monitor tab (dynamic columns) + per-channel tabs (live panels + time-series graphs) + Material dark theme — while separating realtime poll I/O from config/command I/O in the backend.

**Architecture:** Clean QML rewrite; C++ backend gets targeted changes only (new `doPollStatus` worker slot, poll-only timer, auto-clear status notifications). `ModbusWorker` runs all serial I/O on its own `QThread`; GUI thread is never blocked. `activeColumns` in MonitorTab is computed from board capability flags on connect and cleared on disconnect.

**Tech Stack:** Qt 6.8.5, Qt Quick Controls 2 (Material dark), QtCharts (`ChartView` + `LineSeries`), `Repeater + GridLayout` for table, C++17.

---

## File Map

**Modified (backend):**
- `tools/hvb_demo_app/gui/modbus_worker.h` — add `doPollStatus()` slot, cached `SystemInfo`/`ChannelInfo` members
- `tools/hvb_demo_app/gui/modbus_worker.cpp` — implement `doPollStatus()`; update existing reads to populate cache
- `tools/hvb_demo_app/gui/modbus_backend.h` — add `m_statusClearTimer`
- `tools/hvb_demo_app/gui/modbus_backend.cpp` — change `pollTick()`, change `onOperationComplete()`, change default poll interval
- `tools/hvb_demo_app/gui/CMakeLists.txt` — add `Qt6::Charts`, singleton Theme, new QML files, remove deleted
- `tools/hvb_demo_app/gui/main.cpp` — Material dark style

**New (QML):**
- `qml/Theme.qml` — singleton: column widths, colours, conversion helpers
- `qml/ConnectionModal.qml` — Port/Baud/SlaveAddr/Poll connect popup
- `qml/SysConfigDialog.qml` — WorkingMode/StartupPolicy/Save/Load/Factory popup
- `qml/MonitorTab.qml` — dynamic channel table
- `qml/ChannelTab.qml` — per-channel: live + control + protection + recovery + graphs
- `qml/main.qml` — root window
- `qml/components/BreathingIndicator.qml` — animated status dot
- `qml/components/LabeledValue.qml` — compact key:value chip
- `qml/components/ChannelGraph.qml` — rolling QtCharts time-series panel

**Deleted (QML):**
- `qml/ConnectionBar.qml`
- `qml/SystemInfoTab.qml`
- `qml/SystemConfigTab.qml`

**Unchanged:**
- `qml/RawDebugDialog.qml`
- `qml/components/ReadOnlyField.qml`
- `qml/components/EditableField.qml`
- `qml/components/EnumCombo.qml`
- `qml/components/StatusBadge.qml`
- `modbus_worker.h/.cpp` signals and all `doXxx` write slots (untouched)

---

## Task 1: ModbusWorker — add realtime-only poll slot

**Files:**
- Modify: `tools/hvb_demo_app/gui/modbus_worker.h`
- Modify: `tools/hvb_demo_app/gui/modbus_worker.cpp`

- [ ] **Step 1.1 — Add cache members and doPollStatus declaration to header**

In `modbus_worker.h`, add inside `private:`:
```cpp
// Cached structs for realtime poll — populated by doRefreshSystemInfo/doRefreshChannelInfo
static constexpr int WORKER_MAX_CH = 16;
hvb::SystemInfo  m_cachedSysInfo{};
hvb::ChannelInfo m_cachedChInfo[WORKER_MAX_CH]{};
int m_channelCount = 0;
```

Add to `public slots:` (after `doScanPorts`):
```cpp
void doPollStatus();   // realtime registers only — called by poll timer
```

- [ ] **Step 1.2 — Implement doPollStatus**

Add at the end of `modbus_worker.cpp`:
```cpp
void ModbusWorker::doPollStatus()
{
    if (!m_client.isConnected() || m_channelCount == 0) return;

    m_client.readSystemStatus(m_cachedSysInfo);
    if (!m_client.isConnected()) return;

    uint16_t activeMask = m_cachedSysInfo.activeChMask;
    for (int ch = 0; ch < m_channelCount; ++ch) {
        if ((activeMask & (1u << ch)) == 0) continue;
        m_client.readChannelStatus(ch, m_cachedChInfo[ch].chCapFlags, m_cachedChInfo[ch]);
        if (!m_client.isConnected()) return;
    }

    emit systemInfoReady(systemInfoToMap(m_cachedSysInfo));
    for (int ch = 0; ch < m_channelCount; ++ch) {
        if ((activeMask & (1u << ch)) != 0)
            emit channelInfoReady(ch, channelInfoToMap(ch, m_cachedChInfo[ch]));
    }
}
```

- [ ] **Step 1.3 — Update doRefreshSystemInfo to populate cache**

Replace the body of `ModbusWorker::doRefreshSystemInfo()`:
```cpp
void ModbusWorker::doRefreshSystemInfo()
{
    m_cachedSysInfo = m_client.readSystemInfo();
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    int n = m_cachedSysInfo.supportedChannels;
    m_channelCount = (n >= 1 && n <= WORKER_MAX_CH) ? n : 0;
    emit systemInfoReady(systemInfoToMap(m_cachedSysInfo));
}
```

- [ ] **Step 1.4 — Update doRefreshChannelInfo to populate cache**

Replace the body of `ModbusWorker::doRefreshChannelInfo(int ch)`:
```cpp
void ModbusWorker::doRefreshChannelInfo(int ch)
{
    if (ch < 0 || ch >= WORKER_MAX_CH) return;
    m_cachedChInfo[ch] = m_client.readChannelInfo(ch);
    if (!m_client.isConnected()) { emit operationComplete(false, "Read failed"); return; }
    emit channelInfoReady(ch, channelInfoToMap(ch, m_cachedChInfo[ch]));
}
```

- [ ] **Step 1.5 — Reset channel count on disconnect**

Replace the body of `ModbusWorker::doDisconnect()`:
```cpp
void ModbusWorker::doDisconnect()
{
    m_channelCount = 0;
    m_client.disconnect();
    emit disconnected();
}
```

- [ ] **Step 1.6 — Verify it builds**

```bash
cd /home/yong/backup/src/xlab/jianwei/hvb_wkspc/hvb_firmware.git
cmake --build build/gui 2>&1 | tail -20
# If no build dir yet: cmake -S tools/hvb_demo_app/gui -B build/gui -DCMAKE_PREFIX_PATH=~/backup/Qt/6.8.5/gcc_64
```
Expected: compiles without errors.

- [ ] **Step 1.7 — Commit**

```bash
git add tools/hvb_demo_app/gui/modbus_worker.h tools/hvb_demo_app/gui/modbus_worker.cpp
git commit -m "feat(gui): add doPollStatus — realtime registers only, no config/cal"
```

---

## Task 2: ModbusBackend — poll separation and write notification

**Files:**
- Modify: `tools/hvb_demo_app/gui/modbus_backend.h`
- Modify: `tools/hvb_demo_app/gui/modbus_backend.cpp`

- [ ] **Step 2.1 — Add status clear timer to header**

In `modbus_backend.h` inside `private:`, add after `QTimer m_pollTimer;`:
```cpp
QTimer m_statusClearTimer;
```

Change default poll interval:
```cpp
int m_pollInterval = 1000;   // was 2000
```

- [ ] **Step 2.2 — Wire status clear timer in constructor**

In `modbus_backend.cpp`, inside `ModbusBackend::ModbusBackend(...)` constructor, after the poll timer setup block:
```cpp
// Status auto-clear for successful operations
m_statusClearTimer.setSingleShot(true);
connect(&m_statusClearTimer, &QTimer::timeout,
        this, [this] { setStatus(QString()); });
```

- [ ] **Step 2.3 — Change pollTick to use doPollStatus only**

Replace `ModbusBackend::pollTick()`:
```cpp
void ModbusBackend::pollTick()
{
    if (!m_connected) return;
    QMetaObject::invokeMethod(m_worker, "doPollStatus", Qt::QueuedConnection);
}
```

- [ ] **Step 2.4 — Change onOperationComplete to prefix and auto-clear**

Replace `ModbusBackend::onOperationComplete(bool ok, const QString& msg)`:
```cpp
void ModbusBackend::onOperationComplete(bool ok, const QString& msg)
{
    if (ok) {
        setStatus(QString("✓ %1").arg(msg));
        m_statusClearTimer.start(4000);
    } else {
        m_statusClearTimer.stop();
        setStatus(QString("✗ %1").arg(msg));
    }
}
```

- [ ] **Step 2.5 — Verify build**

```bash
cmake --build build/gui 2>&1 | tail -20
```
Expected: compiles without errors.

- [ ] **Step 2.6 — Commit**

```bash
git add tools/hvb_demo_app/gui/modbus_backend.h tools/hvb_demo_app/gui/modbus_backend.cpp
git commit -m "feat(gui): poll separation — timer uses doPollStatus only; write results prefixed ✓/✗"
```

---

## Task 3: Build infrastructure — CMakeLists + Material style

**Files:**
- Modify: `tools/hvb_demo_app/gui/CMakeLists.txt`
- Modify: `tools/hvb_demo_app/gui/main.cpp`

- [ ] **Step 3.1 — Update CMakeLists.txt**

Replace the entire `tools/hvb_demo_app/gui/CMakeLists.txt`:
```cmake
set(CMAKE_AUTOMOC ON)

# Theme.qml is a QML singleton
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(hvb_demo_gui
    main.cpp
    modbus_backend.cpp modbus_backend.h
    modbus_worker.cpp  modbus_worker.h
)

qt_add_qml_module(hvb_demo_gui
    URI HvbTool
    NO_PLUGIN
    QML_FILES
        qml/Theme.qml
        qml/main.qml
        qml/ConnectionModal.qml
        qml/SysConfigDialog.qml
        qml/MonitorTab.qml
        qml/ChannelTab.qml
        qml/RawDebugDialog.qml
        qml/components/BreathingIndicator.qml
        qml/components/LabeledValue.qml
        qml/components/ChannelGraph.qml
        qml/components/ReadOnlyField.qml
        qml/components/EditableField.qml
        qml/components/EnumCombo.qml
        qml/components/StatusBadge.qml
    SOURCES
        main.cpp
        modbus_backend.cpp modbus_backend.h
        modbus_worker.cpp  modbus_worker.h
)

target_link_libraries(hvb_demo_gui PRIVATE
    hvb_modbus_core
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Charts
)

target_compile_features(hvb_demo_gui PRIVATE cxx_std_17)
target_compile_options(hvb_demo_gui PRIVATE -Wall -Wextra)

set_target_properties(hvb_demo_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
```

- [ ] **Step 3.2 — Set Material dark style in main.cpp**

Replace `tools/hvb_demo_app/gui/main.cpp`:
```cpp
#include "modbus_backend.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("HVB Modbus Tool");
    app.setOrganizationName("jianwei");

    QQuickStyle::setStyle("Material");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Cyan");

    QQmlApplicationEngine engine;

    ModbusBackend backend;
    engine.rootContext()->setContextProperty("backend", &backend);

    const QUrl url("qrc:/HvbTool/qml/main.qml");
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
```

- [ ] **Step 3.3 — Delete old QML files**

```bash
git rm tools/hvb_demo_app/gui/qml/ConnectionBar.qml \
       tools/hvb_demo_app/gui/qml/SystemInfoTab.qml \
       tools/hvb_demo_app/gui/qml/SystemConfigTab.qml
```

- [ ] **Step 3.4 — Verify CMake reconfigures cleanly**

```bash
cmake -S tools/hvb_demo_app/gui -B build/gui \
      -DCMAKE_PREFIX_PATH=~/backup/Qt/6.8.5/gcc_64 2>&1 | tail -10
```
Expected: no errors about missing files or missing Qt modules.

- [ ] **Step 3.5 — Commit**

```bash
git add tools/hvb_demo_app/gui/CMakeLists.txt tools/hvb_demo_app/gui/main.cpp
git commit -m "build(gui): add QtCharts, Material dark style, Theme singleton, new QML file list"
```

---

## Task 4: Theme.qml — singleton with constants and conversion helpers

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/Theme.qml`

- [ ] **Step 4.1 — Write Theme.qml**

```qml
// tools/hvb_demo_app/gui/qml/Theme.qml
pragma Singleton
import QtQuick

QtObject {
    // Monitor table column widths (px)
    readonly property int colCh:     50
    readonly property int colVset:   80
    readonly property int colStatus: 90
    readonly property int colVop:    80
    readonly property int colV:      80
    readonly property int colI:      90
    readonly property int colRamp:   72
    readonly property int colLimit:  90
    readonly property int colFault:  110

    // Status colours
    readonly property color colorOk:      "#4CAF50"
    readonly property color colorError:   "#F44336"
    readonly property color colorWarn:    "#FFC107"
    readonly property color colorOffline: "#555555"
    readonly property color colorCyan:    "#00BCD4"

    // Voltage: 1 LSB = 0.1 V  (register_map.h scale::VOLTAGE_LSB_TO_V)
    function voltageFromV(v)   { return Math.round(v / 0.1) }
    function voltageToV(raw)   { return raw * 0.1 }

    // Current: 1 LSB = 1 nA   (register_map.h scale::CURRENT_LSB_TO_A = 1e-9)
    // raw = nA directly — no conversion needed for nA display
    function currentNaFromA(a) { return Math.round(a * 1e9) }
    function currentAFromNa(na){ return na * 1e-9 }

    // Format helpers
    function fmtV(raw)  { return (raw * 0.1).toFixed(1) + " V" }
    function fmtNa(raw) { return raw + " nA" }
}
```

- [ ] **Step 4.2 — Verify build (Theme singleton registered)**

```bash
cmake --build build/gui 2>&1 | grep -E "error:|warning:|Theme" | head -20
```
Expected: no errors; Theme.qml compiled as singleton.

- [ ] **Step 4.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/Theme.qml
git commit -m "feat(gui): Theme singleton — column widths, colours, V/nA conversion helpers"
```

---

## Task 5: BreathingIndicator.qml and LabeledValue.qml

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/components/BreathingIndicator.qml`
- Create: `tools/hvb_demo_app/gui/qml/components/LabeledValue.qml`

- [ ] **Step 5.1 — Write BreathingIndicator.qml**

```qml
// tools/hvb_demo_app/gui/qml/components/BreathingIndicator.qml
import QtQuick

Rectangle {
    id: root
    width: 12; height: 12; radius: 6

    // connected: bool, connecting: bool — set by parent
    required property bool connected
    required property bool connecting

    color: connected  ? "#4CAF50"
         : connecting ? "#FFC107"
         : "#555555"

    // Baseline — animations override when running
    opacity: (root.connected || root.connecting) ? 1.0 : 0.4

    // Breathing animation when connected
    SequentialAnimation on opacity {
        running: root.connected
        loops: Animation.Infinite
        NumberAnimation { to: 0.25; duration: 900; easing.type: Easing.InOutSine }
        NumberAnimation { to: 1.0;  duration: 900; easing.type: Easing.InOutSine }
    }

    // Fast blink when connecting
    SequentialAnimation on opacity {
        running: root.connecting && !root.connected
        loops: Animation.Infinite
        NumberAnimation { to: 0.2; duration: 200 }
        NumberAnimation { to: 1.0; duration: 200 }
    }
}
```

- [ ] **Step 5.2 — Write LabeledValue.qml**

```qml
// tools/hvb_demo_app/gui/qml/components/LabeledValue.qml
import QtQuick
import QtQuick.Controls

Row {
    spacing: 2
    required property string label
    required property string value

    Label {
        text: root.label + ":"
        opacity: 0.6
        font.pixelSize: 11
    }
    Label {
        text: root.value
        font.pixelSize: 11
        font.bold: true
    }

    id: root
}
```

- [ ] **Step 5.3 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```
Expected: no errors.

- [ ] **Step 5.4 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/components/BreathingIndicator.qml \
        tools/hvb_demo_app/gui/qml/components/LabeledValue.qml
git commit -m "feat(gui): BreathingIndicator and LabeledValue components"
```

---

## Task 6: ConnectionModal.qml

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/ConnectionModal.qml`

- [ ] **Step 6.1 — Write ConnectionModal.qml**

```qml
// tools/hvb_demo_app/gui/qml/ConnectionModal.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    modal: true
    anchors.centerIn: parent
    padding: 16
    closePolicy: Popup.CloseOnEscape

    onOpened: backend.scanPorts()

    ColumnLayout {
        spacing: 12
        width: 280

        Label {
            text: "Connection Settings"
            font.bold: true
            font.pixelSize: 14
            Layout.alignment: Qt.AlignHCenter
        }

        GridLayout {
            columns: 2
            rowSpacing: 8
            columnSpacing: 8

            Label { text: "Port" }
            ComboBox {
                id: portCombo
                Layout.fillWidth: true
                model: backend.ports
                editable: true
                onCurrentTextChanged: backend.selectedPort = currentText
                Component.onCompleted: {
                    if (backend.selectedPort)
                        currentIndex = find(backend.selectedPort)
                }
            }

            Label { text: "Baud" }
            ComboBox {
                id: baudCombo
                Layout.fillWidth: true
                model: ["9600", "115200"]
                currentIndex: backend.baudRate === 115200 ? 1 : 0
                onActivated: backend.baudRate = parseInt(currentText)
            }

            Label { text: "Slave ID" }
            SpinBox {
                id: slaveSpin
                from: 0; to: 247
                value: backend.slaveId
                onValueModified: backend.slaveId = value
            }

            Label { text: "Poll" }
            ComboBox {
                id: pollCombo
                model: ["0.5 s", "1 s", "2 s", "5 s", "10 s"]
                currentIndex: 1   // default 1 s
                onActivated: {
                    var ms = [500, 1000, 2000, 5000, 10000]
                    backend.setPollIntervalMs(ms[currentIndex])
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button {
                text: "Connect"
                highlighted: true
                enabled: portCombo.currentText.length > 0
                onClicked: {
                    backend.connectToDevice()
                    root.close()
                }
            }

            Button {
                text: "Cancel"
                onClicked: root.close()
            }
        }
    }
}
```

- [ ] **Step 6.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 6.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/ConnectionModal.qml
git commit -m "feat(gui): ConnectionModal — Port/Baud/SlaveID/Poll popup"
```

---

## Task 7: SysConfigDialog.qml

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/SysConfigDialog.qml`

- [ ] **Step 7.1 — Write SysConfigDialog.qml**

```qml
// tools/hvb_demo_app/gui/qml/SysConfigDialog.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    modal: true
    anchors.centerIn: parent
    padding: 16
    closePolicy: Popup.CloseOnEscape

    ColumnLayout {
        spacing: 12
        width: 260

        Label {
            text: "System Config"
            font.bold: true
            font.pixelSize: 14
            Layout.alignment: Qt.AlignHCenter
        }

        GridLayout {
            columns: 2
            rowSpacing: 8
            columnSpacing: 8

            Label { text: "Working Mode" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Normal", "Automatic"]
                currentIndex: backend.sysConfig.operatingMode || 0
                onActivated: backend.writeOperatingMode(currentIndex)
            }

            Label { text: "Startup Policy" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Load NVS Config", "Factory Default"]
                currentIndex: backend.sysConfig.startupChannelPolicy || 0
                onActivated: backend.writeStartupChannelPolicy(currentIndex)
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8

            Button { text: "Save";    onClicked: backend.saveSystem() }
            Button { text: "Load";    onClicked: backend.loadSystem() }
            Button {
                text: "Factory"
                Material.background: Material.Red
                onClicked: backend.factoryResetSystem()
            }
        }

        Button {
            text: "Close"
            Layout.alignment: Qt.AlignHCenter
            onClicked: root.close()
        }
    }
}
```

- [ ] **Step 7.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 7.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/SysConfigDialog.qml
git commit -m "feat(gui): SysConfigDialog — WorkingMode/StartupPolicy/Save/Load/Factory popup"
```

---

## Task 8: ChannelGraph.qml — rolling QtCharts time-series panel

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/components/ChannelGraph.qml`

- [ ] **Step 8.1 — Write ChannelGraph.qml**

```qml
// tools/hvb_demo_app/gui/qml/components/ChannelGraph.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCharts

ColumnLayout {
    id: root
    spacing: 4

    required property int    channelIndex
    required property string title
    // seriesConfigs: [{name: string, color: string, valueKey: string}]
    required property var    seriesConfigs
    property int  windowMinutes: 5

    // Internal ring buffers: array of arrays of {t: ms, v: number}
    property var _buffers: []
    // Checkbox visible states parallel to seriesConfigs
    property var _visible: []

    Component.onCompleted: {
        for (var i = 0; i < seriesConfigs.length; i++) {
            _buffers.push([])
            _visible.push(true)
        }
        _createSeries()
    }

    function _createSeries() {
        for (var i = 0; i < seriesConfigs.length; i++) {
            var cfg = seriesConfigs[i]
            var s = chartView.createSeries(ChartView.SeriesTypeLine, cfg.name, axisX, axisY)
            s.color = cfg.color
            s.width = 1.5
            s.useOpenGL = false   // OpenGL can conflict with software renderer
        }
    }

    function _appendAndTrim(now) {
        var cutoff = now - root.windowMinutes * 60000
        var chList = backend.channelInfoList
        var cfList = backend.channelConfigList
        if (root.channelIndex >= chList.length) return
        var chInfo = chList[root.channelIndex]
        var chCfg  = cfList.length > root.channelIndex ? cfList[root.channelIndex] : {}

        var yMin = Infinity, yMax = -Infinity

        for (var i = 0; i < root.seriesConfigs.length; i++) {
            var cfg = root.seriesConfigs[i]
            // source: "config" reads from channelConfigList; default reads channelInfoList
            var src = (cfg.source === "config") ? chCfg : chInfo
            var v   = src[cfg.valueKey] !== undefined ? src[cfg.valueKey] : 0
            var buf = root._buffers[i]

            buf.push({ t: now, v: v })
            while (buf.length > 0 && buf[0].t < cutoff)
                buf.shift()

            if (chartView.count <= i) continue
            var series = chartView.series(i)
            series.clear()
            for (var j = 0; j < buf.length; j++)
                series.append(buf[j].t, buf[j].v)

            if (!root._visible[i]) continue
            for (var j = 0; j < buf.length; j++) {
                if (buf[j].v < yMin) yMin = buf[j].v
                if (buf[j].v > yMax) yMax = buf[j].v
            }
        }

        // Update axes
        axisX.max = new Date(now)
        axisX.min = new Date(cutoff)

        if (yMin === Infinity) { yMin = 0; yMax = 1 }
        else if (yMin === yMax) { yMin -= 1; yMax += 1 }
        var margin = (yMax - yMin) * 0.1 || 0.5
        axisY.min = yMin - margin
        axisY.max = yMax + margin
    }

    function _rebuildFromBuffers() {
        var now = Date.now()
        var cutoff = now - root.windowMinutes * 60000
        for (var i = 0; i < root._buffers.length; i++) {
            var buf = root._buffers[i]
            // trim old
            while (buf.length > 0 && buf[0].t < cutoff) buf.shift()
            if (chartView.count <= i) continue
            var series = chartView.series(i)
            series.clear()
            for (var j = 0; j < buf.length; j++)
                series.append(buf[j].t, buf[j].v)
        }
        axisX.max = new Date(now)
        axisX.min = new Date(cutoff)
    }

    // Header row
    RowLayout {
        Label { text: root.title; font.bold: true }

        Repeater {
            model: root.seriesConfigs.length
            CheckBox {
                text: root.seriesConfigs[index].name
                checked: true
                onCheckedChanged: {
                    root._visible[index] = checked
                    if (chartView.count > index)
                        chartView.series(index).visible = checked
                }
                contentItem: Label {
                    leftPadding: parent.indicator.width + parent.spacing
                    text: parent.text
                    font: parent.font
                    color: root.seriesConfigs[index].color
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Item { Layout.fillWidth: true }

        Label { text: "Window:" }
        ComboBox {
            model: ["1 min", "5 min", "10 min", "30 min"]
            currentIndex: 1
            implicitWidth: 90
            onActivated: {
                var mins = [1, 5, 10, 30]
                root.windowMinutes = mins[currentIndex]
                root._rebuildFromBuffers()
            }
        }
    }

    // Chart
    ChartView {
        id: chartView
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 160
        antialiasing: true
        theme: ChartView.ChartThemeDark
        legend.visible: false
        margins.left: 0; margins.right: 0; margins.top: 4; margins.bottom: 0

        DateTimeAxis {
            id: axisX
            format: "hh:mm:ss"
            tickCount: 5
            min: new Date(Date.now() - root.windowMinutes * 60000)
            max: new Date(Date.now())
        }

        ValueAxis {
            id: axisY
            min: 0
            max: 1
        }
    }

    Connections {
        target: backend
        function onChannelDataChanged() {
            if (!backend.connected) return
            root._appendAndTrim(Date.now())
        }
        function onConnectedChanged() {
            if (!backend.connected) {
                for (var i = 0; i < root._buffers.length; i++) root._buffers[i] = []
                for (var i = 0; chartView.count > i; i++) chartView.series(i).clear()
            }
        }
    }
}
```

- [ ] **Step 8.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 8.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/components/ChannelGraph.qml
git commit -m "feat(gui): ChannelGraph — rolling QtCharts time-series with per-series checkbox and window selector"
```

---

## Task 9: MonitorTab.qml — dynamic channel table

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/MonitorTab.qml`

- [ ] **Step 9.1 — Write MonitorTab.qml**

```qml
// tools/hvb_demo_app/gui/qml/MonitorTab.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HvbTool

Item {
    id: root

    // Descriptor array computed from board capability flags.
    // Rebuilt on channelDataChanged; cleared on disconnect.
    property var activeColumns: []

    function computeActiveColumns() {
        if (!backend.connected || backend.channelCount === 0) {
            activeColumns = []
            return
        }
        var chList = backend.channelInfoList
        var hasOutEn = false, hasVolt = false, hasCurr = false
        for (var i = 0; i < chList.length; i++) {
            var caps = chList[i].chCapFlags || 0
            if (caps & 0x0001) hasOutEn = true
            if (caps & 0x0004) hasVolt  = true
            if (caps & 0x0008) hasCurr  = true
        }
        var cols = []
        cols.push({ key: "ch",     label: "CH",         width: Theme.colCh })
        if (hasOutEn) cols.push({ key: "vset",   label: "Vset (V)",  width: Theme.colVset })
        if (hasOutEn) cols.push({ key: "status", label: "Status",    width: Theme.colStatus })
        cols.push(    { key: "vop",    label: "Vop (V)",    width: Theme.colVop })
        if (hasVolt)  cols.push({ key: "v",      label: "V (V)",     width: Theme.colV })
        if (hasCurr)  cols.push({ key: "i",      label: "I (nA)",    width: Theme.colI })
        if (hasOutEn) cols.push({ key: "ru",     label: "Ru (V)",    width: Theme.colRamp })
        if (hasOutEn) cols.push({ key: "rd",     label: "Rd (V)",    width: Theme.colRamp })
        if (hasCurr)  cols.push({ key: "limit",  label: "Lim(nA)",   width: Theme.colLimit })
        cols.push(    { key: "fault",  label: "Fault",      width: Theme.colFault })
        activeColumns = cols
    }

    Connections {
        target: backend
        function onChannelDataChanged() { root.computeActiveColumns() }
        function onConnectedChanged()   { if (!backend.connected) root.activeColumns = [] }
    }

    // ── Not connected placeholder ───────────────────────────────────────────
    Label {
        anchors.centerIn: parent
        text: "Not connected — click Connect in the toolbar"
        visible: !backend.connected
        opacity: 0.5
        font.pixelSize: 14
    }

    // ── Table ───────────────────────────────────────────────────────────────
    ScrollView {
        anchors.fill: parent
        visible: backend.connected && root.activeColumns.length > 0
        clip: true

        ColumnLayout {
            spacing: 0

            // Header row
            Row {
                id: headerRow
                height: 32
                Repeater {
                    model: root.activeColumns
                    Label {
                        width: modelData.width
                        height: 32
                        text: modelData.label
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Rectangle { height: 1; width: headerRow.width; color: "#444" }

            // Data rows
            Repeater {
                id: rowRepeater
                model: backend.channelCount

                Column {
                    property int chIdx: index
                    property var ci: (backend.channelInfoList  || [])[chIdx] || {}
                    property var cc: (backend.channelConfigList|| [])[chIdx] || {}
                    property int caps: ci.chCapFlags || 0

                    Row {
                        height: 36

                        Repeater {
                            model: root.activeColumns

                            Item {
                                width: modelData.width
                                height: 36

                                // CH label
                                Label {
                                    anchors.centerIn: parent
                                    text: "CH" + chIdx
                                    visible: modelData.key === "ch"
                                    font.bold: true
                                }

                                // Vset input
                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "vset" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: ci.operationalTargetV !== undefined
                                        ? ci.operationalTargetV.toFixed(1) : ""
                                    onAccepted: backend.writeTargetVoltage(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0))
                                }

                                // Status button
                                Button {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "status" && (caps & 0x0001) !== 0
                                    text: {
                                        if (!ci.status) return "OFF"
                                        if (ci.statusRamping) return "RAMP"
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0) ? "ON" : "OFF"
                                    }
                                    Material.background: {
                                        if (ci.statusRamping) return Material.Amber
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0)
                                            ? Material.Green : Material.Grey
                                    }
                                    implicitWidth: Theme.colStatus - 4
                                    onClicked: {
                                        if (ci.statusRamping) return
                                        var on = ci.statusOutDrive && ci.operationalTargetV !== 0
                                        backend.sendOutputAction(chIdx, on ? 2 : 1)  // 2=DisableGraceful 1=Enable
                                    }
                                }

                                // Vop read-only
                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "vop"
                                    text: ci.operationalTargetV !== undefined
                                        ? ci.operationalTargetV.toFixed(1) : "--"
                                }

                                // V read-only
                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "v"
                                    text: ci.voltageV !== undefined ? ci.voltageV.toFixed(1) : "--"
                                }

                                // I read-only
                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "i"
                                    text: ci.currentRaw !== undefined ? ci.currentRaw + "" : "--"
                                }

                                // Ramp Up input
                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "ru" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: cc.rampUpStepRaw !== undefined
                                        ? Theme.voltageToV(cc.rampUpStepRaw).toFixed(1) : ""
                                    onAccepted: backend.writeRampUp(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0),
                                        cc.rampUpInterval || 1)
                                }

                                // Ramp Down input
                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "rd" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: cc.rampDownStepRaw !== undefined
                                        ? Theme.voltageToV(cc.rampDownStepRaw).toFixed(1) : ""
                                    onAccepted: backend.writeRampDown(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0),
                                        cc.rampDownInterval || 1)
                                }

                                // I Limit input — raw = nA directly
                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "limit" && (caps & 0x0008) !== 0
                                    placeholderText: "nA"
                                    text: cc.iLimitThresholdRaw !== undefined
                                        ? cc.iLimitThresholdRaw + "" : ""
                                    onAccepted: backend.writeCurrentProtection(chIdx,
                                        cc.iProtMode || 0,
                                        cc.iProtOutputAction || 0,
                                        parseInt(text) || 0)
                                }

                                // Fault label
                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "fault"
                                    text: {
                                        var f = ci.activeFault || 0
                                        return f ? "0x" + f.toString(16).toUpperCase() : "—"
                                    }
                                    color: (ci.activeFault || 0) ? "#F44336" : "#aaaaaa"
                                }

                                // "--" placeholder for capability-absent cell
                                Label {
                                    anchors.centerIn: parent
                                    text: "--"
                                    opacity: 0.3
                                    visible: {
                                        if (modelData.key === "vset"  || modelData.key === "status" ||
                                            modelData.key === "ru"    || modelData.key === "rd")
                                            return (caps & 0x0001) === 0
                                        if (modelData.key === "i" || modelData.key === "limit")
                                            return (caps & 0x0008) === 0
                                        return false
                                    }
                                }
                            }
                        }
                    }

                    Rectangle { height: 1; width: headerRow.width; color: "#333" }
                }
            }
        }
    }
}
```

- [ ] **Step 9.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 9.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/MonitorTab.qml
git commit -m "feat(gui): MonitorTab — dynamic columns from board capability flags, inline editable cells"
```

---

## Task 10: ChannelTab.qml — per-channel panels + live graphs

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/ChannelTab.qml`

- [ ] **Step 10.1 — Write ChannelTab.qml**

```qml
// tools/hvb_demo_app/gui/qml/ChannelTab.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HvbTool
import "components"

ScrollView {
    id: root
    clip: true

    required property int channelIndex

    property var ci: (backend.channelInfoList  || [])[channelIndex] || {}
    property var cc: (backend.channelConfigList|| [])[channelIndex] || {}
    property int caps: ci.chCapFlags || 0xFFFF

    property bool hasOutEn: (caps & 0x0001) !== 0
    property bool hasVolt:  (caps & 0x0004) !== 0
    property bool hasCurr:  (caps & 0x0008) !== 0

    property int windowMinutes: 5   // shared between both graphs

    ColumnLayout {
        width: root.width
        spacing: 8

        // ── Live status panel ────────────────────────────────────────────────
        Pane {
            Layout.fillWidth: true
            padding: 8

            RowLayout {
                spacing: 16
                anchors.fill: parent

                RowLayout {
                    spacing: 4
                    visible: root.hasOutEn
                    Label { text: "Vset:"; opacity: 0.6 }
                    TextField {
                        implicitWidth: 80
                        placeholderText: "V"
                        text: root.ci.operationalTargetV !== undefined
                            ? root.ci.operationalTargetV.toFixed(1) : ""
                        onAccepted: backend.writeTargetVoltage(root.channelIndex,
                            Theme.voltageFromV(parseFloat(text) || 0))
                    }
                    Label { text: "V"; opacity: 0.6 }
                }

                LabeledValue {
                    label: "Vop"
                    value: root.ci.operationalTargetV !== undefined
                        ? root.ci.operationalTargetV.toFixed(1) + " V" : "--"
                    visible: root.hasVolt
                }

                LabeledValue {
                    label: "V"
                    value: root.ci.voltageV !== undefined
                        ? root.ci.voltageV.toFixed(2) + " V" : "--"
                    visible: root.hasVolt
                }

                LabeledValue {
                    label: "I"
                    value: root.ci.currentRaw !== undefined
                        ? root.ci.currentRaw + " nA" : "--"
                    visible: root.hasCurr
                }

                StatusBadge {
                    active: root.ci.statusOutEn || false
                    label: root.ci.statusRamping ? "RAMP"
                         : (root.ci.statusOutDrive ? "ON" : "OFF")
                }

                LabeledValue {
                    label: "Retries"
                    value: (root.ci.retryCount || 0).toString()
                }
            }
        }

        // ── Control + Protection row ────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // Control
            GroupBox {
                title: "Control"
                Layout.fillWidth: true
                visible: root.hasOutEn

                ColumnLayout {
                    RowLayout {
                        Button {
                            text: "Enable"
                            onClicked: backend.sendOutputAction(root.channelIndex, 1)
                        }
                        Button {
                            text: "Disable"
                            onClicked: backend.sendOutputAction(root.channelIndex, 2)
                        }
                        Button {
                            text: "Kill"
                            Material.background: Material.Red
                            onClicked: backend.sendOutputAction(root.channelIndex, 3)
                        }
                    }
                    RowLayout {
                        Label { text: "Ru:" }
                        TextField {
                            implicitWidth: 70
                            placeholderText: "V/step"
                            text: root.cc.rampUpStepRaw !== undefined
                                ? Theme.voltageToV(root.cc.rampUpStepRaw).toFixed(1) : ""
                            onAccepted: backend.writeRampUp(root.channelIndex,
                                Theme.voltageFromV(parseFloat(text) || 0),
                                root.cc.rampUpInterval || 1)
                        }
                        Label { text: "Rd:" }
                        TextField {
                            implicitWidth: 70
                            placeholderText: "V/step"
                            text: root.cc.rampDownStepRaw !== undefined
                                ? Theme.voltageToV(root.cc.rampDownStepRaw).toFixed(1) : ""
                            onAccepted: backend.writeRampDown(root.channelIndex,
                                Theme.voltageFromV(parseFloat(text) || 0),
                                root.cc.rampDownInterval || 1)
                        }
                    }
                }
            }

            // Protection
            GroupBox {
                title: "Protection"
                Layout.fillWidth: true
                visible: root.hasCurr

                ColumnLayout {
                    RowLayout {
                        Label { text: "I-Limit (nA):" }
                        TextField {
                            implicitWidth: 80
                            text: (root.cc.iLimitThresholdRaw || 0).toString()
                            onAccepted: backend.writeCurrentProtection(root.channelIndex,
                                root.cc.iProtMode || 0,
                                root.cc.iProtOutputAction || 0,
                                parseInt(text) || 0)
                        }
                    }
                    RowLayout {
                        ComboBox {
                            model: ["Disabled", "FlagOnly", "Apply-Action"]
                            currentIndex: root.cc.iProtMode || 0
                            onActivated: backend.writeCurrentProtection(root.channelIndex,
                                currentIndex,
                                root.cc.iProtOutputAction || 0,
                                root.cc.iLimitThresholdRaw || 0)
                        }
                        ComboBox {
                            model: ["None", "Dis-Graceful", "Dis-Immed", "ForceZero"]
                            currentIndex: root.cc.iProtOutputAction || 0
                            onActivated: backend.writeCurrentProtection(root.channelIndex,
                                root.cc.iProtMode || 0,
                                currentIndex,
                                root.cc.iLimitThresholdRaw || 0)
                        }
                    }
                    RowLayout {
                        Label { text: "Safe Band %:" }
                        SpinBox {
                            from: 0; to: 50
                            value: root.cc.currentSafeBandPct || 10
                            onValueModified: backend.writeChannelSafeBand(root.channelIndex, value)
                        }
                    }
                    RowLayout {
                        Button {
                            text: "Clr Active"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 1)
                        }
                        Button {
                            text: "Clr History"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 2)
                        }
                    }
                }
            }
        }

        // ── Recovery + Persistence row ───────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // Recovery
            GroupBox {
                title: "Recovery"
                Layout.fillWidth: true

                ColumnLayout {
                    RowLayout {
                        Label { text: "Policy:" }
                        ComboBox {
                            model: ["ManualLatch", "AutoRetry", "AutoDerate", "NeverRetry"]
                            currentIndex: root.cc.recoveryPolicyMode || 0
                            onActivated: backend.writeChannelRecovery(root.channelIndex,
                                currentIndex,
                                root.cc.autoRetryDelay     || 0,
                                root.cc.autoRetryMaxCount  || 0,
                                root.cc.autoRetryWindow    || 0)
                        }
                    }
                    RowLayout {
                        Label { text: "Dly:" }
                        SpinBox { from: 0; to: 3600; value: root.cc.autoRetryDelay || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0, value,
                                root.cc.autoRetryMaxCount || 0, root.cc.autoRetryWindow || 0) }
                        Label { text: "Max:" }
                        SpinBox { from: 0; to: 100; value: root.cc.autoRetryMaxCount || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, value, root.cc.autoRetryWindow || 0) }
                        Label { text: "Win:" }
                        SpinBox { from: 0; to: 86400; value: root.cc.autoRetryWindow || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, root.cc.autoRetryMaxCount || 0, value) }
                    }
                    RowLayout {
                        Label { text: "Derate (LSB):" }
                        TextField {
                            implicitWidth: 80
                            text: (root.cc.derateStepRaw || 0).toString()
                            onAccepted: backend.writeDerateStep(root.channelIndex, parseInt(text) || 0)
                        }
                    }
                }
            }

            // Persistence
            GroupBox {
                title: "Persistence"

                ColumnLayout {
                    Button { text: "Save";    Layout.fillWidth: true
                        onClicked: backend.saveChannel(root.channelIndex) }
                    Button { text: "Load";    Layout.fillWidth: true
                        onClicked: backend.loadChannel(root.channelIndex) }
                    Button { text: "Factory"; Layout.fillWidth: true
                        Material.background: Material.Red
                        onClicked: backend.factoryResetChannel(root.channelIndex) }
                }
            }
        }

        // ── Graphs row ───────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            height: 260

            ChannelGraph {
                Layout.fillWidth: true
                Layout.fillHeight: true
                channelIndex: root.channelIndex
                title: "Voltage"
                windowMinutes: root.windowMinutes
                seriesConfigs: [
                    // Vset = configured target — comes from channelConfigList
                    { name: "Vset", color: "#2196F3", valueKey: "configuredTargetV",   source: "config" },
                    // Vop = operational target (ramp in progress) — from channelInfoList
                    { name: "Vop",  color: "#00BCD4", valueKey: "operationalTargetV",  source: "info" },
                    // V = measured voltage — from channelInfoList
                    { name: "V",    color: "#4CAF50", valueKey: "voltageV",             source: "info" }
                ]
            }

            ChannelGraph {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.hasCurr
                channelIndex: root.channelIndex
                title: "Current"
                windowMinutes: root.windowMinutes
                seriesConfigs: [
                    { name: "I",     color: "#FF9800", valueKey: "currentRaw",          source: "info" },
                    { name: "Limit", color: "#F44336", valueKey: "iLimitThresholdRaw",  source: "config" }
                ]
            }
        }
    }
}
```

- [ ] **Step 10.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 10.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/ChannelTab.qml
git commit -m "feat(gui): ChannelTab — live panel, control/protection/recovery GroupBoxes, voltage+current graphs"
```

---

## Task 11: main.qml — root window

**Files:**
- Create: `tools/hvb_demo_app/gui/qml/main.qml`

- [ ] **Step 11.1 — Write main.qml**

```qml
// tools/hvb_demo_app/gui/qml/main.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import HvbTool
import "components"

ApplicationWindow {
    id: window
    visible: true
    width: 1100
    height: 780
    title: "HVB Modbus Tool"

    // Modals
    ConnectionModal { id: connModal }
    SysConfigDialog { id: sysCfgDialog }
    RawDebugDialog  { id: rawDebugDialog }

    // Track connecting state locally — backend has no explicit property for this
    property bool _connecting: false
    Connections {
        target: backend
        function onStatusMessageChanged() {
            window._connecting = (backend.statusMessage === "Connecting...")
        }
        function onConnectedChanged() {
            if (backend.connected) window._connecting = false
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Menu bar ─────────────────────────────────────────────────────────
        Pane {
            Layout.fillWidth: true
            padding: 6
            Material.elevation: 4

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    text: "HVB"
                    font.bold: true
                    font.pixelSize: 15
                }

                ToolSeparator {}

                BreathingIndicator {
                    connected:  backend.connected
                    connecting: window._connecting
                }

                // SysMode toggle — visible only when connected
                ComboBox {
                    visible: backend.connected
                    model: ["Normal", "Automatic"]
                    currentIndex: backend.sysInfo.activeOpMode || 0
                    implicitWidth: 110
                    onActivated: backend.writeOperatingMode(currentIndex)
                }

                // Uptime — visible only when connected
                Label {
                    visible: backend.connected
                    text: {
                        var s = backend.sysInfo.uptimeSec || 0
                        var h = Math.floor(s / 3600)
                        var m = Math.floor((s % 3600) / 60)
                        return h > 0 ? h + "h " + m + "m" : m + "m " + (s % 60) + "s"
                    }
                    opacity: 0.7
                }

                Item { Layout.fillWidth: true }

                // Connection info label
                Label {
                    visible: backend.connected
                    text: backend.selectedPort + " @" + backend.baudRate + " #" + backend.slaveId
                    opacity: 0.6
                    font.pixelSize: 11
                }

                Button {
                    text: backend.connected ? "Disconnect" : "Connect"
                    highlighted: !backend.connected
                    Material.background: backend.connected ? Material.Red : Material.accent
                    onClicked: {
                        if (backend.connected) backend.disconnectFromDevice()
                        else connModal.open()
                    }
                }

                Button {
                    text: "Quit"
                    onClicked: Qt.quit()
                }
            }
        }

        // ── Tab bar ───────────────────────────────────────────────────────────
        TabBar {
            id: tabBar
            Layout.fillWidth: true

            TabButton { text: "Monitor" }
            Repeater {
                model: backend.channelCount
                TabButton { text: "CH" + index }
            }

            // Reset to Monitor tab on disconnect
            Connections {
                target: backend
                function onConnectedChanged() {
                    if (!backend.connected) tabBar.currentIndex = 0
                }
            }
        }

        // ── Tab content ───────────────────────────────────────────────────────
        StackLayout {
            id: tabContent
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            MonitorTab {}

            Repeater {
                model: backend.channelCount
                ChannelTab { channelIndex: index }
            }
        }

        // ── Status bar ────────────────────────────────────────────────────────
        Pane {
            Layout.fillWidth: true
            padding: 4
            Material.elevation: 2

            RowLayout {
                anchors.fill: parent
                spacing: 8

                // SysConfig button
                Button {
                    text: "⚙ Config"
                    flat: true
                    enabled: backend.connected
                    onClicked: sysCfgDialog.open()
                }

                ToolSeparator {}

                // SysInfo strip
                LabeledValue {
                    label: "FW"
                    value: backend.connected
                        ? "0x" + ((backend.sysInfo.fwVersion || 0) >>> 0).toString(16).toUpperCase()
                        : "--"
                }
                LabeledValue {
                    label: "Proto"
                    value: backend.connected
                        ? (backend.sysInfo.protoMajor || 0) + "." + (backend.sysInfo.protoMinor || 0)
                        : "--"
                }
                LabeledValue {
                    label: "Variant"
                    value: (backend.sysInfo.variantId || "--").toString()
                }

                ToolSeparator {}

                // Environment
                LabeledValue {
                    label: "T"
                    value: backend.connected && backend.sysInfo.boardTempC !== undefined
                        ? backend.sysInfo.boardTempC.toFixed(1) + "°C" : "--"
                    visible: backend.sysInfo.capEnvSensor || false
                }
                LabeledValue {
                    label: "H"
                    value: backend.connected && backend.sysInfo.boardHumidityPct !== undefined
                        ? backend.sysInfo.boardHumidityPct.toFixed(1) + "%" : "--"
                    visible: backend.sysInfo.capEnvSensor || false
                }

                Item { Layout.fillWidth: true }

                // Debug toggle
                Button {
                    id: debugToggle
                    text: "■ Debug"
                    flat: true
                    checkable: true
                    checked: false
                }

                ToolSeparator {}

                // Error / status box
                Label {
                    id: statusLabel
                    text: backend.statusMessage
                    color: backend.statusMessage.startsWith("✓") ? Theme.colorOk
                         : backend.statusMessage.startsWith("✗") ? Theme.colorError
                         : Material.foreground
                    Layout.preferredWidth: 300
                    elide: Text.ElideRight
                }
            }
        }

        // ── Raw log panel (hidden until debug toggle) ────────────────────────
        ScrollView {
            id: rawLogPanel
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            visible: debugToggle.checked
            clip: true

            TextArea {
                id: rawLogArea
                readOnly: true
                font.family: "monospace"
                font.pixelSize: 10
                text: backend.rawLog
                wrapMode: TextEdit.NoWrap
                background: Rectangle { color: "#0d0d1a" }
                color: "#bbbbbb"
            }

            Connections {
                target: backend
                function onRawLogChanged() {
                    rawLogArea.cursorPosition = rawLogArea.length - 1
                }
            }
        }
    }
}
```

- [ ] **Step 11.2 — Verify build**

```bash
cmake --build build/gui 2>&1 | tail -30
```
Expected: compiles and links cleanly with no errors.

- [ ] **Step 11.3 — Commit**

```bash
git add tools/hvb_demo_app/gui/qml/main.qml
git commit -m "feat(gui): main.qml — menu bar, tab container, status bar, debug panel wired up"
```

---

## Task 12: Full build and smoke test

- [ ] **Step 12.1 — Clean build**

```bash
cmake -S tools/hvb_demo_app/gui -B build/gui \
      -DCMAKE_PREFIX_PATH=~/backup/Qt/6.8.5/gcc_64 \
      -DCMAKE_BUILD_TYPE=Debug && \
cmake --build build/gui -j$(nproc) 2>&1 | tail -30
```
Expected: exits with code 0; `bin/hvb_demo_gui` present.

- [ ] **Step 12.2 — Run and verify offline state**

```bash
~/backup/Qt/6.8.5/gcc_64/bin/QT_QUICK_CONTROLS_STYLE=Material bin/hvb_demo_gui &
```
Visual checklist (offline):
- [ ] Window opens with dark Material theme
- [ ] Menu bar shows "HVB" + grey dot + "Connect" button + "Quit"
- [ ] Single "Monitor" tab visible; shows "Not connected — click Connect" message
- [ ] Status bar shows "⚙ Config" (disabled), "--" for FW/Proto/Variant, "■ Debug" toggle
- [ ] "■ Debug" toggle reveals raw log panel below
- [ ] "Connect" button opens `ConnectionModal` with Port/Baud/SlaveAddr/Poll fields
- [ ] Cancel closes modal; Escape also closes

- [ ] **Step 12.3 — Connect to board (if available) or note for later**

If board is connected:
- [ ] Click Connect → status bar shows "Connecting..." → changes to "✓ Connected — N channels"
- [ ] Menu bar: breathing green dot, SysMode combo, uptime visible
- [ ] Monitor tab: column headers appear dynamically from board caps; channel rows appear
- [ ] CH0, CH1, … tabs appear in tab bar
- [ ] Navigate to CH0 tab: live panel shows Vop/V/I; panels show config values; graph areas visible
- [ ] After ~5s of polling: graphs start populating with rolling data
- [ ] Write a Vset value → status bar shows "✓ Target V" briefly then clears
- [ ] Write an invalid value → status bar shows "✗ ..." and stays visible
- [ ] ⚙ Config opens SysConfigDialog; Working Mode combo writes immediately
- [ ] Disconnect → CH tabs disappear; Monitor tab shows offline message; dot goes grey

- [ ] **Step 12.4 — Final commit**

```bash
git add -A
git commit -m "feat(gui): complete QML rewrite — Monitor tab, channel tabs, graphs, Material dark theme"
```

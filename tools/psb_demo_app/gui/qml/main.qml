import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbTool
import "components"

ApplicationWindow {
    id: window
    visible: true
    width: 1300
    height: 860
    title: "HVB Modbus Tool"
    // App-wide base font — Qt Quick Controls propagate this to every
    // descendant Control/Label that doesn't set its own font.pixelSize.
    font.pixelSize: 16

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
            if (!backend.connected) tabContent.currentIndex = 0
        }
        // modeCombo.currentIndex destroys its declarative `expr` binding the
        // first time the user picks an item (same QQC2 quirk fixed for the
        // Protection/Recovery combos in ChannelTab.qml and the Working
        // Mode/Startup Policy combos in SysConfigDialog.qml) — without this,
        // it would never reflect backend.sysInfo.activeOpMode again after
        // the first manual switch, including if the write silently failed
        // or the mode changed for any other reason. Clamp to 0 (Normal) for
        // activeOpMode 2 (Calibration) — modeCombo's model only has 2 items
        // and is hidden in that state anyway (see calModeLabel below); the
        // demo GUI can't enter/exit Calibration Mode itself (factory-tool-
        // only unlock sequence, see docs/guide/calibration-guide.md).
        function onSysInfoChanged() {
            var m = backend.sysInfo.activeOpMode || 0
            modeCombo.currentIndex = (m === 2) ? 0 : m
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Menu bar — layout mirrors psb_demo_tui's menu bar: identity/mode on
        // the left, breathing indicator + uptime + T/H centered, connect/quit
        // on the right.
        Pane {
            Layout.fillWidth: true
            padding: 10
            Material.elevation: 4

            RowLayout {
                anchors.fill: parent
                spacing: 12

                Label {
                    text: "HVB"
                    font.bold: true
                    font.pixelSize: 19
                }

                ToolSeparator {}

                Label {
                    visible: backend.connected
                    text: backend.channelCount + " Channels"
                    opacity: 0.8
                }

                ToolSeparator { visible: backend.connected }

                ComboBox {
                    id: modeCombo
                    visible: backend.connected && backend.sysInfo.activeOpMode !== 2
                    model: ["Normal", "Automatic"]
                    currentIndex: backend.sysInfo.activeOpMode || 0
                    implicitWidth: 130
                    onActivated: backend.writeOperatingMode(currentIndex)
                }

                // A factory-tool session (or a stuck exit — see
                // docs/guide/calibration-guide.md §10 on the auto-exit
                // watchdog) can leave the board in Calibration Mode while
                // the demo GUI is connected. The demo GUI can't enter/exit
                // it (factory-tool-only unlock), so show it plainly instead
                // of letting the Normal/Automatic combo silently mis-render
                // an out-of-range index.
                Label {
                    id: calModeLabel
                    visible: backend.connected && backend.sysInfo.activeOpMode === 2
                    text: "Calibration Mode (factory)"
                    color: Material.color(Material.Amber)
                    font.bold: true
                }

                Item { Layout.fillWidth: true }

                // Center group — heartbeat + uptime + environment, always
                // visible (the indicator itself communicates offline/
                // connecting/connected), telemetry text only once connected.
                RowLayout {
                    spacing: 10

                    BreathingIndicator {
                        connected:  backend.connected
                        connecting: window._connecting
                    }

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

                    Label {
                        visible: backend.connected && (backend.sysInfo.capEnvSensor || false)
                        text: "|  T: " + (backend.sysInfo.boardTempC !== undefined
                                ? backend.sysInfo.boardTempC.toFixed(1) + "°C" : "--")
                            + "  H: " + (backend.sysInfo.boardHumidityPct !== undefined
                                ? backend.sysInfo.boardHumidityPct.toFixed(1) + "%" : "--")
                        opacity: 0.7
                    }
                }

                Item { Layout.fillWidth: true }

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

        Rectangle { Layout.fillWidth: true; height: 1; color: "#3a3a3a" }

        // Sidebar + tab content
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ColumnLayout {
                // Layout.preferredWidth alone isn't respected here — this
                // nested ColumnLayout's own implicit width (inflated by its
                // Layout.fillWidth children) ends up used instead, grabbing
                // nearly all of the RowLayout's space. Layout.maximumWidth
                // is a hard constraint that isn't subject to that.
                Layout.preferredWidth: 140
                Layout.maximumWidth: 140
                Layout.fillHeight: true
                spacing: 0

                Repeater {
                    model: 1 + backend.channelCount
                    delegate: ItemDelegate {
                        required property int index
                        Layout.fillWidth: true
                        text: index === 0 ? "Monitor" : "CH" + (index - 1)
                        highlighted: tabContent.currentIndex === index
                        onClicked: tabContent.currentIndex = index
                    }
                }

                Item { Layout.fillHeight: true }
            }

            Rectangle { Layout.fillHeight: true; width: 1; color: "#3a3a3a" }

            // StackLayout is wrapped in a plain Item that takes the
            // Layout.fillWidth/fillHeight instead of putting those directly
            // on the StackLayout: nested one level inside this sidebar
            // RowLayout, StackLayout's own implicit-size negotiation
            // collapsed to a few pixels wide regardless of Layout.fillWidth
            // (verified live — MonitorTab's root Item reported width: 8).
            // anchors.fill sidesteps that negotiation entirely.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    id: tabContent
                    anchors.fill: parent
                    currentIndex: 0

                    MonitorTab {}

                    Repeater {
                        model: backend.channelCount
                        ChannelTab {
                            required property int index
                            channelIndex: index
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#3a3a3a" }

        // Status bar — command I/O feedback (statusMessage) leftmost and
        // prominent, version info centered, connection info + Config in the
        // bottom-right corner, mirroring psb_demo_tui's status bar.
        Pane {
            Layout.fillWidth: true
            padding: 8
            Material.elevation: 2

            RowLayout {
                anchors.fill: parent
                spacing: 10

                Label {
                    id: statusLabel
                    text: backend.statusMessage
                    // Hardcoded to match PsbTheme's colorOk/colorError rather than
                    // referencing the singleton directly — see MonitorTab.qml's
                    // computeActiveColumns() for why (qmlcachegen AOT limitation).
                    color: backend.statusMessage.startsWith("✓") ? "#4CAF50"
                         : backend.statusMessage.startsWith("✗") ? "#F44336"
                         : Material.foreground
                    Layout.preferredWidth: 340
                    elide: Text.ElideRight
                }

                Item { Layout.fillWidth: true }

                RowLayout {
                    spacing: 10
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
                    LabeledValue {
                        label: "GUI"
                        value: backend.toolVersion
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    id: debugToggle
                    text: "■ Debug"
                    flat: true
                    checkable: true
                    checked: false
                }

                ToolSeparator {}

                Label {
                    visible: backend.connected
                    text: backend.selectedPort + " @" + backend.baudRate + " #" + backend.slaveId
                    opacity: 0.6
                    font.pixelSize: 13
                }

                ToolSeparator {}

                Button {
                    text: "⚙ Config"
                    flat: true
                    enabled: backend.connected
                    onClicked: sysCfgDialog.open()
                }
            }
        }

        // Raw log panel — hidden until debug toggle is checked
        ScrollView {
            id: rawLogPanel
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            visible: debugToggle.checked
            clip: true

            TextArea {
                id: rawLogArea
                readOnly: true
                font.family: "monospace"
                font.pixelSize: 12
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

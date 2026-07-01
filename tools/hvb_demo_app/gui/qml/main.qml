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

        // Menu bar
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

                ComboBox {
                    visible: backend.connected
                    model: ["Normal", "Automatic"]
                    currentIndex: backend.sysInfo.activeOpMode || 0
                    implicitWidth: 110
                    onActivated: backend.writeOperatingMode(currentIndex)
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

                Item { Layout.fillWidth: true }

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

        // Tab bar
        TabBar {
            id: tabBar
            Layout.fillWidth: true

            TabButton { text: "Monitor" }
            Repeater {
                model: backend.channelCount
                TabButton { text: "CH" + index }
            }

            Connections {
                target: backend
                function onConnectedChanged() {
                    if (!backend.connected) tabBar.currentIndex = 0
                }
            }
        }

        // Tab content
        StackLayout {
            id: tabContent
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            MonitorTab {}

            Repeater {
                model: backend.channelCount
                ChannelTab {
                    required property int index
                    channelIndex: index
                }
            }
        }

        // Status bar
        Pane {
            Layout.fillWidth: true
            padding: 4
            Material.elevation: 2

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Button {
                    text: "⚙ Config"
                    flat: true
                    enabled: backend.connected
                    onClicked: sysCfgDialog.open()
                }

                ToolSeparator {}

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

                Button {
                    id: debugToggle
                    text: "■ Debug"
                    flat: true
                    checkable: true
                    checked: false
                }

                ToolSeparator {}

                Label {
                    id: statusLabel
                    text: backend.statusMessage
                    color: backend.statusMessage.startsWith("✓") ? HvbTheme.colorOk
                         : backend.statusMessage.startsWith("✗") ? HvbTheme.colorError
                         : Material.foreground
                    Layout.preferredWidth: 300
                    elide: Text.ElideRight
                }
            }
        }

        // Raw log panel — hidden until debug toggle is checked
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

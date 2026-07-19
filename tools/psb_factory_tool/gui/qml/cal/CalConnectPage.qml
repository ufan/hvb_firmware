import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory
import "../components"

// Step 1: Select port, connect, verify device info
Item {
    id: root
    signal connected

    // Advance automatically when backend connects
    Connections {
        target: Backend
        function onConnectedChanged() {
            if (Backend.connected) root.connected()
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 500)
        spacing: 16

        Label {
            text: "Connect to Device"
            font.pixelSize: 22
            font.bold: true
        }

        DeviceInfoCard { visible: false }  // placeholder for spacing

        GroupBox {
            title: "Serial Port"
            Layout.fillWidth: true

            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8
                width: parent.width

                Label { text: "Port:" }
                RowLayout {
                    ComboBox {
                        id: portCombo
                        model: Backend.ports
                        Layout.fillWidth: true
                        Layout.preferredWidth: 200
                        displayText: count > 0 ? currentText : "(no ports)"
                        Component.onCompleted: Backend.scanPorts()
                    }
                    ToolButton {
                        text: "↺"
                        onClicked: Backend.scanPorts()
                        ToolTip.text: "Refresh port list"
                        ToolTip.visible: hovered
                    }
                }

                Label { text: "Baud:" }
                ComboBox {
                    id: baudCombo
                    model: [9600, 19200, 38400, 57600, 115200]
                    currentIndex: 4   // 115200
                    Layout.preferredWidth: 120
                }

                Label { text: "Slave ID:" }
                SpinBox {
                    id: slaveSpin
                    from: 1; to: 247; value: 1
                    editable: true
                }
            }
        }

        Button {
            text: Backend.connected ? "Disconnect" : "Connect"
            highlighted: !Backend.connected
            enabled: portCombo.count > 0 || Backend.connected
            Layout.fillWidth: true
            onClicked: {
                if (Backend.connected)
                    Backend.disconnectFromDevice()
                else
                    Backend.connectToDevice(portCombo.currentText,
                                            baudCombo.currentValue,
                                            slaveSpin.value)
            }
        }

        DeviceInfoCard {
            visible: Backend.connected
            Layout.fillWidth: true
        }
    }
}

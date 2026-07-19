import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    modal: true
    anchors.centerIn: parent
    padding: 20
    closePolicy: Popup.CloseOnEscape
    // Explicit width — Popup's automatic implicit-size inference from a
    // ColumnLayout child (with its own explicit width plus Layout.fillWidth
    // descendants) undersizes the background Rectangle relative to the
    // actual content, so rows/buttons visibly spill past its right edge.
    width: 380 + leftPadding + rightPadding

    onOpened: backend.scanPorts()

    ColumnLayout {
        spacing: 14
        width: 380

        Label {
            text: "Connection Settings"
            font.bold: true
            font.pixelSize: 18
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
        }

        GridLayout {
            columns: 2
            rowSpacing: 10
            columnSpacing: 10
            Layout.fillWidth: true

            Label { text: "Port"; Layout.preferredWidth: 84 }
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

            Label { text: "Baud"; Layout.preferredWidth: 84 }
            ComboBox {
                id: baudCombo
                Layout.fillWidth: true
                model: ["9600", "19200", "38400", "115200"]
                currentIndex: model.indexOf(String(backend.baudRate))
                onActivated: backend.baudRate = parseInt(currentText)
            }

            Label { text: "Slave ID"; Layout.preferredWidth: 84 }
            SpinBox {
                id: slaveSpin
                Layout.fillWidth: true
                from: 0; to: 247
                value: backend.slaveId
                onValueModified: backend.slaveId = value
            }

            Label { text: "Poll"; Layout.preferredWidth: 84 }
            ComboBox {
                id: pollCombo
                Layout.fillWidth: true
                model: ["0.5 s", "1 s", "2 s", "5 s", "10 s"]
                currentIndex: 1
                onActivated: {
                    var ms = [500, 1000, 2000, 5000, 10000]
                    backend.setPollIntervalMs(ms[currentIndex])
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                text: "Connect"
                highlighted: true
                Layout.fillWidth: true
                enabled: portCombo.currentText.length > 0
                onClicked: {
                    backend.connectToDevice()
                    root.close()
                }
            }

            Button {
                text: "Cancel"
                Layout.fillWidth: true
                onClicked: root.close()
            }
        }
    }
}

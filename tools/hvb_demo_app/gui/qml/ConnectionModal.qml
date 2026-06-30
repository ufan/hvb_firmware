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
                currentIndex: 1
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

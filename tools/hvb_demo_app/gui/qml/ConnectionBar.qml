import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    spacing: 8

    ComboBox {
        id: portCombo
        model: backend.ports
        Layout.preferredWidth: 160
        editable: true
        onCurrentTextChanged: backend.selectedPort = currentText

        Component.onCompleted: {
            if (backend.selectedPort) currentIndex = find(backend.selectedPort)
        }
    }

    ComboBox {
        id: baudCombo
        model: ["9600", "115200"]
        currentIndex: backend.baudRate === 115200 ? 1 : 0
        Layout.preferredWidth: 90
        onCurrentTextChanged: backend.baudRate = parseInt(currentText)
    }

    SpinBox {
        id: slaveIdSpin
        from: 0; to: 247; value: backend.slaveId
        Layout.preferredWidth: 60
        onValueChanged: backend.slaveId = value
    }

    Button {
        text: backend.connected ? "Disconnect" : "Connect"
        Layout.preferredWidth: 100
        palette.button: backend.connected ? "#E53935" : "#43A047"
        palette.buttonText: "white"
        onClicked: {
            if (backend.connected) backend.disconnectFromDevice()
            else backend.connectToDevice()
        }
    }

    ComboBox {
        id: pollCombo
        model: ["0.5s", "1s", "2s", "5s", "10s"]
        Layout.preferredWidth: 70
        onCurrentIndexChanged: {
            var ms = [500, 1000, 2000, 5000, 10000]
            backend.pollIntervalMs = ms[currentIndex]
        }
        currentIndex: 2  // default 2s
    }

    Item { Layout.fillWidth: true }

    Label {
        text: backend.connected ? "HVB v" + (backend.sysInfo.variantId || "?") : "HVB"
        font.bold: true
        font.pixelSize: 13
        Layout.alignment: Qt.AlignVCenter
    }

    Rectangle {
        id: connDot
        width: 12; height: 12; radius: 6
        color: backend.connected ? "#4CAF50" : "#F44336"
        Layout.alignment: Qt.AlignVCenter

        property real pulse: 1.0
        opacity: backend.connected ? pulse : 1.0

        Timer {
            interval: 800
            running: backend.connected
            repeat: true
            onTriggered: connDot.pulse = (connDot.pulse > 0.6) ? 0.3 : 1.0
        }
    }
}

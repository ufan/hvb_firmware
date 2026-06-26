import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 900; height: 700
    visible: true
    title: "HVB Factory Calibration Tool"
    Material.theme: Material.Dark
    Material.accent: Material.Blue

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.margins: 8

            Label { text: "Port:"; font.bold: true }
            ComboBox {
                id: portCombo
                model: backend.ports
                Layout.preferredWidth: 200
                Component.onCompleted: backend.scanPorts()
            }
            Button {
                text: backend.connected ? "Disconnect" : "Connect"
                onClicked: {
                    if (backend.connected)
                        backend.disconnectFromDevice()
                    else
                        backend.connectToDevice(portCombo.currentText, 115200, 1)
                }
            }
            Button { text: "Scan"; onClicked: backend.scanPorts() }
            Item { Layout.fillWidth: true }
            Label {
                text: backend.statusMessage
                font.italic: true
                elide: Text.ElideRight
                Layout.maximumWidth: 300
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Step 1: Unlock
        GroupBox {
            title: "Step 1: Unlock"
            Layout.fillWidth: true
            RowLayout {
                Button { text: "Unlock Step 1 (0xCA1B)"; onClicked: backend.unlockStep1(); enabled: backend.connected }
                Button { text: "Unlock Step 2 (0xA11B)"; onClicked: backend.unlockStep2(); enabled: backend.connected }
                Label { text: backend.calUnlocked ? "UNLOCKED" : "locked"; color: backend.calUnlocked ? "lime" : "gray"; font.bold: true }
            }
        }

        // Step 2: Enter/Exit Mode
        GroupBox {
            title: "Step 2: Calibration Mode"
            Layout.fillWidth: true
            RowLayout {
                Button { text: "Enter Cal Mode"; onClicked: backend.enterCalibrationMode(); enabled: backend.connected && backend.calUnlocked }
                Button { text: "Exit Cal Mode"; onClicked: backend.exitCalibrationMode(); enabled: backend.connected && backend.calActive }
                Label { text: backend.calActive ? "CALIBRATION ACTIVE" : "inactive"; color: backend.calActive ? "orange" : "gray"; font.bold: true }
            }
        }

        // Step 3: Channel Control
        GroupBox {
            title: "Step 3: Channel Control"
            Layout.fillWidth: true
            enabled: backend.calActive

            ColumnLayout {
                RowLayout {
                    Label { text: "Channel:" }
                    SpinBox { id: chSpin; from: 0; to: 1; value: backend.activeChannel; onValueModified: backend.activeChannel = value }
                    Button { text: "Enable Output"; onClicked: backend.enableOutput(true) }
                    Button { text: "Disable Output"; onClicked: backend.enableOutput(false) }
                }
                RowLayout {
                    Label { text: "Raw DAC:" }
                    SpinBox { id: dacSpin; from: 0; to: 4095; editable: true }
                    Button { text: "Set DAC"; onClicked: backend.writeRawDac(dacSpin.value) }
                    Button { text: "Sample ADC"; onClicked: backend.triggerSample() }
                    Button { text: "Refresh"; onClicked: backend.refreshSnapshot() }
                }

                Label {
                    id: snapLabel
                    text: "Snapshot: (click Refresh)"
                    font.family: "monospace"
                }

                Connections {
                    target: backend
                    function onSnapshotUpdated(snapshot) {
                        snapLabel.text = "Out=" + (snapshot.outputEnabled ? "ON" : "OFF")
                            + "  DAC=" + snapshot.rawDacCode + "  readback=" + snapshot.rawDacReadback
                            + "  limit=" + snapshot.maxRawDacLimit
                            + "\nADC V=" + snapshot.rawAdcVoltage + "  I=" + snapshot.rawAdcCurrent
                    }
                }
            }
        }

        // Step 4: Coefficients
        GroupBox {
            title: "Step 4: Coefficients"
            Layout.fillWidth: true
            enabled: backend.calActive

            GridLayout {
                columns: 5
                Label { text: "Type" } Label { text: "K" } Label { text: "B" } Label { text: "" } Label { text: "" }
                Label { text: "Output" }
                SpinBox { id: outK; from: 0; to: 65535; value: 10000; editable: true }
                SpinBox { id: outB; from: -32768; to: 32767; value: 0; editable: true }
                Button { text: "Write"; onClicked: backend.writeCoefficients("out", outK.value, outB.value) }
                Item {}
                Label { text: "Meas V" }
                SpinBox { id: mvK; from: 0; to: 65535; value: 10000; editable: true }
                SpinBox { id: mvB; from: -32768; to: 32767; value: 0; editable: true }
                Button { text: "Write"; onClicked: backend.writeCoefficients("meas-v", mvK.value, mvB.value) }
                Item {}
                Label { text: "Meas I" }
                SpinBox { id: miK; from: 0; to: 65535; value: 10000; editable: true }
                SpinBox { id: miB; from: -32768; to: 32767; value: 0; editable: true }
                Button { text: "Write"; onClicked: backend.writeCoefficients("meas-i", miK.value, miB.value) }
                Item {}
            }
        }

        // Step 5: Commit & Exit
        GroupBox {
            title: "Step 5: Commit & Exit"
            Layout.fillWidth: true
            RowLayout {
                Button { text: "Commit CH" + chSpin.value; onClicked: backend.commitChannel(); enabled: backend.calActive }
                Button { text: "SAFE ALL"; onClicked: backend.safeAll(); enabled: backend.connected; Material.background: Material.Red }
            }
        }

        Item { Layout.fillHeight: true }
    }
}

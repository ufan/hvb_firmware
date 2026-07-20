import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory
import "../components"

Item {
    id: root

    property var lastResults: []

    Connections {
        target: Backend
        function onFuncTestDone(pass, points) {
            root.lastResults = points
            statusLabel.text  = pass ? "✓ PASS" : "✗ FAIL"
            statusLabel.color = pass ? Material.color(Material.Green)
                                     : Material.color(Material.Red)
        }
        function onFuncTestProgress(ch, idx, total) {
            progressBar.value = total > 0 ? idx / total : 0
            progressLabel.text = "CH" + ch + "  point " + idx + "/" + total
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        // Config
        GroupBox {
            title: "Test Configuration"
            Layout.fillWidth: true

            GridLayout {
                columns: 4
                columnSpacing: 16
                rowSpacing: 8

                Label { text: "Channel:" }
                ComboBox {
                    id: chCombo
                    model: {
                        const opts = ["All"]
                        for (let i = 0; i < Backend.numChannels; ++i) opts.push("CH" + i)
                        return opts
                    }
                    Layout.preferredWidth: 100
                }

                Label { text: "Tolerance %:" }
                TextField {
                    id: tolField
                    text: Backend.funcTestConfig().tolerancePct
                    validator: DoubleValidator { bottom: 0.01; top: 50.0 }
                    Layout.preferredWidth: 80
                }

                Label { text: "Settle ms:" }
                TextField {
                    id: settleField
                    text: Backend.funcTestConfig().settleMs
                    validator: IntValidator { bottom: 50; top: 10000 }
                    Layout.preferredWidth: 80
                }

                Label { text: "Retries on fault:" }
                SpinBox {
                    id: retriesSpin
                    from: 0; to: 10
                    value: Backend.funcTestConfig().retriesOnFault
                }

                Label { text: "Target voltages (V, comma-sep):" }
                TextField {
                    id: targetVoltsField
                    text: Backend.funcTestConfig().targetVolts
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    placeholderText: "e.g. 5.0,10.0,20.0,30.0"
                }
            }
        }

        // Run controls
        RowLayout {
            spacing: 12

            Button {
                text: Backend.testRunning ? "Abort" : "Run Functional Test"
                highlighted: !Backend.testRunning
                enabled: Backend.connected
                onClicked: {
                    if (Backend.testRunning) {
                        Backend.abortTest()
                    } else {
                        const cfg = {
                            ch:            chCombo.currentIndex === 0 ? -1 : chCombo.currentIndex - 1,
                            tolerancePct:  parseFloat(tolField.text),
                            settleMs:      parseInt(settleField.text),
                            retriesOnFault: retriesSpin.value,
                            targetVolts:   targetVoltsField.text
                        }
                        Backend.startFunctionalTest(cfg)
                    }
                }
            }

            Label {
                id: statusLabel
                font.bold: true
                font.pixelSize: 14
            }

            Item { Layout.fillWidth: true }
        }

        ProgressBar {
            id: progressBar
            visible: Backend.testRunning
            Layout.fillWidth: true
        }
        Label {
            id: progressLabel
            visible: Backend.testRunning
            opacity: 0.7
            font.pixelSize: 11
        }

        // Results table
        ResultsTable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.lastResults.length > 0
            columns: ["Ch", "Target V", "Tol%", "Measured V", "Error%", "Result"]
            rows: root.lastResults.map(p => [
                "CH" + p.ch,
                p.targetV.toFixed(3),
                "±" + p.tolerancePct.toFixed(1),
                p.measuredV.toFixed(3),
                p.errorPct.toFixed(2) + "%",
                p.pass ? "PASS" : "FAIL"
            ])
            passColumn: 5
        }
    }
}

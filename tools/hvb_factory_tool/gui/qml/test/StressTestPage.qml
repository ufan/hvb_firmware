import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtCharts
import HvbFactory

Item {
    id: root

    property var lastResults: []

    Connections {
        target: Backend

        function onStressProgress(ch, elapsed, total, v) {
            progressBar.value = total > 0 ? elapsed / total : 0
            progressLabel.text = "CH" + ch + "  " + elapsed.toFixed(0) + "/" + total.toFixed(0)
                                 + " s  " + v.toFixed(4) + " V"
            liveSeries.append(elapsed, v)
        }

        function onStressDone(pass, channels) {
            root.lastResults = channels
            statusLabel.text  = pass ? "✓ PASS" : "✗ FAIL"
            statusLabel.color = pass ? Material.color(Material.Green)
                                     : Material.color(Material.Red)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        GroupBox {
            title: "Stress Test Configuration"
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

                Label { text: "Duration (s):" }
                TextField {
                    id: durationField
                    text: Backend.stressConfig().durationSec
                    validator: IntValidator { bottom: 10; top: 86400 }
                    Layout.preferredWidth: 80
                }

                Label { text: "Poll interval (ms):" }
                TextField {
                    id: pollField
                    text: Backend.stressConfig().pollMs
                    validator: IntValidator { bottom: 100; top: 60000 }
                    Layout.preferredWidth: 80
                }

                Label { text: "Fault tolerance:" }
                SpinBox {
                    id: faultTolSpin
                    from: 0; to: 100
                    value: Backend.stressConfig().faultTolerance
                }

                Label { text: "Tolerance %:" }
                TextField {
                    id: stressTolField
                    text: Backend.stressConfig().tolerancePct
                    validator: DoubleValidator { bottom: 0.1; top: 50.0 }
                    Layout.preferredWidth: 80
                }

                Label { text: "Target voltages (V, comma-sep):" }
                TextField {
                    id: stressTargetField
                    text: Backend.stressConfig().targetVolts
                    Layout.columnSpan: 3
                    Layout.fillWidth: true
                    placeholderText: "e.g. 20.0  (one per channel in order)"
                }
            }
        }

        RowLayout {
            spacing: 12

            Button {
                text: Backend.testRunning ? "Abort" : "Run Stress Test"
                highlighted: !Backend.testRunning
                enabled: Backend.connected
                onClicked: {
                    if (Backend.testRunning) {
                        Backend.abortTest()
                    } else {
                        liveSeries.clear()
                        const cfg = {
                            ch:             chCombo.currentIndex === 0 ? -1 : chCombo.currentIndex - 1,
                            durationSec:    parseInt(durationField.text),
                            pollMs:         parseInt(pollField.text),
                            faultTolerance: faultTolSpin.value,
                            tolerancePct:   parseFloat(stressTolField.text),
                            targetVolts:    stressTargetField.text
                        }
                        Backend.startStressTest(cfg)
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

        // Live chart
        LiveChart {
            id: liveChart
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            visible: Backend.testRunning || root.lastResults.length > 0
            seriesRef: liveSeries
        }

        // Hidden series feeding the chart
        ChartView {
            id: hiddenChart
            visible: false
            width: 1; height: 1
            LineSeries { id: liveSeries; name: "Voltage" }
        }

        // Results table
        ResultsTable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.lastResults.length > 0
            columns: ["Ch", "Target V", "Duration", "Avg V", "Min V", "Max V", "σ V", "Faults", "Result"]
            rows: root.lastResults.map(r => [
                "CH" + r.ch,
                r.targetV.toFixed(3),
                r.durationSec.toFixed(0) + " s",
                r.avgV.toFixed(4),
                r.minV.toFixed(4),
                r.maxV.toFixed(4),
                r.stddevV.toFixed(4),
                "" + r.faultCount,
                r.pass ? "PASS" : "FAIL"
            ])
            passColumn: 8
        }
    }
}

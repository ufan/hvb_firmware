import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import HvbFactory

// Step 4: Review committed coefficients and exit cal mode
Item {
    id: root
    signal rollback

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: "Calibration Complete"
            font.pixelSize: 22
            font.bold: true
        }

        // Summary table of committed coefficients
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            model: Backend.calSummary()
            spacing: 8

            delegate: Pane {
                required property var modelData
                width: ListView.view.width
                Material.elevation: 1

                ColumnLayout {
                    width: parent.width
                    spacing: 4

                    RowLayout {
                        Label {
                            text: "CH" + modelData.ch
                            font.bold: true
                            font.pixelSize: 14
                        }
                        Label {
                            text: modelData.committed ? "✓ Committed" : "⚠ Not committed"
                            color: modelData.committed
                                   ? Material.color(Material.Green)
                                   : Material.color(Material.Orange)
                        }
                        Label {
                            visible: !modelData.needsCal
                            text: "(no calibration axes)"
                            opacity: 0.5
                        }
                    }

                    GridLayout {
                        visible: modelData.needsCal
                        columns: 4
                        columnSpacing: 16

                        Label { text: "Axis";    font.bold: true; opacity: 0.7 }
                        Label { text: "K (dev)"; font.bold: true; opacity: 0.7 }
                        Label { text: "B (dev)"; font.bold: true; opacity: 0.7 }
                        Label { text: "R²";      font.bold: true; opacity: 0.7 }

                        // NVS saved before calibration (for comparison)
                        Label { text: "NVS (before)"; opacity: 0.5; font.pixelSize: 10;
                                Layout.columnSpan: 4 }

                        Repeater {
                            model: {
                                const nvs = Backend.channelNvsCoeffs(modelData.ch)
                                return [
                                    {label: "out",    k: nvs.outCalK,   b: nvs.outCalB,
                                     active: modelData.hasOut},
                                    {label: "meas-V", k: nvs.measVCalK, b: nvs.measVCalB,
                                     active: modelData.hasMeasV},
                                    {label: "meas-I", k: nvs.measICalK, b: nvs.measICalB,
                                     active: modelData.hasMeasI}
                                ].filter(r => r.active)
                            }
                            Label { required property var modelData; text: modelData.label; opacity: 0.6 }
                            Label { required property var modelData; text: modelData.k; opacity: 0.6 }
                            Label { required property var modelData; text: modelData.b; opacity: 0.6 }
                            Label { text: "—"; opacity: 0.6 }
                        }

                        // New coefficients
                        Label { text: "New (written)"; opacity: 0.9; font.pixelSize: 10;
                                Layout.columnSpan: 4 }

                        Repeater {
                            model: [
                                {label: "out",    k: modelData.outKDevice,
                                 b: modelData.outBDevice,   r2: modelData.outR2,
                                 active: modelData.hasOut},
                                {label: "meas-V", k: modelData.measVKDevice,
                                 b: modelData.measVBDevice, r2: modelData.measVR2,
                                 active: modelData.hasMeasV},
                                {label: "meas-I", k: modelData.measIKDevice,
                                 b: modelData.measIBDevice, r2: modelData.measIR2,
                                 active: modelData.hasMeasI}
                            ].filter(r => r.active)

                            Label { required property var modelData; text: modelData.label }
                            Label { required property var modelData; text: modelData.k }
                            Label { required property var modelData; text: modelData.b }
                            Label {
                                required property var modelData
                                text: modelData.r2.toFixed(6)
                                color: modelData.r2 >= 0.9999
                                       ? Material.color(Material.Green)
                                       : modelData.r2 >= 0.999
                                         ? Material.color(Material.Orange)
                                         : Material.color(Material.Red)
                            }
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            spacing: 12
            Layout.fillWidth: true

            Button {
                text: "← Back to Channels"
                onClicked: {
                    Backend.rollbackToCal()
                    root.rollback()
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Exit Calibration Mode"
                highlighted: true
                onClicked: Backend.exitCalMode()
            }
        }
    }
}

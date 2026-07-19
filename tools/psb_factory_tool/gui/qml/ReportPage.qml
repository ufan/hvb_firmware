import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import PsbFactory
import "components"

Item {
    id: root

    Connections {
        target: Backend
        function onReportGenerated(ok, pathOrError) {
            genStatus.text  = ok ? "✓ Saved: " + pathOrError : "✗ " + pathOrError
            genStatus.color = ok ? Material.color(Material.Green)
                                 : Material.color(Material.Red)
        }
    }

    FileDialog {
        id: pdfDialog
        title: "Save PDF Report"
        fileMode: FileDialog.SaveFile
        nameFilters: ["PDF files (*.pdf)", "All files (*)"]
        defaultSuffix: "pdf"
        onAccepted: Backend.generatePdf(selectedFile.toString().replace("file://", ""))
    }

    FileDialog {
        id: mdDialog
        title: "Save Markdown Report"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md)", "All files (*)"]
        defaultSuffix: "md"
        onAccepted: Backend.generateMarkdown(selectedFile.toString().replace("file://", ""))
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: "Report"
            font.pixelSize: 22
            font.bold: true
        }

        // Metadata
        GroupBox {
            title: "Report Metadata"
            Layout.fillWidth: true

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                width: parent.width

                Label { text: "Board serial:" }
                TextField {
                    Layout.fillWidth: true
                    text: Backend.reportBoardSerial
                    onTextChanged: Backend.reportBoardSerial = text
                    placeholderText: "e.g. HVB-001-2026"
                }

                Label { text: "Operator ID:" }
                TextField {
                    Layout.fillWidth: true
                    text: Backend.reportOperatorId
                    onTextChanged: Backend.reportOperatorId = text
                    placeholderText: "e.g. J.Smith"
                }

                Label { text: "Notes:" }
                TextField {
                    Layout.fillWidth: true
                    text: Backend.reportNotes
                    onTextChanged: Backend.reportNotes = text
                    placeholderText: "Optional remarks"
                }
            }
        }

        // Data availability checklist
        GroupBox {
            title: "Report Contents"
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 6

                Row {
                    spacing: 8
                    Label {
                        text: Backend.calDataAvailable ? "✓" : "○"
                        color: Backend.calDataAvailable
                               ? Material.color(Material.Green)
                               : Material.foreground
                        font.pixelSize: 16
                    }
                    Label { text: "Calibration data" }
                }
                Row {
                    spacing: 8
                    Label {
                        text: Backend.funcTestAvailable ? "✓" : "○"
                        color: Backend.funcTestAvailable
                               ? Material.color(Material.Green)
                               : Material.foreground
                        font.pixelSize: 16
                    }
                    Label { text: "Functional test" }
                }
                Row {
                    spacing: 8
                    Label {
                        text: Backend.stressTestAvailable ? "✓" : "○"
                        color: Backend.stressTestAvailable
                               ? Material.color(Material.Green)
                               : Material.foreground
                        font.pixelSize: 16
                    }
                    Label { text: "Stress test" }
                }
            }
        }

        // Device info card in report
        DeviceInfoCard {
            visible: Backend.connected
            Layout.fillWidth: true
        }

        Item { Layout.fillHeight: true }

        // Generation controls
        RowLayout {
            spacing: 12

            Button {
                text: "Export PDF…"
                highlighted: true
                onClicked: pdfDialog.open()
            }

            Button {
                text: "Export Markdown…"
                onClicked: mdDialog.open()
            }

            Item { Layout.fillWidth: true }

            Label {
                id: genStatus
                font.pixelSize: 12
            }
        }
    }
}

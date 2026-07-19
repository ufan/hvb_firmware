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
                    id: boardSerialField
                    Layout.fillWidth: true
                    placeholderText: "e.g. HVB-001-2026"
                    // Binding on text (rather than a plain text: assignment) so a
                    // backend-side change to reportBoardSerial can still reach this
                    // field — a plain `text: Backend.reportBoardSerial` binding is
                    // destroyed the instant the user's own typing sets `text`
                    // imperatively, same QQC2 behavior as ComboBox/SpinBox.
                    Binding on text {
                        value: Backend.reportBoardSerial
                        when: !boardSerialField.activeFocus
                    }
                    onTextChanged: if (boardSerialField.activeFocus) Backend.reportBoardSerial = text
                }

                Label { text: "Operator ID:" }
                TextField {
                    id: operatorIdField
                    Layout.fillWidth: true
                    placeholderText: "e.g. J.Smith"
                    Binding on text {
                        value: Backend.reportOperatorId
                        when: !operatorIdField.activeFocus
                    }
                    onTextChanged: if (operatorIdField.activeFocus) Backend.reportOperatorId = text
                }

                Label { text: "Notes:" }
                TextField {
                    id: notesField
                    Layout.fillWidth: true
                    placeholderText: "Optional remarks"
                    Binding on text {
                        value: Backend.reportNotes
                        when: !notesField.activeFocus
                    }
                    onTextChanged: if (notesField.activeFocus) Backend.reportNotes = text
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

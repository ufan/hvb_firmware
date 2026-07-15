import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory

// Step 2: Unlock the calibration door and enter cal mode (atomic)
Item {
    id: root
    signal entered
    signal rollback

    Connections {
        target: Backend
        function onCalStateChanged() {
            if (Backend.calActive) root.entered()
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 500)
        spacing: 16

        Label {
            text: "Unlock Calibration Mode"
            font.pixelSize: 22
            font.bold: true
        }

        Pane {
            Layout.fillWidth: true
            Material.elevation: 2

            ColumnLayout {
                width: parent.width
                spacing: 8

                Label {
                    text: "This will send the two-step unlock sequence\n(0xCA1B → 0xA11B) then enter calibration mode.\n\nMake sure there is no load on any channel."
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    opacity: 0.85
                }

                Label {
                    text: "⚠ All outputs will be safe-off during calibration"
                    color: Material.color(Material.Orange)
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        RowLayout {
            spacing: 12
            Layout.fillWidth: true

            Button {
                text: "← Back"
                onClicked: {
                    Backend.rollbackToConnect()
                    root.rollback()
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Unlock & Enter Cal Mode"
                highlighted: true
                enabled: Backend.connected && !Backend.calActive
                onClicked: Backend.unlockAndEnter()
            }
        }
    }
}

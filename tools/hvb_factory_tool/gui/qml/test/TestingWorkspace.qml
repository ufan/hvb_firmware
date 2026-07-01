import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import HvbFactory

// Testing workspace — no strict step order, channel selection free-form
Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: "Testing"
            font.pixelSize: 22
            font.bold: true
        }

        // Warning if not connected
        Pane {
            visible: !Backend.connected
            Layout.fillWidth: true
            Material.elevation: 1
            Label {
                text: "⚠ Not connected to device. Switch to Calibration → Connect first."
                color: Material.color(Material.Orange)
                wrapMode: Text.WordWrap
                width: parent.width
            }
        }

        TabBar {
            id: tabBar
            Layout.fillWidth: true

            TabButton { text: "Functional Test" }
            TabButton { text: "Stress Test" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            FunctionalTestPage { }
            StressTestPage { }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory

Rectangle {
    id: root
    width: 160
    color: Qt.rgba(1, 1, 1, 0.04)

    signal modeSelected(string mode)
    required property string currentMode

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Label {
            text: "WORK MODE"
            font.pixelSize: 10
            opacity: 0.5
            topPadding: 8
            leftPadding: 4
        }

        SidebarButton {
            label: "Calibration"
            icon: "⚙"
            active: root.currentMode === "cal"
            onClicked: root.modeSelected("cal")
        }

        SidebarButton {
            label: "Testing"
            icon: "✓"
            active: root.currentMode === "test"
            onClicked: root.modeSelected("test")
        }

        SidebarButton {
            label: "Report"
            icon: "📄"
            active: root.currentMode === "report"
            onClicked: root.modeSelected("report")
        }

        Item { Layout.fillHeight: true }

        // Connection status indicator
        Rectangle {
            Layout.fillWidth: true
            height: 32
            radius: 4
            color: Backend.connected ? "#1a4a1a" : "#3a1a1a"
            border.color: Backend.connected ? "#4CAF50" : "#f44336"
            border.width: 1

            Label {
                anchors.centerIn: parent
                text: Backend.connected ? "● Connected" : "○ Disconnected"
                color: Backend.connected ? "#4CAF50" : "#f44336"
                font.pixelSize: 11
            }
        }

        Item { height: 8 }
    }

    // Inline helper component — avoids separate file for a trivial button
    component SidebarButton: ItemDelegate {
        required property string label
        required property string icon
        required property bool active

        Layout.fillWidth: true
        highlighted: active
        text: icon + "  " + label
        font.pixelSize: 13
    }
}

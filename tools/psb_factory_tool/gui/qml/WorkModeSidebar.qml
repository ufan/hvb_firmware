import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory
import "components"

Rectangle {
    id: root
    width: 160
    color: Qt.rgba(1, 1, 1, 0.04)

    signal modeSelected(string mode)
    required property string currentMode

    // Backend has no explicit "connecting" property — derived the same way
    // as demo_gui's main.qml, from the transient status message connectToDevice()
    // sets before dispatching the async connect.
    property bool connecting: false
    Connections {
        target: Backend
        function onStatusMessageChanged() {
            root.connecting = (Backend.statusMessage === "Connecting...")
        }
        function onConnectedChanged() {
            if (Backend.connected) root.connecting = false
        }
    }

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
            iconText: "⚙"
            active: root.currentMode === "cal"
            onClicked: root.modeSelected("cal")
        }

        SidebarButton {
            label: "Testing"
            iconText: "✓"
            active: root.currentMode === "test"
            onClicked: root.modeSelected("test")
        }

        SidebarButton {
            label: "Report"
            iconText: "📄"
            active: root.currentMode === "report"
            onClicked: root.modeSelected("report")
        }

        Item { Layout.fillHeight: true }

        // Connection status indicator — 3-state (offline/connecting/connected)
        // breathing dot, matching demo_gui's menu bar indicator, instead of a
        // static 2-state box with no feedback while a connect is in flight.
        Rectangle {
            Layout.fillWidth: true
            height: 32
            radius: 4
            color: Backend.connected ? "#1a4a1a" : root.connecting ? "#4a3a1a" : "#3a1a1a"
            border.color: Backend.connected ? "#4CAF50" : root.connecting ? "#FFC107" : "#f44336"
            border.width: 1

            RowLayout {
                anchors.centerIn: parent
                spacing: 6
                BreathingIndicator {
                    connected: Backend.connected
                    connecting: root.connecting
                }
                Label {
                    text: Backend.connected ? "Connected" : root.connecting ? "Connecting..." : "Disconnected"
                    color: Backend.connected ? "#4CAF50" : root.connecting ? "#FFC107" : "#f44336"
                    font.pixelSize: 11
                }
            }
        }

        Item { height: 8 }
    }

    // Inline helper component — avoids separate file for a trivial button
    component SidebarButton: ItemDelegate {
        required property string label
        required property string iconText
        required property bool active

        Layout.fillWidth: true
        highlighted: active
        text: iconText + "  " + label
        font.pixelSize: 13
    }
}

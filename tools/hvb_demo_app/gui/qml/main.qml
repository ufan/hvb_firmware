import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    visible: true
    width: 960
    height: 700
    title: "HVB Modbus Tool"

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        // Connection bar
        ConnectionBar { Layout.fillWidth: true }

        // Tab bar — first two tabs are always present; channel tabs appear after connect.
        TabBar {
            id: tabBar
            Layout.fillWidth: true
            TabButton { text: "System Info" }
            TabButton { text: "System Config" }
            Repeater {
                model: backend.channelCount
                TabButton { text: "Channel " + index }
            }
        }

        // Tab content — StackLayout index tracks tabBar.
        StackLayout {
            id: tabContent
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            SystemInfoTab { }
            SystemConfigTab { }
            Repeater {
                model: backend.channelCount
                ChannelTab { channelIndex: index }
            }
        }

        // Status bar
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: backend.statusMessage
                color: backend.connected ? "#4CAF50" : "#F44336"
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Button {
                text: "Debug"
                onClicked: rawDebugDialog.open()
                visible: backend.connected
            }
            Button {
                text: "Refresh"
                onClicked: backend.refreshAll()
                visible: backend.connected
            }
        }

        // Raw log panel
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            clip: true

            TextArea {
                id: rawLogArea
                readOnly: true
                font.family: "monospace"
                font.pixelSize: 11
                text: backend.rawLog
                wrapMode: TextEdit.NoWrap
                background: Rectangle { color: "#1a1a2e" }
                color: "#cccccc"
            }
        }
    }

    // Connections for auto-scroll on new log
    Connections {
        target: backend
        function onRawLogChanged() {
            rawLogArea.cursorPosition = rawLogArea.length - 1
        }
    }

    RawDebugDialog { id: rawDebugDialog }
}

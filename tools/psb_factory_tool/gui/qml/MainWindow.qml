import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory
import "cal"
import "test"

ApplicationWindow {
    id: root
    visible: true
    width: 1200
    height: 800
    title: "HVB Factory Tool"

    Material.theme: Material.Dark
    Material.accent: Material.Cyan

    property string workMode: "cal"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 0

            Label {
                text: "HVB Factory Tool"
                font.bold: true
                font.pixelSize: 16
                leftPadding: 12
            }

            Item { Layout.fillWidth: true }

            Label {
                text: Backend.statusMessage
                elide: Text.ElideRight
                Layout.maximumWidth: 400
                leftPadding: 8
                rightPadding: 8
                opacity: 0.8
            }

            ToolButton {
                enabled: Backend.connected
                text: "SAFE ALL"
                Material.foreground: Material.Red
                onClicked: Backend.safeAll()
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        WorkModeSidebar {
            id: sidebar
            Layout.fillHeight: true
            currentMode: root.workMode
            onModeSelected: function(mode) { root.workMode = mode }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.workMode === "cal" ? 0
                        : root.workMode === "test" ? 1 : 2

            CalibrationWorkspace { }
            TestingWorkspace { }
            ReportPage { }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: root
    modal: true
    anchors.centerIn: parent
    padding: 16
    closePolicy: Popup.CloseOnEscape

    ColumnLayout {
        spacing: 12
        width: 260

        Label {
            text: "System Config"
            font.bold: true
            font.pixelSize: 14
            Layout.alignment: Qt.AlignHCenter
        }

        GridLayout {
            columns: 2
            rowSpacing: 8
            columnSpacing: 8

            Label { text: "Working Mode" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Normal", "Automatic"]
                currentIndex: backend.sysConfig.operatingMode || 0
                onActivated: backend.writeOperatingMode(currentIndex)
            }

            Label { text: "Startup Policy" }
            ComboBox {
                Layout.fillWidth: true
                model: ["Load NVS Config", "Factory Default"]
                currentIndex: backend.sysConfig.startupChannelPolicy || 0
                onActivated: backend.writeStartupChannelPolicy(currentIndex)
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8

            Button { text: "Save";    onClicked: backend.saveSystem() }
            Button { text: "Load";    onClicked: backend.loadSystem() }
            Button {
                text: "Factory"
                Material.background: Material.Red
                onClicked: backend.factoryResetSystem()
            }
        }

        Button {
            text: "Close"
            Layout.alignment: Qt.AlignHCenter
            onClicked: root.close()
        }
    }
}

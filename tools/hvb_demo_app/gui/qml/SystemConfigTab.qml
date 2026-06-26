import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

ScrollView {
    clip: true

    GridLayout {
        columns: 4
        columnSpacing: 12
        rowSpacing: 8
        anchors.margins: 12

        // Operating Mode
        Label { text: "Operating Mode:" }
        ComboBox {
            model: ["Normal", "Automatic"]
            currentIndex: backend.sysConfig.operatingMode || 0
            onCurrentIndexChanged: backend.writeOperatingMode(currentIndex)
        }
        Label { text: "Slave Addr:" }
        SpinBox {
            from: 0; to: 247
            value: backend.sysConfig.slaveAddr || 1
            onValueChanged: backend.writeSlaveAddress(value)
            Layout.preferredWidth: 80
        }

        // Baud Rate
        Label { text: "Baud Rate:" }
        ComboBox {
            model: ["115200", "9600"]
            currentIndex: backend.sysConfig.baudRateCode || 0
            onCurrentIndexChanged: backend.writeBaudRate(currentIndex)
            Layout.preferredWidth: 100
        }

        // Startup Channel Policy (v3: recovery/safe-band moved to per-channel)
        Label { text: "Startup Policy:" }
        ComboBox {
            model: ["Load NVS Config", "Factory Default"]
            currentIndex: backend.sysConfig.startupChannelPolicy || 0
            onCurrentIndexChanged: backend.writeStartupChannelPolicy(currentIndex)
            Layout.preferredWidth: 150
        }

        // Spacer
        Item { Layout.columnSpan: 2; Layout.preferredHeight: 4 }

        // Actions
        Item { Layout.columnSpan: 4; Layout.preferredHeight: 8 }
        Row {
            Layout.columnSpan: 4
            spacing: 10
            Button { text: "Save System"; onClicked: backend.saveSystem() }
            Button { text: "Load System"; onClicked: backend.loadSystem() }
            Button { text: "Factory Reset"; onClicked: backend.factoryResetSystem()
                palette.button: "#F44336"; palette.buttonText: "white" }
            Button { text: "Software Reset"; onClicked: backend.softwareReset()
                palette.button: "#E53935"; palette.buttonText: "white" }
        }
    }
}

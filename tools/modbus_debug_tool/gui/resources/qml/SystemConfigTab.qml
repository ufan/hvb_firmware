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

        // Recovery group header
        Label { text: "Recovery Policy:"; font.bold: true }
        Item { Layout.columnSpan: 3 }

        Label { text: "Policy:" }
        ComboBox {
            id: recoveryCb
            model: ["ManualLatch", "AutoRetry", "AutoDerate", "NeverRetry"]
            currentIndex: backend.sysConfig.recoveryPolicy || 0
            Layout.preferredWidth: 130
        }
        Label { text: "Retry Delay (s):" }
        SpinBox { id: retryD; from: 0; to: 3600; value: backend.sysConfig.retryDelay || 0 }

        Label { text: "Retry Max:" }
        SpinBox { id: retryM; from: 0; to: 100; value: backend.sysConfig.retryMax || 0 }
        Label { text: "Retry Window (s):" }
        SpinBox { id: retryW; from: 0; to: 86400; value: backend.sysConfig.retryWindow || 0 }

        Button {
            text: "Apply Recovery"
            onClicked: backend.writeRecoveryPolicy(recoveryCb.currentIndex, retryD.value, retryM.value, retryW.value)
        }

        // Safe bands
        Label { text: "" }
        Label { text: "" }
        Label { text: "" }

        Label { text: "V Safe Band (%):" }
        SpinBox { id: vBand; from: 0; to: 50; value: backend.sysConfig.voltageSafeBandPct || 10 }
        Label { text: "I Safe Band (%):" }
        SpinBox { id: iBand; from: 0; to: 50; value: backend.sysConfig.currentSafeBandPct || 10 }

        Button {
            text: "Apply Safe Bands"
            onClicked: backend.writeSafeBands(vBand.value, iBand.value)
        }

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

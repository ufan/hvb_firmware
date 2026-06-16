import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

ScrollView {
    clip: true
    property int channelIndex: 0

    function info() { return channelIndex === 0 ? (backend.ch0Info || {}) : (backend.ch1Info || {}) }
    function cfg()  { return channelIndex === 0 ? (backend.ch0Config || {}) : (backend.ch1Config || {}) }

    RowLayout {
        anchors.margins: 12
        spacing: 20

        ColumnLayout {
            Layout.preferredWidth: 350
            spacing: 6

            Label { text: "Channel " + channelIndex + " Measurements"; font.bold: true; font.pixelSize: 14 }

            ReadOnlyField { label: "Measured V (raw)"; value: (info().voltageRaw || 0) + " LSB" }
            ReadOnlyField { label: "Measured V (est)"; value: (info().voltageV || 0).toFixed(1) + " V" }
            ReadOnlyField { label: "Measured I (raw)"; value: (info().currentRaw || 0) + " LSB" }
            ReadOnlyField { label: "Measured I (est)"; value: {
                    var a = info().currentA || 0
                    if (Math.abs(a) >= 1) return a.toFixed(3) + " A"
                    else if (Math.abs(a) >= 1e-3) return (a*1e3).toFixed(2) + " mA"
                    else return (a*1e6).toFixed(1) + " uA"
                }}
            ReadOnlyField { label: "Op Target (raw)"; value: (info().operationalTargetVRaw || 0) + " LSB" }
            ReadOnlyField { label: "Op Target (est)"; value: (info().operationalTargetV || 0).toFixed(1) + " V" }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            Label { text: "Status"; font.bold: true }
            ReadOnlyField { label: "Reg"; value: "0x" + ((info().status || 0) >>> 0).toString(16) }
            StatusBadge { active: info().statusOutDrive || false; label: "Output Drive" }
            StatusBadge { active: info().statusOutEn || false; label: "Output Enable" }
            StatusBadge { active: info().statusRamping || false; label: "Ramping" }
            StatusBadge { active: info().statusActiveFault || false; label: "Active Fault"; color: "red" }
            StatusBadge { active: info().statusFaultHistory || false; label: "Fault History"; color: "orange" }
            StatusBadge { active: info().statusCooldown || false; label: "Cooldown"; color: "orange" }
            StatusBadge { active: info().statusRetryExhausted || false; label: "Retry Exhausted"; color: "red" }
            StatusBadge { active: info().statusUnsupported || false; label: "Unsupported"; color: "gray" }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            Label { text: "Faults"; font.bold: true }
            StatusBadge { active: info().faultVLimit || false; label: "Voltage Limit"; color: "red" }
            StatusBadge { active: info().faultILimit || false; label: "Current Limit"; color: "red" }
            StatusBadge { active: info().faultMeasInvalid || false; label: "Meas Invalid"; color: "red" }
            StatusBadge { active: info().faultHw || false; label: "HW Fault"; color: "red" }
            StatusBadge { active: info().faultInterlock || false; label: "Interlock"; color: "red" }
            StatusBadge { active: info().faultRetryExhausted || false; label: "Retry Exh"; color: "orange" }
            StatusBadge { active: info().faultConfigInvalid || false; label: "Config Invalid"; color: "orange" }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            ReadOnlyField { label: "Retry Count"; value: (info().retryCount || 0).toString() }
            ReadOnlyField { label: "Cooldown"; value: (info().cooldownSec || 0) + " s" }
            ReadOnlyField { label: "Last Fault TS"; value: (info().lastFaultTimestamp || 0) + " s" }

            Row { spacing: 8
                StatusBadge { active: info().capOutEn || false; label: "OutEn" }
                StatusBadge { active: info().capCurrentMeas || false; label: "CurrMeas" }
                StatusBadge { active: info().capAutoRec || false; label: "AutoRec" }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            Label { text: "Configuration (raw LSB)"; font.bold: true; font.pixelSize: 14 }

            RowLayout {
                Label { text: "Target (raw):"; Layout.preferredWidth: 100 }
                TextField { id: targetVRaw; Layout.preferredWidth: 80; text: (cfg().configuredTargetVRaw || 0).toString() }
                Label { text: "≈ " + ((cfg().configuredTargetV || 0).toFixed(1)) + " V" }
                Button { text: "Set"; onClicked: backend.writeTargetVoltage(channelIndex, parseInt(targetVRaw.text) || 0) }
            }

            RowLayout {
                Label { text: "Output Action:"; Layout.preferredWidth: 100 }
                ComboBox { id: outActionCb; model: ["None", "Enable", "Disable Graceful", "Disable Immediate"]
                    Layout.preferredWidth: 150 }
                Button { text: "Send"; onClicked: backend.sendOutputAction(channelIndex, outActionCb.currentIndex) }
            }

            Row { spacing: 8
                Button { text: "Clear Active Fault"; onClicked: backend.sendFaultCmd(channelIndex, 1) }
                Button { text: "Clear Fault History"; onClicked: backend.sendFaultCmd(channelIndex, 2) }
            }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            Label { text: "Ramping (raw)"; font.bold: true }
            RowLayout {
                Label { text: "Up:"; Layout.preferredWidth: 40 }
                TextField { id: rampUpStep; Layout.preferredWidth: 60; text: (cfg().rampUpStepRaw || 0).toString() }
                Label { text: " LSB /" }
                TextField { id: rampUpInt; Layout.preferredWidth: 60; text: (cfg().rampUpInterval || 0).toString() }
                Label { text: " x10s" }
                Button { text: "Set"; onClicked: backend.writeRampUp(channelIndex, parseInt(rampUpStep.text)||0, parseInt(rampUpInt.text)||0) }
            }
            RowLayout {
                Label { text: "Down:"; Layout.preferredWidth: 40 }
                TextField { id: rampDnStep; Layout.preferredWidth: 60; text: (cfg().rampDownStepRaw || 0).toString() }
                Label { text: " LSB /" }
                TextField { id: rampDnInt; Layout.preferredWidth: 60; text: (cfg().rampDownInterval || 0).toString() }
                Label { text: " x10s" }
                Button { text: "Set"; onClicked: backend.writeRampDown(channelIndex, parseInt(rampDnStep.text)||0, parseInt(rampDnInt.text)||0) }
            }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            Label { text: "V Protection (raw)"; font.bold: true }
            RowLayout {
                ComboBox { id: vProtMode; model: ["Disabled", "Flag Only", "Apply Action"]
                    currentIndex: cfg().vProtMode || 0; Layout.preferredWidth: 120 }
                ComboBox { id: vProtAct; model: ["None", "Disable Graceful", "Disable Immediate", "Force Zero", "Clamp"]
                    currentIndex: (cfg().vProtOutputAction || 0); Layout.preferredWidth: 150 }
                TextField { id: vLimitRaw; Layout.preferredWidth: 80; text: (cfg().vLimitThresholdRaw || 0).toString() }
                Label { text: "≈ " + ((cfg().vLimitThresholdV || 0).toFixed(1)) + " V" }
                Button { text: "Set"; onClicked: backend.writeVoltageProtection(channelIndex, vProtMode.currentIndex, vProtAct.currentIndex, parseInt(vLimitRaw.text)||0) }
            }

            Label { text: "I Protection (raw)"; font.bold: true }
            RowLayout {
                ComboBox { id: iProtMode; model: ["Disabled", "Flag Only", "Apply Action"]
                    currentIndex: cfg().iProtMode || 0; Layout.preferredWidth: 120 }
                ComboBox { id: iProtAct; model: ["None", "Disable Graceful", "Disable Immediate", "Force Zero"]
                    currentIndex: (cfg().iProtOutputAction || 0); Layout.preferredWidth: 150 }
                TextField { id: iLimitRaw; Layout.preferredWidth: 80; text: (cfg().iLimitThresholdRaw || 0).toString() }
                Label { text: "≈ " + ((cfg().iLimitThresholdA || 0).toExponential(3)) + " A" }
                Button { text: "Set"; onClicked: backend.writeCurrentProtection(channelIndex, iProtMode.currentIndex, iProtAct.currentIndex, parseInt(iLimitRaw.text)||0) }
            }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            RowLayout {
                Label { text: "Derate (raw):"; Layout.preferredWidth: 100 }
                TextField { id: derateRaw; Layout.preferredWidth: 80; text: (cfg().derateStepRaw || 0).toString() }
                Label { text: "≈ " + ((cfg().derateStepV || 0).toFixed(1)) + " V" }
                Button { text: "Set"; onClicked: backend.writeDerateStep(channelIndex, parseInt(derateRaw.text)||0) }
            }

            Label { text: "Calibration"; font.bold: true }
            GridLayout {
                columns: 5
                Label { text: "Output" }
                Label { text: "K:" }
                TextField { id: outCalK; Layout.preferredWidth: 60; text: cfg().outCalK || 10000 }
                Label { text: "B:" }
                TextField { id: outCalB; Layout.preferredWidth: 60; text: cfg().outCalB || 0 }
                Button { text: "Set"; onClicked: backend.writeCalOutput(channelIndex, parseInt(outCalK.text)||10000, parseInt(outCalB.text)||0) }

                Label { text: "Meas V" }
                Label { text: "K:" }
                TextField { id: mvCalK; Layout.preferredWidth: 60; text: cfg().measVCalK || 10000 }
                Label { text: "B:" }
                TextField { id: mvCalB; Layout.preferredWidth: 60; text: cfg().measVCalB || 0 }
                Button { text: "Set"; onClicked: backend.writeCalMeasV(channelIndex, parseInt(mvCalK.text)||10000, parseInt(mvCalB.text)||0) }

                Label { text: "Meas I" }
                Label { text: "K:" }
                TextField { id: miCalK; Layout.preferredWidth: 60; text: cfg().measICalK || 10000 }
                Label { text: "B:" }
                TextField { id: miCalB; Layout.preferredWidth: 60; text: cfg().measICalB || 0 }
                Button { text: "Set"; onClicked: backend.writeCalMeasI(channelIndex, parseInt(miCalK.text)||10000, parseInt(miCalB.text)||0) }
            }

            Rectangle { Layout.preferredHeight: 1; Layout.fillWidth: true; color: "#ddd" }

            Row {
                spacing: 8
                Button { text: "Save CH"; onClicked: backend.saveChannel(channelIndex) }
                Button { text: "Load CH"; onClicked: backend.loadChannel(channelIndex) }
                Button { text: "Factory CH"; onClicked: backend.factoryResetChannel(channelIndex)
                    palette.button: "#F44336"; palette.buttonText: "white" }
            }
        }
    }
}

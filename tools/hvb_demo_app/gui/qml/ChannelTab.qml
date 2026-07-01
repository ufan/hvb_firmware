import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import HvbTool
import "components"

ScrollView {
    id: root
    clip: true

    required property int channelIndex

    property var ci: (backend.channelInfoList   || [])[channelIndex] || {}
    property var cc: (backend.channelConfigList || [])[channelIndex] || {}
    property int caps: ci.chCapFlags || 0xFFFF

    property bool hasOutEn: (caps & 0x0001) !== 0
    property bool hasVolt:  (caps & 0x0004) !== 0
    property bool hasCurr:  (caps & 0x0008) !== 0

    property int windowMinutes: 5

    ColumnLayout {
        width: root.width
        spacing: 8

        // Live status panel
        Pane {
            Layout.fillWidth: true
            padding: 8

            RowLayout {
                spacing: 16
                anchors.fill: parent

                RowLayout {
                    spacing: 4
                    visible: root.hasOutEn
                    Label { text: "Vset:"; opacity: 0.6 }
                    TextField {
                        implicitWidth: 80
                        placeholderText: "V"
                        text: root.ci.operationalTargetV !== undefined
                            ? root.ci.operationalTargetV.toFixed(1) : ""
                        onAccepted: backend.writeTargetVoltage(root.channelIndex,
                            Math.round((parseFloat(text) || 0) / 0.1))
                    }
                    Label { text: "V"; opacity: 0.6 }
                }

                LabeledValue {
                    label: "Vop"
                    value: root.ci.operationalTargetV !== undefined
                        ? root.ci.operationalTargetV.toFixed(1) + " V" : "--"
                    visible: root.hasVolt
                }

                LabeledValue {
                    label: "V"
                    value: root.ci.voltageV !== undefined
                        ? root.ci.voltageV.toFixed(2) + " V" : "--"
                    visible: root.hasVolt
                }

                LabeledValue {
                    label: "I"
                    value: root.ci.currentRaw !== undefined
                        ? root.ci.currentRaw + " nA" : "--"
                    visible: root.hasCurr
                }

                StatusBadge {
                    active: root.ci.statusOutEn || false
                    label: root.ci.statusRamping ? "RAMP"
                         : (root.ci.statusOutDrive ? "ON" : "OFF")
                }

                LabeledValue {
                    label: "Retries"
                    value: (root.ci.retryCount || 0).toString()
                }
            }
        }

        // Control + Protection row
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            GroupBox {
                title: "Control"
                Layout.fillWidth: true
                visible: root.hasOutEn

                ColumnLayout {
                    RowLayout {
                        Button {
                            text: "Enable"
                            onClicked: backend.sendOutputAction(root.channelIndex, 1)
                        }
                        Button {
                            text: "Disable"
                            onClicked: backend.sendOutputAction(root.channelIndex, 2)
                        }
                        Button {
                            text: "Kill"
                            Material.background: Material.Red
                            onClicked: backend.sendOutputAction(root.channelIndex, 3)
                        }
                    }
                    RowLayout {
                        Label { text: "Ru:" }
                        TextField {
                            implicitWidth: 70
                            placeholderText: "V/step"
                            text: root.cc.rampUpStepRaw !== undefined
                                ? (root.cc.rampUpStepRaw * 0.1).toFixed(1) : ""
                            onAccepted: backend.writeRampUp(root.channelIndex,
                                Math.round((parseFloat(text) || 0) / 0.1),
                                root.cc.rampUpInterval || 1)
                        }
                        Label { text: "Rd:" }
                        TextField {
                            implicitWidth: 70
                            placeholderText: "V/step"
                            text: root.cc.rampDownStepRaw !== undefined
                                ? (root.cc.rampDownStepRaw * 0.1).toFixed(1) : ""
                            onAccepted: backend.writeRampDown(root.channelIndex,
                                Math.round((parseFloat(text) || 0) / 0.1),
                                root.cc.rampDownInterval || 1)
                        }
                    }
                }
            }

            GroupBox {
                title: "Protection"
                Layout.fillWidth: true
                visible: root.hasCurr

                ColumnLayout {
                    RowLayout {
                        Label { text: "I-Limit (nA):" }
                        TextField {
                            implicitWidth: 80
                            text: (root.cc.iLimitThresholdRaw || 0).toString()
                            onAccepted: backend.writeCurrentProtection(root.channelIndex,
                                root.cc.iProtMode || 0,
                                root.cc.iProtOutputAction || 0,
                                parseInt(text) || 0)
                        }
                    }
                    RowLayout {
                        ComboBox {
                            model: ["Disabled", "FlagOnly", "Apply-Action"]
                            currentIndex: root.cc.iProtMode || 0
                            onActivated: backend.writeCurrentProtection(root.channelIndex,
                                currentIndex,
                                root.cc.iProtOutputAction || 0,
                                root.cc.iLimitThresholdRaw || 0)
                        }
                        ComboBox {
                            model: ["None", "Dis-Graceful", "Dis-Immed", "ForceZero"]
                            currentIndex: root.cc.iProtOutputAction || 0
                            onActivated: backend.writeCurrentProtection(root.channelIndex,
                                root.cc.iProtMode || 0,
                                currentIndex,
                                root.cc.iLimitThresholdRaw || 0)
                        }
                    }
                    RowLayout {
                        Label { text: "Safe Band %:" }
                        SpinBox {
                            from: 0; to: 50
                            value: root.cc.currentSafeBandPct || 10
                            onValueModified: backend.writeChannelSafeBand(root.channelIndex, value)
                        }
                    }
                    RowLayout {
                        Button {
                            text: "Clr Active"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 1)
                        }
                        Button {
                            text: "Clr History"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 2)
                        }
                    }
                }
            }
        }

        // Recovery + Persistence row
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            GroupBox {
                title: "Recovery"
                Layout.fillWidth: true

                ColumnLayout {
                    RowLayout {
                        Label { text: "Policy:" }
                        ComboBox {
                            model: ["ManualLatch", "AutoRetry", "AutoDerate", "NeverRetry"]
                            currentIndex: root.cc.recoveryPolicyMode || 0
                            onActivated: backend.writeChannelRecovery(root.channelIndex,
                                currentIndex,
                                root.cc.autoRetryDelay    || 0,
                                root.cc.autoRetryMaxCount || 0,
                                root.cc.autoRetryWindow   || 0)
                        }
                    }
                    RowLayout {
                        Label { text: "Dly:" }
                        SpinBox { from: 0; to: 3600; value: root.cc.autoRetryDelay || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0, value,
                                root.cc.autoRetryMaxCount || 0, root.cc.autoRetryWindow || 0) }
                        Label { text: "Max:" }
                        SpinBox { from: 0; to: 100; value: root.cc.autoRetryMaxCount || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, value, root.cc.autoRetryWindow || 0) }
                        Label { text: "Win:" }
                        SpinBox { from: 0; to: 86400; value: root.cc.autoRetryWindow || 0
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, root.cc.autoRetryMaxCount || 0, value) }
                    }
                    RowLayout {
                        Label { text: "Derate (LSB):" }
                        TextField {
                            implicitWidth: 80
                            text: (root.cc.derateStepRaw || 0).toString()
                            onAccepted: backend.writeDerateStep(root.channelIndex, parseInt(text) || 0)
                        }
                    }
                }
            }

            GroupBox {
                title: "Persistence"

                ColumnLayout {
                    Button { text: "Save";    Layout.fillWidth: true
                        onClicked: backend.saveChannel(root.channelIndex) }
                    Button { text: "Load";    Layout.fillWidth: true
                        onClicked: backend.loadChannel(root.channelIndex) }
                    Button { text: "Factory"; Layout.fillWidth: true
                        Material.background: Material.Red
                        onClicked: backend.factoryResetChannel(root.channelIndex) }
                }
            }
        }

        // Graphs row
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            height: 260

            ChannelGraph {
                Layout.fillWidth: true
                Layout.fillHeight: true
                channelIndex: root.channelIndex
                title: "Voltage"
                windowMinutes: root.windowMinutes
                seriesConfigs: [
                    { name: "Vset", color: "#2196F3", valueKey: "configuredTargetV",  source: "config" },
                    { name: "Vop",  color: "#00BCD4", valueKey: "operationalTargetV", source: "info"   },
                    { name: "V",    color: "#4CAF50", valueKey: "voltageV",            source: "info"   }
                ]
            }

            ChannelGraph {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.hasCurr
                channelIndex: root.channelIndex
                title: "Current"
                windowMinutes: root.windowMinutes
                seriesConfigs: [
                    { name: "I",     color: "#FF9800", valueKey: "currentRaw",         source: "info"   },
                    { name: "Limit", color: "#F44336", valueKey: "iLimitThresholdRaw", source: "config" }
                ]
            }
        }
    }
}

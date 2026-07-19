import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbTool
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
                    spacing: 6
                    visible: root.hasOutEn
                    Label { text: "Vset:"; opacity: 0.6 }
                    TextField {
                        id: vsetField
                        implicitWidth: 100
                        placeholderText: "V"
                        // Binding on text (rather than a plain text: assignment) so the
                        // live-polled value only overwrites the field while the user
                        // isn't editing it — otherwise a poll tick landing mid-edit
                        // silently clobbers whatever the user just typed.
                        Binding on text {
                            value: root.ci.operationalTargetV !== undefined
                                ? root.ci.operationalTargetV.toFixed(1) : ""
                            when: !vsetField.activeFocus
                        }
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

        // Control + Protection + Recovery + Persistence — one row of four
        // compact, evenly-sized GroupBoxes instead of a 2x2 grid, so the
        // parameter section takes less vertical space and the graphs below
        // get more room.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            GroupBox {
                title: "Control"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                visible: root.hasOutEn

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    RowLayout {
                        spacing: 6
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
                        spacing: 6
                        Label { text: "Ru:" }
                        TextField {
                            id: ruField
                            Layout.fillWidth: true
                            implicitWidth: 85
                            placeholderText: "V/step"
                            Binding on text {
                                value: root.cc.rampUpStepRaw !== undefined
                                    ? (root.cc.rampUpStepRaw * 0.1).toFixed(1) : ""
                                when: !ruField.activeFocus
                            }
                            onAccepted: backend.writeRampUp(root.channelIndex,
                                Math.round((parseFloat(text) || 0) / 0.1),
                                root.cc.rampUpInterval || 1)
                        }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Rd:" }
                        TextField {
                            id: rdField
                            Layout.fillWidth: true
                            implicitWidth: 85
                            placeholderText: "V/step"
                            Binding on text {
                                value: root.cc.rampDownStepRaw !== undefined
                                    ? (root.cc.rampDownStepRaw * 0.1).toFixed(1) : ""
                                when: !rdField.activeFocus
                            }
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
                Layout.preferredWidth: 1
                visible: root.hasCurr

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    RowLayout {
                        spacing: 6
                        Label { text: "I-Limit (nA):" }
                        TextField {
                            id: iLimitField
                            Layout.fillWidth: true
                            implicitWidth: 100
                            Binding on text {
                                value: (root.cc.iLimitThresholdRaw || 0).toString()
                                when: !iLimitField.activeFocus
                            }
                            onAccepted: backend.writeCurrentProtection(root.channelIndex,
                                root.cc.iProtMode || 0,
                                root.cc.iProtOutputAction || 0,
                                parseInt(text) || 0)
                        }
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["Disabled", "FlagOnly", "Apply-Action"]
                        currentIndex: root.cc.iProtMode || 0
                        onActivated: backend.writeCurrentProtection(root.channelIndex,
                            currentIndex,
                            root.cc.iProtOutputAction || 0,
                            root.cc.iLimitThresholdRaw || 0)
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["None", "Dis-Graceful", "Dis-Immed", "ForceZero"]
                        currentIndex: root.cc.iProtOutputAction || 0
                        onActivated: backend.writeCurrentProtection(root.channelIndex,
                            root.cc.iProtMode || 0,
                            currentIndex,
                            root.cc.iLimitThresholdRaw || 0)
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Safe Band %:" }
                        SpinBox {
                            Layout.fillWidth: true
                            from: 0; to: 50
                            implicitWidth: 120
                            value: root.cc.currentSafeBandPct || 10
                            onValueModified: backend.writeChannelSafeBand(root.channelIndex, value)
                        }
                    }
                    RowLayout {
                        spacing: 6
                        Button {
                            Layout.fillWidth: true
                            text: "Clr Active"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 1)
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "Clr History"
                            onClicked: backend.sendFaultCmd(root.channelIndex, 2)
                        }
                    }
                }
            }

            GroupBox {
                title: "Recovery"
                Layout.fillWidth: true
                Layout.preferredWidth: 1

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    RowLayout {
                        spacing: 6
                        Label { text: "Policy:" }
                        ComboBox {
                            Layout.fillWidth: true
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
                        spacing: 6
                        Label { text: "Dly:"; Layout.preferredWidth: 34 }
                        SpinBox { from: 0; to: 3600; value: root.cc.autoRetryDelay || 0
                            Layout.fillWidth: true
                            implicitWidth: 120
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0, value,
                                root.cc.autoRetryMaxCount || 0, root.cc.autoRetryWindow || 0) }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Max:"; Layout.preferredWidth: 34 }
                        SpinBox { from: 0; to: 100; value: root.cc.autoRetryMaxCount || 0
                            Layout.fillWidth: true
                            implicitWidth: 120
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, value, root.cc.autoRetryWindow || 0) }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Win:"; Layout.preferredWidth: 34 }
                        SpinBox { from: 0; to: 86400; value: root.cc.autoRetryWindow || 0
                            Layout.fillWidth: true
                            implicitWidth: 130
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, root.cc.autoRetryMaxCount || 0, value) }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Derate:" }
                        TextField {
                            id: derateField
                            Layout.fillWidth: true
                            implicitWidth: 100
                            placeholderText: "LSB"
                            Binding on text {
                                value: (root.cc.derateStepRaw || 0).toString()
                                when: !derateField.activeFocus
                            }
                            onAccepted: backend.writeDerateStep(root.channelIndex, parseInt(text) || 0)
                        }
                    }
                }
            }

            GroupBox {
                title: "Persistence"
                Layout.fillWidth: true
                Layout.preferredWidth: 1

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
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

        // Graphs row — sized to roughly half the tab's available height, so
        // trend data (the reason to have a Channel tab at all) dominates the
        // screen instead of the parameter panels above it.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Layout.preferredHeight: Math.max(240, root.height * 0.46)

            ChannelGraph {
                Layout.fillWidth: true
                Layout.fillHeight: true
                channelIndex: root.channelIndex
                title: "Voltage"
                windowMinutes: root.windowMinutes
                seriesConfigs: [
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
                    { name: "I", color: "#FF9800", valueKey: "currentRaw", source: "info" }
                ]
            }
        }
    }
}

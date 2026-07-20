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

    // CH_CAP_OUTPUT_ENABLE ("switchable on/off") and CH_CAP_RAW_OUTPUT_DRIVE
    // ("has a DAC, level is settable") are independent — see
    // docs/guide/channel-capability-model.md §1. jw_lvb ch1-9 have
    // hasOutEn without hasDrive (fixed-voltage, switchable); jw_hvb has
    // both together. Anything gated on hasOutEn alone that should really
    // require a DAC (target voltage, ramp, output calibration) is wrong on
    // that shape of channel — see §7.
    property bool hasOutEn: (caps & 0x0001) !== 0
    property bool hasDrive: (caps & 0x0002) !== 0
    property bool hasVolt:  (caps & 0x0004) !== 0
    property bool hasCurr:  (caps & 0x0008) !== 0

    // SysCap::CALIBRATION_MODE (0x0004) — a system-wide capability, not a
    // per-channel one. Coefficients are read-only here; writing them is a
    // factory-tool-only operation (see docs/guide/calibration-guide.md).
    property bool hasCal: ((backend.sysInfo.sysCapFlags || 0) & 0x0004) !== 0

    property int windowMinutes: 5

    // ComboBox.currentIndex and SpinBox.value destroy their declarative
    // `expr` binding the first time the user interacts with the control
    // (QQC2 sets the property imperatively on selection/+-click), so once
    // that happens the control never reflects backend.channelConfigList
    // again — including the confirmed value from a post-write refresh.
    // channelConfigUpdated (unlike channelDataChanged) fires only when this
    // channel's config actually refreshes — connect-time full read or a
    // post-write narrow refresh — never on a routine 1s status-only poll
    // tick, so resyncing here won't fight the user mid-interaction.
    Connections {
        target: backend
        function onChannelConfigUpdated(ch) {
            if (ch !== root.channelIndex) return
            enSwitch.checked = root.cc.outputEnabledCfg || false
            iModeCombo.currentIndex = root.cc.iProtMode || 0
            iActCombo.currentIndex = root.cc.iProtOutputAction || 0
            safeBandSpin.value = root.cc.currentSafeBandPct || 10
            recovCombo.currentIndex = root.cc.recoveryPolicyMode || 0
            dlySpin.value = root.cc.autoRetryDelay || 0
            maxSpin.value = root.cc.autoRetryMaxCount || 0
            winSpin.value = root.cc.autoRetryWindow || 0
        }
    }

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

                // DAC channels (CH_CAP_RAW_OUTPUT_DRIVE): target-voltage input.
                RowLayout {
                    spacing: 6
                    visible: root.hasDrive
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

                // Fixed-voltage switchable channels (CH_CAP_OUTPUT_ENABLE without
                // CH_CAP_RAW_OUTPUT_DRIVE, e.g. jw_lvb ch1-9): no DAC to set a level
                // on, so this is CFG_OUTPUT_ENABLED (startup intent) as a plain
                // on/off toggle instead — mutually exclusive with the Vset row above
                // by capability. See docs/guide/channel-capability-model.md §2/§7.
                RowLayout {
                    spacing: 6
                    visible: root.hasOutEn && !root.hasDrive
                    Label { text: "Startup:"; opacity: 0.6 }
                    Switch {
                        id: enSwitch
                        text: checked ? "On" : "Off"
                        checked: root.cc.outputEnabledCfg || false
                        onToggled: backend.writeOutputEnabled(root.channelIndex, checked)
                    }
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

                // active/label both come from the backend's capability-aware
                // ci.isOn (psb::channelIsOn, channel_policy.h) — previously
                // active used statusOutEn while label used statusOutDrive,
                // two different bits that can disagree with each other and
                // with MonitorTab's badge on an output-enable-only channel
                // (no drive concept at all). See
                // docs/guide/client-architecture-and-pitfalls.md §2.9.
                StatusBadge {
                    visible: root.hasOutEn || root.hasDrive
                    active: root.ci.isOn || false
                    label: root.ci.statusRamping ? "RAMP"
                         : (root.ci.isOn ? "ON" : "OFF")
                }
                // Neither capability: locked always-on with no software on/off
                // concept at all (e.g. jw_lvb ch0) — not "off", genuinely n/a.
                LabeledValue {
                    label: "Status"
                    value: "n/a"
                    visible: !root.hasOutEn && !root.hasDrive
                }

                LabeledValue {
                    label: "Retries"
                    value: (root.ci.retryCount || 0).toString()
                }
            }
        }

        // Read-only calibration coefficients — advisory/diagnostic info
        // only. Calibration writes are a factory-tool-only operation (see
        // docs/guide/calibration-guide.md); the demo GUI never exposes them
        // as editable fields. Hidden entirely on boards that don't report
        // the calibration capability bit.
        Pane {
            Layout.fillWidth: true
            padding: 8
            visible: root.hasCal

            RowLayout {
                spacing: 16
                anchors.fill: parent

                Label { text: "Calibration (read-only):"; opacity: 0.6; font.italic: true }

                LabeledValue {
                    label: "Out K/e/b"
                    value: root.cc.outCalK !== undefined
                        ? root.cc.outCalK + " / " + root.cc.outCalKExp + " / " + root.cc.outCalB
                        : "--"
                    // OUTPUT_CAL_K/B require RAW_OUTPUT_DRIVE, not OUTPUT_ENABLE
                    // (channel-capability-model.md §2) — there's no output
                    // calibration to speak of on a channel with no DAC.
                    visible: root.hasDrive
                }
                LabeledValue {
                    label: "Vmeas K/e/b"
                    value: root.cc.measVCalK !== undefined
                        ? root.cc.measVCalK + " / " + root.cc.measVCalKExp + " / " + root.cc.measVCalB
                        : "--"
                    visible: root.hasVolt
                }
                LabeledValue {
                    label: "Imeas K/e/b"
                    value: root.cc.measICalK !== undefined
                        ? root.cc.measICalK + " / " + root.cc.measICalKExp + " / " + root.cc.measICalB
                        : "--"
                    visible: root.hasCurr
                }

                Item { Layout.fillWidth: true }
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
                // Enable/Disable/Kill are meaningful whenever there's any output
                // capability at all (Disable is coerced to force-off without a
                // DAC to ramp — channel-capability-model.md §3 rule 2); Ru/Rd
                // below are gated individually on hasDrive since ramping only
                // applies to DAC channels.
                visible: root.hasOutEn || root.hasDrive

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
                        visible: root.hasDrive
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
                        visible: root.hasDrive
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
                        id: iModeCombo
                        Layout.fillWidth: true
                        model: ["Disabled", "FlagOnly", "Apply-Action"]
                        currentIndex: root.cc.iProtMode || 0
                        onActivated: backend.writeCurrentProtection(root.channelIndex,
                            currentIndex,
                            root.cc.iProtOutputAction || 0,
                            root.cc.iLimitThresholdRaw || 0)
                    }
                    ComboBox {
                        id: iActCombo
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
                            id: safeBandSpin
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
                            id: recovCombo
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
                        SpinBox { id: dlySpin; from: 0; to: 3600; value: root.cc.autoRetryDelay || 0
                            Layout.fillWidth: true
                            implicitWidth: 120
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0, value,
                                root.cc.autoRetryMaxCount || 0, root.cc.autoRetryWindow || 0) }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Max:"; Layout.preferredWidth: 34 }
                        SpinBox { id: maxSpin; from: 0; to: 100; value: root.cc.autoRetryMaxCount || 0
                            Layout.fillWidth: true
                            implicitWidth: 120
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, value, root.cc.autoRetryWindow || 0) }
                    }
                    RowLayout {
                        spacing: 6
                        Label { text: "Win:"; Layout.preferredWidth: 34 }
                        SpinBox { id: winSpin; from: 0; to: 86400; value: root.cc.autoRetryWindow || 0
                            Layout.fillWidth: true
                            implicitWidth: 130
                            onValueModified: backend.writeChannelRecovery(root.channelIndex,
                                root.cc.recoveryPolicyMode || 0,
                                root.cc.autoRetryDelay || 0, root.cc.autoRetryMaxCount || 0, value) }
                    }
                    RowLayout {
                        spacing: 6
                        // AUTO_DERATE_STEP requires RAW_OUTPUT_DRIVE *and*
                        // VOLTAGE_MEASUREMENT (channel-capability-model.md §2) —
                        // derating only means something on a DAC channel that can
                        // also see the voltage it's derating.
                        visible: root.hasDrive && root.hasVolt
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

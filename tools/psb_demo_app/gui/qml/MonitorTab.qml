import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PsbTool

Item {
    id: root

    // Column descriptor array — rebuilt from board capability flags; cleared on disconnect.
    // Each entry: {key, label, minWidth, weight}. Rendered width is
    // Math.max(minWidth, weight-proportional share of the available table
    // width) — see colWidth() — so columns stretch to fill wide windows and
    // fall back to their minimum (with a horizontal scrollbar) when narrow.
    property var activeColumns: []

    function computeActiveColumns() {
        if (!backend.connected || backend.channelCount === 0) {
            if (activeColumns.length > 0) activeColumns = []
            return
        }
        var chList = backend.channelInfoList
        if (chList.length === 0) return
        // Wait for every channel's first real read — chCapFlags is undefined
        // on the placeholder QVariantMap each channel starts with before its
        // own doRefreshChannelInfo completes. Computing (and publishing) a
        // column set from that placeholder produces a premature partial set
        // that gets immediately replaced once real data arrives, which was
        // observed to corrupt row layout during that extra transition.
        for (var j = 0; j < chList.length; j++) {
            if (chList[j].chCapFlags === undefined) return
        }
        // CH_CAP_OUTPUT_ENABLE ("can be switched on/off") and
        // CH_CAP_RAW_OUTPUT_DRIVE ("has a DAC, level is settable") are
        // independent capabilities — see docs/guide/channel-capability-model.md
        // §1. jw_lvb channels 1-9 have OUTPUT_ENABLE without RAW_OUTPUT_DRIVE
        // (switchable, fixed-voltage); jw_hvb channels have both. The Vset
        // column must render as a target-voltage input for drive channels but
        // an on/off toggle for enable-only ones — conflating the two (gating
        // everything on OUTPUT_ENABLE alone) was a real bug: it showed a
        // target-voltage field with no DAC behind it on enable-only channels.
        var hasOutEn = false, hasDrive = false, hasVolt = false, hasCurr = false
        for (var i = 0; i < chList.length; i++) {
            var caps = chList[i].chCapFlags || 0
            if (caps & 0x0001) hasOutEn = true
            if (caps & 0x0002) hasDrive = true
            if (caps & 0x0004) hasVolt  = true
            if (caps & 0x0008) hasCurr  = true
        }
        // Widths are plain numbers rather than referencing a PsbTheme
        // singleton: qmlcachegen's AOT compilation of this file hits a known
        // Qt Quick Compiler limitation ("PsbTheme was a singleton at compile
        // time, but is not a singleton anymore"), silently evaluating every
        // PsbTheme.* access to undefined (logged as a QML TypeError, not
        // thrown) instead of its real value.
        // vset/status header label: per-board (capability is uniform across
        // a board's channels in every real case), but each row still renders
        // per its own channel's actual capability — see the Repeater below.
        var vsetLabel = hasDrive ? "Vset (V)" : "En"
        var cols = []
        cols.push({ key: "ch",     label: "CH",        minWidth: 55,  weight: 0.7  })
        if (hasOutEn || hasDrive) cols.push({ key: "vset",   label: vsetLabel, minWidth: 95,  weight: 1.0  })
        if (hasOutEn || hasDrive) cols.push({ key: "status", label: "Status",   minWidth: 100, weight: 1.0  })
        cols.push(    { key: "vop",    label: "Vop (V)",   minWidth: 90,  weight: 1.0  })
        if (hasVolt)  cols.push({ key: "v",      label: "V (V)",    minWidth: 90,  weight: 1.0  })
        if (hasCurr)  cols.push({ key: "i",      label: "I (nA)",   minWidth: 100, weight: 1.05 })
        if (hasDrive) cols.push({ key: "ru",     label: "Ru (V)",   minWidth: 85,  weight: 0.9  })
        if (hasDrive) cols.push({ key: "rd",     label: "Rd (V)",   minWidth: 85,  weight: 0.9  })
        if (hasCurr)  cols.push({ key: "limit",  label: "Lim(nA)",  minWidth: 100, weight: 1.05 })
        cols.push(    { key: "fault",  label: "Fault",     minWidth: 90,  weight: 0.95 })

        // Column set is derived from hardware capability flags, which are
        // fixed for the lifetime of a connection — only replace the array
        // (and thus rebuild the header/row Repeaters) when the computed
        // set actually changes. Reassigning it on every channelDataChanged
        // tick (every poll interval) tore down and rebuilt the whole grid
        // continuously, which hung the render thread indefinitely.
        var newKeys = cols.map(function(c) { return c.key }).join(",")
        var oldKeys = activeColumns.map(function(c) { return c.key }).join(",")
        if (newKeys !== oldKeys) activeColumns = cols
    }

    // Proportional column width: fills the available table width on wide
    // windows, clamped to each column's minWidth on narrow ones (the
    // ScrollView then provides a horizontal scrollbar for the overflow).
    function colWidth(col) {
        if (!col) return 0
        var totalWeight = 0
        for (var i = 0; i < activeColumns.length; i++) totalWeight += activeColumns[i].weight
        var avail = root.width - 20  // scrollbar + margin allowance
        var share = avail * col.weight / (totalWeight || 1)
        return Math.max(col.minWidth, share)
    }

    Connections {
        target: backend
        function onChannelDataChanged() { root.computeActiveColumns() }
        function onConnectedChanged()   { if (!backend.connected) root.activeColumns = [] }
    }

    // Offline placeholder
    Label {
        anchors.centerIn: parent
        text: "Not connected — click Connect in the toolbar"
        visible: !backend.connected
        opacity: 0.5
        font.pixelSize: 16
    }

    ScrollView {
        anchors.fill: parent
        visible: backend.connected && root.activeColumns.length > 0
        clip: true

        Column {
            spacing: 0

            // Header row
            Row {
                id: headerRow
                height: 42
                Repeater {
                    model: root.activeColumns
                    Label {
                        width: root.colWidth(modelData)
                        height: 42
                        text: modelData.label
                        font.pixelSize: 15
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Rectangle { height: 1; width: headerRow.width; color: "#444" }

            // Data rows
            Repeater {
                id: rowRepeater
                model: backend.channelCount

                Column {
                    property int chIdx: index
                    property var ci: (backend.channelInfoList   || [])[chIdx] || {}
                    property var cc: (backend.channelConfigList || [])[chIdx] || {}
                    property int caps: ci.chCapFlags || 0

                    Row {
                        height: 46

                        // A channel that fails kChannelOfflineThreshold
                        // (modbus_worker.cpp) polls in a row is flagged
                        // offline rather than silently left showing its
                        // last-known values with no visual distinction from
                        // a healthy, freshly-polled channel — mirrors
                        // demo_tui's explicit red OFFLINE row treatment.
                        Rectangle {
                            anchors.fill: parent
                            visible: !!ci.offline
                            color: "#4a1414"
                            opacity: 0.5
                        }

                        Repeater {
                            model: root.activeColumns

                            Item {
                                width: root.colWidth(modelData)
                                height: 46

                                Label {
                                    anchors.centerIn: parent
                                    text: "CH" + chIdx
                                    visible: modelData.key === "ch"
                                    font.pixelSize: 15
                                    font.bold: true
                                }

                                // DAC channels (CH_CAP_RAW_OUTPUT_DRIVE): target-voltage input.
                                TextField {
                                    id: vsetField
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "vset" && !!ci.capRawDrive
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    // Binding on text (rather than a plain text: assignment) so
                                    // the live-polled value only overwrites the field while the
                                    // user isn't editing it — otherwise a poll tick landing
                                    // mid-edit silently clobbers whatever was just typed.
                                    Binding on text {
                                        value: ci.operationalTargetV !== undefined
                                            ? ci.operationalTargetV.toFixed(1) : ""
                                        when: !vsetField.activeFocus
                                    }
                                    onAccepted: backend.writeTargetVoltage(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1))
                                }

                                // Fixed-voltage switchable channels (CH_CAP_OUTPUT_ENABLE
                                // without CH_CAP_RAW_OUTPUT_DRIVE, e.g. jw_lvb ch1-9): no DAC
                                // to set a level on, so this slot is an on/off toggle for
                                // CFG_OUTPUT_ENABLED (startup intent) instead — occupies the
                                // same table slot as vsetField; the two are mutually exclusive
                                // per channel, same as demo_tui's tab_monitor.h. See
                                // docs/guide/channel-capability-model.md §2/§7.
                                Switch {
                                    id: enSwitch
                                    anchors.centerIn: parent
                                    visible: modelData.key === "vset" && !!ci.capOutEn && !ci.capRawDrive
                                    text: checked ? "On" : "Off"
                                    // Binding element (not a plain `checked:` assignment) so a
                                    // click's imperative set doesn't permanently sever the link
                                    // to cc.outputEnabledCfg — mirrors vsetField's text Binding
                                    // above. Paused only while actively pressed.
                                    Binding on checked {
                                        value: !!cc.outputEnabledCfg
                                        when: !enSwitch.pressed
                                    }
                                    onToggled: backend.writeOutputEnabled(chIdx, checked)
                                }

                                Button {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "status" && (!!ci.capOutEn || !!ci.capRawDrive)
                                    font.pixelSize: 14
                                    // "on" comes from the backend's capability-aware
                                    // ci.isOn (psb::channelIsOn, channel_policy.h) — not a
                                    // raw statusOutDrive/operationalTargetV check computed
                                    // here, which would disagree with ChannelTab's badge and
                                    // (worse) ignore OUTPUT_ENABLE_ACTIVE entirely. See
                                    // docs/guide/client-architecture-and-pitfalls.md §2.9.
                                    text: {
                                        if (ci.statusRamping) return "RAMP"
                                        return ci.isOn ? "ON" : "OFF"
                                    }
                                    Material.background: {
                                        if (ci.statusRamping) return Material.Amber
                                        return ci.isOn ? Material.Green : Material.Grey
                                    }
                                    implicitWidth: parent.width - 8
                                    implicitHeight: 36
                                    onClicked: {
                                        if (ci.statusRamping) return
                                        backend.sendOutputAction(chIdx, ci.isOn ? 2 : 1)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "vop"
                                    font.pixelSize: 14
                                    // Only meaningful on DAC channels — fixed-voltage channels
                                    // have no target-voltage concept at all (§7).
                                    text: (ci.capRawDrive && ci.operationalTargetV !== undefined)
                                        ? ci.operationalTargetV.toFixed(1) : "--"
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "v"
                                    font.pixelSize: 14
                                    text: ci.voltageV !== undefined ? ci.voltageV.toFixed(1) : "--"
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "i"
                                    font.pixelSize: 14
                                    text: ci.currentRaw !== undefined ? ci.currentRaw + "" : "--"
                                }

                                TextField {
                                    id: ruField
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "ru" && !!ci.capRawDrive
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    Binding on text {
                                        value: cc.rampUpStepRaw !== undefined
                                            ? (cc.rampUpStepRaw * 0.1).toFixed(1) : ""
                                        when: !ruField.activeFocus
                                    }
                                    onAccepted: backend.writeRampUp(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1),
                                        cc.rampUpInterval || 1)
                                }

                                TextField {
                                    id: rdField
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "rd" && !!ci.capRawDrive
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    Binding on text {
                                        value: cc.rampDownStepRaw !== undefined
                                            ? (cc.rampDownStepRaw * 0.1).toFixed(1) : ""
                                        when: !rdField.activeFocus
                                    }
                                    onAccepted: backend.writeRampDown(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1),
                                        cc.rampDownInterval || 1)
                                }

                                TextField {
                                    id: limitField
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "limit" && (caps & 0x0008) !== 0
                                    font.pixelSize: 14
                                    placeholderText: "nA"
                                    Binding on text {
                                        value: cc.iLimitThresholdRaw !== undefined
                                            ? cc.iLimitThresholdRaw + "" : ""
                                        when: !limitField.activeFocus
                                    }
                                    onAccepted: backend.writeCurrentProtection(chIdx,
                                        cc.iProtMode || 0,
                                        cc.iProtOutputAction || 0,
                                        parseInt(text) || 0)
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "fault"
                                    font.pixelSize: 14
                                    font.bold: !!ci.offline
                                    text: {
                                        if (ci.offline) return "OFFLINE"
                                        var f = ci.activeFault || 0
                                        return f ? "0x" + f.toString(16).toUpperCase() : "—"
                                    }
                                    color: ci.offline ? "#F44336" : (ci.activeFault || 0) ? "#F44336" : "#aaaaaa"
                                }

                                // "--" placeholder for capability-absent cells
                                Label {
                                    anchors.centerIn: parent
                                    text: "--"
                                    font.pixelSize: 14
                                    opacity: 0.3
                                    visible: {
                                        if (modelData.key === "vset")
                                            return !ci.capOutEn && !ci.capRawDrive
                                        if (modelData.key === "status")
                                            return !ci.capOutEn && !ci.capRawDrive
                                        if (modelData.key === "ru" || modelData.key === "rd")
                                            return !ci.capRawDrive
                                        if (modelData.key === "i" || modelData.key === "limit")
                                            return (caps & 0x0008) === 0
                                        return false
                                    }
                                }
                            }
                        }
                    }

                    Rectangle { height: 1; width: headerRow.width; color: "#333" }
                }
            }
        }
    }
}

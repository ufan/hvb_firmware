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
        var hasOutEn = false, hasVolt = false, hasCurr = false
        for (var i = 0; i < chList.length; i++) {
            var caps = chList[i].chCapFlags || 0
            if (caps & 0x0001) hasOutEn = true
            if (caps & 0x0004) hasVolt  = true
            if (caps & 0x0008) hasCurr  = true
        }
        // Widths are plain numbers rather than referencing a PsbTheme
        // singleton: qmlcachegen's AOT compilation of this file hits a known
        // Qt Quick Compiler limitation ("PsbTheme was a singleton at compile
        // time, but is not a singleton anymore"), silently evaluating every
        // PsbTheme.* access to undefined (logged as a QML TypeError, not
        // thrown) instead of its real value.
        var cols = []
        cols.push({ key: "ch",     label: "CH",        minWidth: 55,  weight: 0.7  })
        if (hasOutEn) cols.push({ key: "vset",   label: "Vset (V)", minWidth: 95,  weight: 1.0  })
        if (hasOutEn) cols.push({ key: "status", label: "Status",   minWidth: 100, weight: 1.0  })
        cols.push(    { key: "vop",    label: "Vop (V)",   minWidth: 90,  weight: 1.0  })
        if (hasVolt)  cols.push({ key: "v",      label: "V (V)",    minWidth: 90,  weight: 1.0  })
        if (hasCurr)  cols.push({ key: "i",      label: "I (nA)",   minWidth: 100, weight: 1.05 })
        if (hasOutEn) cols.push({ key: "ru",     label: "Ru (V)",   minWidth: 85,  weight: 0.9  })
        if (hasOutEn) cols.push({ key: "rd",     label: "Rd (V)",   minWidth: 85,  weight: 0.9  })
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

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "vset" && (caps & 0x0001) !== 0
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    text: ci.operationalTargetV !== undefined
                                        ? ci.operationalTargetV.toFixed(1) : ""
                                    onAccepted: backend.writeTargetVoltage(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1))
                                }

                                Button {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "status" && (caps & 0x0001) !== 0
                                    font.pixelSize: 14
                                    text: {
                                        if (ci.statusRamping) return "RAMP"
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0) ? "ON" : "OFF"
                                    }
                                    Material.background: {
                                        if (ci.statusRamping) return Material.Amber
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0)
                                            ? Material.Green : Material.Grey
                                    }
                                    implicitWidth: parent.width - 8
                                    implicitHeight: 36
                                    onClicked: {
                                        if (ci.statusRamping) return
                                        var on = ci.statusOutDrive && ci.operationalTargetV !== 0
                                        backend.sendOutputAction(chIdx, on ? 2 : 1)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "vop"
                                    font.pixelSize: 14
                                    text: ci.operationalTargetV !== undefined
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
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "ru" && (caps & 0x0001) !== 0
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    text: cc.rampUpStepRaw !== undefined
                                        ? (cc.rampUpStepRaw * 0.1).toFixed(1) : ""
                                    onAccepted: backend.writeRampUp(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1),
                                        cc.rampUpInterval || 1)
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "rd" && (caps & 0x0001) !== 0
                                    font.pixelSize: 14
                                    placeholderText: "V"
                                    text: cc.rampDownStepRaw !== undefined
                                        ? (cc.rampDownStepRaw * 0.1).toFixed(1) : ""
                                    onAccepted: backend.writeRampDown(chIdx,
                                        Math.round((parseFloat(text) || 0) / 0.1),
                                        cc.rampDownInterval || 1)
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 4
                                    visible: modelData.key === "limit" && (caps & 0x0008) !== 0
                                    font.pixelSize: 14
                                    placeholderText: "nA"
                                    text: cc.iLimitThresholdRaw !== undefined
                                        ? cc.iLimitThresholdRaw + "" : ""
                                    onAccepted: backend.writeCurrentProtection(chIdx,
                                        cc.iProtMode || 0,
                                        cc.iProtOutputAction || 0,
                                        parseInt(text) || 0)
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "fault"
                                    font.pixelSize: 14
                                    text: {
                                        var f = ci.activeFault || 0
                                        return f ? "0x" + f.toString(16).toUpperCase() : "—"
                                    }
                                    color: (ci.activeFault || 0) ? "#F44336" : "#aaaaaa"
                                }

                                // "--" placeholder for capability-absent cells
                                Label {
                                    anchors.centerIn: parent
                                    text: "--"
                                    font.pixelSize: 14
                                    opacity: 0.3
                                    visible: {
                                        if (modelData.key === "vset"  || modelData.key === "status" ||
                                            modelData.key === "ru"    || modelData.key === "rd")
                                            return (caps & 0x0001) === 0
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

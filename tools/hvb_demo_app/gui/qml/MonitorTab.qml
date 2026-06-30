import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import HvbTool

Item {
    id: root

    // Column descriptor array — rebuilt from board capability flags; cleared on disconnect.
    property var activeColumns: []

    function computeActiveColumns() {
        if (!backend.connected || backend.channelCount === 0) {
            activeColumns = []
            return
        }
        var chList = backend.channelInfoList
        var hasOutEn = false, hasVolt = false, hasCurr = false
        for (var i = 0; i < chList.length; i++) {
            var caps = chList[i].chCapFlags || 0
            if (caps & 0x0001) hasOutEn = true
            if (caps & 0x0004) hasVolt  = true
            if (caps & 0x0008) hasCurr  = true
        }
        var cols = []
        cols.push({ key: "ch",     label: "CH",        width: Theme.colCh })
        if (hasOutEn) cols.push({ key: "vset",   label: "Vset (V)", width: Theme.colVset })
        if (hasOutEn) cols.push({ key: "status", label: "Status",   width: Theme.colStatus })
        cols.push(    { key: "vop",    label: "Vop (V)",   width: Theme.colVop })
        if (hasVolt)  cols.push({ key: "v",      label: "V (V)",    width: Theme.colV })
        if (hasCurr)  cols.push({ key: "i",      label: "I (nA)",   width: Theme.colI })
        if (hasOutEn) cols.push({ key: "ru",     label: "Ru (V)",   width: Theme.colRamp })
        if (hasOutEn) cols.push({ key: "rd",     label: "Rd (V)",   width: Theme.colRamp })
        if (hasCurr)  cols.push({ key: "limit",  label: "Lim(nA)",  width: Theme.colLimit })
        cols.push(    { key: "fault",  label: "Fault",     width: Theme.colFault })
        activeColumns = cols
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
        font.pixelSize: 14
    }

    ScrollView {
        anchors.fill: parent
        visible: backend.connected && root.activeColumns.length > 0
        clip: true

        ColumnLayout {
            spacing: 0

            // Header row
            Row {
                id: headerRow
                height: 32
                Repeater {
                    model: root.activeColumns
                    Label {
                        width: modelData.width
                        height: 32
                        text: modelData.label
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
                        height: 36

                        Repeater {
                            model: root.activeColumns

                            Item {
                                width: modelData.width
                                height: 36

                                Label {
                                    anchors.centerIn: parent
                                    text: "CH" + chIdx
                                    visible: modelData.key === "ch"
                                    font.bold: true
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "vset" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: ci.operationalTargetV !== undefined
                                        ? ci.operationalTargetV.toFixed(1) : ""
                                    onAccepted: backend.writeTargetVoltage(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0))
                                }

                                Button {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "status" && (caps & 0x0001) !== 0
                                    text: {
                                        if (ci.statusRamping) return "RAMP"
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0) ? "ON" : "OFF"
                                    }
                                    Material.background: {
                                        if (ci.statusRamping) return Material.Amber
                                        return (ci.statusOutDrive && ci.operationalTargetV !== 0)
                                            ? Material.Green : Material.Grey
                                    }
                                    implicitWidth: Theme.colStatus - 4
                                    onClicked: {
                                        if (ci.statusRamping) return
                                        var on = ci.statusOutDrive && ci.operationalTargetV !== 0
                                        backend.sendOutputAction(chIdx, on ? 2 : 1)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "vop"
                                    text: ci.operationalTargetV !== undefined
                                        ? ci.operationalTargetV.toFixed(1) : "--"
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "v"
                                    text: ci.voltageV !== undefined ? ci.voltageV.toFixed(1) : "--"
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: modelData.key === "i"
                                    text: ci.currentRaw !== undefined ? ci.currentRaw + "" : "--"
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "ru" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: cc.rampUpStepRaw !== undefined
                                        ? Theme.voltageToV(cc.rampUpStepRaw).toFixed(1) : ""
                                    onAccepted: backend.writeRampUp(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0),
                                        cc.rampUpInterval || 1)
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "rd" && (caps & 0x0001) !== 0
                                    placeholderText: "V"
                                    text: cc.rampDownStepRaw !== undefined
                                        ? Theme.voltageToV(cc.rampDownStepRaw).toFixed(1) : ""
                                    onAccepted: backend.writeRampDown(chIdx,
                                        Theme.voltageFromV(parseFloat(text) || 0),
                                        cc.rampDownInterval || 1)
                                }

                                TextField {
                                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                                    anchors.margins: 2
                                    visible: modelData.key === "limit" && (caps & 0x0008) !== 0
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCharts

ColumnLayout {
    id: root
    spacing: 4

    required property int    channelIndex
    required property string title
    // seriesConfigs: [{name, color, valueKey, source}]
    // source: "config" → channelConfigList; anything else → channelInfoList
    required property var    seriesConfigs
    property int  windowMinutes: 5

    // Ring buffers: array of arrays of {t: ms, v: number}
    property var _buffers: []
    // Checkbox visible states parallel to seriesConfigs
    property var _visible: []
    // backend.channelDataChanged is a single shared "something changed"
    // signal fired once per channel per poll cycle (see modbus_backend.cpp
    // onChInfoReady) — with N channels it fires N times/second, not once.
    // Every ChannelGraph instance for every channel listens to it (all
    // ChannelTabs stay alive under main.qml's StackLayout+Repeater), so
    // without this throttle each graph rebuilt its whole point buffer N
    // times/second instead of once, growing buffers ~N× past the intended
    // windowMinutes point count and re-appending all of them each time —
    // on jw_lvb's 10 channels this pegged the main thread at 100% CPU and
    // froze the UI after a few minutes, once the buffers grew large enough.
    property real _lastAppendT: 0

    Component.onCompleted: {
        for (var i = 0; i < seriesConfigs.length; i++) {
            _buffers.push([])
            _visible.push(true)
        }
        _createSeries()
    }

    function _createSeries() {
        for (var i = 0; i < seriesConfigs.length; i++) {
            var cfg = seriesConfigs[i]
            var s = chartView.createSeries(ChartView.SeriesTypeLine, cfg.name, axisX, axisY)
            s.color = cfg.color
            s.width = 1.5
            s.useOpenGL = false
        }
    }

    function _appendAndTrim(now) {
        var cutoff = now - root.windowMinutes * 60000
        var chList = backend.channelInfoList
        var cfList = backend.channelConfigList
        if (root.channelIndex >= chList.length) return
        var chInfo = chList[root.channelIndex]
        var chCfg  = cfList.length > root.channelIndex ? cfList[root.channelIndex] : {}

        var yMin = Infinity, yMax = -Infinity

        for (var i = 0; i < root.seriesConfigs.length; i++) {
            var cfg = root.seriesConfigs[i]
            var src = (cfg.source === "config") ? chCfg : chInfo
            var v   = src[cfg.valueKey] !== undefined ? src[cfg.valueKey] : 0
            var buf = root._buffers[i]

            buf.push({ t: now, v: v })
            while (buf.length > 0 && buf[0].t < cutoff)
                buf.shift()

            if (chartView.count <= i) continue
            var series = chartView.series(i)
            series.clear()
            for (var j = 0; j < buf.length; j++)
                series.append(buf[j].t, buf[j].v)

            if (!root._visible[i]) continue
            for (var j = 0; j < buf.length; j++) {
                if (buf[j].v < yMin) yMin = buf[j].v
                if (buf[j].v > yMax) yMax = buf[j].v
            }
        }

        axisX.max = new Date(now)
        axisX.min = new Date(cutoff)

        if (yMin === Infinity) { yMin = 0; yMax = 1 }
        else if (yMin === yMax) { yMin -= 1; yMax += 1 }
        var margin = (yMax - yMin) * 0.1 || 0.5
        axisY.min = yMin - margin
        axisY.max = yMax + margin
    }

    function _rebuildFromBuffers() {
        var now = Date.now()
        var cutoff = now - root.windowMinutes * 60000
        for (var i = 0; i < root._buffers.length; i++) {
            var buf = root._buffers[i]
            while (buf.length > 0 && buf[0].t < cutoff) buf.shift()
            if (chartView.count <= i) continue
            var series = chartView.series(i)
            series.clear()
            for (var j = 0; j < buf.length; j++)
                series.append(buf[j].t, buf[j].v)
        }
        axisX.max = new Date(now)
        axisX.min = new Date(cutoff)
    }

    // Header row: title + per-series checkboxes + window selector
    RowLayout {
        Label { text: root.title; font.bold: true }

        Repeater {
            model: root.seriesConfigs.length
            CheckBox {
                id: cbx
                text: root.seriesConfigs[index].name
                checked: true
                onCheckedChanged: {
                    root._visible[index] = checked
                    if (chartView.count > index)
                        chartView.series(index).visible = checked
                }
                contentItem: Label {
                    leftPadding: cbx.indicator.width + cbx.spacing
                    text: cbx.text
                    font: cbx.font
                    color: root.seriesConfigs[index].color
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Item { Layout.fillWidth: true }

        Label { text: "Window:" }
        ComboBox {
            model: ["1 min", "5 min", "10 min", "30 min"]
            currentIndex: 1
            implicitWidth: 90
            onActivated: {
                var mins = [1, 5, 10, 30]
                root.windowMinutes = mins[currentIndex]
                root._rebuildFromBuffers()
            }
        }
    }

    ChartView {
        id: chartView
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 160
        antialiasing: true
        theme: ChartView.ChartThemeDark
        legend.visible: false
        margins.left: 0; margins.right: 0; margins.top: 4; margins.bottom: 0

        DateTimeAxis {
            id: axisX
            format: "hh:mm:ss"
            tickCount: 5
            min: new Date(Date.now() - root.windowMinutes * 60000)
            max: new Date(Date.now())
        }

        ValueAxis {
            id: axisY
            min: 0
            max: 1
        }
    }

    Connections {
        target: backend
        function onChannelDataChanged() {
            if (!backend.connected) return
            var now = Date.now()
            if (now - root._lastAppendT < 900) return
            root._lastAppendT = now
            root._appendAndTrim(now)
        }
        function onConnectedChanged() {
            if (!backend.connected) {
                for (var i = 0; i < root._buffers.length; i++) root._buffers[i] = []
                for (var i = 0; chartView.count > i; i++) chartView.series(i).clear()
            }
        }
    }
}

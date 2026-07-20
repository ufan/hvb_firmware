import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtCharts

// Self-contained live line chart. Creates and owns its series via
// createSeries() (ChartView.addSeries() is not QML-invokable in this Qt
// version) — callers feed data through append()/clear() instead of
// managing a LineSeries themselves.
ChartView {
    id: root
    antialiasing: true
    theme: ChartView.ChartThemeDark
    legend.visible: false
    margins.left: 0
    margins.right: 0
    margins.top: 4
    margins.bottom: 0

    property var _series: null

    ValueAxis {
        id: axisX
        titleText: "Time (s)"
        labelsFont.pixelSize: 10
    }
    ValueAxis {
        id: axisY
        titleText: "Voltage (V)"
        labelsFont.pixelSize: 10
    }

    Component.onCompleted: {
        _series = root.createSeries(ChartView.SeriesTypeLine, "value", axisX, axisY)
    }

    function append(x, y) {
        if (!_series) return
        _series.append(x, y)
        if (_series.count === 1) {
            axisX.min = 0; axisX.max = Math.max(x, 10)
            axisY.min = y * 0.9; axisY.max = y * 1.1
        } else {
            axisX.max = Math.max(axisX.max, x)
            axisY.min = Math.min(axisY.min, y * 0.99)
            axisY.max = Math.max(axisY.max, y * 1.01)
        }
    }

    function clear() {
        if (_series) _series.clear()
    }
}

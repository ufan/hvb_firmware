import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtCharts

// Thin wrapper that binds an externally-owned LineSeries into a ChartView
ChartView {
    id: root
    antialiasing: true
    theme: ChartView.ChartThemeDark
    legend.visible: false
    margins.left: 0
    margins.right: 0
    margins.top: 4
    margins.bottom: 0

    // Caller must pass the LineSeries to display
    required property LineSeries seriesRef

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
        // Attach externally-managed series to our axes
        if (seriesRef) {
            root.addSeries(seriesRef)
            root.setAxisX(axisX, seriesRef)
            root.setAxisY(axisY, seriesRef)
        }
    }

    // Auto-scale axes when series data changes
    Connections {
        target: root.seriesRef
        function onPointAdded(index) {
            const p = root.seriesRef.at(index)
            if (index === 0) {
                axisX.min = 0; axisX.max = Math.max(p.x, 10)
                axisY.min = p.y * 0.9; axisY.max = p.y * 1.1
            } else {
                axisX.max = Math.max(axisX.max, p.x)
                axisY.min = Math.min(axisY.min, p.y * 0.99)
                axisY.max = Math.max(axisY.max, p.y * 1.01)
            }
        }
    }
}

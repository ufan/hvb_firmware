import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory

Pane {
    Material.elevation: 1

    GridLayout {
        columns: 4
        columnSpacing: 24
        rowSpacing: 4
        width: parent.width

        Repeater {
            model: [
                {label: "Protocol",  value: Backend.deviceInfo.protoMajor + "." + Backend.deviceInfo.protoMinor},
                {label: "Firmware",  value: Backend.deviceInfo.fwVersion},
                {label: "Variant",   value: Backend.deviceInfo.variantName + " (" + Backend.deviceInfo.boardHwRevisionLabel + ")"},
                {label: "Channels",  value: Backend.deviceInfo.supportedChannels},
                {label: "Board °C",  value: Backend.deviceInfo.boardTemp !== undefined
                                            ? Backend.deviceInfo.boardTemp.toFixed(1) : "—"},
                {label: "Uptime",    value: Backend.deviceInfo.uptimeSec + " s"}
            ]
            ColumnLayout {
                required property var modelData
                spacing: 2
                Label { text: modelData.label; font.pixelSize: 10; opacity: 0.6 }
                Label { text: "" + (modelData.value ?? "—"); font.pixelSize: 13; font.bold: true }
            }
        }
    }
}

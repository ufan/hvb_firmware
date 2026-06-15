import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property string label: ""
    property string value: ""
    spacing: 6

    Label {
        text: label + ":"
        Layout.preferredWidth: 110
        font.pixelSize: 12
        color: "#555"
        horizontalAlignment: Text.AlignRight
    }
    Label {
        text: value
        font.pixelSize: 12
        font.family: "monospace"
    }
}

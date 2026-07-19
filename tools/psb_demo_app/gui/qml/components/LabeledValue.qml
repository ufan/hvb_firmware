import QtQuick
import QtQuick.Controls

Row {
    id: root
    spacing: 4
    required property string label
    required property string value

    Label {
        text: root.label + ":"
        opacity: 0.6
        font.pixelSize: 13
    }
    Label {
        text: root.value
        font.pixelSize: 13
        font.bold: true
    }
}

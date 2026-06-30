import QtQuick
import QtQuick.Controls

Row {
    id: root
    spacing: 2
    required property string label
    required property string value

    Label {
        text: root.label + ":"
        opacity: 0.6
        font.pixelSize: 11
    }
    Label {
        text: root.value
        font.pixelSize: 11
        font.bold: true
    }
}

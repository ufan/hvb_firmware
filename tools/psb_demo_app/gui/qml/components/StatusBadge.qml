import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property bool active: false
    property string label: ""
    property color color: "green"

    spacing: 6

    Rectangle {
        implicitWidth: 12; implicitHeight: 12; radius: 6
        color: active ? parent.color : "#ccc"
    }
    Label {
        text: label
        font.pixelSize: 14
        color: active ? parent.color : "#999"
    }
}

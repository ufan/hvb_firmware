import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property bool active: false
    property string label: ""
    property color color: "green"

    spacing: 6

    Rectangle {
        implicitWidth: 10; implicitHeight: 10; radius: 5
        color: active ? parent.color : "#ccc"
    }
    Label {
        text: (active ? "● " : "○ ") + label
        font.pixelSize: 12
        color: active ? parent.color : "#999"
    }
}

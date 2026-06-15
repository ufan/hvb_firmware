import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property bool active: false
    property string label: ""
    property string color: "green"

    spacing: 6

    Rectangle {
        width: 10; height: 10; radius: 5
        color: active ? parent.color : "#ccc"
    }
    Label {
        text: (active ? "● " : "○ ") + label
        font.pixelSize: 12
        color: active ? parent.color : "#999"
    }
}

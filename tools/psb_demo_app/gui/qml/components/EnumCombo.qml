import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property string label: ""
    property var model: []
    property int currentIndex: 0
    signal selected(int index)

    spacing: 6

    Label {
        text: label + ":"
        Layout.preferredWidth: 100
        font.pixelSize: 12
        color: "#555"
        horizontalAlignment: Text.AlignRight
    }
    ComboBox {
        model: parent.model
        currentIndex: parent.currentIndex
        onCurrentIndexChanged: selected(currentIndex)
        Layout.preferredWidth: 150
    }
}

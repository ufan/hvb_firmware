import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    property string label: ""
    property string text: ""
    property alias readOnly: field.readOnly
    signal submitted(string value)

    spacing: 6

    Label {
        text: label + ":"
        Layout.preferredWidth: 100
        font.pixelSize: 12
        color: "#555"
        horizontalAlignment: Text.AlignRight
    }
    TextField {
        id: field
        Layout.preferredWidth: 100
        font.pixelSize: 12
        text: parent.text
        onTextChanged: parent.text = text
    }
    Button {
        text: "Set"
        onClicked: submitted(field.text)
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    title: "Raw Modbus Debug"
    standardButtons: Dialog.Close
    width: 480
    height: 400

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Label { text: "FC:" }
            ComboBox { id: rawFc; model: ["04 (Input)", "03 (Holding)"]; Layout.preferredWidth: 120 }
            Label { text: "Addr:" }
            TextField { id: rawAddr; Layout.preferredWidth: 70; text: "0" }
            Label { text: "Count:" }
            TextField { id: rawCount; Layout.preferredWidth: 50; text: "16" }
            Button {
                text: "Read"
                onClicked: {
                    var addr = parseInt(rawAddr.text) || 0
                    var count = parseInt(rawCount.text) || 16
                    if (rawFc.currentIndex === 0) backend.rawReadFc04(addr, count)
                    else backend.rawReadFc03(addr, count)
                }
            }
        }

        RowLayout {
            Label { text: "Write 16:" }
            TextField { id: wAddr; Layout.preferredWidth: 70; text: "0" }
            TextField { id: wVal; Layout.preferredWidth: 70; text: "0" }
            Button {
                text: "FC06"
                onClicked: backend.rawWriteFc06(parseInt(wAddr.text)||0, parseInt(wVal.text)||0)
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: rawResult
                readOnly: true
                font.family: "monospace"
                font.pixelSize: 12
                wrapMode: TextEdit.NoWrap
                background: Rectangle { color: "#1a1a2e" }
                color: "#cccccc"
            }
        }
    }

    Connections {
        target: backend
        function onRawHexReady(hex) {
            rawResult.text = hex
        }
    }
}

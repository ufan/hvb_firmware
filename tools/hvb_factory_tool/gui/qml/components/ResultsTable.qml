import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Generic results table
// columns: list of header strings
// rows:    list of list of cell strings
// passColumn: column index (0-based) whose value is colored green/red for PASS/FAIL
Item {
    id: root

    required property var columns    // string[]
    required property var rows       // string[][]
    property int passColumn: -1      // -1 = no coloring

    property real colWidth: Math.max(60, (width - 2) / Math.max(columns.length, 1))

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            height: 28
            color: Qt.rgba(1, 1, 1, 0.08)

            Row {
                anchors.fill: parent
                Repeater {
                    model: root.columns
                    Label {
                        required property string modelData
                        required property int index
                        width: root.colWidth
                        height: parent.height
                        text: modelData
                        font.bold: true
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 6
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Rectangle { color: Qt.rgba(1, 1, 1, 0.15); height: 1; Layout.fillWidth: true }

        // Rows
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                model: root.rows
                spacing: 0

                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 26
                    color: index % 2 === 0 ? "transparent" : Qt.rgba(1, 1, 1, 0.04)

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: modelData
                            Label {
                                required property string modelData
                                required property int index
                                width: root.colWidth
                                height: parent.height
                                text: modelData
                                font.pixelSize: 11
                                font.family: "monospace"
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 6
                                elide: Text.ElideRight
                                color: (root.passColumn >= 0 && index === root.passColumn)
                                       ? (modelData === "PASS"
                                          ? Material.color(Material.Green)
                                          : modelData === "FAIL"
                                            ? Material.color(Material.Red)
                                            : Material.foreground)
                                       : Material.foreground
                            }
                        }
                    }
                }
            }
        }
    }
}

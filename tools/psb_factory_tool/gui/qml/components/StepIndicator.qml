import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Horizontal step breadcrumb
Item {
    id: root
    height: 44

    required property var steps       // list of strings
    required property int currentStep // 0-based

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Repeater {
            model: root.steps

            RowLayout {
                required property int index
                required property string modelData
                spacing: 0

                // Connector line (not before first step)
                Rectangle {
                    visible: index > 0
                    width: 24; height: 2
                    color: index <= root.currentStep
                           ? Material.color(Material.Cyan)
                           : Qt.rgba(1, 1, 1, 0.15)
                }

                // Step circle + label
                ColumnLayout {
                    spacing: 2

                    Rectangle {
                        width: 28; height: 28; radius: 14
                        color: index < root.currentStep  ? Material.color(Material.Cyan)
                             : index === root.currentStep ? Material.accent
                             : Qt.rgba(1, 1, 1, 0.1)
                        border.color: index <= root.currentStep
                                      ? Material.color(Material.Cyan)
                                      : Qt.rgba(1, 1, 1, 0.3)
                        border.width: 1

                        Label {
                            anchors.centerIn: parent
                            text: index < root.currentStep ? "✓" : "" + (index + 1)
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }

                    Label {
                        text: modelData
                        font.pixelSize: 10
                        opacity: index === root.currentStep ? 1.0 : 0.6
                    }
                }
            }
        }

        Item { Layout.fillWidth: true }
    }
}

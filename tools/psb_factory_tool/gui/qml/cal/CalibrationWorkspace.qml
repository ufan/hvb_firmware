import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory
import "../components"

// Cal wizard — strict step order enforced by stepIndex
Item {
    id: root

    // Steps: 0=Connect, 1=Unlock, 2=PerChannel, 3=Commit
    property int stepIndex: 0

    // Reset wizard state when cal mode exits
    Connections {
        target: Backend
        function onCalStateChanged() {
            if (!Backend.calActive && !Backend.calUnlocked)
                root.stepIndex = 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        StepIndicator {
            Layout.fillWidth: true
            steps: ["Connect", "Unlock & Enter", "Calibrate Channels", "Commit"]
            currentStep: root.stepIndex
        }

        // Page stack — only the active step is visible
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.stepIndex

            CalConnectPage {
                onConnected: root.stepIndex = 1
            }

            CalUnlockPage {
                onEntered: root.stepIndex = 2
                onRollback: root.stepIndex = 0
            }

            CalChannelPage {
                onAllDone: root.stepIndex = 3
                onRollback: root.stepIndex = 1
            }

            CalCommitPage {
                onRollback: root.stepIndex = 2
            }
        }
    }
}

import QtQuick

Rectangle {
    id: root
    width: 12; height: 12; radius: 6

    required property bool connected
    required property bool connecting

    color: connected  ? "#4CAF50"
         : connecting ? "#FFC107"
         : "#555555"

    // Baseline — animations override when running
    opacity: (root.connected || root.connecting) ? 1.0 : 0.4

    // Breathing animation when connected
    SequentialAnimation on opacity {
        running: root.connected
        loops: Animation.Infinite
        NumberAnimation { to: 0.25; duration: 900; easing.type: Easing.InOutSine }
        NumberAnimation { to: 1.0;  duration: 900; easing.type: Easing.InOutSine }
    }

    // Fast blink when connecting
    SequentialAnimation on opacity {
        running: root.connecting && !root.connected
        loops: Animation.Infinite
        NumberAnimation { to: 0.2; duration: 200 }
        NumberAnimation { to: 1.0; duration: 200 }
    }
}

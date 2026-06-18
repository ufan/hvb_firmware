pragma Singleton

import QtQuick

QtObject {
    readonly property color primary:       "#1565C0"
    readonly property color primaryLight:  "#1E88E5"
    readonly property color accent:        "#43A047"
    readonly property color accentLight:   "#66BB6A"
    readonly property color danger:        "#E53935"
    readonly property color warning:       "#FB8C00"
    readonly property color warningLight:  "#FFA726"

    readonly property color bgDark:        "#1a1a2e"
    readonly property color bgMedium:      "#263238"
    readonly property color bgLight:       "#ECEFF1"
    readonly property color surface:       "#FFFFFF"
    readonly property color textPrimary:   "#212121"
    readonly property color textSecondary: "#757575"
    readonly property color textOnDark:    "#CCCCCC"
    readonly property color border:        "#BDBDBD"

    readonly property int   radius:        4
    readonly property int   spacing:       8
    readonly property int   fontSizeSm:    11
    readonly property int   fontSize:      12
    readonly property int   fontSizeLg:    14
}

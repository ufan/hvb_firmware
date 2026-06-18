pragma Singleton
import QtQuick

QtObject {
    enum Theme { Light, Dark }
    property int current: Theme.Dark

    readonly property color background:     current === Theme.Dark ? "#1e1e2e" : "#eff1f5"
    readonly property color surface:        current === Theme.Dark ? "#313244" : "#ccd0da"
    readonly property color surfaceAlt:     current === Theme.Dark ? "#45475a" : "#bcc0cc"
    readonly property color text:           current === Theme.Dark ? "#cdd6f4" : "#4c4f69"
    readonly property color textSubtle:     current === Theme.Dark ? "#a6adc8" : "#6c6f85"
    readonly property color primary:        current === Theme.Dark ? "#89b4fa" : "#1e66f5"
    readonly property color success:        current === Theme.Dark ? "#a6e3a1" : "#40a02b"
    readonly property color warning:        current === Theme.Dark ? "#f9e2af" : "#df8e1d"
    readonly property color error:          current === Theme.Dark ? "#f38ba8" : "#d20f39"
    readonly property color border:         current === Theme.Dark ? "#585b70" : "#9ca0b0"

    readonly property int fontSizeSmall:   11
    readonly property int fontSizeNormal:  13
    readonly property int fontSizeLarge:   16
    readonly property int fontSizeTitle:   20

    readonly property int spacingSmall:    4
    readonly property int spacingNormal:   8
    readonly property int spacingLarge:    16

    readonly property int radiusSmall:     4
    readonly property int radiusNormal:    8

    function toggle() {
        current = (current === Theme.Dark) ? Theme.Light : Theme.Dark
    }
}

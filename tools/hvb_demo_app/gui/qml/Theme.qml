pragma Singleton
import QtQuick

QtObject {
    // Monitor table column widths (px)
    readonly property int colCh:     50
    readonly property int colVset:   80
    readonly property int colStatus: 90
    readonly property int colVop:    80
    readonly property int colV:      80
    readonly property int colI:      90
    readonly property int colRamp:   72
    readonly property int colLimit:  90
    readonly property int colFault:  110

    // Status colours
    readonly property color colorOk:      "#4CAF50"
    readonly property color colorError:   "#F44336"
    readonly property color colorWarn:    "#FFC107"
    readonly property color colorOffline: "#555555"
    readonly property color colorCyan:    "#00BCD4"

    // Voltage: 1 LSB = 0.1 V
    function voltageFromV(v)   { return Math.round(v / 0.1) }
    function voltageToV(raw)   { return raw * 0.1 }

    // Current: 1 LSB = 1 nA — raw value IS nA directly
    function currentNaFromA(a) { return Math.round(a * 1e9) }
    function currentAFromNa(na){ return na * 1e-9 }

    // Format helpers
    function fmtV(raw)  { return (raw * 0.1).toFixed(1) + " V" }
    function fmtNa(raw) { return raw + " nA" }
}

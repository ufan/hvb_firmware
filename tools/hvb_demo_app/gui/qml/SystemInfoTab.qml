import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

ScrollView {
    clip: true

    GridLayout {
        columns: 2
        columnSpacing: 16
        rowSpacing: 6
        anchors.margins: 12

        ReadOnlyField { label: "Protocol";  value: (backend.sysInfo.protoMajor || 0) + "." + (backend.sysInfo.protoMinor || 0) }
        ReadOnlyField { label: "Variant ID"; value: backend.sysInfo.variantId || 0 }
        ReadOnlyField { label: "Active OpMode"; value: backend.sysInfo.activeOpMode === 1 ? "Automatic" : "Normal" }
        ReadOnlyField { label: "Uptime";   value: {
                var s = backend.sysInfo.uptimeSec || 0
                var h = Math.floor(s/3600), m = Math.floor((s%3600)/60)
                return h > 0 ? h+"h "+m+"m" : m+"m "+ (s%60) + "s"
            }}

        RowLayout {
            Layout.columnSpan: 2
            spacing: 12
            StatusBadge { active: backend.sysInfo.capAutoMode || false; label: "Auto" }
            StatusBadge { active: backend.sysInfo.capEnvSensor || false; label: "Env" }
        }

        ReadOnlyField { label: "Board Temp"; value: (backend.sysInfo.boardTempC || 0).toFixed(1) + " °C" }
        ReadOnlyField { label: "Board Humidity"; value: (backend.sysInfo.boardHumidityPct || 0).toFixed(1) + " %" }

        ReadOnlyField { label: "FW Version"; value: "0x" + ((backend.sysInfo.fwVersion || 0) >>> 0).toString(16).toUpperCase() }

        ReadOnlyField { label: "Channels"; value: backend.sysInfo.supportedChannels + " (mask 0x" + ((backend.sysInfo.activeChMask || 0) >>> 0).toString(16) + ")" }

        ReadOnlyField { label: "Sys Status"; value: "0x" + ((backend.sysInfo.sysStatus || 0) >>> 0).toString(16) }
        ReadOnlyField { label: "Fault Cause"; value: "0x" + ((backend.sysInfo.faultCause || 0) >>> 0).toString(16) }
    }
}

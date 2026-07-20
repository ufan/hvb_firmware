import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

RowLayout {
    required property string label
    required property var fit   // {valid, kDevice, bDevice, r2}

    spacing: 16

    Label {
        text: root.label
        font.bold: true
        Layout.preferredWidth: 140
    }

    Label {
        text: fit.valid
              ? ("K=" + fit.kDevice + "  B=" + fit.bDevice)
              : "—"
        font.family: "monospace"
        Layout.preferredWidth: 180
    }

    Label {
        text: fit.valid ? "R²=" + fit.r2.toFixed(6) : ""
        color: fit.valid
               ? (fit.r2 >= 0.9999 ? Material.color(Material.Green)
                : fit.r2 >= 0.999  ? Material.color(Material.Orange)
                :                    Material.color(Material.Red))
               : Material.foreground
        font.family: "monospace"
    }

    Label {
        visible: !fit.valid
        text: "(not computed)"
        opacity: 0.4
    }

    id: root
}

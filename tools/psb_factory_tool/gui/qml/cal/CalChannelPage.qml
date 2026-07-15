import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import PsbFactory

// Step 3: Per-channel calibration — sweep, enter DMM readings, compute fit, write coefficients
Item {
    id: root
    signal allDone
    signal rollback

    property int currentCh: 0
    property int numCh: Backend.numChannels
    property var sweepPoints: []     // [{dacCode, adcV, adcI}]
    property bool swept: false

    // Per-channel fit results for display
    property var fitOut:   ({valid: false, kDevice: 10000, bDevice: 0, r2: 0})
    property var fitMeasV: ({valid: false, kDevice: 10000, bDevice: 0, r2: 0})
    property var fitMeasI: ({valid: false, kDevice: 10000, bDevice: 0, r2: 0})

    function resetChannel() {
        swept       = false
        sweepPoints = []
        fitOut      = {valid: false, kDevice: 10000, bDevice: 0, r2: 0}
        fitMeasV    = {valid: false, kDevice: 10000, bDevice: 0, r2: 0}
        fitMeasI    = {valid: false, kDevice: 10000, bDevice: 0, r2: 0}
    }

    onCurrentChChanged: resetChannel()

    Connections {
        target: Backend

        function onSweepFinished(ch, points) {
            if (ch !== root.currentCh) return
            root.sweepPoints = points
            root.swept = true
            sweepProgress.visible = false
        }

        function onSweepStep(step, total, dacCode, adcV, adcI) {
            sweepProgress.visible = true
            sweepProgress.value   = step / total
            sweepProgress.label   = "Step " + step + "/" + total + "  DAC=" + dacCode
        }

        function onFitReady(ch, outFit, measVFit, measIFit) {
            if (ch !== root.currentCh) return
            root.fitOut   = outFit
            root.fitMeasV = measVFit
            root.fitMeasI = measIFit
        }

        function onWriteCoeffsResult(ch, ok, message) {
            if (ch !== root.currentCh) return
            writeStatus.text  = ok ? "✓ " + message : "✗ " + message
            writeStatus.color = ok ? Material.color(Material.Green)
                                   : Material.color(Material.Red)
        }

        function onChannelCommitted(ch, ok, message) {
            if (ch !== root.currentCh) return
            if (ok && root.currentCh < root.numCh - 1) {
                root.currentCh++
            } else if (ok) {
                root.allDone()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // Channel selector
        RowLayout {
            Label { text: "Channel:"; font.bold: true }
            Repeater {
                model: root.numCh
                Button {
                    required property int index
                    text: "CH" + index
                    highlighted: index === root.currentCh
                    enabled: Backend.calActive && !Backend.sweepRunning
                    checkable: true
                    checked: index === root.currentCh
                    onClicked: root.currentCh = index
                }
            }

            Item { Layout.fillWidth: true }

            // Channel capability tags
            Row {
                spacing: 6
                Repeater {
                    model: {
                        const caps = Backend.channelCaps(root.currentCh)
                        const tags = []
                        if (caps & 0x02) tags.push("OUT")
                        if (caps & 0x04) tags.push("MEAS-V")
                        if (caps & 0x08) tags.push("MEAS-I")
                        return tags
                    }
                    Rectangle {
                        required property string modelData
                        width: capLabel.implicitWidth + 12
                        height: 20
                        radius: 3
                        color: Material.color(Material.Teal)
                        Label {
                            id: capLabel
                            anchors.centerIn: parent
                            text: parent.modelData
                            font.pixelSize: 10
                        }
                    }
                }
            }
        }

        // Sweep config quick-access
        GroupBox {
            title: "Sweep Config"
            Layout.fillWidth: true
            visible: !swept

            RowLayout {
                spacing: 16

                Repeater {
                    model: [
                        {key: "dacMin",       label: "DAC min"},
                        {key: "dacMax",       label: "DAC max"},
                        {key: "stepSize",     label: "Step"},
                        {key: "settlementMs", label: "Settle ms"},
                        {key: "cooldownMs",   label: "Cooldown ms"}
                    ]
                    ColumnLayout {
                        required property var modelData
                        Label { text: modelData.label; font.pixelSize: 10; opacity: 0.7 }
                        TextField {
                            id: cfgField
                            text: Backend.sweepConfig()[modelData.key]
                            implicitWidth: 70
                            validator: IntValidator { bottom: 0; top: 99999 }
                            onEditingFinished: {
                                const cfg = Backend.sweepConfig()
                                cfg[modelData.key] = parseInt(text)
                                Backend.setSweepConfig(cfg)
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: Backend.sweepRunning ? "Abort" : "Run Sweep"
                    highlighted: !Backend.sweepRunning
                    enabled: Backend.calActive
                    onClicked: Backend.sweepRunning ? Backend.abortSweep()
                                                    : Backend.startSweep(root.currentCh)
                }
            }
        }

        // Progress bar during sweep
        ColumnLayout {
            id: sweepProgress
            visible: false
            Layout.fillWidth: true
            property real value: 0
            property string label: ""
            ProgressBar { Layout.fillWidth: true; value: sweepProgress.value }
            Label { text: sweepProgress.label; font.pixelSize: 11; opacity: 0.8 }
        }

        // Sweep results table + DMM entry
        SweepTable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: swept
            points: root.sweepPoints
            hasOut:   (Backend.channelCaps(root.currentCh) & 0x02) !== 0
            hasMeasV: (Backend.channelCaps(root.currentCh) & 0x04) !== 0
            hasMeasI: (Backend.channelCaps(root.currentCh) & 0x08) !== 0
            onComputeFit: function(dmmPoints) {
                Backend.computeFit(root.currentCh, dmmPoints)
            }
        }

        // Fit results
        GroupBox {
            title: "Fit Results"
            Layout.fillWidth: true
            visible: swept

            ColumnLayout {
                width: parent.width
                spacing: 4

                FitResultRow {
                    label: "out (V→DAC)"
                    fit: root.fitOut
                    visible: (Backend.channelCaps(root.currentCh) & 0x02) !== 0
                }
                FitResultRow {
                    label: "meas-V (ADC→V)"
                    fit: root.fitMeasV
                    visible: (Backend.channelCaps(root.currentCh) & 0x04) !== 0
                }
                FitResultRow {
                    label: "meas-I (ADC→I)"
                    fit: root.fitMeasI
                    visible: (Backend.channelCaps(root.currentCh) & 0x08) !== 0
                }
            }
        }

        // Write status
        Label {
            id: writeStatus
            visible: text.length > 0
            font.pixelSize: 12
        }

        // Actions
        RowLayout {
            spacing: 12
            Layout.fillWidth: true

            Button {
                text: "← Back"
                onClicked: {
                    Backend.rollbackToUnlock()
                    root.rollback()
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Write Coefficients"
                enabled: swept && (root.fitOut.valid || root.fitMeasV.valid || root.fitMeasI.valid)
                onClicked: {
                    Backend.writeCoefficients(
                        root.currentCh,
                        root.fitOut.k,   root.fitOut.b,
                        root.fitMeasV.k, root.fitMeasV.b,
                        root.fitMeasI.k, root.fitMeasI.b
                    )
                }
            }

            Button {
                text: root.currentCh < root.numCh - 1
                      ? "Next Channel →" : "→ Commit"
                highlighted: true
                enabled: {
                    const fit = Backend.channelSweepFit(root.currentCh)
                    return fit.coeffsWritten || !fit.needsCal
                }
                onClicked: {
                    // commitChannel advances currentCh via onChannelCommitted signal
                    Backend.commitChannel(root.currentCh)
                }
            }
        }
    }
}

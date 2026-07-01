import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shows sweep points + DMM entry fields; emits computeFit when operator clicks
Item {
    id: root

    required property var points    // [{dacCode, adcV, adcI}]
    required property bool hasOut
    required property bool hasMeasV
    required property bool hasMeasI

    signal computeFit(var dmmPoints)  // [{dmmV, dmmI}]

    // Internal model — mirrors points with editable DMM fields
    property var dmmValues: []

    onPointsChanged: {
        dmmValues = points.map(() => ({dmmV: "", dmmI: ""}))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Header row
        Row {
            spacing: 0
            Repeater {
                model: buildHeaders()
                delegate: Label {
                    required property string modelData
                    width: colWidth(index)
                    text: modelData
                    font.bold: true
                    font.pixelSize: 11
                    opacity: 0.7
                    leftPadding: 4
                }
            }
        }

        Rectangle { color: Qt.rgba(1, 1, 1, 0.1); height: 1; Layout.fillWidth: true }

        // Scrollable rows
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                model: root.points
                spacing: 2

                delegate: Row {
                    required property var modelData
                    required property int index
                    spacing: 0

                    property int rowIdx: index

                    // DAC code
                    Label {
                        width: root.colWidth(0)
                        text: modelData.dacCode
                        leftPadding: 4
                        font.family: "monospace"
                    }
                    // ADC V
                    Label {
                        width: root.colWidth(1)
                        text: modelData.adcV
                        leftPadding: 4
                        font.family: "monospace"
                    }
                    // ADC I
                    Label {
                        width: root.colWidth(2)
                        text: modelData.adcI
                        leftPadding: 4
                        font.family: "monospace"
                    }
                    // DMM V (editable)
                    TextField {
                        visible: root.hasMeasV || root.hasOut
                        width: root.colWidth(3)
                        placeholderText: "V"
                        font.family: "monospace"
                        validator: DoubleValidator {}
                        onTextChanged: {
                            if (root.dmmValues.length > rowIdx) {
                                const copy = root.dmmValues.slice()
                                copy[rowIdx] = {dmmV: text, dmmI: copy[rowIdx].dmmI}
                                root.dmmValues = copy
                            }
                        }
                    }
                    // DMM I (editable)
                    TextField {
                        visible: root.hasMeasI
                        width: root.colWidth(4)
                        placeholderText: "mA"
                        font.family: "monospace"
                        validator: DoubleValidator {}
                        onTextChanged: {
                            if (root.dmmValues.length > rowIdx) {
                                const copy = root.dmmValues.slice()
                                copy[rowIdx] = {dmmV: copy[rowIdx].dmmV, dmmI: text}
                                root.dmmValues = copy
                            }
                        }
                    }
                }
            }
        }

        Button {
            text: "Compute Fit from DMM Readings"
            highlighted: true
            enabled: root.points.length > 0
            onClicked: {
                const dmmPoints = root.dmmValues.map(d => {
                    const m = {}
                    if (d.dmmV !== "") m.dmmV = parseFloat(d.dmmV)
                    if (d.dmmI !== "") m.dmmI = parseFloat(d.dmmI)
                    return m
                })
                root.computeFit(dmmPoints)
            }
        }
    }

    function buildHeaders() {
        const h = ["DAC Code", "ADC V", "ADC I"]
        if (hasOut || hasMeasV) h.push("DMM V (V)")
        if (hasMeasI)           h.push("DMM I (mA)")
        return h
    }

    function colWidth(col) {
        switch (col) {
        case 0: return 90
        case 1: return 100
        case 2: return 100
        case 3: return 110
        case 4: return 110
        default: return 90
        }
    }
}

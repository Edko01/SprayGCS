import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

/// Left-hand panel on the Plan map with the job at a glance: field name,
/// Undo / Redo and the job numbers of the plan's Spray Area. Shown whenever
/// the plan has a Spray Area, whichever item is selected.
Rectangle {
    id: _root

    property var missionController

    readonly property var _sprayArea: {
        var items = missionController ? missionController.visualItems : null
        if (!items || items.count < 0) {
            return null
        }
        for (var i = 0; i < items.count; i++) {
            var item = items.get(i)
            if (item && item.isSprayArea === true) {
                return item
            }
        }
        return null
    }

    readonly property real _margin: ScreenTools.defaultFontPixelWidth
    readonly property real _radius: ScreenTools.defaultFontPixelWidth / 2

    width:   ScreenTools.defaultFontPixelWidth * 28
    height:  panelColumn.height + (_margin * 2)
    visible: _sprayArea !== null
    color:   qgcPal.windowShadeDark
    radius:  _radius

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    MouseArea { anchors.fill: parent }   // keep taps off the map

    ColumnLayout {
        id: panelColumn
        anchors {
            top:     parent.top
            left:    parent.left
            right:   parent.right
            margins: _margin
        }
        spacing: _margin

        // ---- field name ---------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing:          _margin

            QGCLabel {
                text:           qsTr("Field name")
                font.pointSize: ScreenTools.mediumFontPointSize
            }
            QGCTextField {
                Layout.fillWidth:   true
                text:               _sprayArea ? _sprayArea.fieldName : ""
                placeholderText:    qsTr("e.g. North 40")
                maximumLength:      24
                onEditingFinished: {
                    if (_sprayArea) {
                        _sprayArea.fieldName = text
                    }
                }
            }
        }

        // ---- undo / redo --------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing:          _margin / 2

            Repeater {
                model: [
                    { text: qsTr("Undo"), redo: false },
                    { text: qsTr("Redo"), redo: true }
                ]

                delegate: Rectangle {
                    property bool _enabled: _sprayArea ? (modelData.redo ? _sprayArea.canRedo : _sprayArea.canUndo) : false

                    Layout.fillWidth: true
                    implicitHeight:   ScreenTools.defaultFontPixelHeight * 2
                    radius:           _radius
                    color:            qgcPal.windowShade
                    opacity:          _enabled ? 1.0 : 0.4

                    QGCLabel {
                        anchors.centerIn: parent
                        text:             modelData.text
                        font.pointSize:   ScreenTools.mediumFontPointSize
                        font.bold:        true
                    }
                    QGCMouseArea {
                        anchors.fill: parent
                        enabled:      parent._enabled
                        onClicked:    modelData.redo ? _sprayArea.redo() : _sprayArea.undo()
                    }
                }
            }
        }

        // ---- job numbers --------------------------------------------------------
        GridLayout {
            Layout.fillWidth: true
            columns:          2
            columnSpacing:    _margin / 2
            rowSpacing:       _margin / 2
            visible:          _sprayArea !== null && _sprayArea.pathValid

            Repeater {
                model: _sprayArea ? [
                    { value: _sprayArea.sprayedAcresEst.toFixed(1),  caption: _sprayArea.hasSprayed ? qsTr("acres left") : qsTr("acres sprayed") },
                    { value: _sprayArea.estimatedVolume.toFixed(1),  caption: qsTr("gal product") },
                    { value: _sprayArea.estimatedMinutes.toFixed(0), caption: qsTr("min flight") },
                    { value: _sprayArea.passCount + (_sprayArea.headlandPass.rawValue ? "+1" : ""), caption: qsTr("passes") }
                ] : []

                delegate: Rectangle {
                    Layout.fillWidth: true
                    implicitHeight:   tileColumn.height + _margin
                    color:            qgcPal.windowShade
                    radius:           _radius

                    Column {
                        id:               tileColumn
                        anchors.centerIn: parent

                        QGCLabel {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text:                     modelData.value
                            font.pointSize:           ScreenTools.largeFontPointSize
                            font.bold:                true
                        }
                        QGCLabel {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text:                     modelData.caption
                            font.pointSize:           ScreenTools.smallFontPointSize
                        }
                    }
                }
            }
        }

        // Resuming a job: what's already done (the tiles show what's left).
        QGCLabel {
            Layout.fillWidth:    true
            horizontalAlignment: Text.AlignHCenter
            text:                _sprayArea ? qsTr("Already sprayed: %1 ac").arg(_sprayArea.sprayedDoneAcres.toFixed(1)) : ""
            visible:             _sprayArea !== null && _sprayArea.hasSprayed
        }

        QGCLabel {
            Layout.fillWidth:    true
            horizontalAlignment: Text.AlignHCenter
            wrapMode:            Text.WordWrap
            color:               "#f97316"
            text:                qsTr("Breakpoint saved. Select the Spray Area to resume from it.")
            visible:             _sprayArea !== null && _sprayArea.hasBreakpoint
        }
    }
}

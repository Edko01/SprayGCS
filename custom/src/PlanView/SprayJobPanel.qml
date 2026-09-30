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
                    { value: _sprayArea.tripsValid ? _sprayArea.estimatedTrips : "?",
                      caption: _sprayArea.estimatedTrips === 1 ? qsTr("trip") : (_sprayArea.hasSprayed ? qsTr("trips left") : qsTr("trips")) },
                    { value: (_sprayArea.tripsValid ? _sprayArea.allTripsMinutes : _sprayArea.estimatedMinutes).toFixed(0),
                      caption: qsTr("min flying") },
                    { value: _sprayArea.fullLoadGallons.toFixed(1),  caption: qsTr("gal per fill") },
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

        // Trips: which runs out first, and how much to fill.
        QGCLabel {
            Layout.fillWidth:    true
            horizontalAlignment: Text.AlignHCenter
            wrapMode:            Text.WordWrap
            color:               _sprayArea && !_sprayArea.tripsValid ? qgcPal.warningText : qgcPal.text
            visible:             _sprayArea !== null && _sprayArea.pathValid
            text: {
                if (!_sprayArea) {
                    return ""
                }
                if (!_sprayArea.tripsValid) {
                    return qsTr("One battery can't get to this field and back. Check Flight time per battery (Drone tab).")
                }
                if (_sprayArea.estimatedTrips < 2) {
                    return qsTr("One trip: fill %1 gal.").arg(_sprayArea.lastLoadGallons.toFixed(1))
                }
                return (_sprayArea.tripsLimitedBy === "tank"
                        ? qsTr("The tank runs out first. Fill %1 gal each trip, %2 gal for the last.")
                        : qsTr("The battery runs out first. About %1 gal a trip; fill %2 gal for the last."))
                       .arg(_sprayArea.fullLoadGallons.toFixed(1)).arg(_sprayArea.lastLoadGallons.toFixed(1))
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

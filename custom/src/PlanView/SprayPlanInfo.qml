import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

/// SprayGCS Plan Info (replaces QGC's). With an empty plan it's the guided
/// start, "New Spray Plan":
///   1. name the field
///   2. place the takeoff point: the drone's position when one is connected,
///      otherwise tap the map (tapping the map moves it either way)
///   3. Draw Field Boundary: builds takeoff + spray area + return, selects the
///      Spray Area and leaves its boundary tools open
/// With a plan loaded it shows the plan file name.
Rectangle {
    id: _root

    required property var planMasterController
    required property var missionController
    required property var editorMap

    readonly property bool  _creating:           planMasterController.showCreateFromTemplate
    readonly property var   _vehicle:            globals.activeVehicle
    readonly property bool  _vehicleHasPosition: _vehicle ? _vehicle.coordinate.isValid : false
    readonly property bool  _takeoffSet:         missionController.homePositionSet
    readonly property real  _margin:             ScreenTools.defaultFontPixelWidth
    readonly property real  _radius:             ScreenTools.defaultFontPixelWidth / 2
    readonly property color _accent:             "#ffd400"   // XAG-style yellow, as in the Spray Area panel
    readonly property color _accentText:         "#111111"
    property bool           _usedDronePosition:  false

    width:  parent ? parent.width : 0
    height: mainColumn.height + ScreenTools.defaultFontPixelHeight
    color:  qgcPal.windowShadeDark

    QGCPalette { id: qgcPal; colorGroupEnabled: _root.enabled }

    function _sprayCreator() {
        var creators = planMasterController.planCreators
        for (var i = 0; creators && i < creators.count; i++) {
            var creator = creators.get(i)
            if (creator && typeof creator.createSprayPlan === "function") {
                return creator
            }
        }
        return null
    }

    function _useDronePosition() {
        if (_vehicleHasPosition) {
            missionController.setHomePosition(_vehicle.coordinate)
            editorMap.center = _vehicle.coordinate
            _usedDronePosition = true
        }
    }

    // With a drone connected and no takeoff point yet, start from the drone.
    function _autoPlace() {
        if (_creating && !_takeoffSet && _vehicleHasPosition) {
            _useDronePosition()
        }
    }

    function _start() {
        var creator = _sprayCreator()
        if (creator && _takeoffSet) {
            creator.createSprayPlan(editorMap.center, nameField.text)
        }
    }

    Component.onCompleted:        _autoPlace()
    on_VehicleHasPositionChanged: _autoPlace()
    on_CreatingChanged: {
        _usedDronePosition = false
        nameField.text = ""
        _autoPlace()
    }

    // Tapping the map after "Use the Drone's Position" moves the takeoff point away from the drone.
    Connections {
        target: missionController
        function onPlannedHomePositionChanged(coordinate) {
            if (_usedDronePosition && (!_vehicleHasPosition || coordinate.distanceTo(_vehicle.coordinate) > 2)) {
                _usedDronePosition = false
            }
        }
    }

    // A numbered step heading. (Inline components don't see this file's ids,
    // so it carries its own palette and colours.)
    component StepHeader: RowLayout {
        property int    number
        property string title
        property bool   done: false

        Layout.fillWidth: true
        spacing:          ScreenTools.defaultFontPixelWidth

        QGCPalette { id: stepPal }

        Rectangle {
            implicitWidth:  ScreenTools.defaultFontPixelHeight * 1.5
            implicitHeight: implicitWidth
            radius:         width / 2
            color:          done ? stepPal.colorGreen : "#ffd400"

            QGCLabel {
                anchors.centerIn: parent
                text:             done ? "\u2713" : number
                color:            done ? "white" : "#111111"
                font.bold:        true
            }
        }
        QGCLabel {
            Layout.fillWidth: true
            text:             title
            font.pointSize:   ScreenTools.mediumFontPointSize
            font.bold:        true
            wrapMode:         Text.WordWrap
        }
    }

    ColumnLayout {
        id:             mainColumn
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: _margin
        spacing:        _margin

        // ===== New Spray Plan (empty plan) ====================================
        ColumnLayout {
            Layout.fillWidth: true
            spacing:          _margin
            visible:          _creating

            // 1. Field name
            StepHeader {
                number: 1
                title:  qsTr("Name the field")
                done:   nameField.text.trim() !== ""
            }
            QGCTextField {
                id:                 nameField
                Layout.fillWidth:   true
                placeholderText:    qsTr("e.g. North 40 (optional)")
                maximumLength:      24
            }

            // 2. Takeoff point
            StepHeader {
                number: 2
                title:  qsTr("Place the takeoff point")
                done:   _takeoffSet
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text: {
                    if (!_takeoffSet) {
                        return qsTr("Tap the map where the drone will take off.")
                    }
                    if (_usedDronePosition) {
                        return qsTr("Set to the drone's position. Tap the map to move it.")
                    }
                    return qsTr("Takeoff point set. Tap the map to move it.")
                }
            }
            QGCButton {
                Layout.fillWidth: true
                text:             qsTr("Use the Drone's Position")
                visible:          _vehicleHasPosition
                onClicked:        _useDronePosition()
            }

            // 3. Boundary
            StepHeader {
                number: 3
                title:  qsTr("Draw the field boundary")
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight:   ScreenTools.defaultFontPixelHeight * 3
                radius:           _radius
                color:            _takeoffSet ? _accent : qgcPal.windowShade
                opacity:          _takeoffSet ? 1 : 0.6

                QGCLabel {
                    anchors.centerIn: parent
                    text:             qsTr("Draw Field Boundary")
                    font.pointSize:   ScreenTools.largeFontPointSize
                    font.bold:        true
                    color:            _takeoffSet ? _accentText : qgcPal.text
                }
                QGCMouseArea {
                    anchors.fill: parent
                    enabled:      _takeoffSet
                    onClicked:    _start()
                }
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                font.pointSize:   ScreenTools.smallFontPointSize
                text:             _takeoffSet
                                  ? qsTr("Then tap the field's corners on the map, or use the boundary tools above the map to draw a circle or load a KML or SHP file.")
                                  : qsTr("Place the takeoff point first.")
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                font.pointSize:   ScreenTools.smallFontPointSize
                color:            qgcPal.colorGrey
                text:             qsTr("To work on a saved plan instead, use Open in the toolbar.")
            }
        }

        // ===== Plan loaded ====================================================
        ColumnLayout {
            Layout.fillWidth: true
            spacing:          0
            visible:          !_creating

            QGCLabel {
                text: qsTr("Plan File")
            }
            QGCLabel {
                Layout.fillWidth: true
                elide:            Text.ElideMiddle
                textFormat:       Text.PlainText
                enabled:          planMasterController.currentPlanFileName !== ""
                text:             planMasterController.currentPlanFileName === "" ? qsTr("<Untitled>") : planMasterController.currentPlanFileName
            }
            QGCLabel {
                Layout.fillWidth:   true
                Layout.topMargin:   _margin / 2
                wrapMode:           Text.WordWrap
                font.pointSize:     ScreenTools.smallFontPointSize
                color:              qgcPal.colorGrey
                text:               qsTr("To move the takeoff point, drag its marker on the map.")
            }
        }
    }
}

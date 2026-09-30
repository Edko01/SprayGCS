import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import Custom.Widgets

Item {
    property var parentToolInsets                       // These insets tell you what screen real estate is available for positioning the controls in your overlay
    property var totalToolInsets:   _totalToolInsets    // The insets updated for the custom overlay additions
    property var mapControl

    readonly property string noGPS:         qsTr("NO GPS")
    readonly property real   indicatorValueWidth:   ScreenTools.defaultFontPixelWidth * 7

    property var    _activeVehicle:         QGroundControl.multiVehicleManager.activeVehicle
    property real   _indicatorDiameter:     ScreenTools.defaultFontPixelWidth * 18
    property real   _indicatorsHeight:      ScreenTools.defaultFontPixelHeight
    property var    _sepColor:              qgcPal.globalTheme === QGCPalette.Light ? Qt.rgba(0,0,0,0.5) : Qt.rgba(1,1,1,0.5)
    property color  _indicatorsColor:       qgcPal.text
    property bool   _isVehicleGps:          _activeVehicle ? _activeVehicle.gps.count.rawValue > 1 && _activeVehicle.gps.hdop.rawValue < 1.4 : false
    property string _altitude:              _activeVehicle ? (isNaN(_activeVehicle.altitudeRelative.value) ? "0.0" : _activeVehicle.altitudeRelative.value.toFixed(1)) + ' ' + _activeVehicle.altitudeRelative.units : "0.0"
    property string _distanceStr:           isNaN(_distance) ? "0" : _distance.toFixed(0) + ' ' + QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
    property real   _heading:               _activeVehicle   ? _activeVehicle.heading.rawValue : 0
    property real   _distance:              _activeVehicle ? _activeVehicle.distanceToHome.rawValue : 0
    property string _messageTitle:          ""
    property string _messageText:           ""
    property real   _toolsMargin:           ScreenTools.defaultFontPixelWidth * 0.75

    function secondsToHHMMSS(timeS) {
        var sec_num = parseInt(timeS, 10);
        var hours   = Math.floor(sec_num / 3600);
        var minutes = Math.floor((sec_num - (hours * 3600)) / 60);
        var seconds = sec_num - (hours * 3600) - (minutes * 60);
        if (hours   < 10) {hours   = "0"+hours;}
        if (minutes < 10) {minutes = "0"+minutes;}
        if (seconds < 10) {seconds = "0"+seconds;}
        return hours+':'+minutes+':'+seconds;
    }

    QGCToolInsets {
        id:                     _totalToolInsets
        leftEdgeTopInset:       parentToolInsets.leftEdgeTopInset
        leftEdgeCenterInset:    exampleRectangle.leftEdgeCenterInset
        leftEdgeBottomInset:    parentToolInsets.leftEdgeBottomInset
        rightEdgeTopInset:      parentToolInsets.rightEdgeTopInset
        rightEdgeCenterInset:   parentToolInsets.rightEdgeCenterInset
        rightEdgeBottomInset:   parent.width - compassBackground.x
        topEdgeLeftInset:       parentToolInsets.topEdgeLeftInset
        // SprayGCS: the heading strip sits at the bottom centre, so it's a bottom
        // inset. (Reporting it as a top inset, measured down to the strip, left
        // the Fly view a sliver at the bottom to keep the drone in, so the map
        // kept snapping the drone to the bottom centre.)
        topEdgeCenterInset:     parentToolInsets.topEdgeCenterInset
        topEdgeRightInset:      parentToolInsets.topEdgeRightInset
        bottomEdgeLeftInset:    parentToolInsets.bottomEdgeLeftInset
        bottomEdgeCenterInset:  Math.max(parentToolInsets.bottomEdgeCenterInset, parent.height - compassBar.y)
        bottomEdgeRightInset:   parent.height - attitudeIndicator.y
    }

    // This is an example of how you can use parent tool insets to position an element on the custom fly view layer
    // - we use parent topEdgeLeftInset to position the widget below the toolstrip
    // - we use parent bottomEdgeLeftInset to dodge the virtual joystick if enabled
    // - we use the parent leftEdgeTopInset to size our element to the same width as the ToolStripAction
    // - we export the width of this element as the leftEdgeCenterInset so that the map will recenter if the vehicle flys behind this element
    Rectangle {
        id: exampleRectangle
        visible: false // to see this example, set this to true. To view insets, enable the insets viewer FlyView.qml
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: parentToolInsets.topEdgeLeftInset + _toolsMargin
        anchors.bottomMargin: parentToolInsets.bottomEdgeLeftInset + _toolsMargin
        anchors.leftMargin: _toolsMargin
        width: parentToolInsets.leftEdgeTopInset - _toolsMargin
        color: 'red'

        property real leftEdgeCenterInset: visible ? x + width : 0
    }

    //-------------------------------------------------------------------------
    //-- Heading Indicator
    Rectangle {
        id:                         compassBar
        height:                     ScreenTools.defaultFontPixelHeight * 1.5
        width:                      ScreenTools.defaultFontPixelWidth  * 50
        anchors.bottom:             parent.bottom
        anchors.bottomMargin:       _toolsMargin
        color:                      "#DEDEDE"
        radius:                     2
        clip:                       true
        anchors.horizontalCenter:   parent.horizontalCenter
        Repeater {
            model: 720
            QGCLabel {
                function _normalize(degrees) {
                    var a = degrees % 360
                    if (a < 0) a += 360
                    return a
                }
                property int _startAngle: modelData + 180 + _heading
                property int _angle: _normalize(_startAngle)
                anchors.verticalCenter: parent.verticalCenter
                x:              visible ? ((modelData * (compassBar.width / 360)) - (width * 0.5)) : 0
                visible:        _angle % 45 == 0
                color:          "#75505565"
                font.pointSize: ScreenTools.smallFontPointSize
                text: {
                    switch(_angle) {
                    case 0:     return "N"
                    case 45:    return "NE"
                    case 90:    return "E"
                    case 135:   return "SE"
                    case 180:   return "S"
                    case 225:   return "SW"
                    case 270:   return "W"
                    case 315:   return "NW"
                    }
                    return ""
                }
            }
        }
    }
    Rectangle {
        id:                         headingIndicator
        height:                     ScreenTools.defaultFontPixelHeight
        width:                      ScreenTools.defaultFontPixelWidth * 4
        color:                      qgcPal.windowShadeDark
        anchors.top:                compassBar.top
        anchors.topMargin:          -headingIndicator.height / 2
        anchors.horizontalCenter:   parent.horizontalCenter
        QGCLabel {
            text:                   _heading
            color:                  qgcPal.text
            font.pointSize:         ScreenTools.smallFontPointSize
            anchors.centerIn:       parent
        }
    }
    Image {
        id:                         compassArrowIndicator
        height:                     _indicatorsHeight
        width:                      height
        source:                     "/custom/img/compass_pointer.svg"
        fillMode:                   Image.PreserveAspectFit
        sourceSize.height:          height
        anchors.top:                compassBar.bottom
        anchors.topMargin:          -height / 2
        anchors.horizontalCenter:   parent.horizontalCenter
    }

    Rectangle {
        id:                     compassBackground
        anchors.bottom:         attitudeIndicator.bottom
        anchors.right:          attitudeIndicator.left
        anchors.rightMargin:    -attitudeIndicator.width / 2
        width:                  -anchors.rightMargin + compassBezel.width + (_toolsMargin * 2)
        height:                 attitudeIndicator.height * 0.75
        radius:                 2
        color:                  qgcPal.window

        Rectangle {
            id:                     compassBezel
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin:     _toolsMargin
            anchors.left:           parent.left
            width:                  height
            height:                 parent.height - (northLabelBackground.height / 2) - (headingLabelBackground.height / 2)
            radius:                 height / 2
            border.color:           qgcPal.text
            border.width:           1
            color:                  Qt.rgba(0,0,0,0)
        }

        Rectangle {
            id:                         northLabelBackground
            anchors.top:                compassBezel.top
            anchors.topMargin:          -height / 2
            anchors.horizontalCenter:   compassBezel.horizontalCenter
            width:                      northLabel.contentWidth * 1.5
            height:                     northLabel.contentHeight * 1.5
            radius:                     ScreenTools.defaultFontPixelWidth  * 0.25
            color:                      qgcPal.windowShade

            QGCLabel {
                id:                 northLabel
                anchors.centerIn:   parent
                text:               "N"
                color:              qgcPal.text
                font.pointSize:     ScreenTools.smallFontPointSize
            }
        }

        Image {
            id:                 headingNeedle
            anchors.centerIn:   compassBezel
            height:             compassBezel.height * 0.75
            width:              height
            source:             "/custom/img/compass_needle.svg"
            fillMode:           Image.PreserveAspectFit
            sourceSize.height:  height
            transform: [
                Rotation {
                    origin.x:   headingNeedle.width  / 2
                    origin.y:   headingNeedle.height / 2
                    angle:      _heading
                }]
        }

        Rectangle {
            id:                         headingLabelBackground
            anchors.top:                compassBezel.bottom
            anchors.topMargin:          -height / 2
            anchors.horizontalCenter:   compassBezel.horizontalCenter
            width:                      headingLabel.contentWidth * 1.5
            height:                     headingLabel.contentHeight * 1.5
            radius:                     ScreenTools.defaultFontPixelWidth  * 0.25
            color:                      qgcPal.windowShade

            QGCLabel {
                id:                 headingLabel
                anchors.centerIn:   parent
                text:               _heading
                color:              qgcPal.text
                font.pointSize:     ScreenTools.smallFontPointSize
            }
        }
    }

    Rectangle {
        id:                     attitudeIndicator
        anchors.bottomMargin:   _toolsMargin + parentToolInsets.bottomEdgeRightInset
        anchors.rightMargin:    _toolsMargin
        anchors.bottom:         parent.bottom
        anchors.right:          parent.right
        height:                 ScreenTools.defaultFontPixelHeight * 6
        width:                  height
        radius:                 height * 0.5
        color:                  qgcPal.windowShade

        CustomAttitudeWidget {
            size:               parent.height * 0.95
            vehicle:            _activeVehicle
            showHeading:        false
            anchors.centerIn:   parent
        }
    }

    // SprayGCS: battery bar across the top of the map, DJI style. The fill is
    // the battery level, coloured by the failsafe zone it's in: green, yellow
    // below PX4's critical level (the Return failsafe), red below the
    // emergency level (Land). The label gives the level and time until Return.
    Item {
        id:                 batteryBar
        anchors.left:       parent.left
        anchors.right:      parent.right
        anchors.leftMargin: -_edgeMargin
        anchors.rightMargin: -_edgeMargin
        y:                  -_edgeMargin
        height:             _barHeight + batteryLabel.height + ScreenTools.defaultFontPixelHeight * 0.2
        visible:            _bar !== null && _bar.valid

        readonly property var  _bar:        QGroundControl.corePlugin.sprayBatteryBar !== undefined ? QGroundControl.corePlugin.sprayBatteryBar : null
        readonly property real _edgeMargin: ScreenTools.defaultFontPixelWidth * 0.75   // the Fly view's widget margin
        readonly property real _barHeight:  ScreenTools.defaultFontPixelHeight * 0.45
        readonly property real _level:      _bar ? Math.max(0, Math.min(100, _bar.percent)) : 0
        readonly property real _returnAt:   _bar ? _bar.returnPercent : 0
        readonly property real _landAt:     _bar ? _bar.landPercent : 0

        function _x(pct) { return width * pct / 100 }
        function _time(seconds) {
            var m = Math.floor(seconds / 60)
            var s = seconds % 60
            return m + ":" + (s < 10 ? "0" : "") + s
        }

        Rectangle {
            id:     barTrack
            width:  parent.width
            height: batteryBar._barHeight
            color:  Qt.rgba(0, 0, 0, 0.45)
        }
        Rectangle {   // red: below the emergency (Land) level
            height: barTrack.height
            width:  batteryBar._x(Math.min(batteryBar._level, batteryBar._landAt))
            color:  "#ef4444"
        }
        Rectangle {   // yellow: between emergency and critical (Return)
            x:      batteryBar._x(batteryBar._landAt)
            height: barTrack.height
            width:  batteryBar._x(Math.max(0, Math.min(batteryBar._level, batteryBar._returnAt) - batteryBar._landAt))
            color:  "#f59e0b"
        }
        Rectangle {   // green: above the Return level
            x:      batteryBar._x(batteryBar._returnAt)
            height: barTrack.height
            width:  batteryBar._x(Math.max(0, batteryBar._level - batteryBar._returnAt))
            color:  "#22c55e"
        }

        // Failsafe markers: a tick on the bar and what PX4 does there.
        Repeater {
            model: batteryBar._bar ? [
                { pct: batteryBar._landAt,   text: batteryBar._bar.landAction,   color: "#ef4444" },
                { pct: batteryBar._returnAt, text: batteryBar._bar.returnAction, color: "#f59e0b" }
            ] : []

            delegate: Item {
                visible: modelData.pct > 0 && modelData.text !== ""
                x:       batteryBar._x(modelData.pct)

                Rectangle {
                    x:      -width / 2
                    width:  2
                    height: batteryBar._barHeight
                    color:  "white"
                }
                Rectangle {
                    x:      -width / 2
                    y:      batteryBar._barHeight
                    width:  markerLabel.width + ScreenTools.defaultFontPixelWidth
                    height: markerLabel.height
                    radius: height / 4
                    color:  modelData.color

                    QGCLabel {
                        id:               markerLabel
                        anchors.centerIn: parent
                        text:             modelData.text
                        color:            "black"
                        font.bold:        true
                        font.pointSize:   ScreenTools.smallFontPointSize
                    }
                }
            }
        }

        // Level and time until the Return failsafe, at the end of the fill.
        Rectangle {
            id:     batteryLabel
            y:      batteryBar._barHeight
            z:      1   // over the failsafe labels when the level is near them
            x:      Math.max(0, Math.min(batteryBar.width - width, batteryBar._x(batteryBar._level) - width / 2))
            width:  batteryText.width + ScreenTools.defaultFontPixelWidth
            height: batteryText.height + ScreenTools.defaultFontPixelHeight * 0.1
            radius: height / 4
            color:  Qt.rgba(0, 0, 0, 0.7)

            QGCLabel {
                id:               batteryText
                anchors.centerIn: parent
                color:            "white"
                font.bold:        true
                text: {
                    var bar = batteryBar._bar
                    if (!bar) {
                        return ""
                    }
                    var t = Math.round(batteryBar._level) + "%"
                    if (bar.secondsToReturn >= 0 && bar.returnAction !== "") {
                        t += "   " + qsTr("%1 until %2").arg(batteryBar._time(bar.secondsToReturn)).arg(bar.returnAction)
                    }
                    return t
                }
            }
        }
    }

    // SprayGCS: after landing mid-job (Return, low battery, empty tank), one
    // tap plans what's left from the breakpoint and uploads it.
    QGCButton {
        id:                         resumeJobButton
        anchors.top:                parent.top
        anchors.topMargin:          parentToolInsets.topEdgeCenterInset + _toolsMargin + (batteryBar.visible ? batteryBar.height : 0)
        anchors.horizontalCenter:   parent.horizontalCenter
        text:                       qsTr("Resume Job")
        primary:                    true
        visible:                    _area !== null && _area.awaitingResume && _activeVehicle !== null && !_activeVehicle.flying
        onClicked:                  QGroundControl.corePlugin.sprayResumeJob()

        readonly property var _area: QGroundControl.corePlugin.sprayPlanArea !== undefined ? QGroundControl.corePlugin.sprayPlanArea : null
    }

    // SprayGCS: the plan's field boundary and the sprayed trail on the map
    // (only on the standard map; the preview GeoMap engine can't take map items).
    Loader {
        source: "qrc:/qml/Custom/Plan/SprayFlyMapItems.qml"
        onLoaded: item.map = Qt.binding(function() {
            return mapControl && typeof mapControl.addMapItem === "function" ? mapControl : null
        })
    }
}

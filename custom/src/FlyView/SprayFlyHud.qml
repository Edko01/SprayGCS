import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

/// Fly view HUD for spray missions: a phase banner (top centre) and a row of
/// large tiles (bottom right) with the job's progress, the spraying, the way
/// home and the conditions. Tank and flow read "—" until the onboard spray
/// app reports them.
Item {
    id: _root

    property var  vehicle           ///< the active vehicle, or null
    property real bannerTopMargin: 0

    readonly property var   _area:       QGroundControl.corePlugin.sprayPlanArea !== undefined ? QGroundControl.corePlugin.sprayPlanArea : null
    readonly property real  _margin:     ScreenTools.defaultFontPixelWidth * 0.75
    readonly property real  _tileWidth:  ScreenTools.defaultFontPixelWidth * 15
    readonly property color _okColor:    "#22c55e"
    readonly property color _warnColor:  "#f59e0b"
    readonly property color _badColor:   "#ef4444"
    readonly property color _dimColor:   "#9ca3af"
    readonly property bool  _flying:     vehicle !== null && vehicle.flying
    readonly property string _phase:     _area ? _area.jobPhase : ""

    /// Tiles area, for the Fly view's insets.
    readonly property alias tiles: tileFlow

    function _num(fact, digits) {
        return fact && !isNaN(fact.value) ? fact.value.toFixed(digits) + " " + fact.units : "—"
    }
    function _minSec(seconds) {
        if (seconds < 0 || isNaN(seconds)) {
            return "—"
        }
        var s = Math.round(seconds)
        var m = Math.floor(s / 60)
        s = s % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function _compass(degrees) {
        var names = [ qsTr("N"), qsTr("NE"), qsTr("E"), qsTr("SE"), qsTr("S"), qsTr("SW"), qsTr("W"), qsTr("NW") ]
        return names[Math.round(((degrees % 360) + 360) % 360 / 45) % 8]
    }

    // ---- phase banner ---------------------------------------------------------
    Rectangle {
        id:                         banner
        anchors.top:                parent.top
        anchors.topMargin:          _root.bannerTopMargin
        anchors.horizontalCenter:   parent.horizontalCenter
        width:                      bannerText.width + ScreenTools.defaultFontPixelWidth * 3
        height:                     bannerText.height + ScreenTools.defaultFontPixelHeight * 0.5
        radius:                     height / 2
        visible:                    _root._flying && _info.text !== ""
        color:                      _info.color

        readonly property var _info: {
            var pass = _root._area && _root._area.passTotal > 0
                       ? "  ·  " + qsTr("Pass %1 / %2").arg(Math.max(1, _root._area.passNumber)).arg(_root._area.passTotal) : ""
            switch (_root._phase) {
            case "toField":   return { text: qsTr("TO FIELD"),                 color: "#2563eb" }
            case "spraying":  return { text: qsTr("SPRAYING") + pass,          color: "#16a34a" }
            case "turning":   return { text: qsTr("TURNING") + pass,           color: "#0e7490" }
            case "toHome":    return { text: qsTr("JOB DONE · HEADING HOME"),  color: "#2563eb" }
            case "returning": return { text: qsTr("RETURNING"),                color: "#d97706" }
            case "paused":    return { text: qsTr("PAUSED"),                   color: "#d97706" }
            case "landing":   return { text: qsTr("LANDING"),                  color: "#d97706" }
            case "other":     return { text: _root.vehicle ? _root.vehicle.flightMode.toUpperCase() : "", color: "#4b5563" }
            }
            return { text: "", color: "transparent" }
        }

        QGCLabel {
            id:                 bannerText
            anchors.centerIn:   parent
            text:               banner._info.text
            color:              "white"
            font.bold:          true
            font.pointSize:     ScreenTools.mediumFontPointSize
        }
    }

    // ---- tiles ------------------------------------------------------------------
    // (An inline component can't see this file's ids: sizes and colours are its own.)
    component Tile: Rectangle {
        id: tile

        property string caption
        property string value
        property color  valueColor: "white"

        readonly property real _pad: ScreenTools.defaultFontPixelWidth * 0.375

        width:  ScreenTools.defaultFontPixelWidth * 15
        height: tileColumn.height + _pad * 2
        radius: _pad
        color:  Qt.rgba(0.08, 0.09, 0.10, 0.82)

        Column {
            id:                 tileColumn
            anchors.left:       parent.left
            anchors.right:      parent.right
            anchors.top:        parent.top
            anchors.margins:    tile._pad

            QGCLabel {
                width:          parent.width
                text:           tile.caption
                color:          "#9ca3af"
                font.pointSize: ScreenTools.smallFontPointSize
                elide:          Text.ElideRight
            }
            QGCLabel {
                width:          parent.width
                text:           tile.value
                color:          tile.valueColor
                font.bold:      true
                font.pointSize: ScreenTools.largeFontPointSize
                elide:          Text.ElideRight
            }
        }
    }

    // Right to left: the most looked-at first.
    Flow {
        id:                 tileFlow
        anchors.right:      parent.right
        anchors.bottom:     parent.bottom
        width:              Math.min(parent.width * 0.75, (_root._tileWidth + spacing) * 5)
        spacing:            _root._margin / 2
        layoutDirection:    Qt.RightToLeft
        visible:            _v !== null

        readonly property var _v: _root.vehicle
        readonly property var _a: _root._area

        Tile {
            readonly property real _plan: tileFlow._a ? tileFlow._a.speed.rawValue : NaN
            readonly property bool _off:  _root._phase === "spraying" && _plan > 0 && tileFlow._v
                                          && Math.abs(tileFlow._v.groundSpeed.rawValue - _plan) / _plan > 0.15
            caption:    qsTr("Speed") + (tileFlow._a ? "  ·  " + qsTr("plan %1").arg(tileFlow._a.speed.valueString) : "")
            value:      tileFlow._v ? _root._num(tileFlow._v.groundSpeed, 1) : "—"
            valueColor: _off ? _root._warnColor : "white"
        }
        Tile {
            caption:    qsTr("Height") + (tileFlow._a ? "  ·  " + qsTr("plan %1").arg(tileFlow._a.altitude.valueString) : "")
            value:      tileFlow._v ? _root._num(tileFlow._v.altitudeRelative, 1) : "—"
        }
        Tile {
            visible:    tileFlow._a !== null
            caption:    qsTr("Pump")
            value:      tileFlow._a && tileFlow._a.pumpOn ? qsTr("ON") : qsTr("OFF")
            valueColor: tileFlow._a && tileFlow._a.pumpOn ? _root._okColor : _root._dimColor
        }
        Tile {
            readonly property real _total: tileFlow._a ? tileFlow._a.sprayAreaAcres : 0
            readonly property real _done:  tileFlow._a ? Math.min(tileFlow._a.acresDone, _total) : 0
            visible:    tileFlow._a !== null
            caption:    qsTr("Sprayed") + (_total > 0 ? "  ·  " + Math.round(100 * _done / _total) + "%" : "")
            value:      _done.toFixed(1) + " / " + _total.toFixed(1) + " " + qsTr("ac")
        }
        Tile {
            visible:    tileFlow._a !== null
            caption:    qsTr("Route left")
            value:      tileFlow._a && tileFlow._a.jobMinutesLeft >= 0 ? Math.ceil(tileFlow._a.jobMinutesLeft) + " " + qsTr("min") : "—"
        }
        Tile {
            readonly property real _homeM:   tileFlow._v ? tileFlow._v.distanceToHome.rawValue : NaN
            readonly property real _transit: tileFlow._a ? tileFlow._a.transitSpeed.rawValue : 0
            caption:    qsTr("Home") + (_transit > 0 && !isNaN(_homeM) ? "  ·  " + _root._minSec(_homeM / _transit) : "")
            value:      tileFlow._v ? _root._num(tileFlow._v.distanceToHome, 0) : "—"
        }
        Tile {
            // PX4's wind estimate gives where the wind blows to; say where it comes from.
            readonly property real _dir: tileFlow._v ? tileFlow._v.wind.direction.rawValue : NaN
            caption:    qsTr("Wind") + (!isNaN(_dir) ? "  ·  " + qsTr("from %1").arg(_root._compass(_dir + 180)) : "")
            value:      tileFlow._v ? _root._num(tileFlow._v.wind.speed, 0) : "—"
        }
        Tile {
            readonly property int _lock: tileFlow._v ? tileFlow._v.gps.lock.rawValue : 0   // 3 3D, 4 DGPS, 5 RTK float, 6 RTK fixed
            caption:    qsTr("GPS") + "  ·  " + qsTr("%1 sats").arg(tileFlow._v && !isNaN(tileFlow._v.gps.count.rawValue) ? tileFlow._v.gps.count.rawValue : 0)
            value:      tileFlow._v && tileFlow._v.gps.lock.enumStringValue !== "" ? tileFlow._v.gps.lock.enumStringValue : "—"
            valueColor: _lock >= 6 ? _root._okColor : (_lock >= 3 ? "white" : _root._badColor)
        }
        Tile {
            caption:    qsTr("Tank  ·  flow")
            value:      "—"
            valueColor: _root._dimColor
        }
    }
}

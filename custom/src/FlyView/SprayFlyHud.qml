import QtQuick
import QtQuick.Shapes

import QGroundControl
import QGroundControl.Controls

/// Fly view HUD for spray missions: a status strip (top centre) with the phase
/// and the job's progress pass by pass, and a flight panel (bottom right) with
/// speed and height against the plan, the pump, the way home and the
/// conditions. Tank reads "—" until the onboard spray app reports it.
Item {
    id: _root

    property var  vehicle           ///< the active vehicle, or null
    property real bannerTopMargin: 0

    readonly property var    _area:   QGroundControl.corePlugin.sprayPlanArea !== undefined ? QGroundControl.corePlugin.sprayPlanArea : null
    readonly property bool   _flying: !!vehicle && vehicle.flying
    readonly property string _phase:  _area ? _area.jobPhase : ""

    /// Flight panel, for the Fly view's insets.
    readonly property alias tiles: flightPanel

    function _value(fact, digits) {
        return fact && !isNaN(fact.value) ? fact.value.toFixed(digits) : "—"
    }
    function _withUnits(fact, digits) {
        return fact && !isNaN(fact.value) ? fact.value.toFixed(digits) + " " + fact.units : "—"
    }
    function _minSec(seconds) {
        if (seconds < 0 || isNaN(seconds) || !isFinite(seconds)) {
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

    // Palette and sizes. The inline components get it as `s`: they can't see this file's ids.
    QtObject {
        id: _s

        readonly property color glass:    Qt.rgba(0.035, 0.08, 0.12, 0.86)
        readonly property color edge:     "#5ce1ff"
        readonly property color hairline: Qt.rgba(0.36, 0.88, 1.0, 0.28)
        readonly property color ink:      "#f2f8fc"
        readonly property color dim:      "#8ea3b4"
        readonly property color track:    Qt.rgba(1, 1, 1, 0.13)
        readonly property color spray:    "#4a94ff"     // the sprayed trail's blue, lifted for dark glass
        readonly property color ok:       "#2ee59d"
        readonly property color warn:     "#ffb224"
        readonly property color bad:      "#ff5252"
        readonly property real  u:        ScreenTools.defaultFontPixelWidth
        readonly property real  h:        ScreenTools.defaultFontPixelHeight
    }

    // ---- components -------------------------------------------------------------

    // The HUD's typeface, registered by CustomPlugin. Drawn as curves, like the
    // panels: the glyph-cache text path showed this font in the wrong colours
    // under WSL (white as yellow, grey as black).
    component HudText: QGCLabel {
        renderType:  Text.CurveRendering
        font.family: "Chakra Petch"
        font.weight: Font.Medium
    }

    // Glass panel with two cut corners and bright edges on the cuts.
    component Panel: Shape {
        id: panel

        required property var s
        readonly property real _cut: s.u * 1.4

        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: 1
            strokeColor: panel.s.hairline
            fillColor:   panel.s.glass
            startX:      panel._cut
            startY:      0
            PathLine { x: panel.width;              y: 0 }
            PathLine { x: panel.width;              y: panel.height - panel._cut }
            PathLine { x: panel.width - panel._cut; y: panel.height }
            PathLine { x: 0;                        y: panel.height }
            PathLine { x: 0;                        y: panel._cut }
            PathLine { x: panel._cut;               y: 0 }
        }
        ShapePath {
            strokeWidth: 2
            strokeColor: panel.s.edge
            fillColor:   "transparent"
            capStyle:    ShapePath.FlatCap
            startX:      0
            startY:      panel._cut * 2.4
            PathLine { x: 0;                              y: panel._cut }
            PathLine { x: panel._cut;                     y: 0 }
            PathLine { x: panel._cut * 2.4;               y: 0 }
            PathMove { x: panel.width;                    y: panel.height - panel._cut * 2.4 }
            PathLine { x: panel.width;                    y: panel.height - panel._cut }
            PathLine { x: panel.width - panel._cut;       y: panel.height }
            PathLine { x: panel.width - panel._cut * 2.4; y: panel.height }
        }
    }

    // A big number with its units, a caption, and a meter against the plan.
    component Readout: Item {
        id: readout

        required property var s
        property string caption
        property string value:      "—"
        property string units
        property real   actual:     NaN     ///< raw value, for the meter
        property real   plan:       NaN     ///< raw planned value; the meter shows when it's > 0
        property string planText
        property bool   alignRight: false

        readonly property bool _hasPlan: plan > 0 && !isNaN(actual)
        readonly property bool _off:     _hasPlan && Math.abs(actual - plan) / plan > 0.15
        readonly property real _scale:   _hasPlan ? Math.max(plan * 1.5, actual) : 1

        height: readoutColumn.height

        Column {
            id:      readoutColumn
            width:   parent.width
            spacing: readout.s.h * 0.1

            Item {
                width:  parent.width
                height: captionLabel.height

                HudText {
                    id:             captionLabel
                    anchors.left:   readout.alignRight ? undefined : parent.left
                    anchors.right:  readout.alignRight ? parent.right : undefined
                    text:           readout.caption
                    color:          readout.s.dim
                    font.pointSize: ScreenTools.defaultFontPointSize
                }
                HudText {
                    anchors.left:   readout.alignRight ? parent.left : undefined
                    anchors.right:  readout.alignRight ? undefined : parent.right
                    text:           readout.planText
                    color:          readout._off ? readout.s.warn : readout.s.dim
                    font.pointSize: ScreenTools.defaultFontPointSize
                    visible:        readout._hasPlan
                }
            }

            Item {
                width:  parent.width
                height: valueLabel.height

                Row {
                    anchors.left:  readout.alignRight ? undefined : parent.left
                    anchors.right: readout.alignRight ? parent.right : undefined
                    spacing:       readout.s.u * 0.5

                    // The font's digits differ in width: size the number for its widest
                    // digits so it doesn't shuffle the units as it changes.
                    HudText {
                        id:                     valueLabel
                        width:                  Math.max(implicitWidth, widestValue.advanceWidth)
                        horizontalAlignment:    Text.AlignRight
                        text:                   readout.value
                        color:                  readout._off ? readout.s.warn : readout.s.ink
                        font.pointSize:         ScreenTools.defaultFontPointSize * 3
                        font.weight:            Font.Bold

                        TextMetrics {
                            id:     widestValue
                            font:   valueLabel.font
                            text:   readout.value.replace(/[0-9]/g, "0")
                        }
                    }
                    HudText {
                        anchors.baseline: valueLabel.baseline
                        text:             readout.units
                        color:            readout.s.dim
                        font.pointSize:   ScreenTools.mediumFontPointSize
                    }
                }
            }

            // Meter: the fill is the actual value, the white tick the plan.
            Item {
                width:   parent.width
                height:  readout.s.h * 0.4
                opacity: readout._hasPlan ? 1 : 0

                readonly property real _planX:   width * readout.plan / readout._scale
                readonly property real _fillW:   width * Math.min(1, readout.actual / readout._scale)

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width:  parent.width
                    height: readout.s.h * 0.16
                    radius: height / 2
                    color:  readout.s.track
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x:      readout.alignRight ? parent.width - width : 0
                    width:  isNaN(parent._fillW) ? 0 : Math.max(0, parent._fillW)
                    height: readout.s.h * 0.16
                    radius: height / 2
                    color:  readout._off ? readout.s.warn : readout.s.spray
                }
                Rectangle {
                    x:      (readout.alignRight ? parent.width - parent._planX : parent._planX) - width / 2
                    width:  2
                    height: parent.height
                    color:  readout.s.ink
                }
            }
        }
    }

    // Nozzle with its spray fan; the fan pulses while the pump is on.
    component PumpGauge: Item {
        id: pump

        required property var s
        property bool spraying: false

        height: pumpColumn.height

        Column {
            id:                         pumpColumn
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    pump.s.h * 0.25

            Item {
                id:                         nozzle
                anchors.horizontalCenter:   parent.horizontalCenter
                width:                      pump.s.u * 7
                height:                     pump.s.h * 2.6

                readonly property real _tipY: height * 0.3

                Shape {
                    id:                     fan
                    anchors.fill:           parent
                    preferredRendererType:  Shape.CurveRenderer

                    ShapePath {
                        strokeWidth: 1.5
                        strokeColor: pump.spraying ? pump.s.spray : pump.s.dim
                        fillGradient: LinearGradient {
                            x1: 0; y1: nozzle._tipY
                            x2: 0; y2: nozzle.height
                            GradientStop { position: 0; color: pump.spraying ? Qt.rgba(0.29, 0.58, 1, 0.9) : "transparent" }
                            GradientStop { position: 1; color: pump.spraying ? Qt.rgba(0.29, 0.58, 1, 0.08) : "transparent" }
                        }
                        startX: nozzle.width / 2
                        startY: nozzle._tipY
                        PathLine { x: nozzle.width; y: nozzle.height }
                        PathLine { x: 0;            y: nozzle.height }
                        PathLine { x: nozzle.width / 2; y: nozzle._tipY }
                    }

                    SequentialAnimation on opacity {
                        running: pump.spraying
                        loops:   Animation.Infinite
                        NumberAnimation { to: 0.5; duration: 650; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 1.0; duration: 650; easing.type: Easing.InOutSine }
                    }
                }
                Shape {
                    anchors.fill:           parent
                    preferredRendererType:  Shape.CurveRenderer

                    ShapePath {
                        strokeColor: "transparent"
                        fillColor:   pump.spraying ? pump.s.ink : pump.s.dim
                        startX:      nozzle.width * 0.38
                        startY:      0
                        PathLine { x: nozzle.width * 0.62; y: 0 }
                        PathLine { x: nozzle.width * 0.55; y: nozzle._tipY }
                        PathLine { x: nozzle.width * 0.45; y: nozzle._tipY }
                        PathLine { x: nozzle.width * 0.38; y: 0 }
                    }
                }

                onVisibleChanged: fan.opacity = 1
                Connections {
                    target: pump
                    function onSprayingChanged() { fan.opacity = 1 }
                }
            }

            HudText {
                anchors.horizontalCenter:   parent.horizontalCenter
                text:                       pump.spraying ? qsTr("Spraying") : qsTr("Pump off")
                color:                      pump.spraying ? pump.s.spray : pump.s.dim
                font.weight:                Font.Bold
                font.pointSize:             ScreenTools.mediumFontPointSize
            }
        }
    }

    // One fact in the panel's bottom row. The divider marks it off from the cell before.
    component InfoCell: Item {
        id: cell

        required property var s
        property string caption
        property string value:        "—"
        property string detail
        property color  valueColor:   s.ink
        property real   arrowDegrees: NaN     ///< optional arrow before the value, 0 = up
        property bool   divider:      true

        readonly property real _inset: divider ? s.u * 0.9 : 0

        height: cellColumn.height

        Rectangle {
            visible: cell.divider
            width:   1
            height:  parent.height
            color:   cell.s.hairline
        }

        Column {
            id:                 cellColumn
            anchors.left:       parent.left
            anchors.right:      parent.right
            anchors.leftMargin: cell._inset
            spacing:            cell.s.h * 0.05

            HudText {
                width:          parent.width
                text:           cell.caption
                color:          cell.s.dim
                font.pointSize: ScreenTools.defaultFontPointSize
                elide:          Text.ElideRight
            }
            Row {
                width:   parent.width
                spacing: cell.s.u * 0.4

                Shape {
                    id:                     arrow
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  cell.s.h * 0.9
                    height:                 width
                    visible:                !isNaN(cell.arrowDegrees)
                    rotation:               isNaN(cell.arrowDegrees) ? 0 : cell.arrowDegrees
                    preferredRendererType:  Shape.CurveRenderer

                    ShapePath {
                        strokeColor: "transparent"
                        fillColor:   cell.s.edge
                        startX:      arrow.width / 2
                        startY:      0
                        PathLine { x: arrow.width;       y: arrow.height }
                        PathLine { x: arrow.width / 2;   y: arrow.height * 0.7 }
                        PathLine { x: 0;                 y: arrow.height }
                        PathLine { x: arrow.width / 2;   y: 0 }
                    }
                }
                HudText {
                    width:          parent.width - (arrow.visible ? arrow.width + parent.spacing : 0)
                    text:           cell.value
                    color:          cell.valueColor
                    font.weight:    Font.Bold
                    font.pointSize: ScreenTools.mediumFontPointSize
                    elide:          Text.ElideRight
                }
            }
            HudText {
                width:          parent.width
                text:           cell.detail
                color:          cell.s.dim
                font.pointSize: ScreenTools.defaultFontPointSize * 0.9
                elide:          Text.ElideRight
            }
        }
    }

    // ---- status strip -----------------------------------------------------------
    Item {
        id:                         statusStrip
        anchors.top:                parent.top
        anchors.topMargin:          _root.bannerTopMargin + _s.h * 0.4
        anchors.horizontalCenter:   parent.horizontalCenter
        width:                      Math.min(_root.width * 0.6, _s.u * 54)
        height:                     stripColumn.height + _s.h * 1.1
        visible:                    _root._flying && _info.text !== ""

        readonly property var _info: {
            switch (_root._phase) {
            case "toField":   return { text: qsTr("TO FIELD"),     color: _s.edge }
            case "spraying":  return { text: qsTr("SPRAYING"),     color: _s.spray }
            case "turning":   return { text: qsTr("TURNING"),      color: _s.spray }
            case "toHome":    return { text: qsTr("JOB DONE"),     color: _s.ok }
            case "returning": return { text: qsTr("RETURNING"),    color: _s.warn }
            case "paused":    return { text: qsTr("PAUSED"),       color: _s.warn }
            case "landing":   return { text: qsTr("LANDING"),      color: _s.warn }
            case "other":     return { text: _root.vehicle ? _root.vehicle.flightMode.toUpperCase() : "", color: _s.dim }
            }
            return { text: "", color: "transparent" }
        }
        readonly property int  _passTotal: _root._area ? _root._area.passTotal : 0
        readonly property int  _pass:      _passTotal > 0 ? Math.max(1, Math.min(_root._area.passNumber, _passTotal)) : 0
        readonly property real _acTotal:   _root._area ? _root._area.sprayAreaAcres : 0
        readonly property real _acDone:    _root._area ? Math.min(_root._area.acresDone, _acTotal) : 0
        readonly property real _fraction:  _acTotal > 0 ? _acDone / _acTotal : 0
        // Share of the job behind the drone, and up to the end of the pass it's on.
        readonly property real _doneTo:    _root._phase === "toHome" ? 1 : (_passTotal > 0 ? (_pass - 1) / _passTotal : _fraction)
        readonly property real _currentTo: _root._phase === "toHome" ? 1 : (_passTotal > 0 ? _pass / _passTotal : _fraction)

        Panel { anchors.fill: parent; s: _s }

        Column {
            id:                     stripColumn
            anchors.left:           parent.left
            anchors.right:          parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin:     _s.u * 2
            anchors.rightMargin:    _s.u * 2
            spacing:                _s.h * 0.35

            Item {
                width:  parent.width
                height: phaseLabel.height

                Rectangle {
                    id:                     phaseTick
                    anchors.verticalCenter: parent.verticalCenter
                    width:                  _s.u * 0.5
                    height:                 phaseLabel.height * 0.7
                    color:                  statusStrip._info.color
                }
                HudText {
                    id:                 phaseLabel
                    anchors.left:       phaseTick.right
                    anchors.leftMargin: _s.u * 0.8
                    text:               statusStrip._info.text
                    color:              statusStrip._info.color
                    font.weight:        Font.Bold
                    font.pointSize:     ScreenTools.largeFontPointSize
                    font.letterSpacing: _s.u * 0.25
                }
                HudText {
                    anchors.right:      parent.right
                    anchors.baseline:   phaseLabel.baseline
                    visible:            statusStrip._passTotal > 0
                    text:               qsTr("Pass %1 of %2").arg(statusStrip._pass).arg(statusStrip._passTotal)
                    color:              _s.ink
                    font.weight:        Font.Bold
                    font.pointSize:     ScreenTools.mediumFontPointSize
                }
            }

            // One slanted segment per pass (a few passes each on very big jobs):
            // sprayed, the one the drone is on, still to fly.
            Row {
                id:      passSegments
                width:   parent.width
                height:  _s.h * 0.6
                spacing: Math.max(1, Math.min(_s.u * 0.35, width / _count * 0.3))
                visible: _root._area !== null

                readonly property int  _count:    statusStrip._passTotal > 0 ? Math.min(statusStrip._passTotal, 60) : 30
                readonly property real _segWidth: (width - spacing * (_count - 1)) / _count
                readonly property real _slant:    height * 0.45

                Repeater {
                    model: passSegments._count

                    delegate: Rectangle {
                        readonly property real _start: index / passSegments._count
                        readonly property real _end:   (index + 1) / passSegments._count
                        readonly property bool _done:    _end <= statusStrip._doneTo + 1e-6
                        readonly property bool _current: !_done && _start < statusStrip._currentTo - 1e-6

                        width:          passSegments._segWidth
                        height:         passSegments.height
                        antialiasing:   true
                        color:          _done ? _s.spray : (_current ? _s.ink : _s.track)
                        transform: Matrix4x4 {
                            // Lean right by the slant, about the middle of the bar.
                            matrix: Qt.matrix4x4(1, -passSegments._slant / passSegments.height, 0, passSegments._slant / 2,
                                                 0, 1, 0, 0,
                                                 0, 0, 1, 0,
                                                 0, 0, 0, 1)
                        }
                    }
                }
            }

            Item {
                width:   parent.width
                height:  percentLabel.height
                visible: _root._area !== null

                Row {
                    spacing: _s.u * 0.6

                    HudText {
                        id:             percentLabel
                        text:           Math.round(100 * statusStrip._fraction) + "%"
                        color:          _s.ink
                        font.weight:    Font.Bold
                        font.pointSize: ScreenTools.mediumFontPointSize
                    }
                    HudText {
                        anchors.baseline:   percentLabel.baseline
                        text:               qsTr("sprayed")
                        color:              _s.dim
                    }
                }
                HudText {
                    anchors.right:      parent.right
                    anchors.baseline:   percentLabel.baseline
                    text:               qsTr("%1 of %2 ac").arg(statusStrip._acDone.toFixed(1)).arg(statusStrip._acTotal.toFixed(1))
                    color:              _s.ink
                }
            }
        }
    }

    // ---- flight panel -------------------------------------------------------------
    Item {
        id:             flightPanel
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        width:          Math.min(_root.width * 0.62, _s.u * 62)
        height:         panelColumn.height + _s.h * 1.3
        visible:        !!_v

        readonly property var    _v:        _root.vehicle
        readonly property var    _a:        _root._area
        readonly property bool   _inField:  _root._phase === "spraying" || _root._phase === "turning"
        readonly property bool   _transit:  _root._phase === "toField" || _root._phase === "toHome" || _root._phase === "returning"
        // The speed the plan asks for right now: spray speed in the field, transit speed on the way.
        readonly property var    _planSpeed: !_a ? null : (_inField ? _a.speed : (_transit ? _a.transitSpeed : null))

        Panel { anchors.fill: parent; s: _s }

        Column {
            id:                     panelColumn
            anchors.left:           parent.left
            anchors.right:          parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin:     _s.u * 2
            anchors.rightMargin:    _s.u * 2
            spacing:                _s.h * 0.6

            Row {
                id:     primaryRow
                width:  parent.width

                readonly property real _sideWidth: (width - pumpGauge.width) / 2

                Readout {
                    s:          _s
                    width:      primaryRow._sideWidth
                    caption:    qsTr("Speed")
                    value:      flightPanel._v ? _root._value(flightPanel._v.groundSpeed, 1) : "—"
                    units:      flightPanel._v ? flightPanel._v.groundSpeed.units : ""
                    actual:     flightPanel._v ? flightPanel._v.groundSpeed.rawValue : NaN
                    plan:       flightPanel._planSpeed ? flightPanel._planSpeed.rawValue : NaN
                    planText:   flightPanel._planSpeed ? qsTr("plan %1").arg(flightPanel._planSpeed.valueString) : ""
                }
                PumpGauge {
                    id:         pumpGauge
                    s:          _s
                    width:      _s.u * 13
                    spraying:   !!flightPanel._a && flightPanel._a.pumpOn
                }
                Readout {
                    s:          _s
                    width:      primaryRow._sideWidth
                    alignRight: true
                    caption:    qsTr("Height")
                    value:      flightPanel._v ? _root._value(flightPanel._v.altitudeRelative, 1) : "—"
                    units:      flightPanel._v ? flightPanel._v.altitudeRelative.units : ""
                    actual:     flightPanel._v ? flightPanel._v.altitudeRelative.rawValue : NaN
                    plan:       flightPanel._a && flightPanel._inField ? flightPanel._a.altitude.rawValue : NaN
                    planText:   flightPanel._a ? qsTr("plan %1").arg(flightPanel._a.altitude.valueString) : ""
                }
            }

            Rectangle {
                width:  parent.width
                height: 1
                color:  _s.hairline
            }

            Row {
                id:     infoRow
                width:  parent.width

                readonly property real _cellWidth: width / 5

                InfoCell {
                    readonly property real _homeM:   flightPanel._v ? flightPanel._v.distanceToHome.rawValue : NaN
                    readonly property real _transit: flightPanel._a ? flightPanel._a.transitSpeed.rawValue : 0
                    s:          _s
                    width:      infoRow._cellWidth
                    divider:    false
                    caption:    qsTr("Home")
                    value:      flightPanel._v ? _root._withUnits(flightPanel._v.distanceToHome, 0) : "—"
                    detail:     _transit > 0 && !isNaN(_homeM) ? qsTr("%1 away").arg(_root._minSec(_homeM / _transit)) : ""
                }
                InfoCell {
                    // PX4's wind estimate gives where the wind blows to: the arrow
                    // points that way, the text says where it comes from.
                    readonly property real _dir: flightPanel._v ? flightPanel._v.wind.direction.rawValue : NaN
                    s:              _s
                    width:          infoRow._cellWidth
                    caption:        qsTr("Wind")
                    value:          flightPanel._v ? _root._withUnits(flightPanel._v.wind.speed, 0) : "—"
                    detail:         !isNaN(_dir) ? qsTr("from %1").arg(_root._compass(_dir + 180)) : ""
                    arrowDegrees:   _dir
                }
                InfoCell {
                    readonly property int _lock: flightPanel._v ? flightPanel._v.gps.lock.rawValue : 0   // 3 3D, 4 DGPS, 5 RTK float, 6 RTK fixed
                    readonly property var _sats: flightPanel._v ? flightPanel._v.gps.count.rawValue : NaN
                    s:          _s
                    width:      infoRow._cellWidth
                    caption:    qsTr("GPS")
                    value:      flightPanel._v && flightPanel._v.gps.lock.enumStringValue !== "" ? flightPanel._v.gps.lock.enumStringValue : "—"
                    valueColor: _lock >= 6 ? _s.ok : (_lock >= 3 ? _s.ink : _s.bad)
                    detail:     isNaN(_sats) ? "" : qsTr("%1 sats").arg(_sats)
                }
                InfoCell {
                    s:          _s
                    width:      infoRow._cellWidth
                    caption:    qsTr("Route left")
                    value:      flightPanel._a && flightPanel._a.jobMinutesLeft >= 0 ? Math.ceil(flightPanel._a.jobMinutesLeft) + " " + qsTr("min") : "—"
                    detail:     flightPanel._a && flightPanel._a.jobMinutesLeft >= 0 ? qsTr("to finish") : ""
                }
                InfoCell {
                    s:          _s
                    width:      infoRow._cellWidth
                    caption:    qsTr("Tank")
                    value:      "—"
                    valueColor: _s.dim
                    detail:     qsTr("flow —")
                }
            }
        }
    }
}

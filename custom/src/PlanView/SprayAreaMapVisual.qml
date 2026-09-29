import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap
import QGroundControl.PlanView

/// Map visuals for SprayAreaComplexItem:
///   * the field boundary, editable with the standard Polygon Tools
///     (draw, circle, trace, load KML/SHP)
///   * the spray area (boundary minus edge margin), shaded
///   * the route, coloured like XAG One: yellow = spray, white = no spray,
///     red = leaves the field boundary, with a dark outline for any imagery
///   * section lengths when zoomed in
///
/// Edit Route mode works like XAG One:
///   * tap a section: pump on/off, add a point, delete the section
///   * press and hold on a section, then drag: adds a point there and moves it
///   * drag a point: moves it; tap a point: delete it
///   * drag anywhere else: pans the map
///
/// Transit (to and from the field) is drawn blue. Edit Transit mode:
///   * mode A: tap a side's number to make it the entry side
///   * mode B: the planned route edits like the spray route (hold and drag on
///     a leg to add a point, drag a point to move it, tap a point to delete it)
/// Align Passes: tap a side's number and the passes run along that side.
/// The green S (start) can be dragged to any end of the first or last pass of
/// a block (green dots show while dragging; the passes are laid out again from
/// there), or onto the E to fly the route the other way round.

Item {
    id: _root

    property var  map
    property var  vehicle
    property bool interactive: true

    signal clicked(int sequenceNumber)

    property var  _missionItem: object
    property var  _polygon:     object.fieldPolygon
    property bool _currentItem: object.isCurrentItem
    property bool _editRoute:   _currentItem && _root.interactive && _missionItem.routeEditMode && _missionItem.pathValid
    property bool _editSides:   _currentItem && _root.interactive && _missionItem.sideEditMode && _polygon.isValid
    property bool _alignPasses: _currentItem && _root.interactive && _missionItem.passAlignMode && _polygon.isValid
    property bool _routeTransit: _missionItem.transitMode.rawValue === 1
    property bool _editTransit: _currentItem && _root.interactive && _missionItem.transitEditMode && _missionItem.pathValid
    property bool _editTransitRoute: _editTransit && _routeTransit && _missionItem.hasTakeoff
    property bool _editGate:    _editTransit && !_routeTransit
    property bool _editActive:  _editRoute || _editTransitRoute     // the touch area below is in use
    property bool _previewing:  _missionItem.previewPath.length > 1   // a slider is being dragged
    readonly property real _routeDim: _previewing ? 0.25 : 1.0
    // While the boundary is being drawn or edited only the boundary is shown, so the
    // generated route doesn't cover the imagery along the edge. It's back on Done.
    readonly property bool _shapingBoundary: _currentItem && _root.interactive
                                             && (_polygon.traceMode || _missionItem.boundaryEditMode)
    property var  _editPoints:  _editRoute ? _missionItem.flightPath : (_editTransitRoute ? _missionItem.transitEditPoints : [])

    readonly property color _boundaryColor: "#ffffff"
    readonly property color _sprayColor:    "#ffd400"   // XAG-style yellow
    readonly property color _offColor:      "#ffffff"
    readonly property color _outsideColor:  "#ff3b30"   // section leaves the field
    readonly property color _outlineColor:  "#000000"
    readonly property color _sideColor:     "#16a34a"   // side with its own buffer (green, like XAG One)
    readonly property color _transitColor:  "#00b3ff"   // flying to and from the field
    readonly property color _startColor:    "#34c759"
    readonly property color _endColor:      "#b91c1c"
    readonly property real  _handleSize:    ScreenTools.defaultFontPixelHeight * 1.1
    readonly property real  _labelMinZoom:  17          // show section lengths from this zoom level in
    readonly property real  _labelMinLenM:  15          // ...on sections at least this long

    // Live preview while dragging a point (the route itself updates on release).
    property bool _previewActive: false
    property var  _previewCoord:  QtPositioning.coordinate()
    property var  _previewPrev:   null
    property var  _previewNext:   null
    property int  _dragPoint:     -1     // point being moved; its handle is hidden meanwhile
    property bool _draggingStart: false  // the S is being dragged: show where it can go

    // Section lengths only when the passes are far enough apart on screen for
    // their labels not to pile up into a band.
    readonly property real _metersPerPixel: {
        var zoom = map.zoomLevel   // re-evaluate on zoom
        var c    = map.center
        var a    = map.fromCoordinate(c, false)
        var b    = map.fromCoordinate(c.atDistanceAndAzimuth(100, 90), false)
        var px   = Math.hypot(b.x - a.x, b.y - a.y)
        return px > 0 ? 100 / px : 1e9
    }
    readonly property bool _labelsFit: _missionItem.swathWidth.rawValue / _metersPerPixel >= ScreenTools.defaultFontPixelHeight * 1.6

    /// True if the passes (at `passAngle`) run along this side, either way.
    function _passesAlongSide(side, passAngle) {
        if (!side || side.bearing < 0) {
            return false
        }
        var d = Math.abs(side.bearing - passAngle) % 180
        return Math.min(d, 180 - d) < 0.05
    }

    function _segmentColor(segment) {
        if (!segment) {
            return _offColor
        }
        return segment.outside ? _outsideColor : (segment.spray ? _sprayColor : _offColor)
    }

    /// What is under the finger: a point (preferred) or a section, or null.
    function _hitTest(x, y) {
        var pts = _editPoints
        var screen = []
        var bestPoint = -1
        var bestPointDist = _handleSize * 0.9
        for (var i = 0; i < pts.length; i++) {
            var s = map.fromCoordinate(pts[i], false /* clipToViewPort */)
            screen.push(s)
            if (_editTransitRoute && i === 0) {
                continue   // the takeoff point is set by the Takeoff item, not here
            }
            var d = Math.hypot(s.x - x, s.y - y)
            if (d <= bestPointDist) {
                bestPointDist = d
                bestPoint = i
            }
        }
        if (bestPoint >= 0) {
            return { type: "point", index: bestPoint }
        }

        var bestSeg = -1
        var bestSegDist = ScreenTools.defaultFontPixelHeight
        var bestT = 0
        for (var j = 0; j + 1 < screen.length; j++) {
            var a = screen[j]
            var b = screen[j + 1]
            var dx = b.x - a.x
            var dy = b.y - a.y
            var len2 = dx * dx + dy * dy
            var t = len2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0
            t = Math.max(0, Math.min(1, t))
            var ds = Math.hypot(a.x + t * dx - x, a.y + t * dy - y)
            if (ds <= bestSegDist) {
                bestSegDist = ds
                bestSeg = j
                bestT = t
            }
        }
        if (bestSeg >= 0) {
            var sa = screen[bestSeg]
            var sb = screen[bestSeg + 1]
            return {
                type:       "segment",
                index:      bestSeg,
                coordinate: map.toCoordinate(Qt.point(sa.x + bestT * (sb.x - sa.x), sa.y + bestT * (sb.y - sa.y)), false)
            }
        }
        return null
    }

    function _startPreview(prevCoord, coord, nextCoord, dragPoint) {
        _previewPrev   = prevCoord
        _previewNext   = nextCoord
        _previewCoord  = coord
        _dragPoint     = dragPoint
        _previewActive = true
    }

    function _endPreview() {
        _previewActive = false
        _dragPoint     = -1
        _previewPrev   = null
        _previewNext   = null
    }

    on_EditActiveChanged: _endPreview()

    // ----- field boundary (provides the Polygon Tools toolbar) ---------------
    QGCMapPolygonVisuals {
        id:              polygonVisuals
        mapControl:      map
        mapPolygon:      _polygon
        // Only in Edit Boundary mode (on by itself while there's no boundary yet).
        interactive:     _currentItem && _root.interactive && (_missionItem.boundaryEditMode || !_polygon.isValid)
                         && !_editRoute && !_editSides && !_editTransit && !_alignPasses
        borderWidth:     3
        borderColor:     _boundaryColor
        interiorColor:   _boundaryColor
        interiorOpacity: 0.0
    }

    // ----- menus -------------------------------------------------------------
    QGCMenu {
        id: segmentMenu

        property int  segmentIndex: -1
        property bool segmentSpray: false
        property var  coordinate

        function popupSegment(index, coord) {
            segmentIndex = index
            segmentSpray = _missionItem.segments[index].spray
            coordinate   = coord
            popup()
        }

        QGCMenuItem {
            text:        segmentMenu.segmentSpray ? qsTr("Turn spray OFF for this section") : qsTr("Turn spray ON for this section")
            onTriggered: _missionItem.toggleSegment(segmentMenu.segmentIndex)
        }
        QGCMenuItem {
            text:        qsTr("Add a point here")
            onTriggered: _missionItem.insertRoutePoint(segmentMenu.segmentIndex, segmentMenu.coordinate)
        }
        QGCMenuItem {
            text:        qsTr("Delete this section")
            enabled:     _missionItem.flightPath.length > 3
            onTriggered: _missionItem.removeSegment(segmentMenu.segmentIndex)
        }
    }

    QGCMenu {
        id: transitSegmentMenu

        property int segmentIndex: -1
        property var coordinate

        function popupSegment(index, coord) {
            segmentIndex = index
            coordinate   = coord
            popup()
        }

        QGCMenuItem {
            text:        qsTr("Add a point here")
            onTriggered: _missionItem.insertTransitPoint(transitSegmentMenu.segmentIndex, transitSegmentMenu.coordinate)
        }
    }

    QGCMenu {
        id: transitPointMenu

        property int pointIndex: -1

        function popupPoint(index) {
            pointIndex = index
            popup()
        }

        QGCMenuItem {
            text:        qsTr("Delete this point")
            enabled:     _missionItem.transitEditPoints.length > 2   // the entry point always stays
            onTriggered: _missionItem.removeTransitPoint(transitPointMenu.pointIndex)
        }
    }

    QGCMenu {
        id: pointMenu

        property int pointIndex: -1

        function popupPoint(index) {
            pointIndex = index
            popup()
        }

        QGCMenuItem {
            text:        qsTr("Delete this point")
            enabled:     _missionItem.flightPath.length > 2
            onTriggered: _missionItem.removeRoutePoint(pointMenu.pointIndex)
        }
    }

    // ----- field boundary line ------------------------------------------------
    // Drawn here because QGCMapPolygonVisuals fades its border with its fill,
    // and the fill is fully transparent. White with a dark edge, so it shows on
    // any imagery; follows the points while they're dragged.
    readonly property var _boundaryPath: {
        var path = _polygon.vertexDrag ? _polygon.dragPath : _polygon.path   // live while a point is dragged
        var closed = []
        for (var i = 0; i < path.length; i++) {
            closed.push(path[i])
        }
        if (closed.length > 2) {
            closed.push(path[0])
        }
        return closed
    }

    Component {
        id: boundaryOutlineComponent

        MapPolyline {
            line.color: _outlineColor
            line.width: 6
            opacity:    0.6 * _root.opacity
            path:       _root._boundaryPath
            visible:    _root._boundaryPath.length > 1
            z:          QGroundControl.zOrderMapItems - 1.6
        }
    }

    Component {
        id: boundaryLineComponent

        MapPolyline {
            line.color: _boundaryColor
            line.width: 3
            opacity:    _root.opacity
            path:       _root._boundaryPath
            visible:    _root._boundaryPath.length > 1
            z:          QGroundControl.zOrderMapItems - 1.5
        }
    }

    // ----- spray area (inside the edge margin) -------------------------------
    Component {
        id: sprayAreaComponent

        MapPolygon {
            color:        _sprayColor
            opacity:      0.12 * _root.opacity
            border.color: _sprayColor
            border.width: 1   // shows where the buffer ends
            path:         _missionItem.sprayAreaPath
            visible:      _missionItem.pathValid && !_root._shapingBoundary
            z:            QGroundControl.zOrderMapItems - 2
        }
    }

    // ----- one line per route section ---------------------------------------
    Component {
        id: segmentOutlineComponent

        MapPolyline {
            property var segment

            line.color: _outlineColor
            line.width: segment && (segment.spray || segment.outside) ? 6 : 4
            opacity:    0.55 * (_currentItem ? 1.0 : 0.7) * _root.opacity * _root._routeDim
            path:       segment ? [ segment.start, segment.end ] : []
            visible:    !_root._shapingBoundary
            z:          QGroundControl.zOrderMapItems - 1
        }
    }

    Component {
        id: segmentLineComponent

        MapPolyline {
            property var segment

            line.color: _segmentColor(segment)
            line.width: segment && (segment.spray || segment.outside) ? 3 : 1.5
            opacity:    (_currentItem ? 1.0 : 0.7) * _root.opacity * _root._routeDim
            path:       segment ? [ segment.start, segment.end ] : []
            visible:    !_root._shapingBoundary
            z:          QGroundControl.zOrderMapItems - 1
        }
    }

    // Section length label, drawn along the section and kept upright.
    Component {
        id: segmentLabelComponent

        MapQuickItem {
            property var segment

            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    segment ? segment.mid : QtPositioning.coordinate()
            visible:       segment && map.zoomLevel >= _labelMinZoom && segment.length >= _labelMinLenM && !_editRoute && _root._labelsFit
                           && !_root._shapingBoundary
            opacity:       _root.opacity
            z:             QGroundControl.zOrderMapItems

            sourceItem: Rectangle {
                width:    lengthLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height:   lengthLabel.contentHeight + 2
                radius:   height / 2
                color:    Qt.rgba(0, 0, 0, 0.65)
                rotation: {
                    if (!segment) {
                        return 0
                    }
                    // Screen angle of the line, then turned so text runs along it upright.
                    var r = (segment.azimuth - map.bearing - 90) % 360
                    if (r < 0) {
                        r += 360
                    }
                    if (r > 90 && r < 270) {
                        r -= 180
                    }
                    return r
                }

                QGCLabel {
                    id:               lengthLabel
                    anchors.centerIn: parent
                    text:             segment ? QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnitsString(segment.length, 0) : ""
                    color:            "white"   // fixed, so it reads on the dark pill in every theme
                    font.pointSize:   ScreenTools.smallFontPointSize
                }
            }
        }
    }

    Repeater {
        model: _missionItem.pathValid ? _missionItem.segments : []

        delegate: Item {
            property var _outline
            property var _line
            property var _label

            Component.onCompleted: {
                _outline = segmentOutlineComponent.createObject(map, { "segment": modelData })
                map.addMapItem(_outline)
                _line = segmentLineComponent.createObject(map, { "segment": modelData })
                map.addMapItem(_line)
                _label = segmentLabelComponent.createObject(map, { "segment": modelData })
                map.addMapItem(_label)
            }
            Component.onDestruction: {
                if (_label) {
                    _label.destroy()
                }
                if (_line) {
                    _line.destroy()
                }
                if (_outline) {
                    _outline.destroy()
                }
            }
        }
    }

    // ----- sides with their own buffer: drawn green ----------------------------
    Component {
        id: customSideLineComponent

        MapPolyline {
            property var side

            line.color: _sideColor
            line.width: 5
            opacity:    _root.opacity
            path:       side ? [ side.start, side.end ] : []
            z:          QGroundControl.zOrderMapItems
        }
    }

    Repeater {
        model: _currentItem ? _missionItem.sides : []

        delegate: Item {
            property var _line

            Component.onCompleted: {
                if (modelData.custom) {
                    _line = customSideLineComponent.createObject(map, { "side": modelData })
                    map.addMapItem(_line)
                }
            }
            Component.onDestruction: {
                if (_line) {
                    _line.destroy()
                }
            }
        }
    }

    // ----- side buffer, entry side and align modes: a numbered badge on every
    //       side (tap to select) -----------------------------------------------
    Component {
        id: sideBadgeComponent

        MapQuickItem {
            property var side

            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    side ? side.mid : QtPositioning.coordinate()
            z:             QGroundControl.zOrderMapItems + 3

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 1.8
                height:       width
                radius:       width / 2
                readonly property bool _highlight: side && (_root._alignPasses
                                                            ? _root._passesAlongSide(side, _missionItem.passAngle.rawValue)
                                                            : (_root._editGate ? _missionItem.gateSides.indexOf(side.side) >= 0 : side.custom))
                readonly property color _highlightColor: _root._alignPasses ? _sprayColor : (_root._editGate ? _transitColor : _sideColor)
                readonly property color _highlightText:  _root._alignPasses ? "black" : "white"

                color:        _highlight ? _highlightColor : "white"
                border.color: _highlight ? _highlightText : _outlineColor
                border.width: 2

                QGCLabel {
                    anchors.centerIn: parent
                    text:             side ? side.number : ""
                    color:            parent._highlight ? parent._highlightText : "black"
                    font.bold:        true
                }
                QGCMouseArea {
                    fillItem:  parent
                    onClicked: {
                        if (_root._alignPasses) {
                            _missionItem.alignPassesToSide(side.side)
                        } else if (_root._editGate) {
                            _missionItem.toggleGateSide(side.side)
                        } else {
                            _missionItem.toggleSide(side.side)
                        }
                    }
                }
            }
        }
    }

    Repeater {
        model: (_editSides || _editGate || _alignPasses) ? _missionItem.sides : []

        delegate: Item {
            property var _badge

            Component.onCompleted: {
                _badge = sideBadgeComponent.createObject(map, { "side": modelData })
                map.addMapItem(_badge)
            }
            Component.onDestruction: {
                if (_badge) {
                    _badge.destroy()
                }
            }
        }
    }

    // ----- transit: legs to and from the field (blue) ------------------------
    Component {
        id: transitLegComponent

        MapPolyline {
            property var leg

            line.color: _transitColor
            line.width: 3
            opacity:    (_currentItem ? 1.0 : 0.6) * _root.opacity
            path:       leg ? [ leg.start, leg.end ] : []
            visible:    !_root._shapingBoundary
            z:          QGroundControl.zOrderMapItems - 1
        }
    }

    Repeater {
        model: _missionItem.pathValid ? _missionItem.transitLegs : []

        delegate: Item {
            property var _line

            Component.onCompleted: {
                _line = transitLegComponent.createObject(map, { "leg": modelData })
                map.addMapItem(_line)
            }
            Component.onDestruction: {
                if (_line) {
                    _line.destroy()
                }
            }
        }
    }

    // Mode A: the entry sides, thick blue.
    Component {
        id: gateLineComponent

        MapPolyline {
            property var corners

            line.color: _transitColor
            line.width: 7
            opacity:    _root.opacity
            path:       corners ? corners : []
            visible:    !_root._shapingBoundary
            z:          QGroundControl.zOrderMapItems
        }
    }

    Repeater {
        model: (_missionItem.pathValid && !_routeTransit) ? _missionItem.gateLines : []

        delegate: Item {
            property var _line

            Component.onCompleted: {
                _line = gateLineComponent.createObject(map, { "corners": modelData })
                map.addMapItem(_line)
            }
            Component.onDestruction: {
                if (_line) {
                    _line.destroy()
                }
            }
        }
    }

    // Start (S, draggable) and end (E) of the spray route.
    Component {
        id: startMarkerComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    _missionItem.startPoint
            visible:       _missionItem.pathValid && !_root._editRoute && !_root._shapingBoundary
            opacity:       _root.opacity
            z:             QGroundControl.zOrderMapItems + 0.5   // under the item's number label

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 1.8
                height:       width
                radius:       width / 2
                color:        _startColor
                border.color: "white"
                border.width: 2

                QGCLabel {
                    anchors.centerIn: parent
                    text:             "S"
                    color:            "white"
                    font.bold:        true
                }
                // Tapping S selects the Spray Area (it has no number marker on the map).
                // While it's selected, the drag area on top moves S instead.
                QGCMouseArea {
                    fillItem:  parent
                    onClicked: _root.clicked(_missionItem.sequenceNumber)
                }
            }
        }
    }

    Component {
        id: startDragComponent

        MissionItemIndicatorDrag {
            property var marker
            property bool _ready: false

            mapControl:     map
            itemCoordinate: _missionItem.startPoint
            visible:        _currentItem && _root.interactive && _missionItem.pathValid
                            && !_root._editRoute && !_root._editSides && !_root._editTransit
                            && !_root._alignPasses && !_root._shapingBoundary
            z:              QGroundControl.zOrderMapItems + 1.5   // under the boundary's handles

            Component.onCompleted: _ready = true

            onDragStart: _root._draggingStart = true

            onItemCoordinateChanged: {
                if (_ready && marker) {
                    marker.coordinate = itemCoordinate
                }
            }
            onDragStop: {
                _root._draggingStart = false
                _missionItem.dropStartPoint(itemCoordinate)
                // Snap the marker onto the route (the start is always an end of it),
                // and replace this drag area: after a drag it no longer follows the marker.
                marker.coordinate = Qt.binding(function() { return _missionItem.startPoint })
                Qt.callLater(_root._createStartDrag)
            }
        }
    }

    Component {
        id: endMarkerComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    _missionItem.endPoint
            visible:       _missionItem.pathValid && !_root._editRoute && !_root._shapingBoundary
            opacity:       _root.opacity
            z:             QGroundControl.zOrderMapItems + 0.5

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 1.8
                height:       width
                radius:       width / 2
                color:        _endColor
                border.color: "white"
                border.width: 2

                QGCLabel {
                    anchors.centerIn: parent
                    text:             "E"
                    color:            "white"
                    font.bold:        true
                }
                QGCMouseArea {
                    fillItem:  parent
                    onClicked: _root.clicked(_missionItem.sequenceNumber)
                }
            }
        }
    }

    // Where the S can be dropped (shown while dragging it).
    Component {
        id: startOptionComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            z:             QGroundControl.zOrderMapItems + 0.4

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 0.9
                height:       width
                radius:       width / 2
                color:        _startColor
                opacity:      0.85
                border.color: "white"
                border.width: 2
            }
        }
    }

    Repeater {
        model: _draggingStart ? _missionItem.startOptions : []

        delegate: Item {
            property var _dot

            Component.onCompleted: {
                _dot = startOptionComponent.createObject(map, { "coordinate": modelData })
                map.addMapItem(_dot)
            }
            Component.onDestruction: {
                if (_dot) {
                    _dot.destroy()
                }
            }
        }
    }

    property var _startMarker
    property var _startDrag

    property bool _destroying: false

    function _createStartDrag() {
        if (_destroying) {
            return
        }
        _draggingStart = false
        if (_startDrag) {
            _startDrag.destroy()
        }
        _startDrag = startDragComponent.createObject(map, { "itemIndicator": _startMarker, "marker": _startMarker })
    }

    // ----- edit mode: a handle on every route point ---------------------------
    Component {
        id: pointHandleComponent

        MapQuickItem {
            property int pointIndex: -1

            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            visible:       pointIndex !== _root._dragPoint
            z:             QGroundControl.zOrderMapItems + 2

            sourceItem: Rectangle {
                readonly property bool _isStart: _root._editRoute &&
                                                 pointIndex === (_missionItem.routeReversed ? _missionItem.flightPath.length - 1 : 0)
                readonly property bool _isEntry: _root._editTransitRoute && pointIndex === _missionItem.transitEditPoints.length - 1

                width:        _handleSize * (_isEntry ? 1.0 : 0.75)
                height:       width
                radius:       width / 2
                color:        _root._editTransitRoute ? _transitColor : (_isStart ? _startColor : "#ffffff")
                border.color: _isEntry ? "white" : _outlineColor
                border.width: 2
                visible:      !(_root._editTransitRoute && pointIndex === 0)   // takeoff has its own marker
            }
        }
    }

    Repeater {
        model: _editActive ? _editPoints : []

        delegate: Item {
            property var _handle

            Component.onCompleted: {
                _handle = pointHandleComponent.createObject(map, { "coordinate": modelData, "pointIndex": index })
                map.addMapItem(_handle)
            }
            Component.onDestruction: {
                if (_handle) {
                    _handle.destroy()
                }
            }
        }
    }

    // ----- slider preview: the passes as they'll be when the slider is released
    Component {
        id: passPreviewComponent

        MapPolyline {
            line.color: _sprayColor
            line.width: 2
            path:       _root._previewing ? _missionItem.previewPath : []
            visible:    _root._previewing
            z:          QGroundControl.zOrderMapItems
        }
    }

    // ----- edit mode: drag preview -------------------------------------------
    Component {
        id: previewLineComponent

        MapPolyline {
            line.color: _sprayColor
            line.width: 2
            visible:    _root._previewActive
            z:          QGroundControl.zOrderMapItems + 3
            path: {
                var p = []
                if (!_root._previewActive) {
                    return p
                }
                if (_root._previewPrev) {
                    p.push(_root._previewPrev)
                }
                p.push(_root._previewCoord)
                if (_root._previewNext) {
                    p.push(_root._previewNext)
                }
                return p
            }
        }
    }

    Component {
        id: previewPointComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    _root._previewCoord
            visible:       _root._previewActive
            z:             QGroundControl.zOrderMapItems + 4

            sourceItem: Rectangle {
                width:        _handleSize * 1.2
                height:       width
                radius:       width / 2
                color:        _sprayColor
                border.color: _outlineColor
                border.width: 2
            }
        }
    }

    // ----- edit mode: touch handling ------------------------------------------
    // One area over the map decides what a press means, so a drag on empty map
    // still pans, and taps and holds work on the thin route lines.
    Component {
        id: editMouseAreaComponent

        MouseArea {
            id:              editArea
            anchors.fill:    map
            enabled:         _root._editActive
            visible:         _root._editActive
            z:               QGroundControl.zOrderMapItems + 5

            property string _mode:    ""      // "", "pan", "point" (moving one), "insert" (adding one)
            property var    _hit:     null
            property bool   _moved:   false   // finger moved past the drag distance since the press
            property real   _pressX:  0
            property real   _pressY:  0
            property real   _lastX:   0
            property real   _lastY:   0
            property real   _offsetX: 0       // grabbed point's screen position minus the finger's,
            property real   _offsetY: 0       // so the point doesn't jump to the finger
            readonly property real _dragDistance: Math.max(Qt.styleHints.startDragDistance, ScreenTools.defaultFontPixelWidth)

            // XAG-style: hold on a section, then drag, to add a point.
            Timer {
                id:       holdTimer
                interval: 500
                repeat:   false
                onTriggered: {
                    if (editArea.pressed && editArea._mode === "" && editArea._hit && editArea._hit.type === "segment") {
                        var pts = _root._editPoints
                        var i   = editArea._hit.index
                        var at  = map.fromCoordinate(editArea._hit.coordinate, false)
                        editArea._offsetX = at.x - editArea._lastX
                        editArea._offsetY = at.y - editArea._lastY
                        editArea._mode = "insert"
                        _root._startPreview(pts[i], editArea._hit.coordinate, pts[i + 1], -1)
                    }
                }
            }

            onPressed: (mouse) => {
                _mode   = ""
                _moved  = false
                _hit    = _root._hitTest(mouse.x, mouse.y)
                _pressX = mouse.x
                _pressY = mouse.y
                _lastX  = mouse.x
                _lastY  = mouse.y
                if (_hit && _hit.type === "segment") {
                    holdTimer.restart()
                }
            }

            onPositionChanged: (mouse) => {
                if (!_moved) {
                    if (Math.abs(mouse.x - _pressX) <= _dragDistance && Math.abs(mouse.y - _pressY) <= _dragDistance) {
                        return
                    }
                    _moved = true
                }
                if (_mode === "") {
                    holdTimer.stop()
                    if (_hit && _hit.type === "point") {
                        var pts = _root._editPoints
                        var i = _hit.index
                        var at = map.fromCoordinate(pts[i], false)
                        _offsetX = at.x - _pressX
                        _offsetY = at.y - _pressY
                        _mode = "point"
                        _root._startPreview(i > 0 ? pts[i - 1] : null, pts[i], i < pts.length - 1 ? pts[i + 1] : null, i)
                    } else {
                        _mode = "pan"
                    }
                }
                if (_mode === "pan") {
                    // pan() takes whole pixels; keep the remainder for the next move.
                    var dx = Math.round(_lastX - mouse.x)
                    var dy = Math.round(_lastY - mouse.y)
                    if (dx !== 0 || dy !== 0) {
                        map.pan(dx, dy)
                        _lastX -= dx
                        _lastY -= dy
                    }
                } else if (_mode === "point" || _mode === "insert") {
                    _root._previewCoord = map.toCoordinate(Qt.point(mouse.x + _offsetX, mouse.y + _offsetY), false)
                }
            }

            onReleased: (mouse) => {
                holdTimer.stop()
                if (_mode === "point") {
                    if (_root._editRoute) {
                        _missionItem.moveRoutePoint(_hit.index, _root._previewCoord)
                    } else {
                        _missionItem.moveTransitPoint(_hit.index, _root._previewCoord)
                    }
                } else if (_mode === "insert") {
                    if (_moved) {
                        if (_root._editRoute) {
                            _missionItem.insertRoutePoint(_hit.index, _root._previewCoord)
                        } else {
                            _missionItem.insertTransitPoint(_hit.index, _root._previewCoord)
                        }
                    } else {
                        // Held but didn't drag: treat it as a tap.
                        _mode = ""
                    }
                }
                _root._endPreview()
                // _mode is kept until the next press so onClicked can tell a tap from a drag.
            }

            onClicked: (mouse) => {
                if (_mode !== "" || _moved || !_hit) {
                    return
                }
                if (_hit.type === "point") {
                    if (_root._editRoute) {
                        pointMenu.popupPoint(_hit.index)
                    } else {
                        transitPointMenu.popupPoint(_hit.index)
                    }
                } else {
                    if (_root._editRoute) {
                        segmentMenu.popupSegment(_hit.index, _hit.coordinate)
                    } else {
                        transitSegmentMenu.popupSegment(_hit.index, _hit.coordinate)
                    }
                }
            }

            onCanceled: {
                holdTimer.stop()
                _mode = "cancelled"
                _root._endPreview()
            }
        }
    }

    QGCDynamicObjectManager { id: objMgr }

    Component.onCompleted: {
        objMgr.createObjects(
            [boundaryOutlineComponent, boundaryLineComponent, sprayAreaComponent, passPreviewComponent, endMarkerComponent,
             previewLineComponent, previewPointComponent],
            map,
            true /* parentObjectIsMap */)
        _startMarker = startMarkerComponent.createObject(map)
        map.addMapItem(_startMarker)
        _createStartDrag()
        objMgr.createObject(editMouseAreaComponent, map, false /* not a map item */)
    }

    Component.onDestruction: {
        _destroying = true
        if (_startDrag) {
            _startDrag.destroy()
        }
        if (_startMarker) {
            _startMarker.destroy()
        }
        objMgr.destroyObjects()
    }
}

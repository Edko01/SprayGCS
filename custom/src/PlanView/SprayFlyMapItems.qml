import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls

/// Fly view map items for the plan's Spray Area: the field boundary, and a
/// blue highlight the width of the swath behind the drone wherever it has
/// sprayed (kept for the whole job, through every trip).
Item {
    id: _root

    property var map    ///< the Fly view's map; null while it isn't a map items can be added to

    readonly property var   _area:           QGroundControl.corePlugin.sprayPlanArea !== undefined ? QGroundControl.corePlugin.sprayPlanArea : null
    readonly property color _sprayedColor:   "#1e6fff"
    readonly property color _boundaryColor:  "#ffffff"
    readonly property color _outlineColor:   "#000000"
    property real           _swathPixels:    2
    property var            _trailLines:     []
    property var            _boundaryItems:  []

    readonly property var _boundaryPath: {
        if (!_area) {
            return []
        }
        var path   = _area.fieldPolygon.path
        var closed = []
        for (var i = 0; i < path.length; i++) {
            closed.push(path[i])
        }
        if (closed.length > 2) {
            closed.push(path[0])
        }
        return closed
    }

    // Map line widths are in pixels: keep the trail one swath wide at every zoom.
    function _updateSwathPixels() {
        if (!map || !_area || !map.center.isValid) {
            return
        }
        var from = map.fromCoordinate(map.center, false /* clipToViewport */)
        var to   = map.fromCoordinate(map.center.atDistanceAndAzimuth(_area.swathWidth.rawValue, 90), false /* clipToViewport */)
        var px   = Math.sqrt(Math.pow(to.x - from.x, 2) + Math.pow(to.y - from.y, 2))
        if (!isNaN(px)) {
            _swathPixels = Math.max(2, px)
        }
    }

    function _addTrailLine(path) {
        var line = trailComponent.createObject(map, { "path": path })
        map.addMapItem(line)
        _trailLines.push(line)
        return line
    }

    function _clearTrail() {
        for (var i = 0; i < _trailLines.length; i++) {
            _trailLines[i].destroy()
        }
        _trailLines = []
    }

    function _rebuildTrail() {
        _clearTrail()
        if (!map || !_area) {
            return
        }
        var segments = _area.coverageSegments()
        for (var i = 0; i < segments.length; i++) {
            _addTrailLine(segments[i])
        }
        _updateSwathPixels()
    }

    function _lastTrailLine() {
        return _trailLines.length > 0 ? _trailLines[_trailLines.length - 1] : _addTrailLine([])
    }

    function _setup() {
        for (var i = 0; i < _boundaryItems.length; i++) {
            _boundaryItems[i].destroy()
        }
        _boundaryItems = []
        if (map) {
            var items = [ boundaryOutlineComponent.createObject(map), boundaryLineComponent.createObject(map) ]
            for (var j = 0; j < items.length; j++) {
                map.addMapItem(items[j])
            }
            _boundaryItems = items
        }
        _rebuildTrail()
    }

    onMapChanged:   _setup()
    on_AreaChanged: _rebuildTrail()

    Component.onDestruction: {
        _clearTrail()
        for (var i = 0; i < _boundaryItems.length; i++) {
            _boundaryItems[i].destroy()
        }
    }

    Connections {
        target:                 _root.map
        ignoreUnknownSignals:   true
        function onZoomLevelChanged()   { _root._updateSwathPixels() }
        function onCenterChanged()      { _root._updateSwathPixels() }
        function onWidthChanged()       { _root._updateSwathPixels() }
    }

    Connections {
        target: _root._area
        function onCoverageReset()                    { _root._rebuildTrail() }
        function onCoverageSegmentStarted()           { if (_root.map) { _root._addTrailLine([]) } }
        function onCoveragePointAdded(coordinate)     { if (_root.map) { _root._lastTrailLine().addCoordinate(coordinate) } }
        function onCoverageLastPointMoved(coordinate) {
            if (_root.map) {
                var line = _root._lastTrailLine()
                if (line.pathLength() > 0) {
                    line.replaceCoordinate(line.pathLength() - 1, coordinate)
                } else {
                    line.addCoordinate(coordinate)
                }
            }
        }
    }

    Connections {
        target:                 _root._area ? _root._area.swathWidth : null
        function onRawValueChanged() { _root._updateSwathPixels() }
    }

    Component {
        id: trailComponent

        MapPolyline {
            line.width: _root._swathPixels
            line.color: _root._sprayedColor
            opacity:    0.45
            z:          QGroundControl.zOrderMapItems - 2
        }
    }

    Component {
        id: boundaryOutlineComponent

        MapPolyline {
            line.color: _root._outlineColor
            line.width: 6
            opacity:    0.6
            path:       _root._boundaryPath
            visible:    _root._boundaryPath.length > 1
            z:          QGroundControl.zOrderMapItems - 1.6
        }
    }

    Component {
        id: boundaryLineComponent

        MapPolyline {
            line.color: _root._boundaryColor
            line.width: 3
            path:       _root._boundaryPath
            visible:    _root._boundaryPath.length > 1
            z:          QGroundControl.zOrderMapItems - 1.5
        }
    }
}

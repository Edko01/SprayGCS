import QtQuick
import QtLocation
import QtPositioning

import QGroundControl

/// The field images (GeoTIFF orthomosaics, see SprayMapLayers) on a map, under
/// the field, route and drone. Used by the Plan and Fly maps.
///
/// Each image has an overview, drawn when zoomed out, and map tiles down to the
/// photo's own detail: zoomed in past the overview, the tiles in and around
/// the view at the map's zoom level are drawn over it (made as the map moves,
/// dropped when they're well out of view). When the zoom level changes, the
/// last level's tiles stay until the new ones have loaded.
Item {
    id: _root

    property var map    ///< the map to draw on; null while it isn't a map items can be added to

    readonly property var  _manager: QGroundControl.corePlugin.sprayMapLayers !== undefined ? QGroundControl.corePlugin.sprayMapLayers : null
    readonly property var  _layers:  _manager ? _manager.layers : []
    readonly property real _zoom:    map ? map.zoomLevel : 0

    /// The map moved or zoomed (at most every viewTimer.interval while it keeps moving).
    signal viewChanged()

    Timer {
        id:       viewTimer
        interval: 100
        onTriggered: _root.viewChanged()
    }

    Connections {
        target:               _root.map
        ignoreUnknownSignals: true
        function onZoomLevelChanged() { if (!viewTimer.running) { viewTimer.start() } }
        function onCenterChanged()    { if (!viewTimer.running) { viewTimer.start() } }
        function onBearingChanged()   { if (!viewTimer.running) { viewTimer.start() } }
        function onWidthChanged()     { if (!viewTimer.running) { viewTimer.start() } }
        function onHeightChanged()    { if (!viewTimer.running) { viewTimer.start() } }
    }

    // The model is the number of layers, so showing or hiding one
    // updates a layer's images instead of making them again.
    Repeater {
        model: _root.map ? _root._layers.length : 0

        delegate: Item {
            id: layerDelegate

            required property int index
            // Not "layer": every Item has a built-in, final property of that name.
            readonly property var  layerData: index < _root._layers.length ? _root._layers[index] : null
            // Zoomed in past the overview's own detail, and there are tiles to show.
            readonly property bool tileMode:  !!layerData && layerData.visible && layerData.tileMaxLevel >= 0
                                              && _root._zoom > layerData.zoomLevel
            property var  _overview: null
            property var  _tiles:    ({})   // "level/x/y" -> tile map item, at _level
            property var  _oldTiles: ({})   // the previous level's, until _tiles have loaded
            property int  _level:    -1
            property int  pending:   0      // of _tiles, still loading

            function _destroyAll(tiles) {
                for (var key in tiles) {
                    tiles[key].destroy()
                }
            }

            function clearTiles() {
                _destroyAll(_tiles)
                _destroyAll(_oldTiles)
                _tiles = {}
                _oldTiles = {}
                _level = -1
                pending = 0
            }

            onPendingChanged: {
                if (pending === 0) {
                    _destroyAll(_oldTiles)
                    _oldTiles = {}
                }
            }

            function updateTiles() {
                var m = _root.map
                if (!tileMode || !m || m.width <= 0 || m.height <= 0) {
                    clearTiles()
                    return
                }
                var level = Math.max(layerData.tileMinLevel, Math.min(layerData.tileMaxLevel, Math.ceil(m.zoomLevel - 0.25)))
                if (level !== _level) {
                    // The current tiles stay under the new level's until those have loaded.
                    _destroyAll(_oldTiles)
                    for (var oldKey in _tiles) {
                        _tiles[oldKey].retire()
                    }
                    _oldTiles = _tiles
                    _tiles = {}
                    _level = level
                    pending = 0
                }
                // The view and half a screen around it, so panning finds tiles ready.
                var mx = m.width / 2, my = m.height / 2
                var corners = [ m.toCoordinate(Qt.point(-mx, -my), false), m.toCoordinate(Qt.point(m.width + mx, -my), false),
                                m.toCoordinate(Qt.point(-mx, m.height + my), false), m.toCoordinate(Qt.point(m.width + mx, m.height + my), false) ]
                var north = -90, south = 90, west = 180, east = -180
                for (var i = 0; i < corners.length; i++) {
                    if (!corners[i].isValid) {
                        return
                    }
                    north = Math.max(north, corners[i].latitude)
                    south = Math.min(south, corners[i].latitude)
                    west  = Math.min(west,  corners[i].longitude)
                    east  = Math.max(east,  corners[i].longitude)
                }
                var wanted = _root._manager.tilesInView(index, level, north, south, west, east, 600)
                var keep = {}
                for (var j = 0; j < wanted.length; j++) {
                    var tile = wanted[j]
                    keep[tile.key] = true
                    if (!_tiles[tile.key]) {
                        var item = tileComponent.createObject(m, {
                            "layerDelegate": layerDelegate,
                            "source":        tile.url,
                            "zoomLevel":     level,
                            "coordinate":    QtPositioning.coordinate(tile.north, tile.west)
                        })
                        m.addMapItem(item)
                        _tiles[tile.key] = item
                        pending++
                    }
                }
                for (var key in _tiles) {
                    if (!keep[key]) {
                        _tiles[key].destroy()
                        delete _tiles[key]
                    }
                }
                if (pending === 0) {
                    _destroyAll(_oldTiles)
                    _oldTiles = {}
                }
            }

            onTileModeChanged:  updateTiles()
            onLayerDataChanged: updateTiles()

            Connections {
                target: _root
                function onViewChanged() { layerDelegate.updateTiles() }
            }

            Component.onCompleted: {
                _overview = overviewComponent.createObject(_root.map, { "layerDelegate": layerDelegate })
                _root.map.addMapItem(_overview)
                Qt.callLater(updateTiles)
            }
            Component.onDestruction: {
                clearTiles()
                if (_overview) {
                    _overview.destroy()
                }
            }
        }
    }

    // With zoomLevel set, a map item's image is drawn as part of the map: it
    // scales, turns and tilts with it, at natural size at that zoom level.
    Component {
        id: overviewComponent

        MapQuickItem {
            id: overviewItem

            property var layerDelegate
            readonly property var _layer: layerDelegate ? layerDelegate.layerData : null

            coordinate:    _layer ? QtPositioning.coordinate(_layer.north, _layer.west) : QtPositioning.coordinate()
            anchorPoint.x: 0
            anchorPoint.y: 0
            zoomLevel:     _layer ? _layer.zoomLevel : 0
            z:             1   // under everything QGC draws (its map items start at 47)
            // Also under the tiles, where it fills gaps while they load.
            visible:       !!_layer && _layer.visible

            sourceItem: Image {
                width:        overviewItem._layer ? overviewItem._layer.width : 0
                height:       overviewItem._layer ? overviewItem._layer.height : 0
                source:       overviewItem._layer ? overviewItem._layer.url : ""
                asynchronous: true
                cache:        false
                smooth:       true
                mipmap:       true

                onStatusChanged: {
                    if (status === Image.Error) {
                        console.warn("SprayGCS field image didn't load:", source)
                    }
                }
            }
        }
    }

    Component {
        id: tileComponent

        MapQuickItem {
            id: tileItem

            property var  layerDelegate
            property url  source
            property bool _settled: false
            property bool _retired: false   // a previous zoom level's tile, kept until the new ones load

            function retire() {
                settle()
                _retired = true
            }

            function settle() {
                if (!_settled) {
                    _settled = true
                    if (layerDelegate) {
                        layerDelegate.pending = Math.max(0, layerDelegate.pending - 1)
                    }
                }
            }

            anchorPoint.x: 0
            anchorPoint.y: 0
            visible:       !!layerDelegate && layerDelegate.tileMode
            z:             _retired ? 2 : 3

            sourceItem: Image {
                // One pixel of overlap hides hairline seams between tiles at in-between zooms.
                width:        257
                height:       257
                source:       tileItem.source
                asynchronous: true
                cache:        true    // panning or zooming back reuses tiles already loaded
                smooth:       true

                onStatusChanged: {
                    if (status === Image.Ready || status === Image.Error) {
                        tileItem.settle()
                    }
                }
            }

            Component.onDestruction: settle()
        }
    }
}

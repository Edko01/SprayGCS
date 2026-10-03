import QtQuick
import QtLocation
import QtPositioning

import QGroundControl

/// The field images (GeoTIFF orthomosaics, see SprayMapLayers) on a map, under
/// the field, route and drone. Used by the Plan and Fly maps.
///
/// Each image has an overview, drawn when zoomed out, and map tiles down to the
/// photo's own detail: zoomed in past the overview, the tiles in view at the
/// map's zoom level are drawn over it (made as the map moves, dropped when
/// they leave the view).
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

    // The model is the number of layers, so a change of opacity or visibility
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
            property var  _tiles:    ({})   // "level/x/y" -> tile map item
            property int  pending:   0      // tiles still loading

            function clearTiles() {
                for (var key in _tiles) {
                    _tiles[key].destroy()
                }
                _tiles = {}
                pending = 0
            }

            function updateTiles() {
                var m = _root.map
                if (!tileMode || !m || m.width <= 0 || m.height <= 0) {
                    clearTiles()
                    return
                }
                var level = Math.max(layerData.tileMinLevel, Math.min(layerData.tileMaxLevel, Math.ceil(m.zoomLevel - 0.25)))
                var corners = [ m.toCoordinate(Qt.point(0, 0), false), m.toCoordinate(Qt.point(m.width, 0), false),
                                m.toCoordinate(Qt.point(0, m.height), false), m.toCoordinate(Qt.point(m.width, m.height), false) ]
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
                var wanted = _root._manager.tilesInView(index, level, north, south, west, east, 400)
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
            opacity:       _layer ? _layer.opacity : 1
            z:             1   // under everything QGC draws (its map items start at 47)
            // Under the tiles it fills gaps while they load; when faded it would
            // double up with them, so then it steps aside once they're in.
            visible:       !!_layer && _layer.visible
                           && (!layerDelegate.tileMode || _layer.opacity > 0.99 || layerDelegate.pending > 0)

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
            opacity:       layerDelegate && layerDelegate.layerData ? layerDelegate.layerData.opacity : 1
            visible:       !!layerDelegate && layerDelegate.tileMode
            z:             2

            sourceItem: Image {
                // One pixel of overlap hides hairline seams between tiles at in-between zooms.
                width:        257
                height:       257
                source:       tileItem.source
                asynchronous: true
                cache:        false
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

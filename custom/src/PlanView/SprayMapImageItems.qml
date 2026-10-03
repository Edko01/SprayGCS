import QtQuick
import QtLocation
import QtPositioning

import QGroundControl

/// The field images (GeoTIFF orthomosaics, see SprayMapLayers) on a map, under
/// the field, route and drone. Used by the Plan and Fly maps.
Item {
    id: _root

    property var map    ///< the map to draw on; null while it isn't a map items can be added to

    readonly property var _manager: QGroundControl.corePlugin.sprayMapLayers !== undefined ? QGroundControl.corePlugin.sprayMapLayers : null
    readonly property var _layers:  _manager ? _manager.layers : []

    // The model is the number of layers, so a change of opacity or visibility
    // updates a layer's image instead of making it again.
    Repeater {
        model: _root.map ? _root._layers.length : 0

        delegate: Item {
            id: layerDelegate

            required property int index
            // Not "layer": every Item has a built-in, final property of that name.
            readonly property var layerData: index < _root._layers.length ? _root._layers[index] : null
            property var _mapItem: null

            Component.onCompleted: {
                _mapItem = imageComponent.createObject(_root.map, { "layerDelegate": layerDelegate })
                _root.map.addMapItem(_mapItem)
            }
            Component.onDestruction: {
                if (_mapItem) {
                    _mapItem.destroy()
                }
            }
        }
    }

    Component {
        id: imageComponent

        // With zoomLevel set, the image is drawn as part of the map: it scales,
        // turns and tilts with it, at natural size at that zoom level.
        MapQuickItem {
            id: imageItem

            property var layerDelegate
            readonly property var _layer: layerDelegate ? layerDelegate.layerData : null

            coordinate:    _layer ? QtPositioning.coordinate(_layer.north, _layer.west) : QtPositioning.coordinate()
            anchorPoint.x: 0
            anchorPoint.y: 0
            zoomLevel:     _layer ? _layer.zoomLevel : 0
            visible:       !!_layer && _layer.visible
            opacity:       _layer ? _layer.opacity : 1
            z:             1   // under everything QGC draws (its map items start at 47)

            sourceItem: Image {
                width:        imageItem._layer ? imageItem._layer.width : 0
                height:       imageItem._layer ? imageItem._layer.height : 0
                source:       imageItem._layer ? imageItem._layer.url : ""
                asynchronous: true
                cache:        false
                smooth:       true
                mipmap:       true

                onStatusChanged: {
                    if (status === Image.Error) {
                        console.warn("SprayGCS field image didn't load:", source)
                    } else if (status === Image.Ready) {
                        console.log("SprayGCS field image loaded:", source, sourceSize.width + "x" + sourceSize.height,
                                    "at zoom", imageItem.zoomLevel)
                    }
                }
            }
        }
    }
}

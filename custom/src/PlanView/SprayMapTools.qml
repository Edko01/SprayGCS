import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls

/// XAG-style map tools for the Plan map, as a bar in the top-left corner:
///   X / >   hide or show the tools
///   Layers  map provider and type (satellite, hybrid, street...), and field
///           images (GeoTIFF orthomosaics) shown under the plan
///   Arrow   centre the map on the drone
///   Target  centre the map on this tablet (the GCS); its position is drawn too
///   Ruler   tap points on the map to measure distance, and area from 3 points
///           (drag still moves the map)
Item {
    id: _root

    property var map   // the Plan view's FlightMap

    property bool expanded:    true
    property bool measuring:   false
    property bool layersOpen:  false
    property var  _points:     []

    readonly property var   _vehicle:      QGroundControl.multiVehicleManager.activeVehicle
    readonly property bool  _vehicleKnown: _vehicle ? _vehicle.coordinate.isValid : false
    readonly property var   _gcs:          map && map.gcsPosition ? map.gcsPosition : QtPositioning.coordinate()
    readonly property bool  _gcsKnown:     _gcs.isValid
    readonly property real  _button:       ScreenTools.defaultFontPixelHeight * 2.4
    readonly property real  _radius:       ScreenTools.defaultFontPixelWidth * 0.75
    readonly property real  _margin:       ScreenTools.defaultFontPixelWidth
    readonly property color _accent:       "#ffd400"
    readonly property color _measureColor: "#ff9f0a"
    readonly property var   _mapSettings:  QGroundControl.settingsManager.flightMapSettings
    readonly property var   _mapLayers:    QGroundControl.corePlugin.sprayMapLayers !== undefined ? QGroundControl.corePlugin.sprayMapLayers : null

    width:  bar.width
    height: bar.height

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // ---- actions ----------------------------------------------------------------
    function _centerOn(coordinate) {
        if (!map || !coordinate || !coordinate.isValid) {
            return
        }
        map.center = coordinate
        if (map.zoomLevel < 16) {
            map.zoomLevel = 17
        }
    }

    function _setMeasuring(on) {
        measuring = on
        layersOpen = false
        _points = []
    }

    function _addPoint(coordinate) {
        var pts = _points.slice()
        pts.push(coordinate)
        _points = pts
    }

    function _undoPoint() {
        var pts = _points.slice()
        pts.pop()
        _points = pts
    }

    function _distanceM() {
        var total = 0
        for (var i = 1; i < _points.length; i++) {
            total += _points[i - 1].distanceTo(_points[i])
        }
        return total
    }

    // Area of the shape the points outline (closed back to the first point),
    // on a local flat projection; fine for field-sized shapes.
    function _areaM2() {
        if (_points.length < 3) {
            return 0
        }
        var lat0 = 0
        for (var i = 0; i < _points.length; i++) {
            lat0 += _points[i].latitude
        }
        lat0 /= _points.length
        var r = 6371000
        var k = Math.PI / 180
        var sum = 0
        for (var j = 0; j < _points.length; j++) {
            var a = _points[j]
            var b = _points[(j + 1) % _points.length]
            var ax = a.longitude * k * r * Math.cos(lat0 * k), ay = a.latitude * k * r
            var bx = b.longitude * k * r * Math.cos(lat0 * k), by = b.latitude * k * r
            sum += ax * by - bx * ay
        }
        return Math.abs(sum) / 2
    }

    // Fits the map to a field image (a layer from SprayMapLayers).
    function _showLayer(layer) {
        if (!map || !layer) {
            return
        }
        var region = QtPositioning.rectangle(QtPositioning.coordinate(layer.north, layer.west),
                                             QtPositioning.coordinate(layer.south, layer.east))
        if (typeof map.setVisibleRegion === "function") {
            map.setVisibleRegion(region)
        } else {
            map.visibleRegion = region
        }
    }

    function _distanceText(meters) {
        return QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnitsString(meters, 0)
    }

    // A tool button. (Inline components don't see this file's ids, so it only
    // uses its own properties and singletons.)
    component ToolButton: Rectangle {
        id: toolButton

        property string icon
        property real   iconRotation: 0
        property bool   active:       false
        property bool   available:    true
        property string label
        signal activated()

        width:   ScreenTools.defaultFontPixelHeight * 2.4
        height:  width
        color:   active ? "#ffd400" : "transparent"
        opacity: available ? 1 : 0.35
        Accessible.name: label

        QGCColoredImage {
            anchors.centerIn:  parent
            width:             parent.width * 0.5
            height:            width
            sourceSize.height: height
            source:            toolButton.icon
            rotation:          toolButton.iconRotation
            color:             "#111111"
        }
        MouseArea {
            anchors.fill: parent
            enabled:      toolButton.available
            onClicked:    toolButton.activated()
        }
    }

    // ---- the bar ----------------------------------------------------------------
    Row {
        id: bar

        // Hide / show
        Rectangle {
            width:  _button
            height: _button
            radius: _radius
            color:  "#111111"
            Accessible.name: _root.expanded ? qsTr("Hide map tools") : qsTr("Show map tools")

            QGCColoredImage {
                anchors.centerIn:  parent
                width:             parent.width * 0.42
                height:            width
                sourceSize.height: height
                source:            _root.expanded ? "/InstrumentValueIcons/close.svg" : "/InstrumentValueIcons/cheveron-right.svg"
                color:             "white"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    _root.expanded = !_root.expanded
                    if (!_root.expanded) {
                        _root._setMeasuring(false)
                        _root.layersOpen = false
                    }
                }
            }
        }

        Rectangle {
            visible: _root.expanded
            width:   tools.width
            height:  _button
            radius:  _radius
            color:   "white"

            Row {
                id: tools

                ToolButton {
                    icon:        "/InstrumentValueIcons/layers.svg"
                    label:       qsTr("Map and field images")
                    active:      _root.layersOpen
                    onActivated: {
                        _root.layersOpen = !_root.layersOpen
                        if (_root.layersOpen) {
                            _root._setMeasuring(false)
                            _root.layersOpen = true
                        }
                    }
                }
                Rectangle { width: 1; height: _button; color: "#dddddd" }
                ToolButton {
                    icon:         "/InstrumentValueIcons/location-current.svg"
                    iconRotation: 45    // point up, like XAG's
                    label:        qsTr("Find the drone")
                    available:    _root._vehicleKnown
                    onActivated:  _root._centerOn(_root._vehicle.coordinate)
                }
                Rectangle { width: 1; height: _button; color: "#dddddd" }
                ToolButton {
                    icon:        "/InstrumentValueIcons/target.svg"
                    label:       qsTr("Find this tablet")
                    available:   _root._gcsKnown
                    onActivated: _root._centerOn(_root._gcs)
                }
                Rectangle { width: 1; height: _button; color: "#dddddd" }
                ToolButton {
                    icon:        "/custom/img/ruler.svg"
                    label:       qsTr("Measure")
                    active:      _root.measuring
                    onActivated: _root._setMeasuring(!_root.measuring)
                }
            }
        }
    }

    // ---- map type panel ---------------------------------------------------------
    Rectangle {
        id:          layersPanel
        visible:     _root.layersOpen && _root.expanded
        anchors.top: bar.bottom
        anchors.topMargin: _margin / 2
        x:           _button
        width:       ScreenTools.defaultFontPixelWidth * 30
        height:      layersColumn.height + _margin * 2
        radius:      _radius
        color:       qgcPal.window

        MouseArea { anchors.fill: parent }   // keep taps off the map

        ColumnLayout {
            id:             layersColumn
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.top:    parent.top
            anchors.margins: _margin
            spacing:        _margin / 2

            QGCLabel {
                text:      qsTr("Map")
                font.bold: true
            }
            QGCComboBox {
                id:               providerCombo
                Layout.fillWidth: true
                model:            QGroundControl.mapEngineManager.mapProviderList
                currentIndex:     model.indexOf(_root._mapSettings.mapProvider.rawValue)
                onActivated: (index) => {
                    var provider = textAt(index)
                    _root._mapSettings.mapProvider.rawValue = provider
                    _root._mapSettings.mapType.rawValue = QGroundControl.mapEngineManager.mapTypeList(provider)[0]
                }
            }
            Repeater {
                model: QGroundControl.mapEngineManager.mapTypeList(_root._mapSettings.mapProvider.rawValue)

                delegate: Rectangle {
                    required property var modelData
                    readonly property bool _selected: modelData === _root._mapSettings.mapType.rawValue

                    Layout.fillWidth: true
                    implicitHeight:   ScreenTools.defaultFontPixelHeight * 2
                    radius:           _root._radius
                    color:            _selected ? _root._accent : qgcPal.windowShade

                    QGCLabel {
                        anchors.centerIn: parent
                        text:             modelData
                        font.bold:        parent._selected
                        color:            parent._selected ? "#111111" : qgcPal.text
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked:    _root._mapSettings.mapType.rawValue = modelData
                    }
                }
            }

            // ---- field images ----
            Rectangle {
                Layout.fillWidth:       true
                Layout.topMargin:       _margin / 2
                implicitHeight:         1
                color:                  qgcPal.text
                opacity:                0.3
            }
            QGCLabel {
                text:      qsTr("Field images")
                font.bold: true
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                font.pointSize:   ScreenTools.smallFontPointSize
                text:             _root._mapLayers && _root._mapLayers.layers.length > 0
                                  ? qsTr("Tap an image's name to go to it.")
                                  : qsTr("Orthomosaics (GeoTIFF) from your mapping software, shown under the plan here and in Fly.")
            }
            // The model is the count, so a change to one layer doesn't rebuild the rows.
            Repeater {
                model: _root._mapLayers ? _root._mapLayers.layers.length : 0

                delegate: ColumnLayout {
                    id: layerRow

                    required property int index
                    readonly property var _layer: index < _root._mapLayers.layers.length ? _root._mapLayers.layers[index] : null

                    Layout.fillWidth: true
                    spacing:          0

                    RowLayout {
                        Layout.fillWidth: true
                        spacing:          _margin / 2

                        QGCCheckBox {
                            checked:   !!layerRow._layer && layerRow._layer.visible
                            onClicked: _root._mapLayers.setLayerVisible(layerRow.index, checked)
                        }
                        QGCLabel {
                            Layout.fillWidth: true
                            text:             layerRow._layer ? layerRow._layer.name : ""
                            elide:            Text.ElideMiddle
                            font.underline:   true

                            // Tap the name to go to the image.
                            MouseArea {
                                anchors.fill: parent
                                onClicked:    _root._showLayer(layerRow._layer)
                            }
                        }
                        QGCButton {
                            text:      qsTr("Remove")
                            onClicked: _root._mapLayers.removeLayer(layerRow.index)
                        }
                    }
                    // A tile link loads from the internet until it's saved here.
                    QGCButton {
                        Layout.fillWidth: true
                        text:             qsTr("Save Offline")
                        visible:          !!layerRow._layer && layerRow._layer.remote && !layerRow._layer.offline
                        enabled:          !_root._mapLayers.loading
                        onClicked:        _root._mapLayers.saveOffline(layerRow.index)
                    }
                }
            }
            QGCLabel {
                Layout.fillWidth: true
                text:             qsTr("%1... %2%").arg(_root._mapLayers ? _root._mapLayers.busyText : "").arg(_root._mapLayers ? _root._mapLayers.progress : 0)
                visible:          !!_root._mapLayers && _root._mapLayers.loading
                font.bold:        true
            }
            QGCButton {
                Layout.fillWidth: true
                text:             qsTr("Add Field Image (GeoTIFF)")
                enabled:          !!_root._mapLayers && !_root._mapLayers.loading
                onClicked:        geoTiffDialog.openForLoad()
            }
            // Map tiles hosted on GitHub.
            QGCTextField {
                id:               tileLinkField
                Layout.fillWidth: true
                placeholderText:  qsTr("GitHub tile link with {z}/{x}/{y}")
            }
            QGCButton {
                Layout.fillWidth: true
                text:             qsTr("Add Tile Link")
                enabled:          !!_root._mapLayers && !_root._mapLayers.loading && tileLinkField.text.trim() !== ""
                onClicked:        _root._mapLayers.addTileUrl(tileLinkField.text)
            }
        }
    }

    QGCFileDialog {
        id:          geoTiffDialog
        title:       qsTr("Add a field image")
        nameFilters: [ qsTr("GeoTIFF (*.tif *.tiff *.TIF *.TIFF)") ]
        // The in-app picker only lists one folder (with Import on Android); on a
        // desktop the system picker reaches the mapping software's exports anywhere.
        folder:      ScreenTools.isMobile ? QGroundControl.settingsManager.appSettings.missionSavePath : ""
        onAcceptedForLoad: (file) => {
            _root._mapLayers.addGeoTiff(file)
            close()
        }
    }

    // A newly added image is shown at once: it may be far from where the map is.
    Connections {
        target: _root._mapLayers
        function onLayerAdded(index) {
            tileLinkField.text = ""
            _root._showLayer(_root._mapLayers.layers[index])
        }
    }

    // Field images on this map, under the plan.
    Loader {
        source:   "qrc:/qml/Custom/Plan/SprayMapImageItems.qml"
        onLoaded: item.map = Qt.binding(function() { return _root.map })
    }

    // ---- measuring: results panel -----------------------------------------------
    Rectangle {
        visible:     _root.measuring && _root.expanded
        anchors.top: bar.bottom
        anchors.topMargin: _margin / 2
        x:           _button
        width:       ScreenTools.defaultFontPixelWidth * 30
        height:      measureColumn.height + _margin * 2
        radius:      _radius
        color:       qgcPal.window

        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id:             measureColumn
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.top:    parent.top
            anchors.margins: _margin
            spacing:        _margin / 2

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             _root._points.length === 0
                                  ? qsTr("Tap the map to add points. Drag to move the map.")
                                  : qsTr("Distance: %1").arg(_root._distanceText(_root._distanceM()))
                font.bold:        _root._points.length > 0
            }
            QGCLabel {
                visible: _root._points.length >= 3
                text:    qsTr("Area: %1 ac  (closed back to the first point)").arg((_root._areaM2() / 4046.8564224).toFixed(2))
            }
            RowLayout {
                Layout.fillWidth: true
                spacing:          _margin / 2
                visible:          _root._points.length > 0

                QGCButton {
                    Layout.fillWidth: true
                    text:             qsTr("Undo Point")
                    onClicked:        _root._undoPoint()
                }
                QGCButton {
                    Layout.fillWidth: true
                    text:             qsTr("Clear")
                    onClicked:        _root._points = []
                }
            }
        }
    }

    // ---- measuring: map overlay (tap adds a point, drag pans) --------------------
    MouseArea {
        id:           measureArea
        parent:       _root.map
        anchors.fill: parent
        enabled:      _root.measuring
        visible:      _root.measuring
        z:            QGroundControl.zOrderMapItems + 10

        property bool _moved:  false
        property real _pressX: 0
        property real _pressY: 0
        property real _lastX:  0
        property real _lastY:  0
        readonly property real _dragDistance: Math.max(Qt.styleHints.startDragDistance, ScreenTools.defaultFontPixelWidth)

        onPressed: (mouse) => {
            _moved  = false
            _pressX = mouse.x
            _pressY = mouse.y
            _lastX  = mouse.x
            _lastY  = mouse.y
        }
        onPositionChanged: (mouse) => {
            if (!_moved && Math.abs(mouse.x - _pressX) <= _dragDistance && Math.abs(mouse.y - _pressY) <= _dragDistance) {
                return
            }
            _moved = true
            // pan() takes whole pixels; keep the remainder for the next move.
            var dx = Math.round(_lastX - mouse.x)
            var dy = Math.round(_lastY - mouse.y)
            if (dx !== 0 || dy !== 0) {
                _root.map.pan(dx, dy)
                _lastX -= dx
                _lastY -= dy
            }
        }
        onClicked: (mouse) => {
            if (!_moved) {
                _root._addPoint(_root.map.toCoordinate(Qt.point(mouse.x, mouse.y), false))
            }
        }
    }

    // ---- map items: measurement and the tablet's position -------------------------
    Component {
        id: measureAreaComponent

        MapPolygon {
            color:        _root._measureColor
            opacity:      0.18
            border.width: 0
            path:         _root._points
            visible:      _root.measuring && _root._points.length >= 3
            z:            QGroundControl.zOrderMapItems + 6
        }
    }

    Component {
        id: measureLineComponent

        MapPolyline {
            line.color: _root._measureColor
            line.width: 3
            path:       _root._points
            visible:    _root.measuring && _root._points.length >= 2
            z:          QGroundControl.zOrderMapItems + 7
        }
    }

    Component {
        id: measurePointComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width / 2
            anchorPoint.y: sourceItem.height / 2
            z:             QGroundControl.zOrderMapItems + 8

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 0.9
                height:       width
                radius:       width / 2
                color:        "white"
                border.color: _root._measureColor
                border.width: 3
            }
        }
    }

    Component {
        id: measureLabelComponent

        MapQuickItem {
            property real meters: 0

            anchorPoint.x: sourceItem.width / 2
            anchorPoint.y: sourceItem.height / 2
            z:             QGroundControl.zOrderMapItems + 9

            sourceItem: Rectangle {
                width:  segLabel.contentWidth + ScreenTools.defaultFontPixelWidth
                height: segLabel.contentHeight + 2
                radius: height / 2
                color:  Qt.rgba(0, 0, 0, 0.7)

                QGCLabel {
                    id:               segLabel
                    anchors.centerIn: parent
                    text:             _root._distanceText(meters)
                    color:            "white"
                    font.pointSize:   ScreenTools.smallFontPointSize
                }
            }
        }
    }

    Component {
        id: gcsMarkerComponent

        MapQuickItem {
            anchorPoint.x: sourceItem.width / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    _root._gcs
            visible:       _root._gcsKnown
            z:             QGroundControl.zOrderMapItems + 4

            sourceItem: Rectangle {
                width:        ScreenTools.defaultFontPixelHeight * 1.1
                height:       width
                radius:       width / 2
                color:        "#1a73e8"
                border.color: "white"
                border.width: 3
            }
        }
    }

    // Points and segment labels follow _points.
    Repeater {
        model: _root.measuring ? _root._points : []

        delegate: Item {
            property var _point
            property var _label

            Component.onCompleted: {
                _point = measurePointComponent.createObject(_root.map, { "coordinate": modelData })
                _root.map.addMapItem(_point)
                if (index > 0) {
                    var a = _root._points[index - 1]
                    var mid = a.atDistanceAndAzimuth(a.distanceTo(modelData) / 2, a.azimuthTo(modelData))
                    _label = measureLabelComponent.createObject(_root.map, { "coordinate": mid, "meters": a.distanceTo(modelData) })
                    _root.map.addMapItem(_label)
                }
            }
            Component.onDestruction: {
                if (_point) {
                    _point.destroy()
                }
                if (_label) {
                    _label.destroy()
                }
            }
        }
    }

    property var _mapItems: []

    onMapChanged: {
        for (var i = 0; i < _mapItems.length; i++) {
            _mapItems[i].destroy()
        }
        _mapItems = []
        if (!map) {
            return
        }
        for (var component of [measureAreaComponent, measureLineComponent, gcsMarkerComponent]) {
            var item = component.createObject(map)
            map.addMapItem(item)
            _mapItems.push(item)
        }
    }

    Component.onDestruction: {
        for (var i = 0; i < _mapItems.length; i++) {
            _mapItems[i].destroy()
        }
    }
}

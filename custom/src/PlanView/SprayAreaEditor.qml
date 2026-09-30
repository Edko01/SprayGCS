import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls
import QGroundControl.FlightMap

/// Editor panel for SprayAreaComplexItem (right-hand side of the Plan view).
///
/// Laid out for field crews, in the style of XAG One: the job numbers on top
/// in large type, then Basic / Advanced tabs with one setting per row (label
/// left, value right). The polygon tools on the map (draw, trace, load KML)
/// come from the map visual automatically.

Rectangle {
    id:     _root
    height: visible ? (editorColumn.height + (_margin * 2)) : 0
    width:  availableWidth
    color:  qgcPal.windowShadeDark
    radius: _radius

    required property var  missionItem
    required property real availableWidth

    property real   _margin:     ScreenTools.defaultFontPixelWidth
    property real   _radius:     ScreenTools.defaultFontPixelWidth / 2
    property real   _rowHeight:  ScreenTools.defaultFontPixelHeight * 2.4
    property real   _fieldWidth: ScreenTools.defaultFontPixelWidth * 11
    property string _tab:        "basic"

    // The Align button lives on the Basic tab, so leaving it ends the mode.
    on_TabChanged: {
        if (_tab !== "basic") {
            missionItem.passAlignMode = false
        }
    }

    readonly property color _accent:     "#ffd400"   // XAG-style yellow, matches spray lines on the map
    readonly property color _accentText: "#111111"
    readonly property color _sideColor:  "#16a34a"   // green, as XAG One marks sides with their own buffer
    readonly property color _transitColor: "#00b3ff" // matches the transit legs on the map
    readonly property bool  _routeTransit: missionItem.transitMode.rawValue === 1

    // Polygon capture callbacks used by QGCMapPolygonVisuals.
    function polygonCaptureStarted()                            { missionItem.clearPolygon() }
    function polygonCaptureFinished(coordinates) {
        for (var i = 0; i < coordinates.length; i++) {
            missionItem.addPolygonCoordinate(coordinates[i])
        }
    }
    function polygonAdjustVertex(vertexIndex, vertexCoordinate) { missionItem.adjustPolygonCoordinate(vertexIndex, vertexCoordinate) }
    function polygonAdjustStarted()  {}
    function polygonAdjustFinished() {}

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Keyboard undo / redo while this item is selected. A text field that has
    // focus keeps Ctrl+Z for its own text.
    Shortcut {
        sequences: [ StandardKey.Undo ]
        enabled:   missionItem.isCurrentItem && missionItem.canUndo
        onActivated: missionItem.undo()
    }
    Shortcut {
        sequences: [ StandardKey.Redo, "Ctrl+Y" ]
        enabled:   missionItem.isCurrentItem && missionItem.canRedo
        onActivated: missionItem.redo()
    }

    // ---- layout ----------------------------------------------------------------

    ColumnLayout {
        id: editorColumn
        anchors {
            top:     parent.top
            left:    parent.left
            right:   parent.right
            margins: _margin
        }
        spacing: _margin

        // Field name, Undo / Redo and the job numbers are in the panel on the
        // left of the map (SprayJobPanel.qml).

        QGCLabel {
            Layout.fillWidth:    true
            wrapMode:            Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pointSize:      ScreenTools.mediumFontPointSize
            text:                qsTr("Draw the field boundary with the Polygon Tools on the map, or load a KML file.")
            visible:             !missionItem.fieldPolygon.isValid
        }

        QGCLabel {
            Layout.fillWidth:    true
            wrapMode:            Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pointSize:      ScreenTools.mediumFontPointSize
            color:               qgcPal.warningText
            text:                qsTr("Field too small for these settings. Reduce the buffer or swath width.")
            visible:             missionItem.fieldPolygon.isValid && !missionItem.pathValid && !missionItem.allSprayed
        }

        QGCLabel {
            Layout.fillWidth:    true
            wrapMode:            Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pointSize:      ScreenTools.mediumFontPointSize
            text:                qsTr("Everything in this field has been sprayed.")
            visible:             missionItem.allSprayed
        }

        // ---- edit boundary ------------------------------------------------------
        // While the boundary is drawn or edited only this section shows, so its
        // Done button stays in view; the settings come back on Done.
        // The boundary tools only work in this mode, so taps meant for other
        // edits can't move or retrace the boundary.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight:   _rowHeight
            radius:           _radius
            color:            missionItem.boundaryEditMode ? "white" : qgcPal.windowShade
            border.color:     missionItem.boundaryEditMode ? _accent : "transparent"
            border.width:     2
            visible:          missionItem.fieldPolygon.isValid || missionItem.boundaryEditMode

            QGCLabel {
                anchors.centerIn: parent
                text:             missionItem.boundaryEditMode ? qsTr("Done Editing Boundary") : qsTr("Edit Boundary")
                font.pointSize:   ScreenTools.mediumFontPointSize
                font.bold:        true
                color:            missionItem.boundaryEditMode ? _accentText : qgcPal.text
            }
            QGCMouseArea {
                anchors.fill: parent
                onClicked:    missionItem.boundaryEditMode = !missionItem.boundaryEditMode
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Drag a corner to move it; drag a + between corners to add one. Basic, Circular, Trace and Load KML above the map start the boundary again.")
            visible:          missionItem.boundaryEditMode && missionItem.fieldPolygon.isValid
        }

        // ---- resume a job -------------------------------------------------------
        // Paused mid-job (or landed to refill): mark what the drone has sprayed,
        // and the passes are laid out again over only what's left, with any new
        // settings. Needs the drone connected; the rest is shaded on the map.
        ColumnLayout {
            id:               resumeSection
            Layout.fillWidth: true
            spacing:          _margin / 2
            visible:          !missionItem.boundaryEditMode && missionItem.fieldPolygon.isValid
                              && (QGroundControl.multiVehicleManager.activeVehicle !== null || missionItem.hasSprayed)

            property string _message: ""

            Rectangle {
                Layout.fillWidth: true
                implicitHeight:   _rowHeight
                radius:           _radius
                color:            qgcPal.windowShade
                visible:          QGroundControl.multiVehicleManager.activeVehicle !== null

                QGCLabel {
                    anchors.centerIn: parent
                    text:             qsTr("Mark Sprayed So Far")
                    font.pointSize:   ScreenTools.mediumFontPointSize
                    font.bold:        true
                }
                QGCMouseArea {
                    anchors.fill: parent
                    onClicked:    resumeSection._message = missionItem.markSprayedFromDrone()
                }
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                color:            qgcPal.warningText
                text:             resumeSection._message
                visible:          resumeSection._message !== ""
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             qsTr("Mid-job, pause the drone (Hold) or land it, then tap this: the route is planned again over only what's left, and you can change the swath, angle and other settings for the rest.")
                visible:          !missionItem.hasSprayed && resumeSection._message === ""
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             (missionItem.resumeInAir
                                   ? qsTr("Already sprayed: %1 ac (shaded). The route covers only what's left and starts where the drone is waiting. Upload, then switch the drone to Mission.")
                                   : qsTr("Already sprayed: %1 ac (shaded). The route covers only what's left, starting near where the drone stopped."))
                                  .arg(missionItem.sprayedDoneAcres.toFixed(1))
                visible:          missionItem.hasSprayed
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight:   _rowHeight
                radius:           _radius
                color:            qgcPal.windowShade
                visible:          missionItem.hasSprayed

                QGCLabel {
                    anchors.centerIn: parent
                    text:             qsTr("Spray Whole Field Again")
                    font.pointSize:   ScreenTools.mediumFontPointSize
                    font.bold:        true
                }
                QGCMouseArea {
                    anchors.fill: parent
                    onClicked: {
                        resumeSection._message = ""
                        missionItem.clearSprayed()
                    }
                }
            }
        }

        // ---- edit route ---------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight:   _rowHeight
            radius:           _radius
            color:            missionItem.routeEditMode ? _accent : qgcPal.windowShade
            visible:          (missionItem.pathValid || missionItem.routeEditMode) && !missionItem.boundaryEditMode

            QGCLabel {
                anchors.centerIn: parent
                text:             missionItem.routeEditMode ? qsTr("Done Editing Route") : qsTr("Edit Route")
                font.pointSize:   ScreenTools.mediumFontPointSize
                font.bold:        true
                color:            missionItem.routeEditMode ? _accentText : qgcPal.text
            }
            QGCMouseArea {
                anchors.fill: parent
                onClicked:    missionItem.routeEditMode = !missionItem.routeEditMode
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Yellow sprays, white doesn't.\n" +
                                   "\u2022 Tap a section to turn the pump on or off, add a point, or delete it.\n" +
                                   "\u2022 Press and hold on a section, then drag, to add a point.\n" +
                                   "\u2022 Drag a point to move it. Tap it to delete it.\n" +
                                   "\u2022 Drag anywhere else to move the map.")
            visible:          missionItem.routeEditMode && missionItem.pathValid && !missionItem.boundaryEditMode
        }

        // ---- side buffers ---------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight:   _rowHeight
            radius:           _radius
            color:            missionItem.sideEditMode ? _sideColor : qgcPal.windowShade
            visible:          (missionItem.fieldPolygon.isValid || missionItem.sideEditMode) && !missionItem.boundaryEditMode

            QGCLabel {
                anchors.centerIn: parent
                text:             missionItem.sideEditMode
                                  ? qsTr("Done Setting Side Buffers")
                                  : (missionItem.customSideCount > 0 ? qsTr("Side Buffers (%1 set)").arg(missionItem.customSideCount)
                                                                     : qsTr("Side Buffers"))
                font.pointSize:   ScreenTools.mediumFontPointSize
                font.bold:        true
                color:            missionItem.sideEditMode ? "white" : qgcPal.text
            }
            QGCMouseArea {
                anchors.fill: parent
                onClicked:    missionItem.sideEditMode = !missionItem.sideEditMode
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Tap a side's number on the map to give it its own buffer. All other sides use the uniform buffer (%1).")
                                  .arg(missionItem.edgeMargin.valueString + " " + missionItem.edgeMargin.units)
            visible:          missionItem.sideEditMode && !missionItem.boundaryEditMode
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing:          _margin / 2
            visible:          (missionItem.sideEditMode || missionItem.customSideCount > 0) && !missionItem.boundaryEditMode

            Repeater {
                model: missionItem.sides

                delegate: Rectangle {
                    readonly property real _display: QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnits(modelData.buffer)
                    readonly property real _step:    1   // one ft (or one m in metric)

                    function _setDisplay(value) {
                        missionItem.setSideBuffer(modelData.side, QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsToMeters(value))
                    }

                    Layout.fillWidth: true
                    implicitHeight:   sideRow.height + _margin
                    radius:           _radius
                    color:            qgcPal.windowShade
                    visible:          modelData.custom

                    RowLayout {
                        id:             sideRow
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: _margin / 2
                        spacing:        _margin / 2

                        Rectangle {
                            implicitWidth:  ScreenTools.defaultFontPixelHeight * 1.6
                            implicitHeight: implicitWidth
                            radius:         width / 2
                            color:          _sideColor

                            QGCLabel {
                                anchors.centerIn: parent
                                text:             modelData.number
                                color:            "white"
                                font.bold:        true
                            }
                        }
                        QGCButton {
                            text:           "−"
                            implicitWidth:  ScreenTools.defaultFontPixelHeight * 2
                            onClicked:      _setDisplay(_display - _step)
                        }
                        QGCTextField {
                            Layout.fillWidth:   true
                            text:               _display.toFixed(1)
                            showUnits:          true
                            unitsLabel:         QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsString
                            numericValuesOnly:  true
                            onEditingFinished: {
                                var v = parseFloat(text)
                                if (!isNaN(v)) {
                                    _setDisplay(v)
                                }
                            }
                        }
                        QGCButton {
                            text:           "+"
                            implicitWidth:  ScreenTools.defaultFontPixelHeight * 2
                            onClicked:      _setDisplay(_display + _step)
                        }
                        QGCColoredImage {
                            implicitWidth:     ScreenTools.defaultFontPixelHeight * 1.4
                            implicitHeight:    implicitWidth
                            sourceSize.height: implicitHeight
                            fillMode:          Image.PreserveAspectFit
                            color:             "#ff3b30"
                            source:            "/res/TrashDelete.svg"

                            QGCMouseArea {
                                fillItem:  parent
                                onClicked: missionItem.clearSideBuffer(modelData.side)
                            }
                        }
                    }
                }
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            font.bold:        true
            color:            "#ff3b30"
            text:             missionItem.outsideCount === 1
                              ? qsTr("1 section (red) leaves the field boundary.")
                              : qsTr("%1 sections (red) leave the field boundary.").arg(missionItem.outsideCount)
            visible:          missionItem.pathValid && missionItem.outsideCount > 0 && !missionItem.boundaryEditMode
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            color:            qgcPal.warningText
            text:             qsTr("The passes were regenerated, so route edits were reset. Undo brings them back.")
            visible:          missionItem.routeEditsWereReset && !missionItem.boundaryEditMode
        }

        // ---- tabs ---------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing:          _margin / 2
            visible:          missionItem.fieldPolygon.isValid && !missionItem.boundaryEditMode

            Repeater {
                model: [ { tab: "basic",    text: qsTr("Basic") },
                         { tab: "transit",  text: qsTr("Transit") },
                         { tab: "advanced", text: qsTr("Advanced") } ]

                delegate: Rectangle {
                    Layout.fillWidth: true
                    implicitHeight:   ScreenTools.defaultFontPixelHeight * 2
                    radius:           _radius
                    color:            _tab === modelData.tab ? _accent : qgcPal.windowShade

                    QGCLabel {
                        anchors.centerIn: parent
                        text:             modelData.text
                        font.pointSize:   ScreenTools.mediumFontPointSize
                        font.bold:        true
                        color:            _tab === modelData.tab ? _accentText : qgcPal.text
                    }
                    QGCMouseArea {
                        anchors.fill: parent
                        onClicked:    _tab = modelData.tab
                    }
                }
            }
        }

        // ---- Basic --------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing:          0
            visible:          missionItem.fieldPolygon.isValid && _tab === "basic" && !missionItem.boundaryEditMode

            // One row per setting: name and value box, then a slider (XAG-style).
            // The slider shows its value while dragging and applies it on release,
            // so the passes aren't rebuilt on every step of the drag.
            Repeater {
                model: [
                    { label: qsTr("Height"),     fact: missionItem.altitude,        step: 0.5, preview: "" },
                    { label: qsTr("Speed"),      fact: missionItem.speed,           step: 0.5, preview: "" },
                    { label: qsTr("Swath"),      fact: missionItem.swathWidth,      step: 0.5, preview: "swath" },
                    { label: qsTr("Rate"),       fact: missionItem.applicationRate, step: 0.1, preview: "" },
                    { label: qsTr("Buffer"),     fact: missionItem.edgeMargin,      step: 0.5, preview: "buffer" },
                    { label: qsTr("Offset"),     fact: missionItem.passOffset,      step: 0.5, preview: "offset", swathRange: true },
                    { label: qsTr("Pass angle"), fact: missionItem.passAngle,       step: 1,   preview: "angle", align: true, wrap: true }
                ]

                delegate: Item {
                    id: basicRow

                    readonly property var  _fact: modelData.fact
                    readonly property real _step: modelData.step
                    property bool          _nudging: false   // an arrow is held
                    property int           _nudgeDir: 0
                    // The value is read out beside the label while the slider is dragged
                    // (the box updates on release). The arrows don't need it: a tap updates
                    // the box straight away.
                    readonly property bool _adjusting: slider.pressed

                    // Nearest step (in the units shown), kept inside the setting's range.
                    function _snap(v) {
                        var snapped = Math.round(v / _step) * _step
                        return Math.min(_fact.max, Math.max(_fact.min, snapped))
                    }
                    function _toMeters(v) {
                        return Number(QGroundControl.unitsConversion.appSettingsHorizontalDistanceUnitsToMeters(v))
                    }
                    // Settings that reshape the passes draw a live preview while dragging.
                    function _preview() {
                        if (modelData.preview === "") {
                            return
                        }
                        var v     = _snap(slider.value)
                        var swath = modelData.preview === "swath"  ? _toMeters(v) : missionItem.swathWidth.rawValue
                        var angle = modelData.preview === "angle"  ? v            : missionItem.passAngle.rawValue
                        var buf   = modelData.preview === "buffer" ? _toMeters(v) : missionItem.edgeMargin.rawValue
                        var off   = modelData.preview === "offset" ? _toMeters(v) : missionItem.passOffset.rawValue
                        missionItem.previewPasses(swath, angle, buf, off)
                    }
                    // Arrow buttons: one step down (-1) or up (+1). Like dragging, the
                    // passes are previewed and the value is applied on release.
                    function _nudge(dir) {
                        var v = Math.round((slider.value + dir * _step) / _step) * _step
                        if (modelData.wrap === true) {
                            v = ((v % 360) + 360) % 360   // pass angle: 359 -> 0 -> 1
                        }
                        v = Math.min(slider.to, Math.max(slider.from, _snap(v)))
                        slider.value = v
                        _preview()
                    }
                    function _startNudge(dir) {
                        _nudgeDir = dir
                        _nudging  = true
                        _nudge(dir)
                        nudgeTimer.interval = 400   // hold to repeat
                        nudgeTimer.restart()
                    }
                    function _endNudge() {
                        if (!_nudging) {
                            return
                        }
                        nudgeTimer.stop()
                        _nudging = false
                        _commit()
                    }
                    function _commit() {
                        previewTimer.stop()
                        missionItem.clearPreview()
                        var v = _snap(slider.value)
                        if (Math.abs(_fact.value - v) > 1e-9) {
                            _fact.value = v
                        }
                        slider.value = Qt.binding(function() { return basicRow._fact.value })
                    }

                    Layout.fillWidth: true
                    implicitHeight:   rowColumn.height + _margin

                    Timer {
                        id:          nudgeTimer
                        repeat:      true
                        onTriggered: {
                            interval = 100
                            basicRow._nudge(basicRow._nudgeDir)
                        }
                    }

                    // At most ~15 previews a second while dragging.
                    Timer {
                        id:          previewTimer
                        interval:    60
                        repeat:      false
                        onTriggered: basicRow._preview()
                    }

                    Column {
                        id:           rowColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        y:            _margin / 2
                        spacing:      _margin / 2

                        Item {
                            width:  parent.width
                            height: _rowHeight

                            QGCLabel {
                                anchors.left:           parent.left
                                anchors.right:          valueField.left
                                anchors.rightMargin:    _margin
                                anchors.verticalCenter: parent.verticalCenter
                                text:                   basicRow._adjusting
                                                        ? modelData.label + "  " + basicRow._snap(slider.value).toFixed(basicRow._fact.decimalPlaces) + " " + basicRow._fact.units
                                                        : modelData.label
                                font.pointSize:         ScreenTools.mediumFontPointSize
                                font.bold:              basicRow._adjusting
                                elide:                  Text.ElideRight
                            }
                            FactTextField {
                                id:                     valueField
                                anchors.right:          parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width:                  _fieldWidth
                                fact:                   modelData.fact
                                showUnits:              true
                            }
                        }

                        // Slider with a step arrow at each end (tap: one step, hold: repeat).
                        RowLayout {
                            width:   parent.width
                            spacing: _margin / 2

                            Repeater {
                                model: [ { dir: -1, icon: "/InstrumentValueIcons/cheveron-left.svg" } ]
                                delegate: stepArrow
                            }
                            QGCSlider {
                                id:                 slider
                                Layout.fillWidth:   true
                                // Offset: half a swath either way covers every position (a whole
                                // swath gives the same passes again); the box takes any value.
                                from:               modelData.swathRange === true ? -missionItem.swathWidth.value / 2 : basicRow._fact.min
                                to:                 modelData.swathRange === true ?  missionItem.swathWidth.value / 2 : basicRow._fact.max
                                value:              basicRow._fact.value
                                showBoundaryValues: true
                                Accessible.name:    modelData.label

                                onPressedChanged: {
                                    if (!pressed) {
                                        basicRow._commit()
                                    }
                                }
                                onValueChanged: {
                                    if (pressed && modelData.preview !== "" && !previewTimer.running) {
                                        previewTimer.start()
                                    }
                                }
                                onMoved: {
                                    if (!pressed) {
                                        basicRow._commit()   // keyboard
                                    }
                                }
                            }
                            Repeater {
                                model: [ { dir: 1, icon: "/InstrumentValueIcons/cheveron-right.svg" } ]
                                delegate: stepArrow
                            }
                        }

                        Component {
                            id: stepArrow

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth:    ScreenTools.defaultFontPixelHeight * 2
                                implicitHeight:   implicitWidth
                                radius:           _radius
                                color:            arrowArea.pressed ? _accent : qgcPal.windowShade
                                Accessible.name:  modelData.dir < 0 ? qsTr("Decrease") : qsTr("Increase")

                                QGCColoredImage {
                                    anchors.centerIn:  parent
                                    width:             parent.width * 0.55
                                    height:            width
                                    sourceSize.height: height
                                    source:            modelData.icon
                                    color:             arrowArea.pressed ? _accentText : qgcPal.text
                                }
                                MouseArea {
                                    id:             arrowArea
                                    anchors.fill:   parent
                                    onPressed:      basicRow._startNudge(modelData.dir)
                                    onReleased:     basicRow._endNudge()
                                    onCanceled:     basicRow._endNudge()
                                }
                            }
                        }

                        // Pass angle: line the passes up with a side of the field.
                        Rectangle {
                            width:   parent.width
                            height:  _rowHeight
                            radius:  _radius
                            color:   missionItem.passAlignMode ? _accent : qgcPal.windowShade
                            visible: modelData.align === true

                            QGCLabel {
                                anchors.centerIn: parent
                                text:             missionItem.passAlignMode ? qsTr("Done Aligning") : qsTr("Align Passes to a Side")
                                font.pointSize:   ScreenTools.mediumFontPointSize
                                font.bold:        true
                                color:            missionItem.passAlignMode ? _accentText : qgcPal.text
                            }
                            QGCMouseArea {
                                anchors.fill: parent
                                onClicked:    missionItem.passAlignMode = !missionItem.passAlignMode
                            }
                        }
                        QGCLabel {
                            width:    parent.width
                            wrapMode: Text.WordWrap
                            text:     qsTr("Tap a side's number on the map. The passes will run along that side (its number turns yellow). Tap another side to try it.")
                            visible:  modelData.align === true && missionItem.passAlignMode
                        }
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width:          parent.width
                        height:         1
                        color:          qgcPal.windowShade
                    }
                }
            }
        }

        // ---- Transit ------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing:          _margin / 2
            visible:          missionItem.fieldPolygon.isValid && _tab === "transit" && !missionItem.boundaryEditMode

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                color:            qgcPal.warningText
                text:             qsTr("Add a Takeoff item and place it where the drone will take off to plan the transit.")
                visible:          !missionItem.hasTakeoff
            }

            // Mode A / B
            RowLayout {
                Layout.fillWidth: true
                spacing:          _margin / 2

                Repeater {
                    model: [ { value: 0, title: qsTr("A"), text: qsTr("Entry side") },
                             { value: 1, title: qsTr("B"), text: qsTr("Planned route") } ]

                    delegate: Rectangle {
                        readonly property bool _selected: missionItem.transitMode.rawValue === modelData.value

                        Layout.fillWidth: true
                        implicitHeight:   modeColumn.height + _margin
                        radius:           _radius
                        color:            _selected ? _accent : qgcPal.windowShade

                        Column {
                            id:               modeColumn
                            anchors.centerIn: parent

                            QGCLabel {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text:                     modelData.title
                                font.pointSize:           ScreenTools.largeFontPointSize
                                font.bold:                true
                                color:                    _selected ? _accentText : qgcPal.text
                            }
                            QGCLabel {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text:                     modelData.text
                                color:                    _selected ? _accentText : qgcPal.text
                            }
                        }
                        QGCMouseArea {
                            anchors.fill: parent
                            onClicked:    missionItem.transitMode.rawValue = modelData.value
                        }
                    }
                }
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             _routeTransit
                                  ? qsTr("Flies the planned route (blue) to its last point, then at spray height to the start. Comes back the same way.")
                                  : qsTr("Flies straight to the entry side (thick blue), then inside the field to the start. Leaves the field through the same side.")
            }

            // Name, value box and up / down arrows (tap: one step, hold: repeat).
            Repeater {
                model: [
                    { label: qsTr("Transit height"), fact: missionItem.transitAltitude, step: 1 },
                    { label: qsTr("Transit speed"),  fact: missionItem.transitSpeed,    step: 0.5 }
                ]

                delegate: Item {
                    id: transitRow

                    readonly property var  _fact: modelData.fact
                    readonly property real _step: modelData.step
                    property int           _stepDir: 0

                    function _stepOnce(dir) {
                        var v = Math.round((_fact.value + dir * _step) / _step) * _step
                        v = Math.min(_fact.max, Math.max(_fact.min, v))
                        if (Math.abs(_fact.value - v) > 1e-9) {
                            _fact.value = v
                        }
                    }
                    function _startStep(dir) {
                        _stepDir = dir
                        _stepOnce(dir)
                        transitStepTimer.interval = 400   // hold to repeat
                        transitStepTimer.restart()
                    }

                    Layout.fillWidth: true
                    implicitHeight:   _rowHeight

                    Timer {
                        id:          transitStepTimer
                        repeat:      true
                        onTriggered: {
                            interval = 100
                            transitRow._stepOnce(transitRow._stepDir)
                        }
                    }

                    QGCLabel {
                        anchors.left:           parent.left
                        anchors.right:          transitControls.left
                        anchors.rightMargin:    _margin
                        anchors.verticalCenter: parent.verticalCenter
                        text:                   modelData.label
                        font.pointSize:         ScreenTools.mediumFontPointSize
                        elide:                  Text.ElideRight
                    }

                    Row {
                        id:                     transitControls
                        anchors.right:          parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing:                _margin / 2

                        FactTextField {
                            anchors.verticalCenter: parent.verticalCenter
                            width:                  _fieldWidth
                            fact:                   transitRow._fact
                            showUnits:              true
                        }
                        // Up over down, like a spin box, so the name keeps its room.
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing:                2

                            Repeater {
                                model: [ { dir:  1, icon: "/InstrumentValueIcons/cheveron-up.svg" },
                                         { dir: -1, icon: "/InstrumentValueIcons/cheveron-down.svg" } ]
                                delegate: transitArrow
                            }
                        }
                    }

                    Component {
                        id: transitArrow

                        Rectangle {
                            width:           ScreenTools.defaultFontPixelHeight * 1.8
                            height:          (_rowHeight - 2) / 2
                            radius:          _radius
                            color:           transitArrowArea.pressed ? _accent : qgcPal.windowShade
                            Accessible.name: modelData.dir < 0 ? qsTr("Decrease") : qsTr("Increase")

                            QGCColoredImage {
                                anchors.centerIn:  parent
                                height:            parent.height * 0.7
                                width:             height
                                sourceSize.height: height
                                source:            modelData.icon
                                color:             transitArrowArea.pressed ? _accentText : qgcPal.text
                            }
                            MouseArea {
                                id:           transitArrowArea
                                anchors.fill: parent
                                onPressed:    transitRow._startStep(modelData.dir)
                                onReleased:   transitStepTimer.stop()
                                onCanceled:   transitStepTimer.stop()
                            }
                        }
                    }
                }
            }

            // Edit the entry side (A) or the planned route (B) on the map.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight:   _rowHeight
                radius:           _radius
                color:            missionItem.transitEditMode ? _transitColor : qgcPal.windowShade
                visible:          (missionItem.pathValid && missionItem.hasTakeoff && (_routeTransit || missionItem.gateSides.length > 0))
                                  || missionItem.transitEditMode

                QGCLabel {
                    anchors.centerIn: parent
                    text:             missionItem.transitEditMode ? qsTr("Done")
                                                                  : (_routeTransit ? qsTr("Edit Transit Route") : qsTr("Choose Entry Sides"))
                    font.pointSize:   ScreenTools.mediumFontPointSize
                    font.bold:        true
                    color:            missionItem.transitEditMode ? "white" : qgcPal.text
                }
                QGCMouseArea {
                    anchors.fill: parent
                    onClicked:    missionItem.transitEditMode = !missionItem.transitEditMode
                }
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                visible:          missionItem.transitEditMode
                text:             _routeTransit
                                  ? qsTr("\u2022 Press and hold on the blue route, then drag, to add a point.\n\u2022 Drag a point to move it; the last one is where the drone enters the field.\n\u2022 Tap a point to delete it.")
                                  : qsTr("Tap sides' numbers to allow or remove them as entry sides. Going in and coming out, the drone uses whichever allowed side is shortest.")
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             qsTr("The takeoff point is inside the field, so the drone flies straight to the start and back.")
                visible:          !_routeTransit && missionItem.hasTakeoff && missionItem.pathValid && missionItem.gateSides.length === 0
            }

            // Mode A: which side
            RowLayout {
                Layout.fillWidth: true
                spacing:          _margin
                visible:          !_routeTransit && missionItem.gateSides.length > 0

                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode:         Text.WordWrap
                    text: {
                        var numbers = missionItem.gateSides.map(function(s) { return s + 1 }).join(", ")
                        if (missionItem.gateSideAuto) {
                            return qsTr("Entry side: %1 (nearest takeoff)").arg(numbers)
                        }
                        return missionItem.gateSides.length === 1 ? qsTr("Entry side: %1 (chosen)").arg(numbers)
                                                                  : qsTr("Entry sides: %1 (chosen)").arg(numbers)
                    }
                }
                QGCButton {
                    text:      qsTr("Use Nearest")
                    visible:   !missionItem.gateSideAuto
                    onClicked: missionItem.useNearestGateSide()
                }
            }

            // Mode B: checks and reset
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                color:            qgcPal.warningText
                text:             qsTr("Transit height is below spray height; the drone will transit at spray height.")
                visible:          missionItem.transitBelowSpray
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                font.bold:        true
                color:            "#ff3b30"
                text:             qsTr("The last point of the transit route is outside the field. Move it inside before saving or uploading.")
                visible:          _routeTransit && !missionItem.transitEntryInside
            }
            QGCButton {
                text:      qsTr("Reset Transit Route")
                visible:   _routeTransit
                enabled:   missionItem.transitRouteCustom
                onClicked: missionItem.resetTransitRoute()
            }

            QGCLabel {
                Layout.fillWidth: true
                Layout.topMargin: _margin
                wrapMode:         Text.WordWrap
                text:             qsTr("Start (S) and end (E) are shown on the map. Drag the S to any corner of the passes (green dots show where it can go), or onto the E to fly the route the other way round.")
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                font.pointSize:   ScreenTools.smallFontPointSize
                text:             qsTr("Heights are above ground. Terrain following comes with the onboard app; until then the drone holds them relative to the takeoff point.")
            }
        }

        // ---- Advanced -----------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing:          0
            visible:          missionItem.fieldPolygon.isValid && _tab === "advanced" && !missionItem.boundaryEditMode

            Item {
                Layout.fillWidth: true
                implicitHeight:   _rowHeight

                FactCheckBoxSlider {
                    anchors.left:           parent.left
                    anchors.right:          parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text:                   qsTr("Headland pass")
                    fact:                   missionItem.headlandPass
                }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: _margin
                columns:          2
                columnSpacing:    _margin
                rowSpacing:       _margin / 2
                visible:          missionItem.pathValid

                QGCLabel { text: qsTr("Field area") }
                QGCLabel { text: missionItem.sprayAreaAcres.toFixed(2) + " " + qsTr("ac") }

                QGCLabel { text: qsTr("Spray length") }
                QGCLabel { text: QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnitsString(missionItem.sprayLengthM, 0) }

                QGCLabel { text: qsTr("Flight distance") }
                QGCLabel { text: QGroundControl.unitsConversion.metersToAppSettingsHorizontalDistanceUnitsString(missionItem.complexDistance, 0) }
            }

            QGCButton {
                Layout.topMargin: _margin
                text:             qsTr("Reset route (discard route edits)")
                enabled:          missionItem.hasRouteEdits
                visible:          missionItem.pathValid
                onClicked:        missionItem.resetRouteEdits()
            }
        }
    }
}

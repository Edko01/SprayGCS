#include "SprayAreaComplexItem.h"

#include "AppMessages.h"
#include "AppSettings.h"
#include "CustomPlugin.h"
#include "JsonParsing.h"
#include "MissionFlightStatus.h"
#include "MissionController.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanMasterController.h"
#include "QGCApplication.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"
#include "TakeoffMissionItem.h"
#include "QGCLoggingCategory.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QScopeGuard>
#include <QtCore/QtMath>

#include <algorithm>
#include <utility>
#include <cmath>

QGC_LOGGING_CATEGORY(SprayAreaLog, "Custom.SprayArea")

SprayAreaComplexItem::SprayAreaComplexItem(PlanMasterController *masterController,
                                           bool flyView,
                                           const QString &kmlOrShpFile)
    : ComplexMissionItem(masterController, flyView)
    , _metaDataMap(FactMetaData::createMapFromJsonFile(
          QStringLiteral(":/json/SprayArea.SettingsGroup.json"), this))
    , _swathWidthFact      (settingsGroup, _metaDataMap[QStringLiteral("SwathWidth")])
    , _passAngleFact       (settingsGroup, _metaDataMap[QStringLiteral("PassAngle")])
    , _passOffsetFact      (settingsGroup, _metaDataMap[QStringLiteral("PassOffset")])
    , _altitudeFact        (settingsGroup, _metaDataMap[QStringLiteral("Altitude")])
    , _speedFact           (settingsGroup, _metaDataMap[QStringLiteral("Speed")])
    , _applicationRateFact (settingsGroup, _metaDataMap[QStringLiteral("ApplicationRate")])
    , _edgeMarginFact      (settingsGroup, _metaDataMap[QStringLiteral("EdgeMargin")])
    , _headlandPassFact    (settingsGroup, _metaDataMap[QStringLiteral("HeadlandPass")])
    , _transitModeFact     (settingsGroup, _metaDataMap[QStringLiteral("TransitMode")])
    , _transitAltitudeFact (settingsGroup, _metaDataMap[QStringLiteral("TransitAltitude")])
    , _transitSpeedFact    (settingsGroup, _metaDataMap[QStringLiteral("TransitSpeed")])
    , _tankCapacityFact    (settingsGroup, _metaDataMap[QStringLiteral("TankCapacity")])
    , _batteryMinutesFact  (settingsGroup, _metaDataMap[QStringLiteral("BatteryMinutes")])
{
    _editorQml = QStringLiteral("qrc:/qml/Custom/Plan/SprayAreaEditor.qml");

    // Any setting change regenerates the passes and marks the plan as modified.
    // The offset belongs to the field: a new Spray Area starts at 0 (a saved plan loads its own).
    _passOffsetFact.setRawValue(0.0);

    for (Fact *fact : { static_cast<Fact *>(&_swathWidthFact), static_cast<Fact *>(&_passAngleFact),
                        static_cast<Fact *>(&_passOffsetFact),
                        static_cast<Fact *>(&_edgeMarginFact), static_cast<Fact *>(&_headlandPassFact) }) {
        connect(fact, &Fact::valueChanged, this, &SprayAreaComplexItem::_regenerate);
        connect(fact, &Fact::valueChanged, this, &SprayAreaComplexItem::_setDirty);
    }
    connect(&_applicationRateFact, &Fact::valueChanged, this, [this]() {
        _setDirty();
        emit pathUpdated();   // volume estimate changes
        _updateTrips();
    });
    // The drone's tank and battery belong to the drone, not the plan: they
    // only change the trip estimate.
    connect(&_tankCapacityFact,   &Fact::valueChanged, this, &SprayAreaComplexItem::_updateTrips);
    connect(&_batteryMinutesFact, &Fact::valueChanged, this, &SprayAreaComplexItem::_updateTrips);
    // Spray height and speed, and the transit settings, change the mission
    // but not the passes.
    for (Fact *fact : { static_cast<Fact *>(&_speedFact), static_cast<Fact *>(&_altitudeFact),
                        static_cast<Fact *>(&_transitModeFact), static_cast<Fact *>(&_transitSpeedFact) }) {
        connect(fact, &Fact::valueChanged, this, [this]() {
            const int oldLastSeq = lastSequenceNumber();
            _rebuildMission();
            setDirty(true);
            _emitMissionChanged(oldLastSeq);
        });
    }
    connect(&_transitAltitudeFact, &Fact::valueChanged, this, [this]() {
        const int oldLastSeq = lastSequenceNumber();
        _rebuildMission();
        setDirty(true);
        _emitMissionChanged(oldLastSeq);
        if (!_restoring && !_loading) {
            syncTakeoffAltitude();   // climb straight to transit height
        }
    });

    // A waypoint dropped by hand starts at transit height (the Defaults section
    // that normally sets it is hidden in SprayGCS).
    connect(&_transitAltitudeFact, &Fact::valueChanged, this, &SprayAreaComplexItem::_syncDefaultWaypointAltitude);
    connect(&_altitudeFact,        &Fact::valueChanged, this, &SprayAreaComplexItem::_syncDefaultWaypointAltitude);
    _syncDefaultWaypointAltitude();

    // Spray heights are always above the takeoff point. The plan's Alt Frame
    // (hidden in SprayGCS) only sets the frame of waypoints dropped by hand, so
    // keep it on Relative too; "Mixed" (a loaded plan with other frames) also
    // gives new waypoints Relative.
    if (_missionController && !flyView) {
        const auto frame = _missionController->globalAltitudeFrame();
        if (frame != QGroundControlQmlGlobal::AltitudeFrameRelative && frame != QGroundControlQmlGlobal::AltitudeFrameMixed) {
            _missionController->setGlobalAltitudeFrame(QGroundControlQmlGlobal::AltitudeFrameRelative);
        }
    }

    // The takeoff point sets the transit legs, and there's only transit while
    // the plan has a Takeoff item: rebuild when either changes. (Item list
    // changes are queued so the mission list finishes its own update first.)
    if (_missionController) {
        auto rebuild = [this]() {
            const int oldLastSeq = lastSequenceNumber();
            _rebuildMission();
            _emitMissionChanged(oldLastSeq);
            _syncEndItem();
        };
        connect(_missionController, &MissionController::plannedHomePositionChanged, this, rebuild);
        auto watchItems = [this, rebuild]() {
            QObject::disconnect(_visualItemsConnection);
            if (QmlObjectListModel *items = _missionController->visualItems()) {
                _visualItemsConnection = connect(items, &QmlObjectListModel::countChanged, this, rebuild, Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(this, rebuild, Qt::QueuedConnection);   // e.g. a plan just loaded
        };
        watchItems();
        connect(_missionController, &MissionController::visualItemsReset, this, watchItems);
    }
    connect(&_fieldPolygon, &QGCMapPolygon::pathChanged, this, &SprayAreaComplexItem::_setDirty);
    connect(&_fieldPolygon, &QGCMapPolygon::pathChanged, this, &SprayAreaComplexItem::_polygonChanged);

    // Leaving the item ends any map edit mode.
    connect(this, &VisualMissionItem::isCurrentItemChanged, this, [this](bool isCurrent) {
        if (!isCurrent) {
            setRouteEditMode(false);
            setSideEditMode(false);
            setPassAlignMode(false);
            setTransitEditMode(false);
            setBoundaryEditMode(_fieldPolygon.count() < 3);   // keep the tools while there's no boundary
        }
    });

    // The buffer is at least 3 ft; older saved settings may be lower.
    if (_edgeMarginFact.rawValue().toDouble() < kMinBufferM) {
        _edgeMarginFact.setRawValue(kMinBufferM);
    }

    // Undo / redo: every edit goes through _noteChange, and is recorded as one
    // step once edits have paused (so a drag or a traced boundary is one step).
    _undoGroupTimer.setSingleShot(true);
    _undoGroupTimer.setInterval(_undoGroupMs);
    connect(&_undoGroupTimer, &QTimer::timeout, this, &SprayAreaComplexItem::_commitChange);
    for (Fact *fact : _editableFacts()) {
        connect(fact, &Fact::valueChanged, this, &SprayAreaComplexItem::_noteChange);
    }
    connect(&_fieldPolygon, &QGCMapPolygon::pathChanged, this, &SprayAreaComplexItem::_noteChange);

    // Resuming a job: follow the drone's mission progress (Plan view only), so
    // "Mark Sprayed So Far" knows where it left the mission.
    if (!flyView) {
        s_planViewItem = this;
        connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged,
                this, &SprayAreaComplexItem::_trackVehicle);
        _trackVehicle(MultiVehicleManager::instance()->activeVehicle());
        if (CustomPlugin *plugin = qobject_cast<CustomPlugin *>(QGCCorePlugin::instance())) {
            plugin->sprayPlanAreaCreated(this);   // the Fly view's Resume Job card follows this item
        }
    }

    if (!kmlOrShpFile.isEmpty()) {
        _fieldPolygon.loadKMLOrSHPFile(kmlOrShpFile);
        _fieldPolygon.setDirty(false);
    }

    _regenerate();
    setDirty(false);
    _resetUndoHistory();
    setBoundaryEditMode(_fieldPolygon.count() < 3);
}

/*---------------------------------------------------------------------------*/

void SprayAreaComplexItem::setDirty(bool dirty)
{
    if (_dirty != dirty) {
        _dirty = dirty;
        emit dirtyChanged(_dirty);
    }
}

void SprayAreaComplexItem::_setDirty()
{
    setDirty(true);
}

void SprayAreaComplexItem::_regenerate()
{
    std::vector<spray::LatLon> boundary;
    boundary.reserve(static_cast<size_t>(_fieldPolygon.count()));
    for (int i = 0; i < _fieldPolygon.count(); ++i) {
        const QGeoCoordinate c = _fieldPolygon.vertexCoordinate(i);
        boundary.push_back({ c.latitude(), c.longitude() });
    }

    const spray::Settings settings = _generatorSettings();

    const int  oldLastSeq = lastSequenceNumber();
    const bool hadEdits   = _route.hasEdits();

    _result = spray::generate(boundary, settings);

    // New passes: on/off edits made for the old passes no longer line up, so
    // start again from the defaults (passes on, transits off) and tell the user.
    _route.reset(_result.flightPath, _result.legSpray);
    if (hadEdits) {
        _editsWereReset = true;
    }

    _rebuildSideVariants();

    _sprayAreaVariant.clear();
    for (const auto &p : _result.sprayArea) {
        _sprayAreaVariant.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
    }
    _startOptionsVariant.clear();
    for (const auto &p : _result.startOptions) {
        _startOptionsVariant.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
    }
    _rebuildRouteVariants();

    qCDebug(SprayAreaLog) << "regenerated: valid" << _result.valid
                          << "passes" << _result.passCount
                          << "waypoints" << _result.flightPath.size();

    emit specifiesCoordinateChanged();
    emit readyForSaveStateChanged();
    _emitRouteChanged(oldLastSeq);
}

spray::Settings SprayAreaComplexItem::_generatorSettings() const
{
    spray::Settings settings;
    settings.swathWidthM  = _swathWidthFact.rawValue().toDouble();
    settings.passAngleDeg = _passAngleFact.rawValue().toDouble();
    settings.offsetM      = _passOffsetFact.rawValue().toDouble();
    settings.edgeMarginM  = _edgeMarginFact.rawValue().toDouble();
    settings.headlandPass = _headlandPassFact.rawValue().toBool();
    settings.hasStartNear = _hasStartNear;
    settings.startNear    = _startNear;
    settings.sprayed       = _sprayed;
    settings.hasResumeFrom = _hasResumeFrom;
    settings.resumeFrom    = _resumeFrom;
    if (_sideBuffers.size() == _fieldPolygon.count()) {
        for (double buffer : _sideBuffers) {
            settings.sideMarginsM.push_back(buffer);   // -1 = use the uniform buffer
        }
    }
    return settings;
}

void SprayAreaComplexItem::previewPasses(double swathWidthM, double passAngleDeg, double edgeMarginM, double offsetM)
{
    if (qIsNaN(swathWidthM) || qIsNaN(passAngleDeg) || qIsNaN(edgeMarginM) || qIsNaN(offsetM)) {
        return;
    }
    spray::Settings settings = _generatorSettings();
    settings.swathWidthM  = qMax(0.1, swathWidthM);
    settings.passAngleDeg = passAngleDeg;
    settings.offsetM      = offsetM;
    settings.edgeMarginM  = qMax(kMinBufferM, edgeMarginM);

    const spray::Result preview = spray::generate(_boundary(), settings);
    _previewPathVariant.clear();
    if (preview.valid) {
        for (const auto &p : preview.flightPath) {
            _previewPathVariant.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
        }
    }
    emit previewPathChanged();
}

void SprayAreaComplexItem::clearPreview()
{
    if (!_previewPathVariant.isEmpty()) {
        _previewPathVariant.clear();
        emit previewPathChanged();
    }
}

void SprayAreaComplexItem::_rebuildRouteVariants()
{
    auto coord = [](const spray::LatLon &p) { return QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)); };
    const auto &points = _route.points();

    // Flag sections that leave the field boundary (only hand edits can).
    std::vector<spray::LatLon> boundary;
    boundary.reserve(static_cast<size_t>(_fieldPolygon.count()));
    for (int i = 0; i < _fieldPolygon.count(); ++i) {
        const QGeoCoordinate c = _fieldPolygon.vertexCoordinate(i);
        boundary.push_back({ c.latitude(), c.longitude() });
    }
    const std::vector<bool> outside = spray::legsOutside(boundary, points);
    _outsideCount = static_cast<int>(std::count(outside.begin(), outside.end(), true));

    _flightPathVariant.clear();
    for (const auto &p : points) {
        _flightPathVariant.append(coord(p));
    }

    _segmentsVariant.clear();
    for (int i = 0; i < _route.segmentCount(); ++i) {
        const auto &a = points[static_cast<size_t>(i)];
        const auto &b = points[static_cast<size_t>(i) + 1];
        QVariantMap segment;
        segment[QStringLiteral("index")] = i;
        segment[QStringLiteral("spray")] = static_cast<bool>(_route.segmentSpray()[static_cast<size_t>(i)]);
        segment[QStringLiteral("outside")] = static_cast<bool>(outside[static_cast<size_t>(i)]);
        segment[QStringLiteral("start")] = coord(a);
        segment[QStringLiteral("end")]   = coord(b);
        segment[QStringLiteral("mid")]   = coord({ (a.lat + b.lat) / 2.0, (a.lon + b.lon) / 2.0 });
        const QGeoCoordinate qa(a.lat, a.lon);
        const QGeoCoordinate qb(b.lat, b.lon);
        segment[QStringLiteral("length")]  = qa.distanceTo(qb);   // metres, for the map labels
        segment[QStringLiteral("azimuth")] = qa.azimuthTo(qb);    // degrees clockwise from north
        _segmentsVariant.append(segment);
    }

    _rebuildMission();
}

void SprayAreaComplexItem::_routeEdited(int oldLastSeq)
{
    _editsWereReset = false;
    _rebuildRouteVariants();
    setDirty(true);
    _emitRouteChanged(oldLastSeq);
    _noteChange();
}

void SprayAreaComplexItem::_emitRouteChanged(int oldLastSeq)
{
    emit pathUpdated();   // route lines and labels
    _emitMissionChanged(oldLastSeq);
}

void SprayAreaComplexItem::_emitMissionChanged(int oldLastSeq)
{
    emit complexDistanceChanged();
    emit coordinateChanged(coordinate());
    emit entryCoordinateChanged(entryCoordinate());   // mission line into the item
    emit exitCoordinateChanged(exitCoordinate());
    emit greatestDistanceToChanged();
    emit amslEntryAltChanged(amslEntryAlt());
    emit amslExitAltChanged(amslExitAlt());
    emit minAMSLAltitudeChanged();
    emit maxAMSLAltitudeChanged();
    emit specifiedFlightSpeedChanged();
    if (lastSequenceNumber() != oldLastSeq) {
        emit lastSequenceNumberChanged(lastSequenceNumber());
    }
}

/*---------------------------------------------------------------------------*/
// Transit, start and end

std::vector<spray::LatLon> SprayAreaComplexItem::_boundary() const
{
    std::vector<spray::LatLon> boundary;
    boundary.reserve(static_cast<size_t>(_fieldPolygon.count()));
    for (int i = 0; i < _fieldPolygon.count(); ++i) {
        const QGeoCoordinate c = _fieldPolygon.vertexCoordinate(i);
        boundary.push_back({ c.latitude(), c.longitude() });
    }
    return boundary;
}

bool SprayAreaComplexItem::hasTakeoff() const
{
    // QGC invents a home position when none is set, so only plan transit when
    // the plan has a Takeoff item (the pilot has placed the launch point).
    if (!_missionController || !_missionController->plannedHomePosition().isValid()) {
        return false;
    }
    QmlObjectListModel *items = _missionController->visualItems();
    for (int i = 0; items && i < items->count(); ++i) {
        if (items->value<TakeoffMissionItem *>(i)) {
            return true;
        }
    }
    return false;
}



QGeoCoordinate SprayAreaComplexItem::takeoffPoint() const
{
    if (!hasTakeoff()) {
        return {};
    }
    const QGeoCoordinate home = _missionController->plannedHomePosition();
    return QGeoCoordinate(home.latitude(), home.longitude());
}

QGeoCoordinate SprayAreaComplexItem::startPoint() const
{
    const auto &points = _route.points();
    if (!_result.valid || points.empty()) {
        return {};
    }
    const auto &p = _routeReversed ? points.back() : points.front();
    return QGeoCoordinate(p.lat, p.lon);
}

QGeoCoordinate SprayAreaComplexItem::endPoint() const
{
    const auto &points = _route.points();
    if (!_result.valid || points.empty()) {
        return {};
    }
    const auto &p = _routeReversed ? points.front() : points.back();
    return QGeoCoordinate(p.lat, p.lon);
}

std::vector<spray::LatLon> SprayAreaComplexItem::_defaultTransitRoute() const
{
    // Straight into the field by the shortest way: to the nearest point of
    // the spray area.
    if (!hasTakeoff() || _result.sprayArea.size() < 3) {
        return {};
    }
    const QGeoCoordinate home = takeoffPoint();
    return { spray::nearestPointOnPolygon(_result.sprayArea, { home.latitude(), home.longitude() }) };
}

void SprayAreaComplexItem::_makeTransitRouteCustom()
{
    if (!_transitRouteCustom) {
        _transitRoute       = _defaultTransitRoute();
        _transitRouteCustom = true;
    }
}

void SprayAreaComplexItem::_rebuildMission()
{
    const std::vector<spray::LatLon> boundary = _boundary();
    const bool                       takeoff  = hasTakeoff();
    const QGeoCoordinate             home     = takeoffPoint();
    const spray::LatLon              homeLL   { home.latitude(), home.longitude() };

    // The pilot's entry sides only apply to the boundary they were chosen on
    // (see _polygonChanged, which keeps them through edits where it can).
    const int boundaryCount = _fieldPolygon.count();
    if (!_gateSides.isEmpty() && boundaryCount >= 3 && _gateSideCount != boundaryCount) {
        _gateSides.clear();
    }
    _gateSides.erase(std::remove_if(_gateSides.begin(), _gateSides.end(),
                                    [boundaryCount](int s) { return s < 0 || (boundaryCount >= 3 && s >= boundaryCount); }),
                     _gateSides.end());
    // No entry side when taking off inside the field (the mission doesn't use one).
    const bool takeoffInside = takeoff && spray::pointInPolygon(boundary, homeLL);
    _resolvedGateSides.clear();
    if (takeoff && !takeoffInside) {
        if (!_gateSides.isEmpty()) {
            _resolvedGateSides = _gateSides;
        } else {
            const int nearest = spray::nearestSide(boundary, homeLL);
            if (nearest >= 0) {
                _resolvedGateSides.append(nearest);
            }
        }
    }

    _missionSteps.clear();
    _missionStats = {};
    _transitLegsVariant.clear();
    _transitEditPointsVariant.clear();
    _gateLinesVariant.clear();
    _transitEntryInside = true;

    const bool routeMode = _transitModeFact.rawValue().toInt() == 1;

    if (_result.valid) {
        spray::MissionInput in;
        in.boundary       = boundary;
        in.sprayArea      = _result.sprayArea;
        in.route          = _routeReversed ? _route.reversed() : _route;
        in.sprayAltM      = _altitudeFact.rawValue().toDouble();
        in.spraySpeedMS   = _speedFact.rawValue().toDouble();
        in.transit        = !takeoff ? spray::TransitMode::None
                                     : (routeMode ? spray::TransitMode::Route : spray::TransitMode::Gate);
        in.takeoff        = homeLL;
        in.transitAltM    = _transitAltitudeFact.rawValue().toDouble();
        in.transitSpeedMS = _transitSpeedFact.rawValue().toDouble();
        in.gateSides      = std::vector<int>(_resolvedGateSides.cbegin(), _resolvedGateSides.cend());
        in.transitRoute   = _effectiveTransitRoute();

        _missionSteps = spray::buildMission(in);
        _missionStats = spray::missionStats(_missionSteps);

        if (routeMode && !in.transitRoute.empty()) {
            _transitEntryInside = spray::pointInPolygon(boundary, in.transitRoute.back());
        }
        if (takeoff && routeMode) {
            _transitEditPointsVariant.append(QVariant::fromValue(home));
            for (const auto &p : in.transitRoute) {
                _transitEditPointsVariant.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
            }
        }

        // Legs flown in transit, for the map: from takeoff, and every leg
        // that starts or ends at a transit waypoint.
        QGeoCoordinate prev        = takeoff ? home : QGeoCoordinate();
        bool           prevTransit = true;
        for (const auto &s : _missionSteps) {
            if (s.kind != spray::PlanStep::Waypoint) {
                continue;
            }
            const QGeoCoordinate c(s.pos.lat, s.pos.lon);
            if (prev.isValid() && (prevTransit || s.transit) && prev.distanceTo(c) > 0.05) {
                QVariantMap leg;
                leg[QStringLiteral("start")] = QVariant::fromValue(prev);
                leg[QStringLiteral("end")]   = QVariant::fromValue(c);
                _transitLegsVariant.append(leg);
            }
            prev        = c;
            prevTransit = s.transit;
        }
    }

    if (_result.valid && !routeMode && _fieldPolygon.count() >= 3) {
        const int n = _fieldPolygon.count();
        for (int side : _resolvedGateSides) {
            QVariantList line;
            line.append(QVariant::fromValue(_fieldPolygon.vertexCoordinate(side)));
            line.append(QVariant::fromValue(_fieldPolygon.vertexCoordinate((side + 1) % n)));
            _gateLinesVariant.append(QVariant(line));
        }
    }

    emit missionUpdated();
    emit readyForSaveStateChanged();
    _updateTrips();
}

void SprayAreaComplexItem::_updateTrips()
{
    _trips = {};
    _tripStopsVariant.clear();
    if (_result.valid && _route.pointCount() >= 2) {
        spray::TripInput in;
        in.route          = _routeReversed ? _route.reversed() : _route;
        const QGeoCoordinate home = takeoffPoint();
        in.takeoff        = (hasTakeoff() && home.isValid()) ? spray::LatLon { home.latitude(), home.longitude() }
                                                            : in.route.points().front();
        in.swathM         = _swathWidthFact.rawValue().toDouble();
        in.volumePerM2    = _applicationRateFact.rawValue().toDouble() / 4046.8564224;   // gal/ac -> gal/m2
        in.tankVolume     = _tankCapacityFact.rawValue().toDouble();
        in.batteryS       = _batteryMinutesFact.rawValue().toDouble() * 60.0;
        in.spraySpeedMS   = _speedFact.rawValue().toDouble();
        in.transitSpeedMS = _transitSpeedFact.rawValue().toDouble();
        in.transitAltM    = qMax(_transitAltitudeFact.rawValue().toDouble(), _altitudeFact.rawValue().toDouble());
        _trips = spray::estimateTrips(in);
        for (const spray::LatLon &p : _trips.stops) {
            _tripStopsVariant.append(QVariant::fromValue(QGeoCoordinate(p.lat, p.lon)));
        }
    }
    emit tripsChanged();
}

QString SprayAreaComplexItem::tripsLimitedBy() const
{
    if (!_trips.valid || _trips.trips < 2) {
        return QString();
    }
    return _trips.tankTrips >= _trips.batteryTrips ? QStringLiteral("tank") : QStringLiteral("battery");
}

void SprayAreaComplexItem::_transitEdited(int oldLastSeq)
{
    _rebuildMission();
    setDirty(true);
    _emitMissionChanged(oldLastSeq);
    _noteChange();
}

void SprayAreaComplexItem::dropStartPoint(const QGeoCoordinate &coordinate)
{
    const QGeoCoordinate start = startPoint();
    const QGeoCoordinate end   = endPoint();
    if (!coordinate.isValid() || !start.isValid()) {
        return;
    }
    // Where S can go: the end of the route (fly it the other way round, which
    // keeps route edits), or any end of a block's first or last pass (the
    // passes are laid out again from there). The nearest one wins.
    enum class Choice { Keep, Reverse, Corner };
    Choice        choice = Choice::Keep;
    double        best   = coordinate.distanceTo(start);
    spray::LatLon corner;
    if (end.isValid() && coordinate.distanceTo(end) < best) {
        best   = coordinate.distanceTo(end);
        choice = Choice::Reverse;
    }
    for (const auto &option : _result.startOptions) {
        const QGeoCoordinate c(option.lat, option.lon);
        const double d = coordinate.distanceTo(c);
        if (d < best - 0.01 && c.distanceTo(start) > 0.01 && c.distanceTo(end) > 0.01) {
            best   = d;
            choice = Choice::Corner;
            corner = option;
        }
    }

    switch (choice) {
    case Choice::Reverse: {
        const int oldLastSeq = lastSequenceNumber();
        _routeReversed = !_routeReversed;
        _transitEdited(oldLastSeq);
        break;
    }
    case Choice::Corner:
        _hasStartNear  = true;
        _startNear     = corner;
        _routeReversed = false;
        _regenerate();   // hand edits to the old passes are reset (Undo brings them back)
        setDirty(true);
        _noteChange();
        break;
    case Choice::Keep:
        emit missionUpdated();   // snap the marker back to the start
        break;
    }
}

QVariantList SprayAreaComplexItem::gateSides() const
{
    QVariantList sides;
    for (int s : _resolvedGateSides) {
        sides.append(s);
    }
    return sides;
}

void SprayAreaComplexItem::toggleGateSide(int side)
{
    if (side < 0 || side >= _fieldPolygon.count()) {
        return;
    }
    const int oldLastSeq = lastSequenceNumber();
    if (_gateSides.isEmpty()) {
        // First pick: start from the side in use (the nearest), then toggle.
        _gateSides = _resolvedGateSides;
    }
    if (_gateSides.contains(side)) {
        _gateSides.removeAll(side);   // removing the last one goes back to nearest
    } else {
        _gateSides.append(side);
        std::sort(_gateSides.begin(), _gateSides.end());
    }
    _gateSideCount = _fieldPolygon.count();
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::useNearestGateSide()
{
    if (_gateSides.isEmpty()) {
        return;
    }
    const int oldLastSeq = lastSequenceNumber();
    _gateSides.clear();
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::insertTransitPoint(int segment, const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return;
    }
    // Segment s runs from edit point s to s+1; edit point k is route point k-1.
    if (segment < 0 || segment >= static_cast<int>(_effectiveTransitRoute().size())) {
        return;
    }
    const int oldLastSeq = lastSequenceNumber();
    _makeTransitRouteCustom();
    _transitRoute.insert(_transitRoute.begin() + segment, spray::LatLon { coordinate.latitude(), coordinate.longitude() });
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::moveTransitPoint(int point, const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return;
    }
    if (point < 1 || point > static_cast<int>(_effectiveTransitRoute().size())) {
        return;
    }
    const int oldLastSeq = lastSequenceNumber();
    _makeTransitRouteCustom();
    _transitRoute[static_cast<size_t>(point - 1)] = spray::LatLon { coordinate.latitude(), coordinate.longitude() };
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::removeTransitPoint(int point)
{
    const size_t count = _effectiveTransitRoute().size();
    if (point < 1 || point > static_cast<int>(count) || count < 2) {
        return;   // the entry point always stays
    }
    const int oldLastSeq = lastSequenceNumber();
    _makeTransitRouteCustom();
    _transitRoute.erase(_transitRoute.begin() + (point - 1));
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::resetTransitRoute()
{
    if (!_transitRouteCustom) {
        return;
    }
    const int oldLastSeq = lastSequenceNumber();
    _transitRouteCustom = false;
    _transitRoute.clear();
    _transitEdited(oldLastSeq);
}

void SprayAreaComplexItem::_syncDefaultWaypointAltitude()
{
    Fact *defaultAltitude = SettingsManager::instance()->appSettings()->defaultMissionItemAltitude();
    const double height = qMax(_transitAltitudeFact.rawValue().toDouble(), _altitudeFact.rawValue().toDouble());
    if (std::fabs(defaultAltitude->rawValue().toDouble() - height) > 1e-6) {
        defaultAltitude->setRawValue(height);
    }
}

void SprayAreaComplexItem::_syncEndItem()
{
    // PX4's Return can follow the plan's way out (the DO_LAND_START marker in
    // the mission) only if the mission ends with a landing, not with Return To
    // Launch. So the item that ends a spray plan is a Land at the takeoff point:
    // an older plan's Return To Launch is turned into one, and it moves with
    // the takeoff point.
    if (!_missionController || flyView() || _loading || _restoring || !hasTakeoff()) {
        return;
    }
    QmlObjectListModel *items = _missionController->visualItems();
    bool after = false;
    for (int i = 0; items && i < items->count(); ++i) {
        QObject *object = items->get(i);
        if (object == this) {
            after = true;
            continue;
        }
        auto *simple = qobject_cast<SimpleMissionItem *>(object);
        if (!after || !simple) {
            continue;
        }
        const int command = simple->command();
        if (command != MAV_CMD_NAV_RETURN_TO_LAUNCH && command != MAV_CMD_NAV_LAND) {
            continue;
        }
        const QGeoCoordinate home = takeoffPoint();
        if (command == MAV_CMD_NAV_RETURN_TO_LAUNCH) {
            simple->setMapCenterHintForCommandChange(home);
            simple->setCommand(MAV_CMD_NAV_LAND);
        }
        const QGeoCoordinate at = simple->coordinate();
        if (!at.isValid() || at.distanceTo(home) > 0.1) {
            simple->setCoordinate(QGeoCoordinate(home.latitude(), home.longitude(), at.isValid() ? at.altitude() : 0.0));
        }
        return;
    }
}

void SprayAreaComplexItem::syncTakeoffAltitude()
{
    if (!_missionController) {
        return;
    }
    QmlObjectListModel *items = _missionController->visualItems();
    for (int i = 0; items && i < items->count(); ++i) {
        if (auto *takeoffItem = items->value<TakeoffMissionItem *>(i)) {
            // The mission never transits below spray height.
            takeoffItem->altitude()->setRawValue(qMax(_transitAltitudeFact.rawValue().toDouble(),
                                                      _altitudeFact.rawValue().toDouble()));
            return;
        }
    }
}

/*---------------------------------------------------------------------------*/
// Undo / redo

namespace {

bool sameStrips(const std::vector<spray::Strip> &x, const std::vector<spray::Strip> &y)
{
    if (x.size() != y.size()) {
        return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        const spray::Strip &a = x[i];
        const spray::Strip &b = y[i];
        if (a.a.lat != b.a.lat || a.a.lon != b.a.lon || a.b.lat != b.b.lat || a.b.lon != b.b.lon || a.widthM != b.widthM) {
            return false;
        }
    }
    return true;
}

/// Each strip as the 4 corners of its sprayed rectangle, for the map.
QVariantList stripRectangles(const std::vector<spray::Strip> &strips)
{
    QVariantList rectangles;
    for (const spray::Strip &strip : strips) {
        const QGeoCoordinate a(strip.a.lat, strip.a.lon);
        const QGeoCoordinate b(strip.b.lat, strip.b.lon);
        const double azimuth = a.azimuthTo(b);
        const double half    = strip.widthM / 2.0;
        QVariantList corners;
        corners.append(QVariant::fromValue(a.atDistanceAndAzimuth(half, azimuth - 90.0)));
        corners.append(QVariant::fromValue(b.atDistanceAndAzimuth(half, azimuth - 90.0)));
        corners.append(QVariant::fromValue(b.atDistanceAndAzimuth(half, azimuth + 90.0)));
        corners.append(QVariant::fromValue(a.atDistanceAndAzimuth(half, azimuth + 90.0)));
        rectangles.append(QVariant(corners));
    }
    return rectangles;
}

} // namespace

bool SprayAreaComplexItem::EditState::operator==(const EditState &other) const
{
    if (polygon != other.polygon || facts != other.facts
            || routeSpray != other.routeSpray || sideBuffers != other.sideBuffers
            || routeReversed != other.routeReversed || gateSides != other.gateSides
            || hasStartNear != other.hasStartNear
            || (hasStartNear && (startNear.lat != other.startNear.lat || startNear.lon != other.startNear.lon))
            || transitRouteCustom != other.transitRouteCustom || transitRoute.size() != other.transitRoute.size()
            || routePoints.size() != other.routePoints.size()) {
        return false;
    }
    for (size_t i = 0; i < routePoints.size(); ++i) {
        if (routePoints[i].lat != other.routePoints[i].lat || routePoints[i].lon != other.routePoints[i].lon) {
            return false;
        }
    }
    for (size_t i = 0; i < transitRoute.size(); ++i) {
        if (transitRoute[i].lat != other.transitRoute[i].lat || transitRoute[i].lon != other.transitRoute[i].lon) {
            return false;
        }
    }
    if (hasResumeFrom != other.hasResumeFrom
            || (hasResumeFrom && (resumeFrom.lat != other.resumeFrom.lat || resumeFrom.lon != other.resumeFrom.lon))
            || !sameStrips(sprayed, other.sprayed)) {
        return false;
    }
    if (bpValid != other.bpValid) {
        return false;
    }
    return !bpValid
           || (sameStrips(bpStrips, other.bpStrips) && bpStop.lat == other.bpStop.lat && bpStop.lon == other.bpStop.lon
               && bpReason == other.bpReason && bpTime == other.bpTime);
}

QList<Fact *> SprayAreaComplexItem::_editableFacts()
{
    return { &_swathWidthFact, &_passAngleFact, &_passOffsetFact, &_altitudeFact, &_speedFact,
             &_applicationRateFact, &_edgeMarginFact, &_headlandPassFact,
             &_transitModeFact, &_transitAltitudeFact, &_transitSpeedFact };
}

SprayAreaComplexItem::EditState SprayAreaComplexItem::_captureState()
{
    EditState state;
    state.polygon = _fieldPolygon.coordinateList();
    for (Fact *fact : _editableFacts()) {
        state.facts.append(fact->rawValue());
    }
    state.routePoints = _route.points();
    state.routeSpray  = _route.segmentSpray();
    state.sideBuffers = _sideBuffers;
    state.routeReversed      = _routeReversed;
    state.hasStartNear       = _hasStartNear;
    state.startNear          = _startNear;
    state.gateSides          = _gateSides;
    state.transitRoute       = _transitRoute;
    state.transitRouteCustom = _transitRouteCustom;
    state.sprayed            = _sprayed;
    state.hasResumeFrom      = _hasResumeFrom;
    state.resumeFrom         = _resumeFrom;
    state.bpValid            = _bpValid;
    state.bpStrips           = _bpStrips;
    state.bpStop             = _bpStop;
    state.bpReason           = _bpReason;
    state.bpTime             = _bpTime;
    return state;
}

void SprayAreaComplexItem::_resetUndoHistory()
{
    _undoGroupTimer.stop();
    _changePending = false;
    _undoStack.clear();
    _redoStack.clear();
    _committed = _captureState();
    emit undoRedoChanged();
}

void SprayAreaComplexItem::_noteChange()
{
    if (_restoring) {
        return;
    }
    _undoGroupTimer.start();   // restart: group with any edits that follow quickly
    if (!_changePending) {
        _changePending = true;
        emit undoRedoChanged();
    }
}

void SprayAreaComplexItem::_commitChange()
{
    _undoGroupTimer.stop();
    if (!_changePending) {
        return;
    }
    _changePending = false;

    EditState state = _captureState();
    if (state != _committed) {
        _undoStack.append(_committed);
        while (_undoStack.count() > _undoLimit) {
            _undoStack.removeFirst();
        }
        _redoStack.clear();
        _committed = std::move(state);
    }
    emit undoRedoChanged();
}

void SprayAreaComplexItem::undo()
{
    _commitChange();   // record anything still being grouped first
    if (_undoStack.isEmpty()) {
        return;
    }
    _redoStack.append(_committed);
    _applyState(_undoStack.takeLast());
    emit undoRedoChanged();
}

void SprayAreaComplexItem::redo()
{
    _commitChange();
    if (_redoStack.isEmpty()) {
        return;
    }
    _undoStack.append(_committed);
    _applyState(_redoStack.takeLast());
    emit undoRedoChanged();
}

void SprayAreaComplexItem::_applyState(const EditState &state)
{
    _restoring = true;
    const QVariant transitAltBefore = _transitAltitudeFact.rawValue();

    // The chosen start first, so every regeneration below uses it.
    const bool startChanged = _hasStartNear != state.hasStartNear
                              || _startNear.lat != state.startNear.lat || _startNear.lon != state.startNear.lon;
    _hasStartNear = state.hasStartNear;
    _startNear    = state.startNear;
    EditState sprayedNow;
    sprayedNow.sprayed       = _sprayed;
    sprayedNow.hasResumeFrom = _hasResumeFrom;
    sprayedNow.resumeFrom    = _resumeFrom;
    EditState sprayedThen;
    sprayedThen.sprayed       = state.sprayed;
    sprayedThen.hasResumeFrom = state.hasResumeFrom;
    sprayedThen.resumeFrom    = state.resumeFrom;
    const bool sprayedChangedNow = sprayedNow != sprayedThen;
    _sprayed       = state.sprayed;
    _hasResumeFrom = state.hasResumeFrom;
    _resumeFrom    = state.resumeFrom;
    if (sprayedChangedNow) {
        _rebuildSprayedVariant();
        emit sprayedChanged();
    }
    if (_bpValid != state.bpValid || (state.bpValid && !sameStrips(_bpStrips, state.bpStrips))) {
        _setBreakpoint(state.bpValid, state.bpStrips, state.bpStop, state.bpReason, state.bpTime);
    }

    // Settings and boundary first; each change regenerates the passes.
    const QList<Fact *> facts = _editableFacts();
    for (int i = 0; i < facts.count() && i < state.facts.count(); ++i) {
        if (facts[i]->rawValue() != state.facts[i]) {
            facts[i]->setRawValue(state.facts[i]);
        }
    }
    if (_fieldPolygon.coordinateList() != state.polygon) {
        _fieldPolygon.clear();
        _fieldPolygon.appendVertices(state.polygon);
    }
    if (_sideBuffers != state.sideBuffers) {
        _sideBuffers = state.sideBuffers;
        _sidePolygon = _fieldPolygon.coordinateList();
        _regenerate();
    } else if (startChanged || sprayedChangedNow) {
        _regenerate();
    }

    // Then the route edits on top of the regenerated passes.
    const int oldLastSeq = lastSequenceNumber();
    if (_result.valid && state.routePoints.size() >= 2) {
        _route.load(state.routePoints, state.routeSpray);
    }
    _routeReversed      = state.routeReversed;
    _gateSides          = state.gateSides;
    _gateSideCount      = _fieldPolygon.count();
    _transitRoute       = state.transitRoute;
    _transitRouteCustom = state.transitRouteCustom;
    _editsWereReset = false;
    _rebuildRouteVariants();
    _emitRouteChanged(oldLastSeq);
    setDirty(true);

    _committed = _captureState();
    _restoring = false;

    // Keep the Takeoff item's height in step with an undone/redone transit height.
    if (_transitAltitudeFact.rawValue() != transitAltBefore) {
        syncTakeoffAltitude();
    }
}

void SprayAreaComplexItem::setRouteEditMode(bool enable)
{
    if (_routeEditMode != enable) {
        _routeEditMode = enable;
        emit routeEditModeChanged();
    }
    if (enable) {
        setSideEditMode(false);   // one map edit mode at a time
        setPassAlignMode(false);
        setTransitEditMode(false);
        setBoundaryEditMode(false);
    }
}

void SprayAreaComplexItem::setSideEditMode(bool enable)
{
    if (_sideEditMode != enable) {
        _sideEditMode = enable;
        emit sideEditModeChanged();
    }
    if (enable) {
        setRouteEditMode(false);
        setPassAlignMode(false);
        setTransitEditMode(false);
        setBoundaryEditMode(false);
    }
}

void SprayAreaComplexItem::setPassAlignMode(bool enable)
{
    if (_passAlignMode != enable) {
        _passAlignMode = enable;
        emit passAlignModeChanged();
    }
    if (enable) {
        setRouteEditMode(false);
        setSideEditMode(false);
        setTransitEditMode(false);
        setBoundaryEditMode(false);
    }
}

void SprayAreaComplexItem::setTransitEditMode(bool enable)
{
    if (_transitEditMode != enable) {
        _transitEditMode = enable;
        emit transitEditModeChanged();
    }
    if (enable) {
        setRouteEditMode(false);
        setSideEditMode(false);
        setPassAlignMode(false);
        setBoundaryEditMode(false);
    }
}

void SprayAreaComplexItem::setBoundaryEditMode(bool enable)
{
    if (_boundaryEditMode != enable) {
        _boundaryEditMode = enable;
        emit boundaryEditModeChanged();
    }
    if (enable) {
        setRouteEditMode(false);
        setSideEditMode(false);
        setPassAlignMode(false);
        setTransitEditMode(false);
    }
}

void SprayAreaComplexItem::setFieldName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed != _fieldName) {
        _fieldName = trimmed;
        setDirty(true);
        emit fieldNameChanged();
        emit commandNameChanged();
        emit commandDescriptionChanged();
    }
}

/*---------------------------------------------------------------------------*/
// Side buffers

namespace {

// Distance in metres from p to the segment a-b (flat approximation around a).
double distanceToSegmentM(const QGeoCoordinate &p, const QGeoCoordinate &a, const QGeoCoordinate &b)
{
    constexpr double kMPerDegLat = 111320.0;
    const double mPerDegLon = kMPerDegLat * qCos(qDegreesToRadians(a.latitude()));
    const double bx = (b.longitude() - a.longitude()) * mPerDegLon;
    const double by = (b.latitude()  - a.latitude())  * kMPerDegLat;
    const double px = (p.longitude() - a.longitude()) * mPerDegLon;
    const double py = (p.latitude()  - a.latitude())  * kMPerDegLat;
    const double len2 = bx * bx + by * by;
    double t = len2 > 0.0 ? (px * bx + py * by) / len2 : 0.0;
    t = qBound(0.0, t, 1.0);
    return qSqrt((px - t * bx) * (px - t * bx) + (py - t * by) * (py - t * by));
}

} // namespace

void SprayAreaComplexItem::_polygonChanged()
{
    // Keep the pilot's entry sides through boundary edits: a moved point keeps
    // side numbers, a reversed boundary (QGC makes it clockwise) maps side k
    // to n-2-k, and adding/removing points falls back to the nearest side.
    // Passing states under 3 points (QGC's clear() + append) are ignored.
    const QList<QGeoCoordinate> now = _fieldPolygon.coordinateList();
    if (!_gateSides.isEmpty() && now.count() >= 3) {
        const int n = now.count();
        if (n == _sidePolygon.count()) {
            bool reversed = true;
            for (int k = 0; k < n && reversed; ++k) {
                reversed = now[k].distanceTo(_sidePolygon[n - 1 - k]) < 0.01;
            }
            if (reversed) {
                for (int &s : _gateSides) {
                    s = (2 * n - 2 - s) % n;
                }
                std::sort(_gateSides.begin(), _gateSides.end());
            }
        } else {
            _gateSides.clear();
        }
        _gateSideCount = n;
    }
    _syncSideBuffers();
    _regenerate();

    // No boundary (cleared, or being drawn): the boundary tools must be available.
    // Undo and loading rebuild the boundary in steps, so leave the mode alone then.
    if (now.count() < 3 && !_restoring && !_loading && isCurrentItem()) {
        setBoundaryEditMode(true);
    }
}

void SprayAreaComplexItem::_syncSideBuffers()
{
    // Keep each side's buffer with its side when the boundary is edited.
    // Same number of points (a point was dragged): sides keep their numbers.
    // Points added or removed: a new side inherits the buffer of the old side
    // it lies along (adding a point splits a side; both halves keep its buffer).
    //
    // QGC rebuilds the boundary with clear() + appendVertices() in places (for
    // example to make it clockwise after a drag), so a boundary of fewer than
    // 3 points is a passing state: keep everything until the real one arrives.
    const QList<QGeoCoordinate> now = _fieldPolygon.coordinateList();
    if (now.count() < 3) {
        return;
    }
    QList<double> buffers(now.count(), -1.0);
    const bool anyCustom = std::any_of(_sideBuffers.cbegin(), _sideBuffers.cend(), [](double b) { return b >= 0.0; });

    if (now.count() == _sideBuffers.count() && now.count() == _sidePolygon.count()) {
        const int n = now.count();
        bool reversed = true;
        for (int k = 0; k < n && reversed; ++k) {
            reversed = now[k].distanceTo(_sidePolygon[n - 1 - k]) < 0.01;
        }
        if (reversed) {
            // Same boundary, opposite direction: new side k is old side n-2-k.
            for (int k = 0; k < n; ++k) {
                buffers[k] = _sideBuffers[(2 * n - 2 - k) % n];
            }
        } else {
            buffers = _sideBuffers;   // a point was moved; sides keep their numbers
        }
    } else if (now.count() == _sideBuffers.count()) {
        buffers = _sideBuffers;
    } else if (anyCustom && _sidePolygon.count() == _sideBuffers.count() && _sidePolygon.count() >= 3) {
        const int oldCount = _sidePolygon.count();
        for (int j = 0; j < now.count(); ++j) {
            const QGeoCoordinate &a = now[j];
            const QGeoCoordinate &b = now[(j + 1) % now.count()];
            for (int i = 0; i < oldCount; ++i) {
                const QGeoCoordinate &oa = _sidePolygon[i];
                const QGeoCoordinate &ob = _sidePolygon[(i + 1) % oldCount];
                const double tolerance = qMin(0.5, 0.1 * a.distanceTo(b));
                if (distanceToSegmentM(a, oa, ob) < tolerance && distanceToSegmentM(b, oa, ob) < tolerance) {
                    buffers[j] = _sideBuffers[i];
                    break;
                }
            }
        }
    }
    _sideBuffers = buffers;
    _sidePolygon = now;
}

void SprayAreaComplexItem::_rebuildSideVariants()
{
    const double uniform = _edgeMarginFact.rawValue().toDouble();
    const int    n       = _fieldPolygon.count();
    const QVariantList previous = _sidesVariant;
    _sidesVariant.clear();
    if (n < 3) {
        if (previous != _sidesVariant) {
            emit sidesChanged();
        }
        return;
    }
    const std::vector<double> bearings = spray::sideBearingsDeg(_boundary());
    for (int i = 0; i < n; ++i) {
        const QGeoCoordinate a = _fieldPolygon.vertexCoordinate(i);
        const QGeoCoordinate b = _fieldPolygon.vertexCoordinate((i + 1) % n);
        const bool custom = i < _sideBuffers.size() && _sideBuffers[i] >= 0.0;
        const double bearing = static_cast<size_t>(i) < bearings.size() ? bearings[static_cast<size_t>(i)] : qQNaN();
        QVariantMap side;
        side[QStringLiteral("side")]   = i;
        side[QStringLiteral("number")] = i + 1;
        side[QStringLiteral("start")]  = QVariant::fromValue(a);
        side[QStringLiteral("end")]    = QVariant::fromValue(b);
        side[QStringLiteral("mid")]    = QVariant::fromValue(a.atDistanceAndAzimuth(a.distanceTo(b) / 2.0, a.azimuthTo(b)));
        side[QStringLiteral("custom")] = custom;
        side[QStringLiteral("buffer")] = custom ? _sideBuffers[i] : uniform;
        side[QStringLiteral("bearing")] = std::isnan(bearing) ? -1.0 : bearing;   ///< -1 = zero length
        _sidesVariant.append(side);
    }
    // Only when something changed, so the side list in the panel isn't rebuilt needlessly.
    if (previous != _sidesVariant) {
        emit sidesChanged();
    }
}

int SprayAreaComplexItem::customSideCount() const
{
    return static_cast<int>(std::count_if(_sideBuffers.cbegin(), _sideBuffers.cend(), [](double b) { return b >= 0.0; }));
}

void SprayAreaComplexItem::_sideBuffersEdited()
{
    _regenerate();
    setDirty(true);
    _noteChange();
}

void SprayAreaComplexItem::toggleSide(int side)
{
    if (side < 0 || side >= _sideBuffers.size()) {
        return;
    }
    if (_sideBuffers[side] >= 0.0) {
        _sideBuffers[side] = -1.0;
    } else {
        _sideBuffers[side] = qBound(kMinBufferM, _edgeMarginFact.rawValue().toDouble(), kMaxBufferM);
    }
    _sideBuffersEdited();
}

void SprayAreaComplexItem::setSideBuffer(int side, double meters)
{
    if (side < 0 || side >= _sideBuffers.size() || qIsNaN(meters)) {
        return;
    }
    const double value = qBound(kMinBufferM, meters, kMaxBufferM);
    if (qAbs(_sideBuffers[side] - value) < 1e-6) {
        emit sidesChanged();   // let a clamped entry in the panel snap back
        return;
    }
    _sideBuffers[side] = value;
    _sideBuffersEdited();
}

void SprayAreaComplexItem::clearSideBuffer(int side)
{
    if (side < 0 || side >= _sideBuffers.size() || _sideBuffers[side] < 0.0) {
        return;
    }
    _sideBuffers[side] = -1.0;
    _sideBuffersEdited();
}

void SprayAreaComplexItem::alignPassesToSide(int side)
{
    const std::vector<double> bearings = spray::sideBearingsDeg(_boundary());
    if (side < 0 || static_cast<size_t>(side) >= bearings.size() || std::isnan(bearings[static_cast<size_t>(side)])) {
        return;
    }
    // A side runs both ways; take the direction nearer the current angle so
    // the route keeps starting from the same end of the field.
    const double current = _passAngleFact.rawValue().toDouble();
    const auto apart = [](double a, double b) {
        const double d = std::fmod(std::fabs(a - b), 360.0);
        return d > 180.0 ? 360.0 - d : d;
    };
    const double forward = bearings[static_cast<size_t>(side)];
    const double back    = std::fmod(forward + 180.0, 360.0);
    double angle = apart(forward, current) <= apart(back, current) ? forward : back;
    const double maxAngle = _passAngleFact.rawMax().toDouble();
    if (angle > maxAngle) {
        angle -= 180.0;   // e.g. 359.95: the same line, and still exactly parallel
    }
    if (std::fabs(angle - current) > 1e-9) {
        _passAngleFact.setRawValue(angle);
    }
}

void SprayAreaComplexItem::toggleSegment(int segment)
{
    const int oldLastSeq = lastSequenceNumber();
    if (_route.toggleSegment(segment)) {
        _routeEdited(oldLastSeq);
    }
}

int SprayAreaComplexItem::insertRoutePoint(int segment, const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return -1;
    }
    const int oldLastSeq = lastSequenceNumber();
    const int point = _route.insertPoint(segment, { coordinate.latitude(), coordinate.longitude() });
    if (point >= 0) {
        _routeEdited(oldLastSeq);
    }
    return point;
}

void SprayAreaComplexItem::moveRoutePoint(int point, const QGeoCoordinate &coordinate)
{
    const int oldLastSeq = lastSequenceNumber();
    if (coordinate.isValid() && _route.movePoint(point, { coordinate.latitude(), coordinate.longitude() })) {
        _routeEdited(oldLastSeq);
    }
}

void SprayAreaComplexItem::removeRoutePoint(int point)
{
    const int oldLastSeq = lastSequenceNumber();
    if (_route.removePoint(point)) {
        _routeEdited(oldLastSeq);
    }
}

void SprayAreaComplexItem::removeSegment(int segment)
{
    const int oldLastSeq = lastSequenceNumber();
    if (_route.removeSegment(segment)) {
        _routeEdited(oldLastSeq);
    }
}

void SprayAreaComplexItem::resetRouteEdits()
{
    const int oldLastSeq = lastSequenceNumber();
    _route.reset(_result.flightPath, _result.legSpray);
    _routeEdited(oldLastSeq);
}

/*---------------------------------------------------------------------------*/
// Resuming a job

void SprayAreaComplexItem::_trackVehicle(Vehicle *vehicle)
{
    for (const QMetaObject::Connection &connection : std::as_const(_trackConnections)) {
        QObject::disconnect(connection);
    }
    _trackConnections.clear();
    _trackedVehicle = vehicle;
    _trackIndex     = -1;
    _trackPos       = QGeoCoordinate();
    _trackInMission = false;
    if (!vehicle || !vehicle->missionManager()) {
        return;
    }

    // While the drone flies the mission, remember where it is; when it leaves
    // Mission mode (Hold, Return, a failsafe) the last fix is where it stopped,
    // and that's saved as a breakpoint to resume from.
    auto update = [this]() {
        Vehicle *v = _trackedVehicle.data();
        if (!v || !v->missionManager()) {
            return;
        }
        const bool wasInMission = _trackInMission;
        _trackInMission = v->flightMode() == v->missionFlightMode();
        if (_trackInMission && v->coordinate().isValid()) {
            _trackIndex = v->missionManager()->currentIndex();
            _trackPos   = v->coordinate();
        }
        if (wasInMission && !_trackInMission && v->flying() && !_returnUploading) {
            _recordBreakpoint(v, v->flightMode());
        } else if (!wasInMission && _trackInMission && _bpValid && _droneFliesThisPlan(v)) {
            discardBreakpoint();   // carrying on with the same mission: nothing to resume
        }
    };
    _trackConnections << connect(vehicle, &Vehicle::coordinateChanged, this, update);
    _trackConnections << connect(vehicle, &Vehicle::flightModeChanged, this, update);
    update();

    // Each upload of the plan sets the drone's Return to suit its transit mode.
    _trackConnections << connect(vehicle->missionManager(), &PlanManager::sendComplete, this, [this](bool error) {
        if (!error && !_returnUploading) {
            _applyReturnSettings(_trackedVehicle.data());
        }
    });
}

void SprayAreaComplexItem::_applyReturnSettings(Vehicle *vehicle)
{
    if (!vehicle || !vehicle->px4Firmware() || !_result.valid) {
        return;
    }
    ParameterManager *params = vehicle->parameterManager();
    if (!params || !params->parametersReady()) {
        return;
    }
    const int component = ParameterManager::defaultComponentId;
    bool      changed   = false;

    // The drone's own Return (failsafes, or Return without SprayGCS) is PX4's
    // mission-landing Return (RTL_TYPE 1): straight to the waypoint after the
    // plan's DO_LAND_START (mode A: the exit on the entry side; mode B: the
    // entry point), then the plan's way home, at transit height.
    const QString typeName = QStringLiteral("RTL_TYPE");
    if (params->parameterExists(component, typeName)) {
        Fact *type = params->getParameter(component, typeName);
        if (type && type->rawValue().toInt() != 1) {
            type->setRawValue(1);
            changed = true;
        }
    }
    const QString altName = QStringLiteral("RTL_RETURN_ALT");
    if (params->parameterExists(component, altName)) {
        Fact *alt = params->getParameter(component, altName);
        const double wanted = qMax(_transitAltitudeFact.rawValue().toDouble(), _altitudeFact.rawValue().toDouble());
        if (alt && qAbs(alt->rawValue().toDouble() - wanted) > 0.05) {
            alt->setRawValue(wanted);
            changed = true;
        }
    }
    if (changed) {
        QGC::showAppMessage(tr("The drone's Return is set for this plan: it leaves the field the planned way at transit height, then lands at the takeoff point."));
    }
}

QString SprayAreaComplexItem::markSprayedFromDrone()
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle || !vehicle->missionManager()) {
        return tr("No drone is connected.");
    }
    if (vehicle->flightMode() == vehicle->missionFlightMode()) {
        return tr("The drone is still flying the mission. Pause it first (Hold), then mark what's sprayed.");
    }
    if (_bpValid) {
        resumeFromBreakpoint();   // the drone's mission may be the way back by now
        return QString();
    }
    return _markSprayed(vehicle);
}

bool SprayAreaComplexItem::_sprayedSoFar(Vehicle *vehicle, std::vector<spray::Strip> &strips,
                                         spray::LatLon &stopPoint, QString &error)
{
    strips.clear();
    if (!vehicle || !vehicle->missionManager()) {
        error = tr("No drone is connected.");
        return false;
    }
    if (_route.pointCount() < 2) {
        error = tr("This Spray Area has no route to compare with.");
        return false;
    }

    MissionManager            *manager = vehicle->missionManager();
    const QList<MissionItem *> &items  = manager->missionItems();
    // The mission item it was flying to while in Mission mode (QGC keeps it
    // through a Return's jump to the landing sequence).
    int current = manager->lastCurrentIndex();
    if (current < 0) {
        current = manager->currentIndex();
    }
    if (items.isEmpty() || current < 0) {
        error = tr("The drone hasn't reported any progress on its mission yet.");
        return false;
    }

    // The route as flown, and where each of its points is in the drone's mission.
    const spray::SprayRoute     flown  = _routeReversed ? _route.reversed() : _route;
    const auto                 &points = flown.points();
    const auto                 &legSpray = flown.segmentSpray();
    auto matches = [&](const MissionItem *item, const spray::LatLon &p) {
        return item->command() == MAV_CMD_NAV_WAYPOINT
               && item->coordinate().distanceTo(QGeoCoordinate(p.lat, p.lon)) < 0.5;
    };
    std::vector<int> seqOf(points.size(), -1);
    int from = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        for (int j = from; j < items.count(); ++j) {
            if (!matches(items[j], points[i])) {
                continue;
            }
            int found = j;
            if (i == 0) {
                // The start can be there twice (arriving at transit height, then
                // at spray height): the route starts at the second.
                for (int n = j + 1; n < items.count(); ++n) {
                    if (items[n]->command() != MAV_CMD_NAV_WAYPOINT) {
                        continue;
                    }
                    if (!matches(items[n], points[i])) {
                        break;
                    }
                    found = n;
                }
            }
            seqOf[i] = items[found]->sequenceNumber();
            from     = found + 1;
            break;
        }
        if (seqOf[i] < 0) {
            error = tr("The drone's mission isn't this plan. Open the plan the drone is flying, or upload this one first.");
            return false;
        }
    }

    if (current > seqOf.back()) {
        error = tr("The drone had finished the spray route.");
        return false;
    }

    // Legs done: those ending before the waypoint it was flying to. The leg it
    // was on counts up to where it stopped.
    const QGeoCoordinate stoppedAt = (_trackedVehicle == vehicle && _trackPos.isValid()) ? _trackPos : vehicle->coordinate();
    const double         width     = _swathWidthFact.rawValue().toDouble();
    stopPoint = points.front();
    for (size_t i = 1; i < points.size(); ++i) {
        if (seqOf[i] < current) {
            stopPoint = points[i];
            if (legSpray[i - 1]) {
                strips.push_back({ points[i - 1], points[i], width });
            }
        } else if (seqOf[i - 1] < current) {
            const QGeoCoordinate a(points[i - 1].lat, points[i - 1].lon);
            const QGeoCoordinate b(points[i].lat, points[i].lon);
            const double length = a.distanceTo(b);
            if (stoppedAt.isValid() && length > 0.01) {
                const double azimuth = a.azimuthTo(b);
                const double along   = a.distanceTo(stoppedAt) * qCos(qDegreesToRadians(a.azimuthTo(stoppedAt) - azimuth));
                const double t       = qBound(0.0, along / length, 1.0);
                const QGeoCoordinate p = a.atDistanceAndAzimuth(t * length, azimuth);
                stopPoint = { p.latitude(), p.longitude() };
                if (legSpray[i - 1] && t * length > 0.5 && stoppedAt.distanceTo(p) < 30.0) {
                    strips.push_back({ points[i - 1], stopPoint, width });
                }
            }
            break;
        } else {
            break;
        }
    }
    if (strips.empty()) {
        error = tr("Nothing has been sprayed on this mission yet.");
        return false;
    }
    return true;
}

QString SprayAreaComplexItem::_markSprayed(Vehicle *vehicle)
{
    std::vector<spray::Strip> strips;
    spray::LatLon             stopPoint;
    QString                   error;
    if (!_sprayedSoFar(vehicle, strips, stopPoint, error)) {
        return error;
    }
    _applySprayed(strips, stopPoint, vehicle);
    return QString();
}

void SprayAreaComplexItem::_applySprayed(const std::vector<spray::Strip> &strips, const spray::LatLon &stopPoint, Vehicle *vehicle)
{
    _sprayed.insert(_sprayed.end(), strips.begin(), strips.end());
    const QGeoCoordinate here = vehicle ? vehicle->coordinate() : QGeoCoordinate();
    if (vehicle && vehicle->flying() && here.isValid()) {
        // In the air: carry on from where it's waiting.
        _hasResumeFrom = true;
        _resumeFrom    = { here.latitude(), here.longitude() };
    } else {
        // Landed (e.g. to refill): take off again as planned, and start near where it stopped.
        _hasResumeFrom = false;
        _hasStartNear  = true;
        _startNear     = stopPoint;
    }
    _routeReversed = false;
    _regenerate();
    _rebuildSprayedVariant();
    emit sprayedChanged();
    setDirty(true);
    _noteChange();

    // After this plan is uploaded, the drone continues from the route's first
    // waypoint (where it's waiting) instead of the start of the mission.
    QObject::disconnect(_sendCompleteConnection);
    if (_hasResumeFrom) {
        QPointer<Vehicle> target = vehicle;
        _sendCompleteConnection = connect(vehicle->missionManager(), &PlanManager::sendComplete, this, [this, target](bool error) {
            if (_returnUploading) {
                return;   // that was SprayGCS's Return route, not this plan
            }
            QObject::disconnect(_sendCompleteConnection);
            if (error || !target || !_hasResumeFrom || !target->flying()) {
                return;   // on the ground the mission starts with the takeoff as usual
            }
            const int seq = _resumeStartSeq();
            if (seq > 0) {
                target->setCurrentMissionSequence(seq);
                QGC::showAppMessage(tr("Plan uploaded. The drone will carry on from where it's waiting: switch it to Mission to continue spraying."));
            }
        });
    }
}

bool SprayAreaComplexItem::_droneFliesThisPlan(Vehicle *vehicle) const
{
    if (!vehicle || !vehicle->missionManager()) {
        return false;
    }
    const spray::SprayRoute flown = _routeReversed ? _route.reversed() : _route;
    if (flown.pointCount() < 2) {
        return false;
    }
    const QList<MissionItem *> &items = vehicle->missionManager()->missionItems();
    auto inMission = [&items](const spray::LatLon &p) {
        const QGeoCoordinate point(p.lat, p.lon);
        return std::any_of(items.cbegin(), items.cend(), [&point](const MissionItem *item) {
            return item->command() == MAV_CMD_NAV_WAYPOINT && item->coordinate().distanceTo(point) < 0.5;
        });
    };
    return inMission(flown.points().front()) && inMission(flown.points().back());
}

void SprayAreaComplexItem::_recordBreakpoint(Vehicle *vehicle, const QString &reason)
{
    // A Return from SprayGCS pauses first, so the drone reports Hold right after.
    const QDateTime now = QDateTime::currentDateTime();
    if (_bpValid && _bpTime.isValid() && _bpTime.secsTo(now) < 5) {
        return;
    }
    std::vector<spray::Strip> strips;
    spray::LatLon             stop;
    QString                   error;
    if (!_sprayedSoFar(vehicle, strips, stop, error)) {
        qCDebug(SprayAreaLog) << "No breakpoint:" << error;
        return;
    }
    _setBreakpoint(true, strips, stop, reason, now);
    setDirty(true);
    _noteChange();
    if (PlanMasterController *master = masterController()) {
        master->saveJobPlan();
    }
    QGC::showAppMessage(tr("Breakpoint saved where spraying stopped (%1). To finish the job later, open Plan and tap Resume From Breakpoint.").arg(reason));
}

void SprayAreaComplexItem::_setBreakpoint(bool valid, const std::vector<spray::Strip> &strips, const spray::LatLon &stop,
                                          const QString &reason, const QDateTime &time)
{
    _bpValid         = valid;
    _bpStrips        = valid ? strips : std::vector<spray::Strip>();
    _bpStop          = valid ? stop : spray::LatLon {};
    _bpReason        = valid ? reason : QString();
    _bpTime          = valid ? time : QDateTime();
    _bpStripsVariant = stripRectangles(_bpStrips);
    emit breakpointChanged();
}

QString SprayAreaComplexItem::breakpointText() const
{
    if (!_bpValid) {
        return QString();
    }
    const QString when = _bpTime.isValid() ? QLocale().toString(_bpTime, QLocale::ShortFormat) : QString();
    return _bpReason.isEmpty() ? tr("Spraying stopped %1").arg(when)
                               : tr("Spraying stopped %1 (%2)").arg(when, _bpReason);
}

void SprayAreaComplexItem::resumeFromBreakpoint()
{
    if (!_bpValid) {
        return;
    }
    const std::vector<spray::Strip> strips = _bpStrips;
    const spray::LatLon             stop   = _bpStop;
    _setBreakpoint(false, {}, {}, QString(), QDateTime());
    _applySprayed(strips, stop, MultiVehicleManager::instance()->activeVehicle());
}

void SprayAreaComplexItem::discardBreakpoint()
{
    if (!_bpValid) {
        return;
    }
    _setBreakpoint(false, {}, {}, QString(), QDateTime());
    setDirty(true);
    _noteChange();
}

QPointer<SprayAreaComplexItem> SprayAreaComplexItem::s_planViewItem;

SprayAreaComplexItem *SprayAreaComplexItem::planViewItem()
{
    return s_planViewItem.data();
}

QString SprayAreaComplexItem::returnViaEntrySide()
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle || !vehicle->missionManager() || !vehicle->px4Firmware()) {
        return tr("No PX4 drone is connected.");
    }
    if (!vehicle->flying()) {
        return tr("The drone isn't flying.");
    }
    if (!_result.valid || !hasTakeoff() || _transitModeFact.rawValue().toInt() == 1) {
        return tr("Not a mode A plan: the drone's own Return is used.");
    }
    if (vehicle->missionManager()->inProgress()) {
        return tr("A mission transfer is in progress.");
    }
    const QGeoCoordinate here = vehicle->coordinate();
    if (!here.isValid()) {
        return tr("The drone's position isn't known.");
    }

    if (!_droneFliesThisPlan(vehicle)) {
        return tr("The drone isn't flying this plan: its own Return is used.");
    }

    // Stop where it is, and save a breakpoint (for resuming later).
    vehicle->pauseVehicle();
    _recordBreakpoint(vehicle, tr("Return"));

    // The way back, mirroring the way in: inside the field to the point of the
    // entry side(s) best for here, across it, straight to takeoff, land.
    const std::vector<spray::LatLon> boundary   = _boundary();
    const QGeoCoordinate             home       = takeoffPoint();
    const spray::LatLon              homeLL     { home.latitude(), home.longitude() };
    const spray::LatLon              hereLL     { here.latitude(), here.longitude() };
    const double                     transitAlt = qMax(_transitAltitudeFact.rawValue().toDouble(), _altitudeFact.rawValue().toDouble());
    const std::vector<int>           allowed(_resolvedGateSides.cbegin(), _resolvedGateSides.cend());
    // Ends at takeoff; outside the field (e.g. still in transit) it's straight there.
    const std::vector<spray::LatLon> path = spray::gateReturnPath(boundary, allowed, homeLL, hereLL);

    QObject *owner = vehicle->missionManager();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QList<MissionItem *> items;
    int seq = 0;
    auto waypoint = [&](const spray::LatLon &p, double alt) {
        items.append(new MissionItem(seq++, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT,
                                     0, 0, 0, nan, p.lat, p.lon, alt, true, false, owner));
    };
    waypoint(homeLL, 0);   // home (not sent to PX4)
    // A failsafe Return during this flight carries on along it.
    items.append(new MissionItem(seq++, MAV_CMD_DO_LAND_START, MAV_FRAME_MISSION, 0, 0, 0, 0, 0, 0, 0, true, false, owner));
    items.append(new MissionItem(seq++, MAV_CMD_DO_SET_ACTUATOR, MAV_FRAME_MISSION,
                                 0.0, nan, nan, nan, nan, nan, 0, true, false, owner));   // pump off
    items.append(new MissionItem(seq++, MAV_CMD_DO_CHANGE_SPEED, MAV_FRAME_MISSION,
                                 1, _transitSpeedFact.rawValue().toDouble(), -1, 0, 0, 0, 0, true, false, owner));
    waypoint(hereLL, transitAlt);   // climb where it is
    for (const auto &p : path) {
        waypoint(p, transitAlt);    // ... the last one over takeoff
    }
    items.append(new MissionItem(seq++, MAV_CMD_NAV_LAND, MAV_FRAME_GLOBAL_RELATIVE_ALT,
                                 0, 0, 0, nan, homeLL.lat, homeLL.lon, 0, true, false, owner));

    QPointer<Vehicle> target = vehicle;
    _returnUploading = true;
    QObject::disconnect(_returnConnection);
    _returnConnection = connect(vehicle->missionManager(), &PlanManager::sendComplete, this, [this, target](bool error) {
        QObject::disconnect(_returnConnection);
        // Other upload handlers run first and skip this one; clear the flag after them.
        QMetaObject::invokeMethod(this, [this]() { _returnUploading = false; }, Qt::QueuedConnection);
        if (!target) {
            return;
        }
        if (error) {
            target->guidedModeRTL(false);
            QGC::showAppMessage(tr("Couldn't send the way back; the drone is using its own Return."));
            return;
        }
        target->setCurrentMissionSequence(1);
        target->setFlightMode(target->missionFlightMode());
        QGC::showAppMessage(tr("Returning through the entry side, then straight to takeoff. A breakpoint is saved where spraying stopped, so the job can be resumed."));
        if (PlanMasterController *master = masterController()) {
            master->saveJobPlan();   // the drone's mission is now the way back: keep the job for after a restart
        }
    });
    vehicle->missionManager()->writeMissionItems(items);
    return QString();
}

void SprayAreaComplexItem::clearSprayed()
{
    if (_sprayed.empty() && !_hasResumeFrom) {
        return;
    }
    _sprayed.clear();
    _hasResumeFrom = false;
    QObject::disconnect(_sendCompleteConnection);
    _regenerate();
    _rebuildSprayedVariant();
    emit sprayedChanged();
    setDirty(true);
    _noteChange();
}

void SprayAreaComplexItem::_rebuildSprayedVariant()
{
    _sprayedStripsVariant = stripRectangles(_sprayed);
}

int SprayAreaComplexItem::_resumeStartSeq() const
{
    // One mission item per step; the route's first waypoint is the first one
    // that isn't transit.
    for (size_t i = 0; i < _missionSteps.size(); ++i) {
        const spray::PlanStep &step = _missionSteps[i];
        if (step.kind == spray::PlanStep::Waypoint && !step.transit) {
            return _sequenceNumber + static_cast<int>(i);
        }
    }
    return -1;
}

/*---------------------------------------------------------------------------*/

double SprayAreaComplexItem::sprayedAcresEst() const
{
    // Sprayed length x swath, capped at the spray area (passes overlap slightly
    // at the headland and in corners).
    const double sqM = _route.sprayLengthM() * _swathWidthFact.rawValue().toDouble();
    return qMin(sqM, _result.sprayAreaM2) / 4046.8564224;
}

double SprayAreaComplexItem::estimatedVolume() const
{
    // Application rate is in gallons per acre.
    return sprayedAcresEst() * _applicationRateFact.rawValue().toDouble();
}

double SprayAreaComplexItem::estimatedMinutes() const
{
    // Whole item: transit in, spray route, transit out, plus the climb and
    // the leg from the takeoff point to the first waypoint (flown before
    // this item's first mission item).
    double seconds = _missionStats.seconds;
    if (hasTakeoff()) {
        for (const auto &s : _missionSteps) {
            if (s.kind == spray::PlanStep::Waypoint) {
                const QGeoCoordinate home  = takeoffPoint();
                const double         speed = qMax(0.1, _transitSpeedFact.rawValue().toDouble());
                seconds += home.distanceTo(QGeoCoordinate(s.pos.lat, s.pos.lon)) / speed + s.altM / 2.0;
                break;
            }
        }
    }
    return seconds / 60.0;
}

namespace {
const spray::PlanStep *firstWaypoint(const std::vector<spray::PlanStep> &steps)
{
    for (const auto &s : steps) {
        if (s.kind == spray::PlanStep::Waypoint) {
            return &s;
        }
    }
    return nullptr;
}
const spray::PlanStep *lastWaypoint(const std::vector<spray::PlanStep> &steps)
{
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (it->kind == spray::PlanStep::Waypoint) {
            return &*it;
        }
    }
    return nullptr;
}
} // namespace

QGeoCoordinate SprayAreaComplexItem::coordinate() const
{
    if (const spray::PlanStep *w = firstWaypoint(_missionSteps)) {
        return QGeoCoordinate(w->pos.lat, w->pos.lon);
    }
    if (_fieldPolygon.count() > 0) {
        return _fieldPolygon.vertexCoordinate(0);
    }
    return {};
}

QGeoCoordinate SprayAreaComplexItem::exitCoordinate() const
{
    if (const spray::PlanStep *w = lastWaypoint(_missionSteps)) {
        return QGeoCoordinate(w->pos.lat, w->pos.lon);
    }
    return coordinate();
}

double SprayAreaComplexItem::amslEntryAlt() const
{
    const spray::PlanStep *w = firstWaypoint(_missionSteps);
    return (w ? w->altM : _altitudeFact.rawValue().toDouble())
           + _missionController->plannedHomePosition().altitude();
}

double SprayAreaComplexItem::amslExitAlt() const
{
    const spray::PlanStep *w = lastWaypoint(_missionSteps);
    return (w ? w->altM : _altitudeFact.rawValue().toDouble())
           + _missionController->plannedHomePosition().altitude();
}

double SprayAreaComplexItem::minAMSLAltitude() const
{
    double alt = _altitudeFact.rawValue().toDouble();
    for (const auto &s : _missionSteps) {
        if (s.kind == spray::PlanStep::Waypoint) {
            alt = qMin(alt, s.altM);
        }
    }
    return alt + _missionController->plannedHomePosition().altitude();
}

double SprayAreaComplexItem::maxAMSLAltitude() const
{
    double alt = _altitudeFact.rawValue().toDouble();
    for (const auto &s : _missionSteps) {
        if (s.kind == spray::PlanStep::Waypoint) {
            alt = qMax(alt, s.altM);
        }
    }
    return alt + _missionController->plannedHomePosition().altitude();
}

double SprayAreaComplexItem::specifiedFlightSpeed()
{
    // The speed in force after this item: the last speed change in it.
    for (auto it = _missionSteps.rbegin(); it != _missionSteps.rend(); ++it) {
        if (it->kind == spray::PlanStep::Speed) {
            return it->speedMS;
        }
    }
    return _speedFact.rawValue().toDouble();
}

int SprayAreaComplexItem::lastSequenceNumber() const
{
    if (!_result.valid || _missionSteps.empty()) {
        return _sequenceNumber;
    }
    // One mission item per step (speed changes, waypoints, spray markers).
    return _sequenceNumber + qMax(0, static_cast<int>(_missionSteps.size()) - 1);
}

VisualMissionItem::ReadyForSaveState SprayAreaComplexItem::readyForSaveState() const
{
    if (!_result.valid) {
        return NotReadyForSaveData;
    }
    // Mode B must enter the field: its last point is where the drone descends
    // to spray height.
    if (hasTakeoff() && _transitModeFact.rawValue().toInt() == 1 && !_transitEntryInside) {
        return NotReadyForSaveData;
    }
    return ReadyForSave;
}

double SprayAreaComplexItem::greatestDistanceTo(const QGeoCoordinate &other) const
{
    double maxDist = 0;
    for (int i = 0; i < _fieldPolygon.count(); ++i) {
        maxDist = qMax(maxDist, _fieldPolygon.vertexCoordinate(i).distanceTo(other));
    }
    // Edited route points and transit legs can be outside the boundary.
    for (const auto &p : _route.points()) {
        maxDist = qMax(maxDist, QGeoCoordinate(p.lat, p.lon).distanceTo(other));
    }
    for (const auto &s : _missionSteps) {
        if (s.kind == spray::PlanStep::Waypoint) {
            maxDist = qMax(maxDist, QGeoCoordinate(s.pos.lat, s.pos.lon).distanceTo(other));
        }
    }
    return maxDist;
}

/*---------------------------------------------------------------------------*/

void SprayAreaComplexItem::setSequenceNumber(int sequenceNumber)
{
    if (_sequenceNumber != sequenceNumber) {
        _sequenceNumber = sequenceNumber;
        emit sequenceNumberChanged(sequenceNumber);
        emit lastSequenceNumberChanged(lastSequenceNumber());
    }
}

void SprayAreaComplexItem::setCoordinate(const QGeoCoordinate & /* coord */)
{
    // Complex items are moved by editing the polygon, not a single coordinate.
}

void SprayAreaComplexItem::applyNewAltitude(double newAltitude)
{
    _altitudeFact.setRawValue(newAltitude);
}

void SprayAreaComplexItem::setMissionFlightStatus(MissionFlightStatus_t &missionFlightStatus)
{
    ComplexMissionItem::setMissionFlightStatus(missionFlightStatus);
}

/*---------------------------------------------------------------------------*/

void SprayAreaComplexItem::appendMissionItems(QList<MissionItem *> &items,
                                              QObject *missionItemParent)
{
    if (!_result.valid) {
        return;
    }

    int          seqNum = _sequenceNumber;
    const double nan    = std::numeric_limits<double>::quiet_NaN();

    // Built by _rebuildMission(): transit in, spray route, transit out.
    for (const auto &step : _missionSteps) {
        switch (step.kind) {
        case spray::PlanStep::Speed:
            items.append(new MissionItem(
                seqNum++,
                MAV_CMD_DO_CHANGE_SPEED,
                MAV_FRAME_MISSION,
                1,              // speed type: ground speed
                step.speedMS,   // m/s
                -1,             // throttle: no change
                0,              // absolute speed
                0, 0, 0,
                true,   // autoContinue
                false,  // isCurrentItem
                missionItemParent));
            break;
        case spray::PlanStep::Waypoint:
            // Heights are above ground; PX4 holds them relative to the takeoff
            // point until the onboard app adds terrain following.
            items.append(new MissionItem(
                seqNum++,
                MAV_CMD_NAV_WAYPOINT,
                MAV_FRAME_GLOBAL_RELATIVE_ALT,
                0,      // hold time
                0.0,    // acceptance radius (default)
                0.0,    // pass through
                nan,    // yaw: unchanged
                step.pos.lat,
                step.pos.lon,
                step.altM,
                true,   // autoContinue
                false,  // isCurrentItem
                missionItemParent));
            break;
        case spray::PlanStep::SprayOn:
        case spray::PlanStep::SprayOff:
            // Spray marker: actuator set 1, output 1. 1 = spray on, 0 = off.
            // Outputs 2-6 are left unchanged (NaN). The onboard app reads these.
            items.append(new MissionItem(
                seqNum++,
                MAV_CMD_DO_SET_ACTUATOR,
                MAV_FRAME_MISSION,
                step.kind == spray::PlanStep::SprayOn ? 1.0 : 0.0,  // actuator 1
                nan,    // actuator 2
                nan,    // actuator 3
                nan,    // actuator 4
                nan,    // actuator 5
                nan,    // actuator 6
                0,      // actuator set index: 0 = set 1
                true,   // autoContinue
                false,  // isCurrentItem
                missionItemParent));
            break;
        case spray::PlanStep::LandStart:
            // Start of the landing sequence: with RTL_TYPE = 1, PX4's Return flies
            // straight to the next waypoint (the way out) and follows the mission
            // from here to the Land at the end. No position, as QGC sends it to PX4.
            items.append(new MissionItem(
                seqNum++,
                MAV_CMD_DO_LAND_START,
                MAV_FRAME_MISSION,
                0, 0, 0, 0, 0, 0, 0,
                true,   // autoContinue
                false,  // isCurrentItem
                missionItemParent));
            break;
        }
    }
}

/*---------------------------------------------------------------------------*/

void SprayAreaComplexItem::save(QJsonArray &missionItems)
{
    QJsonObject saveObject;
    saveObject[JsonParsing::jsonVersionKey]                = 1;
    saveObject[VisualMissionItem::jsonTypeKey]             = VisualMissionItem::jsonTypeComplexItemValue;
    saveObject[ComplexMissionItem::jsonComplexItemTypeKey] = jsonComplexItemTypeValue;
    saveObject[_jsonSwathWidthKey]      = _swathWidthFact.rawValue().toDouble();
    saveObject[_jsonPassAngleKey]       = _passAngleFact.rawValue().toDouble();
    saveObject[_jsonPassOffsetKey]      = _passOffsetFact.rawValue().toDouble();
    saveObject[_jsonAltitudeKey]        = _altitudeFact.rawValue().toDouble();
    saveObject[_jsonSpeedKey]           = _speedFact.rawValue().toDouble();
    saveObject[_jsonApplicationRateKey] = _applicationRateFact.rawValue().toDouble();
    saveObject[_jsonEdgeMarginKey]      = _edgeMarginFact.rawValue().toDouble();
    saveObject[_jsonHeadlandPassKey]    = _headlandPassFact.rawValue().toBool();

    // The field boundary, as drawn or imported.
    _fieldPolygon.saveToJson(saveObject);

    // The generated spray area (boundary minus edge margin), [lat, lon] pairs.
    // Saved for the onboard app, which switches the pump on inside it.
    QJsonArray sprayArea;
    for (const auto &p : _result.sprayArea) {
        sprayArea.append(QJsonArray { p.lat, p.lon });
    }
    saveObject[_jsonSprayAreaKey] = sprayArea;

    // The route as flown, with the planner's edits. Unedited routes are
    // regenerated on load, so they pick up improvements to the generator.
    QJsonArray route;
    for (const auto &p : _route.points()) {
        route.append(QJsonArray { p.lat, p.lon });
    }
    QJsonArray segmentSpray;
    for (bool on : _route.segmentSpray()) {
        segmentSpray.append(on);
    }
    saveObject[_jsonRouteKey]       = route;
    saveObject[_jsonRouteSprayKey]  = segmentSpray;
    saveObject[_jsonRouteEditedKey] = _route.hasEdits();

    saveObject[_jsonNameKey] = _fieldName;
    QJsonArray sideBuffers;
    for (int i = 0; i < _sideBuffers.size(); ++i) {
        if (_sideBuffers[i] >= 0.0) {
            sideBuffers.append(QJsonArray { i, _sideBuffers[i] });
        }
    }
    saveObject[_jsonSideBuffersKey] = sideBuffers;

    // Transit. The gate and takeoff point are also saved resolved, for the
    // onboard app (its Return mode leaves the field through the gate).
    saveObject[_jsonTransitModeKey]     = _transitModeFact.rawValue().toInt();
    saveObject[_jsonTransitAltitudeKey] = _transitAltitudeFact.rawValue().toDouble();
    saveObject[_jsonTransitSpeedKey]    = _transitSpeedFact.rawValue().toDouble();
    saveObject[_jsonRouteReversedKey]   = _routeReversed;
    if (_hasStartNear) {
        saveObject[_jsonStartNearKey]   = QJsonArray { _startNear.lat, _startNear.lon };
    }
    QJsonArray gateSides;
    for (int s : _gateSides) {
        gateSides.append(s);
    }
    saveObject[_jsonGateSidesKey] = gateSides;
    QJsonArray gates;
    for (const QVariant &lineValue : _gateLinesVariant) {
        QJsonArray line;
        for (const QVariant &corner : lineValue.toList()) {
            const QGeoCoordinate c = corner.value<QGeoCoordinate>();
            line.append(QJsonArray { c.latitude(), c.longitude() });
        }
        gates.append(line);
    }
    saveObject[_jsonGatesKey] = gates;
    QJsonArray transitRoute;
    for (const auto &p : _effectiveTransitRoute()) {
        transitRoute.append(QJsonArray { p.lat, p.lon });
    }
    saveObject[_jsonTransitRouteKey]  = transitRoute;
    saveObject[_jsonTransitCustomKey] = _transitRouteCustom;
    if (hasTakeoff()) {
        const QGeoCoordinate home = takeoffPoint();
        saveObject[_jsonTakeoffKey] = QJsonArray { home.latitude(), home.longitude() };
    }

    // Resuming a job: what's already sprayed, and where the drone was waiting.
    if (!_sprayed.empty()) {
        QJsonArray sprayed;
        for (const spray::Strip &strip : _sprayed) {
            sprayed.append(QJsonArray { strip.a.lat, strip.a.lon, strip.b.lat, strip.b.lon, strip.widthM });
        }
        saveObject[_jsonSprayedKey] = sprayed;
    }
    if (_hasResumeFrom) {
        saveObject[_jsonResumeFromKey] = QJsonArray { _resumeFrom.lat, _resumeFrom.lon };
    }
    if (_bpValid) {
        QJsonArray strips;
        for (const spray::Strip &strip : _bpStrips) {
            strips.append(QJsonArray { strip.a.lat, strip.a.lon, strip.b.lat, strip.b.lon, strip.widthM });
        }
        QJsonObject breakpoint;
        breakpoint[QStringLiteral("strips")] = strips;
        breakpoint[QStringLiteral("stop")]   = QJsonArray { _bpStop.lat, _bpStop.lon };
        breakpoint[QStringLiteral("reason")] = _bpReason;
        breakpoint[QStringLiteral("time")]   = _bpTime.toString(Qt::ISODate);
        saveObject[_jsonBreakpointKey] = breakpoint;
    }

    missionItems.append(saveObject);
}

bool SprayAreaComplexItem::load(const QJsonObject &complexObject,
                                int sequenceNumber,
                                QString &errorString)
{
    const QList<JsonParsing::KeyValidateInfo> keyInfoList = {
        { JsonParsing::jsonVersionKey,                QJsonValue::Double, true  },
        { VisualMissionItem::jsonTypeKey,             QJsonValue::String, true  },
        { ComplexMissionItem::jsonComplexItemTypeKey, QJsonValue::String, true  },
        { QGCMapPolygon::jsonPolygonKey,              QJsonValue::Array,  true  },
        { _jsonSwathWidthKey,                         QJsonValue::Double, false },
        { _jsonPassAngleKey,                          QJsonValue::Double, false },
        { _jsonAltitudeKey,                           QJsonValue::Double, false },
        { _jsonSpeedKey,                              QJsonValue::Double, false },
        { _jsonApplicationRateKey,                    QJsonValue::Double, false },
        { _jsonEdgeMarginKey,                         QJsonValue::Double, false },
        { _jsonHeadlandPassKey,                       QJsonValue::Bool,   false },
    };

    if (!JsonParsing::validateKeys(complexObject, keyInfoList, errorString)) {
        return false;
    }

    const QString itemType    = complexObject[VisualMissionItem::jsonTypeKey].toString();
    const QString complexType = complexObject[ComplexMissionItem::jsonComplexItemTypeKey].toString();
    if (itemType != VisualMissionItem::jsonTypeComplexItemValue
            || complexType != jsonComplexItemTypeValue) {
        errorString = tr("%1 does not support loading this complex mission item type: %2:%3")
                          .arg(qgcApp()->applicationName(), itemType, complexType);
        return false;
    }

    const int version = complexObject[JsonParsing::jsonVersionKey].toInt();
    if (version != 1) {
        errorString = tr("%1 version %2 not supported").arg(jsonComplexItemTypeValue).arg(version);
        return false;
    }

    setSequenceNumber(sequenceNumber);

    // While loading, don't change other mission items (e.g. the Takeoff height).
    _loading = true;
    auto loadingGuard = qScopeGuard([this]() { _loading = false; });

    auto loadDouble = [&](const char *key, SettingsFact &fact) {
        if (complexObject.contains(key)) {
            fact.setRawValue(complexObject[key].toDouble());
        }
    };
    loadDouble(_jsonSwathWidthKey,      _swathWidthFact);
    loadDouble(_jsonPassAngleKey,       _passAngleFact);
    _passOffsetFact.setRawValue(complexObject[_jsonPassOffsetKey].toDouble(0.0));   // older plans: no offset
    loadDouble(_jsonAltitudeKey,        _altitudeFact);
    loadDouble(_jsonSpeedKey,           _speedFact);
    loadDouble(_jsonApplicationRateKey, _applicationRateFact);
    loadDouble(_jsonEdgeMarginKey,      _edgeMarginFact);
    if (_edgeMarginFact.rawValue().toDouble() < kMinBufferM) {
        _edgeMarginFact.setRawValue(kMinBufferM);
    }
    if (complexObject.contains(_jsonHeadlandPassKey)) {
        _headlandPassFact.setRawValue(complexObject[_jsonHeadlandPassKey].toBool());
    }
    loadDouble(_jsonTransitAltitudeKey, _transitAltitudeFact);
    loadDouble(_jsonTransitSpeedKey,    _transitSpeedFact);
    if (complexObject.contains(_jsonTransitModeKey)) {
        _transitModeFact.setRawValue(complexObject[_jsonTransitModeKey].toInt() == 1 ? 1 : 0);
    }

    // The chosen start, before the boundary (loading it lays out the passes).
    const QJsonArray startNear = complexObject[_jsonStartNearKey].toArray();
    _hasStartNear = startNear.size() >= 2;
    _startNear    = _hasStartNear ? spray::LatLon { startNear[0].toDouble(), startNear[1].toDouble() } : spray::LatLon {};

    // Resuming a job (also before the boundary).
    auto readStrips = [](const QJsonArray &array) {
        std::vector<spray::Strip> strips;
        for (const QJsonValue &value : array) {
            const QJsonArray strip = value.toArray();
            if (strip.size() >= 5 && strip[4].toDouble() > 0.0) {
                strips.push_back({ { strip[0].toDouble(), strip[1].toDouble() },
                                   { strip[2].toDouble(), strip[3].toDouble() },
                                   strip[4].toDouble() });
            }
        }
        return strips;
    };
    _sprayed = readStrips(complexObject[_jsonSprayedKey].toArray());
    const QJsonArray resumeFrom = complexObject[_jsonResumeFromKey].toArray();
    _hasResumeFrom = resumeFrom.size() >= 2;
    _resumeFrom    = _hasResumeFrom ? spray::LatLon { resumeFrom[0].toDouble(), resumeFrom[1].toDouble() } : spray::LatLon {};
    _rebuildSprayedVariant();
    emit sprayedChanged();

    const QJsonObject               breakpoint = complexObject[_jsonBreakpointKey].toObject();
    const std::vector<spray::Strip> bpStrips   = readStrips(breakpoint[QStringLiteral("strips")].toArray());
    const QJsonArray                bpStop     = breakpoint[QStringLiteral("stop")].toArray();
    if (!bpStrips.empty() && bpStop.size() >= 2) {
        _setBreakpoint(true, bpStrips, { bpStop[0].toDouble(), bpStop[1].toDouble() },
                       breakpoint[QStringLiteral("reason")].toString(),
                       QDateTime::fromString(breakpoint[QStringLiteral("time")].toString(), Qt::ISODate));
    } else {
        _setBreakpoint(false, {}, {}, QString(), QDateTime());
    }

    _fieldPolygon.clear();
    if (!_fieldPolygon.loadFromJson(complexObject, true /* required */, errorString)) {
        return false;
    }

    // Side buffers belong to the boundary just loaded.
    _sidePolygon = _fieldPolygon.coordinateList();
    _sideBuffers = QList<double>(_sidePolygon.count(), -1.0);
    for (const QJsonValue &value : complexObject[_jsonSideBuffersKey].toArray()) {
        const QJsonArray entry = value.toArray();
        const int side = entry.size() >= 2 ? entry[0].toInt(-1) : -1;
        if (side >= 0 && side < _sideBuffers.size()) {
            _sideBuffers[side] = qBound(kMinBufferM, entry[1].toDouble(), kMaxBufferM);
        }
    }

    const QString name = complexObject[_jsonNameKey].toString();
    if (name != _fieldName) {
        _fieldName = name;
        emit fieldNameChanged();
        emit commandNameChanged();
        emit commandDescriptionChanged();
    }

    _regenerate();

    // Re-apply the saved route edits.
    if (_result.valid && complexObject.contains(_jsonRouteKey) && complexObject.contains(_jsonRouteSprayKey)) {
        std::vector<spray::LatLon> points;
        std::vector<bool>          isSplit;
        std::vector<bool>          segmentSpray;
        bool                       pointsOk = true;
        for (const QJsonValue &value : complexObject[_jsonRouteKey].toArray()) {
            const QJsonArray p = value.toArray();
            if (p.size() < 2) {
                pointsOk = false;
                break;
            }
            points.push_back({ p[0].toDouble(), p[1].toDouble() });
            isSplit.push_back(p.size() >= 3 && p[2].toInt() != 0);
        }
        for (const QJsonValue &value : complexObject[_jsonRouteSprayKey].toArray()) {
            segmentSpray.push_back(value.toBool());
        }

        const int  oldLastSeq = lastSequenceNumber();
        bool       applied    = false;
        if (!pointsOk) {
            // fall through to the generated route
        } else if (complexObject.contains(_jsonRouteEditedKey)) {
            // Current format: an edited route is used exactly as saved.
            if (complexObject[_jsonRouteEditedKey].toBool()) {
                applied = _route.load(points, segmentSpray);
                if (!applied) {
                    qCWarning(SprayAreaLog) << "Saved route edits are inconsistent; using the generated route";
                }
            }
        } else {
            // Older plans only saved on/off edits on the generated passes.
            applied = _route.restoreLegacy(points, isSplit, segmentSpray);
            if (!applied) {
                qCWarning(SprayAreaLog) << "Saved spray on/off edits don't match the regenerated passes; using defaults";
            }
        }
        if (applied) {
            _rebuildRouteVariants();
            _emitRouteChanged(oldLastSeq);
        }
    }

    // Transit, start and end.
    {
        const int oldLastSeq = lastSequenceNumber();
        _routeReversed = complexObject[_jsonRouteReversedKey].toBool(false);
        _gateSides.clear();
        if (complexObject.contains(_jsonGateSidesKey)) {
            for (const QJsonValue &value : complexObject[_jsonGateSidesKey].toArray()) {
                const int s = value.toInt(-1);
                if (s >= 0 && s < _fieldPolygon.count() && !_gateSides.contains(s)) {
                    _gateSides.append(s);
                }
            }
            std::sort(_gateSides.begin(), _gateSides.end());
        } else {
            const int s = complexObject[_jsonGateSideKey].toInt(-1);   // older plans
            if (s >= 0 && s < _fieldPolygon.count()) {
                _gateSides.append(s);
            }
        }
        _gateSideCount = _fieldPolygon.count();
        _transitRoute.clear();
        for (const QJsonValue &value : complexObject[_jsonTransitRouteKey].toArray()) {
            const QJsonArray p = value.toArray();
            if (p.size() >= 2) {
                _transitRoute.push_back({ p[0].toDouble(), p[1].toDouble() });
            }
        }
        _transitRouteCustom = complexObject[_jsonTransitCustomKey].toBool(false) && !_transitRoute.empty();
        _rebuildMission();
        _emitMissionChanged(oldLastSeq);
    }

    _editsWereReset = false;
    setDirty(false);
    _resetUndoHistory();   // undo doesn't reach back past opening the plan
    setBoundaryEditMode(_fieldPolygon.count() < 3);   // a loaded field opens locked
    return true;
}

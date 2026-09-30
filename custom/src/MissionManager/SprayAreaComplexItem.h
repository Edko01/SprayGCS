#pragma once

#include <limits>

#include "ComplexMissionItem.h"
#include "FactMetaData.h"
#include "MissionItem.h"
#include "QGCMapPolygon.h"
#include "SettingsFact.h"

#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

#include "SprayMission.h"
#include "SprayPathGenerator.h"
#include "SprayRoute.h"

class PlanMasterController;
class Vehicle;

/// \brief Spray Area: draw or import a field boundary and get spray passes.
///
/// The item generates back-and-forth passes at the swath width and pass angle,
/// pulled in from the boundary by the edge margin, plus an optional headland
/// pass around the edge. Every section of the route is marked spray on or off;
/// passes default to on and transits to off. In Edit Route mode the planner
/// can switch sections on or off, add, move and delete points, and delete
/// sections. Sections that leave the field boundary are flagged.
///
/// The mission contains a speed command, the waypoints, and a spray-on/off
/// marker (MAV_CMD_DO_SET_ACTUATOR, actuator set 1 output 1: 1 = on, 0 = off)
/// wherever the state changes. Everything is uploaded before flight; the
/// onboard Skynode app reads the markers, sprays only inside the spray area,
/// and controls the rate. Nothing is commanded live.
class SprayAreaComplexItem : public ComplexMissionItem
{
    Q_OBJECT

public:
    explicit SprayAreaComplexItem(PlanMasterController *masterController,
                                  bool flyView,
                                  const QString &kmlOrShpFile = QString());

    Q_PROPERTY(bool           isSprayArea      READ isSprayArea      CONSTANT)   ///< lets core QML tell this item apart (e.g. no delete button)
    Q_PROPERTY(QString        fieldName        READ fieldName        WRITE setFieldName NOTIFY fieldNameChanged)
    Q_PROPERTY(QGCMapPolygon *fieldPolygon     READ fieldPolygon     CONSTANT)
    Q_PROPERTY(Fact          *swathWidth       READ swathWidth       CONSTANT)
    Q_PROPERTY(Fact          *passAngle        READ passAngle        CONSTANT)
    Q_PROPERTY(Fact          *passOffset       READ passOffset       CONSTANT)   ///< sideways shift of the passes, m
    Q_PROPERTY(Fact          *altitude         READ altitude         CONSTANT)
    Q_PROPERTY(Fact          *speed            READ speed            CONSTANT)
    Q_PROPERTY(Fact          *applicationRate  READ applicationRate  CONSTANT)
    Q_PROPERTY(Fact          *edgeMargin       READ edgeMargin       CONSTANT)
    Q_PROPERTY(Fact          *headlandPass     READ headlandPass     CONSTANT)
    Q_PROPERTY(Fact          *transitMode      READ transitMode      CONSTANT)   ///< 0 = A (entry side), 1 = B (planned route)
    Q_PROPERTY(Fact          *transitAltitude  READ transitAltitude  CONSTANT)
    Q_PROPERTY(Fact          *transitSpeed     READ transitSpeed     CONSTANT)

    // Transit, start and end (see SprayMission.h). Coordinates are invalid when unknown.
    Q_PROPERTY(bool           hasTakeoff       READ hasTakeoff       NOTIFY missionUpdated)
    Q_PROPERTY(QGeoCoordinate takeoffPoint     READ takeoffPoint     NOTIFY missionUpdated)
    Q_PROPERTY(QGeoCoordinate startPoint       READ startPoint       NOTIFY missionUpdated)
    Q_PROPERTY(QVariantList   startOptions     READ startOptions     NOTIFY pathUpdated)    ///< where S can go: pass corners (plus E, to fly the route backwards)
    Q_PROPERTY(QGeoCoordinate endPoint         READ endPoint         NOTIFY missionUpdated)
    Q_PROPERTY(bool           routeReversed    READ routeReversed    NOTIFY missionUpdated)
    Q_PROPERTY(QVariantList   gateSides        READ gateSides        NOTIFY missionUpdated)  ///< entry sides in use (mode A), 0-based
    Q_PROPERTY(bool           gateSideAuto     READ gateSideAuto     NOTIFY missionUpdated)  ///< using the side nearest takeoff
    Q_PROPERTY(QVariantList   gateLines        READ gateLines        NOTIFY missionUpdated)  ///< [[corner, corner], ...] of the entry sides
    Q_PROPERTY(QVariantList   transitEditPoints READ transitEditPoints NOTIFY missionUpdated) ///< mode B: takeoff, then route points (last = entry)
    Q_PROPERTY(bool           transitRouteCustom READ transitRouteCustom NOTIFY missionUpdated)
    Q_PROPERTY(bool           transitEntryInside READ transitEntryInside NOTIFY missionUpdated)
    Q_PROPERTY(bool           transitBelowSpray  READ transitBelowSpray  NOTIFY missionUpdated)  ///< transit height set below spray height (spray height is used)
    Q_PROPERTY(QVariantList   transitLegs      READ transitLegs      NOTIFY missionUpdated)  ///< [{start, end}] flown in transit
    Q_PROPERTY(bool           transitEditMode  READ transitEditMode  WRITE setTransitEditMode NOTIFY transitEditModeChanged)
    /// The boundary can only be changed in this mode (on by default while there's no boundary),
    /// so clicks meant for other edits can't move or retrace it.
    Q_PROPERTY(bool           boundaryEditMode READ boundaryEditMode WRITE setBoundaryEditMode NOTIFY boundaryEditModeChanged)

    // Generated results, for the map and the editor panel.
    Q_PROPERTY(QVariantList flightPath       READ flightPath       NOTIFY pathUpdated)
    Q_PROPERTY(QVariantList segments         READ segments         NOTIFY pathUpdated)  ///< [{index, spray, outside, start, end, mid, length, azimuth}]
    Q_PROPERTY(int          outsideCount     READ outsideCount     NOTIFY pathUpdated)  ///< sections leaving the field boundary
    Q_PROPERTY(double       sprayLengthM     READ sprayLengthM     NOTIFY pathUpdated)
    Q_PROPERTY(double       sprayedAcresEst  READ sprayedAcresEst  NOTIFY pathUpdated)
    Q_PROPERTY(bool         hasRouteEdits    READ hasRouteEdits    NOTIFY pathUpdated)
    Q_PROPERTY(bool         routeEditsWereReset READ routeEditsWereReset NOTIFY pathUpdated)
    Q_PROPERTY(bool         routeEditMode    READ routeEditMode    WRITE setRouteEditMode NOTIFY routeEditModeChanged)
    Q_PROPERTY(QVariantList sprayAreaPath    READ sprayAreaPath    NOTIFY pathUpdated)
    Q_PROPERTY(bool         pathValid        READ pathValid        NOTIFY pathUpdated)
    Q_PROPERTY(int          passCount        READ passCount        NOTIFY pathUpdated)
    Q_PROPERTY(double       sprayAreaSqM     READ sprayAreaSqM     NOTIFY pathUpdated)
    Q_PROPERTY(double       sprayAreaAcres   READ sprayAreaAcres   NOTIFY pathUpdated)
    Q_PROPERTY(double       estimatedVolume  READ estimatedVolume  NOTIFY pathUpdated)  ///< gallons
    Q_PROPERTY(double       estimatedMinutes READ estimatedMinutes NOTIFY missionUpdated)

    // Live preview of the passes while a slider is dragged (the route itself
    // only changes when the slider is released).
    Q_PROPERTY(QVariantList previewPath      READ previewPath      NOTIFY previewPathChanged)

    // Boundary sides and their buffers. Side i runs from boundary point i to i+1.
    Q_PROPERTY(QVariantList sides            READ sides            NOTIFY sidesChanged)  ///< [{side, number, start, end, mid, custom, buffer}]
    Q_PROPERTY(int          customSideCount  READ customSideCount  NOTIFY sidesChanged)
    Q_PROPERTY(double       minBufferM       READ minBufferM       CONSTANT)
    Q_PROPERTY(double       maxBufferM       READ maxBufferM       CONSTANT)
    Q_PROPERTY(bool         sideEditMode     READ sideEditMode     WRITE setSideEditMode NOTIFY sideEditModeChanged)
    Q_PROPERTY(bool         passAlignMode    READ passAlignMode    WRITE setPassAlignMode NOTIFY passAlignModeChanged)  ///< tap a side to run the passes along it

    // Resuming a job: the ground already sprayed is left out of the passes, and
    // (in the air) the route starts where the drone is waiting.
    Q_PROPERTY(bool         hasSprayed       READ hasSprayed       NOTIFY sprayedChanged)
    Q_PROPERTY(QVariantList sprayedStrips    READ sprayedStrips    NOTIFY sprayedChanged)   ///< [[4 corners], ...] for the map
    Q_PROPERTY(bool         resumeInAir      READ resumeInAir      NOTIFY sprayedChanged)   ///< route starts at the waiting drone
    Q_PROPERTY(double       sprayedDoneAcres READ sprayedDoneAcres NOTIFY pathUpdated)
    Q_PROPERTY(bool         allSprayed       READ allSprayed       NOTIFY pathUpdated)      ///< nothing left to spray

    // Undo / redo of boundary, settings and route edits.
    Q_PROPERTY(bool         canUndo          READ canUndo          NOTIFY undoRedoChanged)
    Q_PROPERTY(bool         canRedo          READ canRedo          NOTIFY undoRedoChanged)

    bool           isSprayArea()     const { return true; }
    QString        fieldName()       const { return _fieldName; }
    void           setFieldName(const QString &name);
    QGCMapPolygon *fieldPolygon()    { return &_fieldPolygon; }
    Fact          *swathWidth()      { return &_swathWidthFact; }
    Fact          *passAngle()       { return &_passAngleFact; }
    Fact          *passOffset()      { return &_passOffsetFact; }
    Fact          *altitude()        { return &_altitudeFact; }
    Fact          *speed()           { return &_speedFact; }
    Fact          *applicationRate() { return &_applicationRateFact; }
    Fact          *edgeMargin()      { return &_edgeMarginFact; }
    Fact          *headlandPass()    { return &_headlandPassFact; }
    Fact          *transitMode()     { return &_transitModeFact; }
    Fact          *transitAltitude() { return &_transitAltitudeFact; }
    Fact          *transitSpeed()    { return &_transitSpeedFact; }

    bool           hasTakeoff()         const;
    QGeoCoordinate takeoffPoint()       const;
    QGeoCoordinate startPoint()         const;
    QGeoCoordinate endPoint()           const;
    bool           routeReversed()      const { return _routeReversed; }
    QVariantList   gateSides()          const;
    bool           gateSideAuto()       const { return _gateSides.isEmpty(); }
    QVariantList   gateLines()          const { return _gateLinesVariant; }
    QVariantList   transitEditPoints()  const { return _transitEditPointsVariant; }
    bool           transitRouteCustom() const { return _transitRouteCustom; }
    bool           transitEntryInside() const { return _transitEntryInside; }
    bool           transitBelowSpray()  const { return _transitAltitudeFact.rawValue().toDouble() < _altitudeFact.rawValue().toDouble(); }
    QVariantList   transitLegs()        const { return _transitLegsVariant; }
    bool           transitEditMode()    const { return _transitEditMode; }
    void           setTransitEditMode(bool enable);
    bool           boundaryEditMode()   const { return _boundaryEditMode; }
    void           setBoundaryEditMode(bool enable);

    QVariantList flightPath()       const { return _flightPathVariant; }
    QVariantList segments()         const { return _segmentsVariant; }
    QVariantList startOptions()     const { return _startOptionsVariant; }
    int          outsideCount()     const { return _outsideCount; }
    double       sprayLengthM()     const { return _route.sprayLengthM(); }
    double       sprayedAcresEst()  const;
    bool         hasRouteEdits()    const { return _route.hasEdits(); }
    bool         routeEditsWereReset() const { return _editsWereReset; }
    bool         routeEditMode()    const { return _routeEditMode; }
    void         setRouteEditMode(bool enable);
    QVariantList sprayAreaPath()    const { return _sprayAreaVariant; }
    bool         pathValid()        const { return _result.valid; }
    int          passCount()        const { return _result.passCount; }
    double       sprayAreaSqM()     const { return _result.sprayAreaM2; }
    double       sprayAreaAcres()   const { return _result.sprayAreaM2 / 4046.8564224; }
    double       estimatedVolume()  const;
    double       estimatedMinutes() const;
    QVariantList sides()            const { return _sidesVariant; }
    int          customSideCount()  const;
    double       minBufferM()       const { return kMinBufferM; }
    double       maxBufferM()       const { return kMaxBufferM; }
    bool         sideEditMode()     const { return _sideEditMode; }
    void         setSideEditMode(bool enable);
    bool         passAlignMode()    const { return _passAlignMode; }
    void         setPassAlignMode(bool enable);
    QVariantList previewPath()      const { return _previewPathVariant; }
    bool         hasSprayed()       const { return !_sprayed.empty(); }
    QVariantList sprayedStrips()    const { return _sprayedStripsVariant; }
    bool         resumeInAir()      const { return _hasResumeFrom; }
    double       sprayedDoneAcres() const { return _result.sprayedDoneM2 / 4046.8564224; }
    bool         allSprayed()       const { return _result.allSprayed; }
    bool         canUndo()          const { return _changePending || !_undoStack.isEmpty(); }
    bool         canRedo()          const { return !_changePending && !_redoStack.isEmpty(); }

    /// Show the passes as they'd be with these settings (raw units: metres, degrees).
    Q_INVOKABLE void previewPasses(double swathWidthM, double passAngleDeg, double edgeMarginM, double offsetM);
    Q_INVOKABLE void clearPreview();

    /// Resuming a job: add what the drone has sprayed of the mission it's flying
    /// (from its mission progress and position) and plan only the rest. Returns
    /// an empty string, or why it couldn't (for the operator).
    Q_INVOKABLE QString markSprayedFromDrone();
    /// Back to spraying the whole field.
    Q_INVOKABLE void    clearSprayed();

    /// Return (RTH) from SprayGCS for a mode A plan: the way it came in, backwards.
    /// Marks what's sprayed, then sends the drone inside the field to the point of
    /// the entry side(s) best for where it is, across it, straight to takeoff, and
    /// lands. Returns an empty string if it took over, or why not (then the
    /// drone's own Return is used).
    Q_INVOKABLE QString returnViaEntrySide();

    /// The Spray Area of the plan open in the Plan view, if any (for the Fly view's Return).
    static SprayAreaComplexItem *planViewItem();

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Route editing (called from the map). Points are indexes into flightPath,
    // sections (segments) run from point i to point i+1.
    Q_INVOKABLE void toggleSegment    (int segment);
    Q_INVOKABLE int  insertRoutePoint (int segment, const QGeoCoordinate &coordinate);  ///< returns the new point, or -1
    Q_INVOKABLE void moveRoutePoint   (int point, const QGeoCoordinate &coordinate);
    Q_INVOKABLE void removeRoutePoint (int point);
    Q_INVOKABLE void removeSegment    (int segment);
    Q_INVOKABLE void resetRouteEdits  ();

    // Side buffers (called from the map and the editor panel).
    Q_INVOKABLE void toggleSide       (int side);                 ///< give a side its own buffer, or back to uniform
    Q_INVOKABLE void setSideBuffer    (int side, double meters);  ///< clamped to minBufferM..maxBufferM
    Q_INVOKABLE void clearSideBuffer  (int side);
    /// Set the pass angle so the passes run parallel to this side.
    Q_INVOKABLE void alignPassesToSide(int side);

    // Start point: dropped near the other end of the route, the route is flown
    // the other way round (the route itself doesn't move).
    Q_INVOKABLE void dropStartPoint   (const QGeoCoordinate &coordinate);
    // Transit (called from the map and the editor panel).
    Q_INVOKABLE void toggleGateSide   (int side);                 ///< add or remove an allowed entry side
    Q_INVOKABLE void useNearestGateSide();                        ///< back to the side nearest takeoff
    Q_INVOKABLE void insertTransitPoint(int segment, const QGeoCoordinate &coordinate);  ///< segment of transitEditPoints
    Q_INVOKABLE void moveTransitPoint (int point, const QGeoCoordinate &coordinate);     ///< index in transitEditPoints (not 0)
    Q_INVOKABLE void removeTransitPoint(int point);                                      ///< index in transitEditPoints (not 0)
    Q_INVOKABLE void resetTransitRoute();
    /// Set the Takeoff item's height to the transit height, so the drone climbs straight to it.
    Q_INVOKABLE void syncTakeoffAltitude();

    // Called by the editor QML's polygon-capture callbacks.
    Q_INVOKABLE void clearPolygon()                                             { _fieldPolygon.clear(); }
    Q_INVOKABLE void addPolygonCoordinate(const QGeoCoordinate &coordinate)    { _fieldPolygon.appendVertex(coordinate); }
    Q_INVOKABLE void adjustPolygonCoordinate(int vertexIndex,
                                             const QGeoCoordinate &coordinate) { _fieldPolygon.adjustVertex(vertexIndex, coordinate); }

    static constexpr const char *canonicalName            = "Spray Area";
    static constexpr const char *jsonComplexItemTypeValue = "sprayArea";
    static constexpr const char *settingsGroup            = "SprayArea";

    // ComplexMissionItem overrides
    QString             patternName         () const final { return tr(canonicalName); }
    double              complexDistance     () const final { return _result.valid ? _missionStats.distanceM : 0.0; }
    double              minAMSLAltitude     () const final;
    double              maxAMSLAltitude     () const final;
    int                 lastSequenceNumber  () const final;
    bool                load                (const QJsonObject &complexObject, int sequenceNumber, QString &errorString) final;
    double              greatestDistanceTo  (const QGeoCoordinate &other) const final;
    QString             mapVisualQML        () const final { return QStringLiteral("qrc:/qml/Custom/Plan/SprayAreaMapVisual.qml"); }

    // VisualMissionItem overrides
    bool                dirty                     () const final { return _dirty; }
    bool                isSimpleItem              () const final { return false; }
    bool                isStandaloneCoordinate    () const final { return false; }
    bool                specifiesCoordinate       () const final { return _result.valid; }
    bool                specifiesAltitudeOnly     () const final { return false; }
    QString             commandDescription        () const final { return commandName(); }
    QString             commandName               () const final { return _fieldName.isEmpty() ? tr("Spray Area") : tr("Spray Area: %1").arg(_fieldName); }
    QString             abbreviation              () const final { return "S"; }
    QGeoCoordinate      coordinate                () const final;
    QGeoCoordinate      entryCoordinate           () const final { return coordinate(); }
    QGeoCoordinate      exitCoordinate            () const final;
    bool                exitCoordinateSameAsEntry () const final { return false; }
    double              editableAlt               () const final { return _altitudeFact.rawValue().toDouble(); }
    double              amslEntryAlt              () const final;
    double              amslExitAlt               () const final;
    int                 sequenceNumber            () const final { return _sequenceNumber; }
    double              specifiedFlightSpeed      () final;
    double              specifiedGimbalYaw        () final { return std::numeric_limits<double>::quiet_NaN(); }
    double              specifiedGimbalPitch      () final { return std::numeric_limits<double>::quiet_NaN(); }
    void                appendMissionItems        (QList<MissionItem *> &items, QObject *missionItemParent) final;
    void                setMissionFlightStatus    (MissionFlightStatus_t &missionFlightStatus) final;
    void                applyNewAltitude          (double newAltitude) final;
    double              additionalTimeDelay       () const final { return 0; }
    ReadyForSaveState   readyForSaveState         () const final;
    void                setDirty                  (bool dirty) final;
    void                setCoordinate             (const QGeoCoordinate &coord) final;
    void                setSequenceNumber         (int sequenceNumber) final;
    void                save                      (QJsonArray &missionItems) final;

signals:
    void pathUpdated();
    void previewPathChanged();
    void routeEditModeChanged();
    void sideEditModeChanged();
    void passAlignModeChanged();
    void sidesChanged();
    void missionUpdated();
    void transitEditModeChanged();
    void boundaryEditModeChanged();
    void fieldNameChanged();
    void undoRedoChanged();
    void sprayedChanged();

private slots:
    void _setDirty();
    void _regenerate();
    void _noteChange();
    void _commitChange();

private:
    /// Everything the planner can edit, for undo / redo.
    struct EditState {
        QList<QGeoCoordinate>      polygon;
        QVariantList               facts;         ///< in _editableFacts() order
        std::vector<spray::LatLon> routePoints;
        std::vector<bool>          routeSpray;
        QList<double>              sideBuffers;
        bool                       routeReversed = false;
        bool                       hasStartNear  = false;
        spray::LatLon              startNear;
        QList<int>                 gateSides;
        std::vector<spray::LatLon> transitRoute;
        bool                       transitRouteCustom = false;
        std::vector<spray::Strip>  sprayed;
        bool                       hasResumeFrom = false;
        spray::LatLon              resumeFrom;

        bool operator==(const EditState &other) const;
        bool operator!=(const EditState &other) const { return !(*this == other); }
    };

    QList<Fact *> _editableFacts();
    EditState     _captureState();
    void          _applyState(const EditState &state);
    void          _resetUndoHistory();

    void _routeEdited(int oldLastSeq);
    void _polygonChanged();
    void _syncSideBuffers();
    void _rebuildSideVariants();
    void _sideBuffersEdited();
    void _syncDefaultWaypointAltitude();    ///< hand-dropped waypoints start at transit height
    void _syncEndItem();                    ///< the plan ends with a Land at the takeoff point (for PX4's mission-landing Return)
    std::vector<spray::LatLon> _boundary() const;
    void _rebuildMission();                 ///< transit + route -> _missionSteps and display lists
    spray::Settings _generatorSettings() const;   ///< the pass settings as they are now
    void _transitEdited(int oldLastSeq);    ///< a planner edit to the transit: rebuild, dirty, undo step
    void _emitMissionChanged(int oldLastSeq);
    std::vector<spray::LatLon> _defaultTransitRoute() const;
    std::vector<spray::LatLon> _effectiveTransitRoute() const { return _transitRouteCustom ? _transitRoute : _defaultTransitRoute(); }

    void _makeTransitRouteCustom();
    void _rebuildSprayedVariant();
    void _trackVehicle(Vehicle *vehicle);   ///< remember where the drone left Mission mode
    int  _resumeStartSeq() const;           ///< plan sequence number of the route's first waypoint
    void _applyReturnSettings(Vehicle *vehicle);   ///< after an upload: the drone's Return to suit this plan
    QString _markSprayed(Vehicle *vehicle);        ///< markSprayedFromDrone() without the paused check
    void _emitRouteChanged(int oldLastSeq);
    void _rebuildRouteVariants();

    int                           _sequenceNumber = 0;
    QGCMapPolygon                 _fieldPolygon;
    QMap<QString, FactMetaData *> _metaDataMap;
    SettingsFact                  _swathWidthFact;
    SettingsFact                  _passAngleFact;
    SettingsFact                  _passOffsetFact;
    SettingsFact                  _altitudeFact;
    SettingsFact                  _speedFact;
    SettingsFact                  _applicationRateFact;
    SettingsFact                  _edgeMarginFact;
    SettingsFact                  _headlandPassFact;
    SettingsFact                  _transitModeFact;
    SettingsFact                  _transitAltitudeFact;
    SettingsFact                  _transitSpeedFact;

    spray::Result                 _result;
    spray::SprayRoute             _route;
    bool                          _routeEditMode  = false;
    bool                          _sideEditMode   = false;
    bool                          _passAlignMode  = false;
    bool                          _transitEditMode = false;
    bool                          _boundaryEditMode = true;   ///< until there is a boundary
    bool                          _routeReversed  = false;
    QList<int>                    _gateSides;               ///< pilot's choice (sorted); empty = nearest takeoff
    int                           _gateSideCount  = 0;      ///< boundary point count _gateSides belong to
    QList<int>                    _resolvedGateSides;
    std::vector<spray::LatLon>    _transitRoute;            ///< mode B, points after takeoff (last = entry)
    bool                          _transitRouteCustom = false;
    bool                          _transitEntryInside = true;
    std::vector<spray::PlanStep>  _missionSteps;
    spray::MissionStats           _missionStats;
    QVariantList                  _gateLinesVariant;
    QVariantList                  _transitEditPointsVariant;
    QVariantList                  _transitLegsVariant;
    QVariantList                  _previewPathVariant;
    QString                       _fieldName;
    QList<double>                 _sideBuffers;        ///< one per side, metres; -1 = uniform buffer
    QList<QGeoCoordinate>         _sidePolygon;        ///< boundary the side buffers belong to
    QVariantList                  _sidesVariant;
    bool                          _editsWereReset = false;
    int                           _outsideCount   = 0;
    QVariantList                  _flightPathVariant;
    QVariantList                  _sprayAreaVariant;
    QVariantList                  _segmentsVariant;
    QVariantList                  _startOptionsVariant;
    bool                          _hasStartNear   = false;  ///< pilot picked a start corner
    spray::LatLon                 _startNear;               ///< ...near here (kept through setting changes)
    std::vector<spray::Strip>     _sprayed;                 ///< resuming: ground already sprayed
    bool                          _hasResumeFrom  = false;  ///< resuming in the air...
    spray::LatLon                 _resumeFrom;              ///< ...from where the drone is waiting
    QVariantList                  _sprayedStripsVariant;
    QPointer<Vehicle>             _trackedVehicle;          ///< the drone whose mission progress is followed
    QList<QMetaObject::Connection> _trackConnections;
    int                           _trackIndex     = -1;     ///< mission item it was flying to when it left Mission mode
    QGeoCoordinate                _trackPos;                ///< ...and where it was
    bool                          _trackInMission = false;
    QMetaObject::Connection       _sendCompleteConnection;  ///< after an upload, continue from the drone
    QMetaObject::Connection       _returnConnection;        ///< Return from SprayGCS: route sent, fly it
    bool                          _returnUploading = false; ///< the upload is the Return route, not this plan
    static QPointer<SprayAreaComplexItem> s_planViewItem;

    // Undo / redo. Changes are grouped: a drag, typing, or a traced boundary
    // lands as one step once things have been still for _undoGroupMs.
    QList<EditState>              _undoStack;
    QList<EditState>              _redoStack;
    EditState                     _committed;          ///< state after the last recorded step
    QTimer                        _undoGroupTimer;
    bool                          _changePending = false;
    bool                          _restoring     = false;
    bool                          _loading       = false;   ///< in load(): don't touch other items
    QMetaObject::Connection       _visualItemsConnection;   ///< mission item count (Takeoff added/removed)
    static constexpr int          _undoGroupMs   = 500;
    static constexpr int          _undoLimit     = 50;

    static constexpr const char *_jsonSwathWidthKey      = "swathWidth";
    static constexpr const char *_jsonPassAngleKey       = "passAngle";
    static constexpr const char *_jsonPassOffsetKey      = "passOffset";
    static constexpr const char *_jsonAltitudeKey        = "altitude";
    static constexpr const char *_jsonSpeedKey           = "speed";
    static constexpr const char *_jsonApplicationRateKey = "applicationRate";
    static constexpr const char *_jsonEdgeMarginKey      = "edgeMargin";
    static constexpr const char *_jsonHeadlandPassKey    = "headlandPass";
    static constexpr const char *_jsonSprayAreaKey       = "sprayArea";      ///< for the onboard app
    static constexpr const char *_jsonRouteKey           = "sprayRoute";     ///< [[lat, lon], ...] (older plans: [lat, lon, isSplit])
    static constexpr const char *_jsonRouteSprayKey      = "spraySegments";  ///< [bool, ...] one per section
    static constexpr const char *_jsonRouteEditedKey     = "routeEdited";    ///< true: use the saved route as is
    static constexpr const char *_jsonNameKey            = "name";
    static constexpr const char *_jsonSideBuffersKey     = "sideBuffers";    ///< [[side, metres], ...] custom sides only
    static constexpr const char *_jsonTransitModeKey     = "transitMode";    ///< 0 = A (entry side), 1 = B (planned route)
    static constexpr const char *_jsonTransitAltitudeKey = "transitAltitude";
    static constexpr const char *_jsonTransitSpeedKey    = "transitSpeed";
    static constexpr const char *_jsonRouteReversedKey   = "routeReversed";
    static constexpr const char *_jsonStartNearKey       = "startNear";      ///< [lat, lon]: start at the pass corner nearest this
    static constexpr const char *_jsonGateSidesKey       = "gateSides";      ///< [side, ...]; empty = side nearest takeoff
    static constexpr const char *_jsonGateSideKey        = "gateSide";       ///< older plans: one side, -1 = nearest
    static constexpr const char *_jsonGatesKey           = "gates";          ///< [[[lat, lon], [lat, lon]], ...] for the onboard app
    static constexpr const char *_jsonTransitRouteKey    = "transitRoute";   ///< [[lat, lon], ...] after takeoff
    static constexpr const char *_jsonTransitCustomKey   = "transitRouteCustom";
    static constexpr const char *_jsonTakeoffKey         = "takeoff";        ///< [lat, lon] for the onboard app
    static constexpr const char *_jsonSprayedKey         = "sprayed";        ///< [[latA, lonA, latB, lonB, widthM], ...] already sprayed
    static constexpr const char *_jsonResumeFromKey      = "resumeFrom";     ///< [lat, lon]: route starts at the waiting drone

    static constexpr double      kMinBufferM             = 0.9144;           ///< 3 ft
    static constexpr double      kMaxBufferM             = 50.0;
};

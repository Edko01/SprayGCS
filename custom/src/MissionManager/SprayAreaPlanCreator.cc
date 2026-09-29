#include "SprayAreaPlanCreator.h"

#include "MissionController.h"
#include "PlanMasterController.h"
#include "QGCMAVLink.h"
#include "SprayAreaComplexItem.h"

SprayAreaPlanCreator::SprayAreaPlanCreator(PlanMasterController *planMasterController)
    : PlanCreator(planMasterController,
                  SprayAreaComplexItem::tr(SprayAreaComplexItem::canonicalName),
                  QStringLiteral("/qmlimages/PlanCreator/SurveyPlanCreator.png"),
                  QGCMAVLink::allVehicleClasses())
{
}

SprayAreaComplexItem *SprayAreaPlanCreator::_create(const QGeoCoordinate &mapCenterCoord)
{
    // The pilot placed the takeoff point (the plan's home position) before
    // starting; keep it through removeAll().
    const QGeoCoordinate takeoff = _missionController->plannedHomePosition();
    _planMasterController->removeAll();
    if (takeoff.isValid()) {
        _missionController->setHomePosition(takeoff);
    }
    const QGeoCoordinate at = takeoff.isValid() ? takeoff : mapCenterCoord;

    _missionController->insertTakeoffItem(at, -1);
    VisualMissionItem *sprayItem = _missionController->insertComplexMissionItem(SprayAreaComplexItem::canonicalName, mapCenterCoord, -1);
    _missionController->insertLandItem(at, -1);   // multirotor: return to launch

    auto *sprayArea = qobject_cast<SprayAreaComplexItem *>(sprayItem);
    if (sprayArea) {
        sprayArea->syncTakeoffAltitude();   // climb straight to transit height
        // Select the Spray Area: its boundary tools are open while it has no boundary.
        _missionController->setCurrentPlanViewSeqNum(sprayArea->sequenceNumber(), true);
    }
    return sprayArea;
}

void SprayAreaPlanCreator::createPlan(const QGeoCoordinate &mapCenterCoord)
{
    (void) _create(mapCenterCoord);
}

void SprayAreaPlanCreator::createSprayPlan(const QGeoCoordinate &mapCenterCoord, const QString &fieldName)
{
    if (SprayAreaComplexItem *sprayArea = _create(mapCenterCoord)) {
        sprayArea->setFieldName(fieldName);
    }
}

#pragma once

#include "PlanCreator.h"

class SprayAreaComplexItem;

/// Builds a spray plan: takeoff, spray area, return to launch.
/// The New Spray Plan panel calls createSprayPlan(); createPlan() is the
/// plain template entry.
class SprayAreaPlanCreator : public PlanCreator
{
    Q_OBJECT

public:
    explicit SprayAreaPlanCreator(PlanMasterController *planMasterController);

    Q_INVOKABLE void createPlan(const QGeoCoordinate &mapCenterCoord) final;

    /// Guided start: the takeoff point is the plan's home position (already
    /// placed by the pilot), the field gets `fieldName`, and the Spray Area is
    /// selected with its boundary tools open, ready to draw.
    Q_INVOKABLE void createSprayPlan(const QGeoCoordinate &mapCenterCoord, const QString &fieldName);

private:
    SprayAreaComplexItem *_create(const QGeoCoordinate &mapCenterCoord);
};

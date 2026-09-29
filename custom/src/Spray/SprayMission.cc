#include "SprayMission.h"

#include <algorithm>
#include <cmath>

namespace spray {

std::vector<PlanStep> buildMission(const MissionInput& in)
{
    std::vector<PlanStep> steps;
    const auto& points = in.route.points();
    if (points.size() < 2) {
        return steps;
    }
    const LatLon start = points.front();
    const LatLon end   = points.back();

    auto speed = [&](double metresPerSecond) {
        PlanStep s;
        s.kind    = PlanStep::Speed;
        s.speedMS = metresPerSecond;
        steps.push_back(s);
    };
    // Transit waypoint; skips an exact repeat of the previous waypoint.
    auto wp = [&](const LatLon& p, double alt) {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            if (it->kind == PlanStep::Waypoint) {
                if (distanceM(it->pos, p) < 0.05 && std::fabs(it->altM - alt) < 0.01) {
                    return;
                }
                break;
            }
        }
        PlanStep s;
        s.kind    = PlanStep::Waypoint;
        s.pos     = p;
        s.altM    = alt;
        s.transit = true;
        steps.push_back(s);
    };
    auto landStart = [&]() {
        PlanStep s;
        s.kind = PlanStep::LandStart;
        steps.push_back(s);
    };
    auto sprayRoute = [&]() {
        for (const auto& step : in.route.toMissionSteps()) {
            PlanStep s;
            if (step.kind == SprayRoute::MissionStep::Waypoint) {
                s.kind = PlanStep::Waypoint;
                s.pos  = points[static_cast<size_t>(step.point)];
                s.altM = in.sprayAltM;
            } else {
                s.kind = step.kind == SprayRoute::MissionStep::SprayOn ? PlanStep::SprayOn : PlanStep::SprayOff;
            }
            steps.push_back(s);
        }
    };

    TransitMode mode = in.transit;
    if (mode == TransitMode::Route && in.transitRoute.empty()) {
        mode = TransitMode::Gate;   // no route planned yet: fall back to the gate
    }

    if (mode == TransitMode::None) {
        speed(in.spraySpeedMS);
        sprayRoute();
        return steps;
    }

    const double transitAlt = std::max(in.transitAltM, in.sprayAltM);   // never below spray height
    const double sprayAlt   = in.sprayAltM;
    const std::vector<LatLon>& sprayArea = in.sprayArea.size() >= 3 ? in.sprayArea : in.boundary;

    if (mode == TransitMode::Gate) {
        const bool takeoffInside = pointInPolygon(in.boundary, in.takeoff);
        std::vector<int> sides;
        for (int s : in.gateSides) {
            if (s >= 0 && s < static_cast<int>(in.boundary.size())) {
                sides.push_back(s);
            }
        }
        if (sides.empty()) {
            const int nearest = nearestSide(in.boundary, in.takeoff);
            if (nearest >= 0) {
                sides.push_back(nearest);
            }
        }
        const bool useGate = !takeoffInside && !sides.empty();

        // In: straight to the gate, then inside the field to the start point.
        speed(in.transitSpeedMS);
        LatLon from = in.takeoff;
        if (useGate) {
            from = bestGatePoint(in.boundary, sides, in.takeoff, start);
            wp(from, transitAlt);
        }
        for (const auto& p : routeInside(in.boundary, from, start)) {
            wp(p, transitAlt);
        }
        speed(in.spraySpeedMS);
        sprayRoute();   // descends to spray height at the start point

        // Out: climb, inside the field to the gate, straight home.
        wp(end, transitAlt);
        speed(in.transitSpeedMS);
        const LatLon exitTo = useGate ? bestGatePoint(in.boundary, sides, in.takeoff, end) : in.takeoff;
        const std::vector<LatLon> out = routeInside(in.boundary, end, exitTo);   // ends at exitTo
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            wp(out[i], transitAlt);
        }
        landStart();                    // Return: straight to the exit, then home
        speed(in.transitSpeedMS);
        wp(exitTo, transitAlt);
        wp(in.takeoff, transitAlt);
        return steps;
    }

    // Route mode.
    const LatLon entry = in.transitRoute.back();
    speed(in.transitSpeedMS);
    for (const auto& p : in.transitRoute) {
        wp(p, transitAlt);
    }
    wp(entry, sprayAlt);           // descend at the entry point
    speed(in.spraySpeedMS);
    // At spray height, stay inside the spray area: if the entry point is in
    // the buffer, step onto the spray area's edge first.
    const LatLon entryIn = pointInPolygon(sprayArea, entry) ? entry : nearestPointOnPolygon(sprayArea, entry);
    wp(entryIn, sprayAlt);
    std::vector<LatLon> toStart = routeInside(sprayArea, entryIn, start);
    toStart.pop_back();            // the route's first waypoint is the start point
    for (const auto& p : toStart) {
        wp(p, sprayAlt);
    }
    sprayRoute();

    // Out: at spray height and speed to the entry point, climb, route back.
    for (const auto& p : routeInside(sprayArea, end, entryIn)) {
        wp(p, sprayAlt);
    }
    wp(entry, sprayAlt);
    landStart();                   // Return: straight to the entry point, then the route home
    wp(entry, transitAlt);
    speed(in.transitSpeedMS);
    for (size_t i = in.transitRoute.size() - 1; i-- > 0;) {
        wp(in.transitRoute[i], transitAlt);
    }
    wp(in.takeoff, transitAlt);
    return steps;
}

MissionStats missionStats(const std::vector<PlanStep>& steps, double climbRateMS)
{
    MissionStats stats;
    double        speedMS = 0.0;
    const PlanStep* prev  = nullptr;
    for (const auto& s : steps) {
        if (s.kind == PlanStep::Speed) {
            speedMS = s.speedMS;
            continue;
        }
        if (s.kind != PlanStep::Waypoint) {
            continue;
        }
        if (prev) {
            const double horizontal = distanceM(prev->pos, s.pos);
            const double vertical   = std::fabs(s.altM - prev->altM);
            stats.distanceM += std::hypot(horizontal, vertical);
            if (horizontal > 0.01 && speedMS > 0.0) {
                stats.seconds += horizontal / speedMS;
            } else if (climbRateMS > 0.0) {
                stats.seconds += vertical / climbRateMS;
            }
        }
        prev = &s;
    }
    return stats;
}

} // namespace spray

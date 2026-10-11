#include "SprayMission.h"

#include <algorithm>
#include <cmath>

namespace spray {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The landing turn: its length grows with the turn (so the drone turns at a
// steady rate), within these limits.
constexpr double kTurnMPerDeg  = 0.5;
constexpr double kMinTurnM     = 15.0;
constexpr double kMaxTurnM     = 90.0;
constexpr double kMaxSpacingM  = 5.0;
// Largest heading change from one waypoint to the next, well inside PX4's
// MIS_YAW_ERR (12 degrees by default) so it never stops to finish a turn.
constexpr double kMaxStepDeg   = 8.0;

double wrap180(double deg)
{
    deg = std::fmod(deg + 180.0, 360.0);
    if (deg < 0.0) {
        deg += 360.0;
    }
    return deg - 180.0;
}

double wrap360(double deg)
{
    deg = std::fmod(deg, 360.0);
    return deg < 0.0 ? deg + 360.0 : deg;
}

// Degrees clockwise from north, in the same flat approximation as distanceM.
double bearingDeg(const LatLon& a, const LatLon& b)
{
    const double cosLat = std::cos((a.lat + b.lat) / 2.0 * kPi / 180.0);
    return wrap360(std::atan2((b.lon - a.lon) * cosLat, b.lat - a.lat) * 180.0 / kPi);
}

} // namespace

std::vector<HeadingPoint> landingTurn(const LatLon& from, const LatLon& to, double headingDeg)
{
    if (!std::isfinite(headingDeg)) {
        return { { to, std::numeric_limits<double>::quiet_NaN() } };
    }
    const double finalDeg = wrap360(headingDeg);
    const double legM     = distanceM(from, to);
    if (legM < kMinTurnM) {
        return { { to, finalDeg } };
    }
    const double courseDeg = bearingDeg(from, to);
    const double turnDeg   = wrap180(finalDeg - courseDeg);   // the short way round
    const double absTurn   = std::fabs(turnDeg);
    if (absTurn < 0.5) {
        return { { to, finalDeg } };
    }

    const double turnM = std::min(legM, std::clamp(absTurn * kTurnMPerDeg, kMinTurnM, kMaxTurnM));
    // Smoothstep eases the turn in and out; its steepest step is 1.5x the average.
    const int count = static_cast<int>(std::max(std::ceil(turnM / kMaxSpacingM), std::ceil(1.5 * absTurn / kMaxStepDeg)));

    std::vector<HeadingPoint> points;
    points.reserve(static_cast<size_t>(count));
    for (int i = 1; i <= count; ++i) {
        const double t    = static_cast<double>(i) / count;
        const double ease = t * t * (3.0 - 2.0 * t);
        const double f    = (legM - turnM * (1.0 - t)) / legM;   // of the way from `from` to `to`
        const LatLon p    = i == count ? to : LatLon { from.lat + (to.lat - from.lat) * f, from.lon + (to.lon - from.lon) * f };
        points.push_back({ p, i == count ? finalDeg : wrap360(courseDeg + turnDeg * ease) });
    }
    return points;
}

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
    auto lastWaypoint = [&]() -> const PlanStep* {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            if (it->kind == PlanStep::Waypoint) {
                return &*it;
            }
        }
        return nullptr;
    };
    // Transit waypoint; skips an exact repeat of the previous waypoint.
    auto wp = [&](const LatLon& p, double alt, double yawDeg = std::numeric_limits<double>::quiet_NaN()) {
        if (const PlanStep* last = lastWaypoint(); last && distanceM(last->pos, p) < 0.05 && std::fabs(last->altM - alt) < 0.01) {
            return;
        }
        PlanStep s;
        s.kind    = PlanStep::Waypoint;
        s.pos     = p;
        s.altM    = alt;
        s.transit = true;
        s.yawDeg  = yawDeg;
        steps.push_back(s);
    };
    // The last leg, to over the takeoff point, ending with the turn to the
    // landing heading.
    auto home = [&](double alt) {
        if (!std::isfinite(in.landingHeadingDeg)) {
            wp(in.takeoff, alt);
            return;
        }
        // Already over takeoff (e.g. taking off inside the field): the turn
        // goes on the leg there instead.
        if (!steps.empty() && steps.back().kind == PlanStep::Waypoint && distanceM(steps.back().pos, in.takeoff) < 0.05
            && std::fabs(steps.back().altM - alt) < 0.01) {
            steps.pop_back();
        }
        const PlanStep* last = lastWaypoint();
        const LatLon    from = last ? last->pos : in.takeoff;
        for (const HeadingPoint& h : landingTurn(from, in.takeoff, in.landingHeadingDeg)) {
            wp(h.pos, alt, h.yawDeg);
        }
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
        home(transitAlt);
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
    home(transitAlt);
    return steps;
}

std::vector<LatLon> gateReturnPath(const std::vector<LatLon>& boundary, const std::vector<int>& sides,
                                   const LatLon& takeoff, const LatLon& from)
{
    std::vector<LatLon> path;
    std::vector<int> allowed;
    for (int s : sides) {
        if (s >= 0 && s < static_cast<int>(boundary.size())) {
            allowed.push_back(s);
        }
    }
    if (boundary.size() >= 3 && !allowed.empty() && pointInPolygon(boundary, from) && !pointInPolygon(boundary, takeoff)) {
        const LatLon gate = bestGatePoint(boundary, allowed, takeoff, from);
        path = routeInside(boundary, from, gate);   // ends at the gate
    }
    path.push_back(takeoff);
    return path;
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

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

TripEstimate estimateTrips(const TripInput& in)
{
    TripEstimate result;
    const std::vector<LatLon>& points = in.route.points();
    const std::vector<bool>&   spray  = in.route.segmentSpray();
    size_t legCount = spray.size();
    while (legCount > 0 && !spray[legCount - 1]) {
        --legCount;   // the flight to the route's end after the last spray doesn't need a trip of its own
    }
    if (points.size() < 2 || legCount == 0 || in.spraySpeedMS <= 0.0 || in.transitSpeedMS <= 0.0) {
        return result;
    }

    const double climbS = in.climbRateMS > 0.0 ? in.transitAltM / in.climbRateMS : 0.0;
    auto outS  = [&](const LatLon& p) { return climbS + distanceM(in.takeoff, p) / in.transitSpeedMS; };
    auto homeS = [&](const LatLon& p) { return distanceM(p, in.takeoff) / in.transitSpeedMS + climbS; };
    auto along = [](const LatLon& a, const LatLon& b, double t) {
        return LatLon { a.lat + (b.lat - a.lat) * t, a.lon + (b.lon - a.lon) * t };
    };
    const bool tankLimit    = in.tankVolume > 0.0 && in.volumePerM2 > 0.0;
    const bool batteryLimit = in.batteryS > 0.0;

    LatLon here     = points[0];
    double t        = outS(here);
    double tankLeft = in.tankVolume;
    double load     = 0.0;    // sprayed this trip
    double progress = 0.0;    // route metres flown this trip
    if (batteryLimit && t + homeS(here) >= in.batteryS) {
        return result;
    }

    size_t leg = 0;
    while (leg < legCount) {
        const LatLon& end    = points[leg + 1];
        const double  length = distanceM(here, end);
        const double  perM   = spray[leg] ? in.swathM * in.volumePerM2 : 0.0;

        double m      = length;
        bool   empty  = false;
        bool   flat   = false;
        // (1e-9: a tank that empties right at the end of a leg covers the leg)
        if (tankLimit && perM > 0.0 && tankLeft + 1e-9 < perM * m) {
            m     = std::max(0.0, tankLeft / perM);
            empty = true;
        }
        if (batteryLimit && length > 0.0) {
            auto timeLeftAt = [&](double d) {
                return in.batteryS - (t + d / in.spraySpeedMS + homeS(along(here, end, d / length)));
            };
            if (timeLeftAt(m) < 0.0) {
                double lo = 0.0, hi = m;
                for (int i = 0; i < 40; ++i) {
                    const double mid = (lo + hi) / 2.0;
                    (timeLeftAt(mid) >= 0.0 ? lo : hi) = mid;
                }
                m     = lo;
                empty = false;
                flat  = true;
            }
        }

        const LatLon stop = length > 0.0 ? along(here, end, m / length) : end;
        t        += m / in.spraySpeedMS;
        tankLeft -= perM * m;
        load     += perM * m;
        progress += m;

        if (!empty && !flat) {
            here = end;
            ++leg;
            continue;
        }

        // Out of liquid or battery: home, refill or swap, and back to here.
        if (progress < 0.01) {
            return TripEstimate {};   // a fresh battery can't get anything done
        }
        result.stops.push_back(stop);
        (empty ? result.tankTrips : result.batteryTrips)++;
        result.fullLoad = std::max(result.fullLoad, load);
        result.totalS  += t + homeS(stop);
        ++result.trips;

        here     = stop;
        t        = outS(here);
        tankLeft = in.tankVolume;
        load     = 0.0;
        progress = 0.0;
        if (batteryLimit && t + homeS(here) >= in.batteryS) {
            return TripEstimate {};
        }
    }

    result.lastLoad = load;
    result.fullLoad = std::max(result.fullLoad, load);
    result.totalS  += t + homeS(here);
    ++result.trips;
    result.valid = true;
    return result;
}

} // namespace spray

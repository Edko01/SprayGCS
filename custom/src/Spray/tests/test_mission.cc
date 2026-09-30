// Unit tests for SprayMission (transit legs + spray route). No Qt needed.
// Build and run from the custom/src/Spray folder:
//   g++ -std=c++17 -O2 -I. tests/test_mission.cc SprayMission.cc SprayRoute.cc SprayPathGenerator.cc -o /tmp/test_mission && /tmp/test_mission
#include "SprayMission.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace spray;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const double kLat0 = 37.4128, kLon0 = -121.9990;
static const double kMLat = 6371000.0 * M_PI / 180.0;
static const double kMLon = kMLat * std::cos(kLat0 * M_PI / 180.0);
static LatLon at(double x, double y) { return { kLat0 + y / kMLat, kLon0 + x / kMLon }; }
static double xOf(const LatLon& p) { return (p.lon - kLon0) * kMLon; }
static double yOf(const LatLon& p) { return (p.lat - kLat0) * kMLat; }
static std::vector<LatLon> poly(const std::vector<std::pair<double, double>>& pts)
{
    std::vector<LatLon> out;
    for (auto [x, y] : pts) out.push_back(at(x, y));
    return out;
}

static std::vector<PlanStep> waypoints(const std::vector<PlanStep>& steps)
{
    std::vector<PlanStep> out;
    for (const auto& s : steps) if (s.kind == PlanStep::Waypoint) out.push_back(s);
    return out;
}

// Common checks: pump only on spray legs at spray height, never in transit;
// every leg except the first and last (takeoff <-> field) stays in the field.
static void commonChecks(const MissionInput& in, const std::vector<PlanStep>& steps, bool takeoffOutside)
{
    bool pumpOn = false;
    int  onCount = 0;
    for (const auto& s : steps) {
        if (s.kind == PlanStep::SprayOn)  { CHECK(!pumpOn, "pump turned on twice"); pumpOn = true; onCount++; }
        if (s.kind == PlanStep::SprayOff) { CHECK(pumpOn, "pump turned off while off"); pumpOn = false; }
        if (s.kind == PlanStep::Waypoint && s.transit) CHECK(!pumpOn, "pump on during a transit leg");
        if (s.kind == PlanStep::Waypoint && pumpOn) CHECK(std::fabs(s.altM - in.sprayAltM) < 1e-9, "spraying above spray height");
    }
    CHECK(!pumpOn, "pump left on at the end");
    CHECK(onCount > 0, "never sprayed");

    auto wps = waypoints(steps);
    std::vector<LatLon> path;
    for (auto& w : wps) path.push_back(w.pos);
    auto out = legsOutside(in.boundary, path);
    for (size_t i = 0; i < out.size(); i++) {
        const bool firstOrLast = i == 0 || i + 1 == out.size();
        if (out[i] && !(takeoffOutside && firstOrLast) && in.transit == TransitMode::Gate) {
            CHECK(false, "leg %zu leaves the field (%.1f,%.1f)->(%.1f,%.1f)", i,
                  xOf(path[i]), yOf(path[i]), xOf(path[i + 1]), yOf(path[i + 1]));
        }
    }
    auto stats = missionStats(steps);
    CHECK(stats.distanceM > 0 && stats.seconds > 0, "stats");
}

int main()
{
    std::vector<LatLon> rect = poly({ {0,0}, {100,0}, {100,60}, {0,60} });
    Result g = generate(rect, Settings{});
    SprayRoute route;
    route.reset(g.flightPath, g.legSpray);

    std::printf("no transit: speed + spray route only\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::None;
        auto steps = buildMission(in);
        CHECK(steps.front().kind == PlanStep::Speed && steps.front().speedMS == in.spraySpeedMS, "first step should set spray speed");
        CHECK(waypoints(steps).size() == route.points().size(), "one waypoint per route point");
        commonChecks(in, steps, false);
    }

    std::printf("mode A: in and out through the side nearest takeoff\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::Gate;
        in.takeoff = at(50, -40);   // south of the field -> bottom side (0)
        in.transitAltM = 12; in.sprayAltM = 3;
        CHECK(nearestSide(rect, in.takeoff) == 0, "nearest side %d", nearestSide(rect, in.takeoff));
        auto steps = buildMission(in);
        CHECK(steps.front().kind == PlanStep::Speed && steps.front().speedMS == in.transitSpeedMS, "starts at transit speed");
        auto wps = waypoints(steps);
        CHECK(std::fabs(yOf(wps.front().pos)) < 0.01 && xOf(wps.front().pos) > 1.9 && xOf(wps.front().pos) < 98.1,
              "first waypoint should be on the bottom side, got (%.2f, %.2f)", xOf(wps.front().pos), yOf(wps.front().pos));
        CHECK(wps.front().altM == 12, "transit height");
        CHECK(distanceM(wps.back().pos, in.takeoff) < 0.01 && wps.back().altM == 12, "ends over takeoff at transit height");
        // Last leg crosses the bottom side too: the second-last waypoint is on it.
        const auto& gateOut = wps[wps.size() - 2];
        CHECK(std::fabs(yOf(gateOut.pos)) < 0.01, "exit should be through the bottom side, got y=%.2f", yOf(gateOut.pos));
        // Pilot-chosen gate: the top side (2).
        in.gateSides = { 2 };
        auto top = waypoints(buildMission(in));
        CHECK(std::fabs(yOf(top.front().pos) - 60) < 0.01, "gate on the top side, got y=%.2f", yOf(top.front().pos));
        commonChecks(in, steps, true);
    }

    std::printf("mode A: field with a bay, start across the bay\n");
    {
        std::vector<LatLon> bay = poly({ {0,0}, {200,0}, {200,150}, {130,150}, {120,60}, {80,60}, {70,150}, {0,150} });
        Settings s; s.headlandPass = false; s.passAngleDeg = 90;
        Result gb = generate(bay, s);
        SprayRoute r; r.reset(gb.flightPath, gb.legSpray);
        MissionInput in; in.boundary = bay; in.route = r; in.transit = TransitMode::Gate;
        in.takeoff = at(100, 200);   // north, above the bay mouth -> nearest side is the bay's
        in.gateSides = { 7 };        // pilot picks the top-left side (70,150)->(0,150)
        auto steps = buildMission(in);
        commonChecks(in, steps, true);
        std::printf("  %zu waypoints\n", waypoints(steps).size());
    }

    std::printf("mode A: start near the corner of a long entry side goes straight in\n");
    {
        // Long west side (x = 0, 0..800 m); the start is 5 m from its top corner.
        std::vector<LatLon> tall = poly({ {0,0}, {100,0}, {100,800}, {0,800} });
        const LatLon takeoff = at(-300, 400);
        const LatLon start   = at(4, 795);
        LatLon g = gatePoint(tall, 3, takeoff, start);   // side 3: (0,800) -> (0,0)
        // The straight line from takeoff to start crosses x = 0 at y = 400 + 395 * 300/304.
        const double yIdeal = 400 + 395.0 * 300.0 / 304.0;
        CHECK(std::fabs(xOf(g)) < 0.01 && std::fabs(yOf(g) - yIdeal) < 0.5,
              "gate point (%.2f, %.2f), expected (0, %.2f)", xOf(g), yOf(g), yIdeal);
        // Right up against the corner: held 2 m off it.
        LatLon c = gatePoint(tall, 3, at(-300, 900), at(1, 799.5));
        CHECK(yOf(c) <= 798.01 && yOf(c) > 790, "corner margin, got y=%.2f", yOf(c));
    }

    std::printf("mode A: several entry sides, each way uses the shortest\n");
    {
        // Takeoff south-west. Start near the top of the left side, end at the
        // bottom right: in through the left side (3), out through the bottom (0).
        std::vector<LatLon> tall = poly({ {0,0}, {200,0}, {200,300}, {0,300} });
        SprayRoute r;
        r.reset({ at(5, 290), at(5, 10), at(195, 10) }, { true, true });
        MissionInput in; in.boundary = tall; in.route = r; in.transit = TransitMode::Gate;
        in.takeoff = at(-100, -60);
        in.gateSides = { 0, 3 };
        auto wps = waypoints(buildMission(in));
        CHECK(std::fabs(xOf(wps.front().pos)) < 0.01, "should enter through the left side, got (%.1f, %.1f)", xOf(wps.front().pos), yOf(wps.front().pos));
        const auto& out = wps[wps.size() - 2];
        CHECK(std::fabs(yOf(out.pos)) < 0.01, "should leave through the bottom, got (%.1f, %.1f)", xOf(out.pos), yOf(out.pos));
        // Only the bottom allowed: both ways use it.
        in.gateSides = { 0 };
        auto only = waypoints(buildMission(in));
        CHECK(std::fabs(yOf(only.front().pos)) < 0.01, "entry should be on the bottom");
        // Out-of-range sides are ignored (falls back to nearest).
        in.gateSides = { 9 };
        CHECK(!waypoints(buildMission(in)).empty(), "bad side must not break the mission");
    }

    std::printf("mode A: takeoff inside the field\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::Gate;
        in.takeoff = at(50, 30);
        auto steps = buildMission(in);
        commonChecks(in, steps, false);
        CHECK(distanceM(waypoints(steps).back().pos, in.takeoff) < 0.01, "ends over takeoff");
    }

    std::printf("mode B: planned route in, spray, same route out\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::Route;
        in.takeoff = at(-80, -50);
        in.transitRoute = { at(-40, -30), at(-10, 20), at(20, 30) };   // last point is inside
        in.transitAltM = 15; in.sprayAltM = 3;
        auto steps = buildMission(in);
        auto wps = waypoints(steps);
        CHECK(distanceM(wps[0].pos, in.transitRoute[0]) < 0.01 && wps[0].altM == 15, "first route point");
        CHECK(distanceM(wps[2].pos, in.transitRoute[2]) < 0.01 && wps[2].altM == 15, "entry point at transit height");
        CHECK(distanceM(wps[3].pos, in.transitRoute[2]) < 0.01 && wps[3].altM == 3, "descend at the entry point");
        // Out: last four waypoints are entry (spray), entry (transit), route back, takeoff.
        const size_t n = wps.size();
        // Transit is never lower than spray height.
        MissionInput low = in; low.transitAltM = 1; low.sprayAltM = 3;
        for (auto& w : waypoints(buildMission(low))) CHECK(w.altM >= 3, "waypoint below spray height");
        CHECK(distanceM(wps[n - 5].pos, in.transitRoute[2]) < 0.01 && wps[n - 5].altM == 3, "back at entry at spray height");
        CHECK(distanceM(wps[n - 4].pos, in.transitRoute[2]) < 0.01 && wps[n - 4].altM == 15, "climb at entry");
        CHECK(distanceM(wps[n - 3].pos, in.transitRoute[1]) < 0.01, "route back, point 2");
        CHECK(distanceM(wps[n - 2].pos, in.transitRoute[0]) < 0.01, "route back, point 1");
        CHECK(distanceM(wps[n - 1].pos, in.takeoff) < 0.01, "home");
        // The climb back to transit happens before transit speed is set again.
        commonChecks(in, steps, true);
    }

    std::printf("mode B: entry point in the buffer, spray-height legs stay in the spray area\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::Route;
        in.sprayArea = g.sprayArea;
        in.takeoff = at(50, -40);
        in.transitRoute = { at(50, 0.5) };   // inside the field but inside the 1 m buffer
        auto steps = buildMission(in);
        std::vector<LatLon> sprayHeightPath;
        for (auto& w : waypoints(steps)) if (w.altM == in.sprayAltM) sprayHeightPath.push_back(w.pos);
        int outside = 0;
        auto legs = legsOutside(g.sprayArea, sprayHeightPath);
        // Only the first and last spray-height legs (entry point <-> spray area edge) may cross the buffer.
        for (size_t i = 1; i + 1 < legs.size(); i++) outside += legs[i];
        CHECK(outside == 0, "%d spray-height legs leave the spray area", outside);
        commonChecks(in, steps, true);
    }

    std::printf("mode B with no planned route falls back to mode A\n");
    {
        MissionInput in; in.boundary = rect; in.route = route; in.transit = TransitMode::Route;
        in.takeoff = at(50, -40);
        auto wps = waypoints(buildMission(in));
        CHECK(std::fabs(yOf(wps.front().pos)) < 0.01, "should enter through the bottom side");
    }

    std::printf("return marker (DO_LAND_START) before the way out\n");
    {
        auto landStarts = [](const std::vector<PlanStep>& steps) {
            int n = 0;
            for (auto& st : steps) n += st.kind == PlanStep::LandStart;
            return n;
        };
        // The first waypoint after the marker, and whether transit speed is set before it.
        auto afterMarker = [](const std::vector<PlanStep>& steps, bool& speedSet) {
            speedSet = false;
            bool seen = false;
            for (auto& st : steps) {
                if (st.kind == PlanStep::LandStart) { seen = true; continue; }
                if (!seen) continue;
                if (st.kind == PlanStep::Speed) speedSet = true;
                if (st.kind == PlanStep::Waypoint) return st;
            }
            return PlanStep{};
        };

        MissionInput a; a.boundary = rect; a.route = route; a.transit = TransitMode::Gate;
        a.takeoff = at(50, -40); a.transitAltM = 12;
        auto sa = buildMission(a);
        CHECK(landStarts(sa) == 1, "mode A: %d markers, expected 1", landStarts(sa));
        bool speedSet = false;
        PlanStep first = afterMarker(sa, speedSet);
        CHECK(std::fabs(yOf(first.pos)) < 0.01 && first.altM == 12, "mode A: after the marker, the exit gate at transit height, got (%.1f, %.1f) %.1f m",
              xOf(first.pos), yOf(first.pos), first.altM);
        CHECK(speedSet, "mode A: transit speed set after the marker");
        auto aw = waypoints(sa);
        CHECK(distanceM(aw.back().pos, a.takeoff) < 0.01, "mode A: still ends over takeoff");
        commonChecks(a, sa, true);
        // Pump is off before the marker (a Return never sprays).
        bool pumpOn = false, onAtMarker = false;
        for (auto& st : sa) {
            if (st.kind == PlanStep::SprayOn) pumpOn = true;
            if (st.kind == PlanStep::SprayOff) pumpOn = false;
            if (st.kind == PlanStep::LandStart) onAtMarker = pumpOn;
        }
        CHECK(!onAtMarker, "pump must be off at the marker");

        // Takeoff inside the field: marker, then straight to takeoff.
        MissionInput in = a; in.takeoff = at(50, 30);
        auto si = buildMission(in);
        CHECK(landStarts(si) == 1, "takeoff inside: one marker");
        PlanStep fi = afterMarker(si, speedSet);
        CHECK(distanceM(fi.pos, in.takeoff) < 0.01, "takeoff inside: after the marker, the takeoff point");

        MissionInput b; b.boundary = rect; b.route = route; b.transit = TransitMode::Route;
        b.takeoff = at(-80, -50); b.transitAltM = 15;
        b.transitRoute = { at(-40, -30), at(-10, 20), at(20, 30) };
        auto sb = buildMission(b);
        CHECK(landStarts(sb) == 1, "mode B: %d markers, expected 1", landStarts(sb));
        PlanStep fb = afterMarker(sb, speedSet);
        CHECK(distanceM(fb.pos, b.transitRoute.back()) < 0.01 && fb.altM == 15, "mode B: after the marker, the entry point at transit height");
        CHECK(distanceM(waypoints(sb).back().pos, b.takeoff) < 0.01, "mode B: still ends over takeoff");

        MissionInput none; none.boundary = rect; none.route = route; none.transit = TransitMode::None;
        CHECK(landStarts(buildMission(none)) == 0, "no transit: no marker");
    }

    std::printf("reversed route starts at the other end\n");
    {
        SprayRoute rev = route.reversed();
        CHECK(distanceM(rev.points().front(), route.points().back()) < 1e-6, "start is the old end");
        CHECK(rev.segmentSpray().front() == route.segmentSpray().back(), "spray states follow their sections");
        CHECK(std::fabs(rev.sprayLengthM() - route.sprayLengthM()) < 1e-6, "same spray length");
        MissionInput in; in.boundary = rect; in.route = rev; in.transit = TransitMode::Gate; in.takeoff = at(50, -40);
        commonChecks(in, buildMission(in), true);
    }

    std::printf("resuming in the air: the route's first waypoint is where the drone waits\n");
    {
        Settings rs;
        for (size_t i = 1; i < g.flightPath.size() / 2; ++i) {
            if (g.legSpray[i - 1]) rs.sprayed.push_back({ g.flightPath[i - 1], g.flightPath[i], rs.swathWidthM });
        }
        rs.hasResumeFrom = true;
        rs.resumeFrom = g.flightPath[g.flightPath.size() / 2];
        Result rg = generate(rect, rs);
        CHECK(rg.valid, "resume route invalid");
        SprayRoute rr;
        rr.reset(rg.flightPath, rg.legSpray);
        for (TransitMode mode : { TransitMode::None, TransitMode::Gate, TransitMode::Route }) {
            MissionInput in; in.boundary = rect; in.route = rr; in.transit = mode;
            in.takeoff = at(50, -40); in.transitAltM = 12; in.sprayAltM = 3;
            if (mode == TransitMode::Route) in.transitRoute = { at(50, -20), at(50, 5) };
            auto steps = buildMission(in);
            const PlanStep* first = nullptr;
            for (const auto& st : steps) {
                if (st.kind == PlanStep::Waypoint && !st.transit) { first = &st; break; }
            }
            CHECK(first && distanceM(first->pos, rs.resumeFrom) < 0.01 && first->altM == 3,
                  "mode %d: first route waypoint should be the waiting drone at spray height", int(mode));
        }
    }

    std::printf("helpers\n");
    {
        CHECK(pointInPolygon(rect, at(50, 30)) && !pointInPolygon(rect, at(50, -1)) && pointInPolygon(rect, at(0, 30)), "pointInPolygon");
        LatLon n = nearestPointOnPolygon(rect, at(50, -40));
        CHECK(std::fabs(xOf(n) - 50) < 0.01 && std::fabs(yOf(n)) < 0.01, "nearest point (%.2f, %.2f)", xOf(n), yOf(n));
        MissionStats st = missionStats({ { PlanStep::Speed, {}, 0, 5, false }, { PlanStep::Waypoint, at(0,0), 10, 0, false },
                                         { PlanStep::Waypoint, at(100,0), 10, 0, false }, { PlanStep::Waypoint, at(100,0), 2, 0, false } }, 2.0);
        CHECK(std::fabs(st.distanceM - 108) < 0.1 && std::fabs(st.seconds - 24) < 0.1, "stats %.1f m %.1f s", st.distanceM, st.seconds);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

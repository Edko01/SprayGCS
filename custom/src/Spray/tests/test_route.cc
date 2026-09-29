// Unit tests for SprayRoute (spray on/off editing and mission markers). No Qt needed.
// Build and run from the custom/src/Spray folder:
//   g++ -std=c++17 -O2 -I. tests/test_route.cc SprayRoute.cc SprayPathGenerator.cc -o /tmp/test_route && /tmp/test_route
#include "SprayRoute.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace spray;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string stepsString(const SprayRoute& r)
{
    std::string s;
    for (const auto& st : r.toMissionSteps()) {
        if (st.kind == SprayRoute::MissionStep::Waypoint) s += "W" + std::to_string(st.point) + " ";
        else s += st.kind == SprayRoute::MissionStep::SprayOn ? "ON " : "OFF ";
    }
    return s;
}

// Points 100 m apart going east from Baylands.
static std::vector<LatLon> line(int n)
{
    std::vector<LatLon> pts;
    const double lat0 = 37.4128, lon0 = -121.9990;
    const double mPerDegLon = 6371000.0 * M_PI / 180.0 * std::cos(lat0 * M_PI / 180.0);
    for (int i = 0; i < n; i++) pts.push_back({ lat0, lon0 + 100.0 * i / mPerDegLon });
    return pts;
}

int main()
{
    std::printf("defaults and markers\n");
    {
        // transit, pass, transit, pass
        SprayRoute r;
        r.reset(line(5), { false, true, false, true });
        CHECK(!r.hasEdits(), "fresh route should have no edits");
        CHECK(stepsString(r) == "W0 W1 ON W2 OFF W3 ON W4 OFF ", "got %s", stepsString(r).c_str());
        CHECK(r.markerCount() == 4, "markers %d", r.markerCount());
        CHECK(std::fabs(r.sprayLengthM() - 200.0) < 0.5, "spray length %.2f", r.sprayLengthM());
        CHECK(std::fabs(r.totalLengthM() - 400.0) < 0.5, "total length %.2f", r.totalLengthM());
    }

    std::printf("toggle a whole pass off\n");
    {
        SprayRoute r;
        r.reset(line(5), { false, true, false, true });
        CHECK(r.toggleSegment(3), "toggle failed");
        CHECK(r.hasEdits(), "should have edits");
        CHECK(stepsString(r) == "W0 W1 ON W2 OFF W3 W4 ", "got %s", stepsString(r).c_str());
        CHECK(r.toggleSegment(3) && !r.hasEdits(), "toggling back should clear edits");
        CHECK(!r.toggleSegment(9) && !r.toggleSegment(-1), "out of range toggles must fail");
    }

    std::printf("divide a pass and spray only the second half\n");
    {
        SprayRoute r;
        r.reset(line(3), { false, true });                  // transit 0-1, pass 1-2
        const LatLon mid { r.points()[1].lat, (r.points()[1].lon + r.points()[2].lon) / 2.0 };
        CHECK(r.insertPoint(1, mid) == 2, "insert should return the new point's index");
        CHECK(r.points().size() == 4 && r.segmentCount() == 3, "sizes after insert");
        CHECK(r.segmentSpray()[1] && r.segmentSpray()[2], "both parts should keep spray on");
        CHECK(r.hasEdits(), "insert is an edit");
        CHECK(r.toggleSegment(1), "toggle first part");
        CHECK(stepsString(r) == "W0 W1 W2 ON W3 OFF ", "got %s", stepsString(r).c_str());
        CHECK(std::fabs(r.sprayLengthM() - 50.0) < 0.5, "spray length %.2f", r.sprayLengthM());

        // A point on top of an existing one is refused.
        CHECK(r.insertPoint(0, r.points()[0]) == -1, "duplicate point must be refused");
        CHECK(r.insertPoint(7, mid) == -1 && r.insertPoint(-1, mid) == -1, "bad section must be refused");
    }

    std::printf("move points anywhere\n");
    {
        SprayRoute r;
        r.reset(line(3), { false, true });
        LatLon moved = r.points()[1];
        moved.lat += 30.0 / (6371000.0 * M_PI / 180.0);    // 30 m north
        CHECK(r.movePoint(1, moved), "move failed");
        CHECK(std::fabs(r.points()[1].lat - moved.lat) < 1e-12, "point not moved");
        CHECK(r.hasEdits(), "move is an edit");
        // 100 m east and 30 m north from point 0: sqrt(100^2 + 30^2) = 104.4
        CHECK(std::fabs(r.segmentLengthM(0) - 104.4) < 0.5, "length %.2f", r.segmentLengthM(0));
        LatLon south = r.points()[0];
        south.lat -= 20.0 / (6371000.0 * M_PI / 180.0);
        CHECK(r.movePoint(0, south) && r.movePoint(2, south), "end points can move too");
        CHECK(!r.movePoint(3, moved) && !r.movePoint(-1, moved), "bad index must fail");
        CHECK(!r.movePoint(1, r.points()[2]), "dropping a point on its neighbour must fail");
        r.reset(line(3), { false, true });
        CHECK(!r.hasEdits(), "reset clears edits");
    }

    std::printf("delete points\n");
    {
        // transit, pass, transit, pass
        SprayRoute r;
        r.reset(line(5), { false, true, false, true });
        // Deleting the end of the first pass joins pass + transit: sprays only if both did (no).
        CHECK(r.removePoint(2), "remove middle point");
        CHECK(r.pointCount() == 4, "points %d", r.pointCount());
        CHECK(stepsString(r) == "W0 W1 W2 ON W3 OFF ", "got %s", stepsString(r).c_str());
        // Deleting a point between two sprayed parts keeps spraying.
        SprayRoute q;
        q.reset(line(3), { true, true });
        CHECK(q.removePoint(1) && q.segmentSpray()[0], "joined sprayed parts should spray");
        // First and last points.
        SprayRoute e;
        e.reset(line(5), { false, true, false, true });
        CHECK(e.removePoint(0) && e.pointCount() == 4 && e.segmentSpray()[0], "remove first");
        CHECK(e.removePoint(3) && e.pointCount() == 3 && !e.segmentSpray().back(), "remove last");
        CHECK(e.removePoint(0) && e.pointCount() == 2, "down to two");
        CHECK(!e.removePoint(0), "a route keeps at least two points");
    }

    std::printf("delete sections\n");
    {
        // transit 0-1, pass 1-2, transit 2-3, pass 3-4, transit 4-5, pass 5-6
        SprayRoute r;
        r.reset(line(7), { false, true, false, true, false, true });
        // Delete the middle pass (3-4): 2 joins 5 with a no-spray leg.
        CHECK(r.removeSegment(3), "remove middle pass");
        CHECK(r.pointCount() == 5 && r.segmentCount() == 4, "sizes %d %d", r.pointCount(), r.segmentCount());
        CHECK(stepsString(r) == "W0 W1 ON W2 OFF W3 ON W4 OFF ", "got %s", stepsString(r).c_str());
        CHECK(std::fabs(r.points()[3].lon - line(7)[5].lon) < 1e-12, "joined to the next pass start");
        // Delete the first section (a transit): the route starts at the old point 2.
        SprayRoute f;
        f.reset(line(7), { false, true, false, true, false, true });
        CHECK(f.removeSegment(0) && f.pointCount() == 5, "remove first section");
        CHECK(std::fabs(f.points()[0].lon - line(7)[2].lon) < 1e-12, "route should start at old point 2");
        // Delete the last section (a pass).
        CHECK(f.removeSegment(f.segmentCount() - 1) && f.pointCount() == 3, "remove last section");
        CHECK(stepsString(f) == "W0 W1 ON W2 OFF ", "got %s", stepsString(f).c_str());
        CHECK(!f.removeSegment(0), "a route keeps at least two points");
    }

    std::printf("save and load an edited route\n");
    {
        SprayRoute a;
        a.reset(line(4), { false, true, true });
        a.insertPoint(2, { a.points()[2].lat + 0.0001, (a.points()[2].lon + a.points()[3].lon) / 2.0 });
        a.toggleSegment(3);
        a.removePoint(0);

        SprayRoute b;
        b.reset(line(4), { false, true, true });
        CHECK(b.load(a.points(), a.segmentSpray()), "load failed");
        CHECK(stepsString(a) == stepsString(b), "loaded route differs");
        CHECK(b.hasEdits(), "loaded route should report edits");
        CHECK(!b.load(a.points(), { true }), "inconsistent sizes must fail");
        CHECK(!b.load({ a.points()[0] }, {}), "a single point must fail");
        CHECK(stepsString(a) == stepsString(b), "failed load must not change the route");
    }

    std::printf("load a plan saved by the older on/off editor\n");
    {
        // Old format: generated points plus split points on the lines.
        auto pts = line(4);
        const LatLon split { pts[2].lat, (pts[2].lon + pts[3].lon) / 2.0 };
        std::vector<LatLon> saved { pts[0], pts[1], pts[2], split, pts[3] };
        std::vector<bool>   isSplit { false, false, false, true, false };
        std::vector<bool>   spray { false, true, true, false };
        SprayRoute r;
        r.reset(pts, { false, true, true });
        CHECK(r.restoreLegacy(saved, isSplit, spray), "legacy restore failed");
        CHECK(r.pointCount() == 5 && r.hasEdits(), "legacy route not applied");
        SprayRoute c;
        c.reset(line(5), { false, true, true, true });
        CHECK(!c.restoreLegacy(saved, isSplit, spray), "legacy edits for other passes must be rejected");
    }

    std::printf("flag sections outside the field\n");
    {
        const double lat0 = 37.4128, lon0 = -121.9990;
        const double mLat = 6371000.0 * M_PI / 180.0, mLon = mLat * std::cos(lat0 * M_PI / 180.0);
        auto at = [&](double x, double y) { return LatLon { lat0 + y / mLat, lon0 + x / mLon }; };
        // L-shaped field; the inside corner is at (50, 40).
        std::vector<LatLon> field { at(0,0), at(120,0), at(120,40), at(50,40), at(50,100), at(0,100) };
        std::vector<LatLon> path { at(10,10), at(110,10), at(110,30), at(40,90), at(10,90), at(10,120) };
        auto out = legsOutside(field, path);
        CHECK(out.size() == 5, "one flag per leg");
        CHECK(!out[0] && !out[1], "legs inside the field");
        CHECK(out[2], "leg across the inside corner must be flagged");
        CHECK(!out[3], "leg inside the field");
        CHECK(out[4], "leg past the top edge must be flagged");

        // Generated routes never leave the field.
        Result g = generate(field, Settings{});
        auto gen = legsOutside(field, g.flightPath);
        int flagged = 0;
        for (bool o : gen) flagged += o;
        CHECK(flagged == 0, "%d generated legs flagged", flagged);
    }

    std::printf("generated spray area round trip\n");
    {
        // Real generator output: every pass on, transits off; markers pair up.
        std::vector<LatLon> field;
        const double lat0 = 37.4128, lon0 = -121.9990;
        const double mLat = 6371000.0 * M_PI / 180.0, mLon = mLat * std::cos(lat0 * M_PI / 180.0);
        for (auto [x, y] : std::vector<std::pair<double,double>>{ {0,0}, {100,0}, {100,60}, {0,60} })
            field.push_back({ lat0 + y / mLat, lon0 + x / mLon });
        Result g = generate(field, Settings{});
        SprayRoute r;
        r.reset(g.flightPath, g.legSpray);
        int on = 0, off = 0;
        for (const auto& st : r.toMissionSteps()) {
            on  += st.kind == SprayRoute::MissionStep::SprayOn;
            off += st.kind == SprayRoute::MissionStep::SprayOff;
        }
        CHECK(on == off && on == g.passCount + 1, "on %d off %d, passes %d (+1 headland)", on, off, g.passCount);
        CHECK(std::fabs(r.sprayLengthM() - g.sprayDistanceM) < 1.0, "spray length %.1f vs %.1f", r.sprayLengthM(), g.sprayDistanceM);
        // Mission item count: 1 speed command + waypoints + markers.
        std::printf("  %zu waypoints + %d markers\n", r.points().size(), r.markerCount());
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

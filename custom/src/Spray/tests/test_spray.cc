// Unit tests for SprayPathGenerator (no Qt needed).
// Build and run from the custom/src/Spray folder:
//   g++ -std=c++17 -O2 -I. tests/test_spray.cc SprayPathGenerator.cc -o /tmp/test_spray && /tmp/test_spray
#include "SprayPathGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace spray;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// Build lat/lon polygon from metre offsets around Baylands Park.
static std::vector<LatLon> fromMetres(const std::vector<XY>& pts)
{
    const double lat0 = 37.4128, lon0 = -121.9990;
    const double R = 6371000.0, d2r = M_PI / 180.0;
    std::vector<LatLon> out;
    for (const auto& p : pts) {
        out.push_back({ lat0 + p.y / (R * d2r), lon0 + p.x / (R * d2r * std::cos(lat0 * d2r)) });
    }
    return out;
}
static XY toM(const LatLon& p)
{
    const double lat0 = 37.4128, lon0 = -121.9990;
    const double R = 6371000.0, d2r = M_PI / 180.0;
    return { (p.lon - lon0) * d2r * R * std::cos(lat0 * d2r), (p.lat - lat0) * d2r * R };
}
static bool pointInPoly(const XY& p, const std::vector<XY>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) {
            in = !in;
        }
    }
    return in;
}
static double segDist(const XY& p, const XY& a, const XY& b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double L2 = dx * dx + dy * dy;
    double t = L2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / L2 : 0;
    t = std::fmax(0, std::fmin(1, t));
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Every point in the spray area should be within half a swath (plus tolerance)
// of the flight path. Returns the fraction of sample points covered.
static double coverage(const Result& r, double swath)
{
    std::vector<XY> area, path;
    for (auto& p : r.sprayArea) area.push_back(toM(p));
    for (auto& p : r.flightPath) path.push_back(toM(p));
    // Only legs marked as spray count toward coverage.
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (auto& p : area) { minX = std::fmin(minX, p.x); maxX = std::fmax(maxX, p.x); minY = std::fmin(minY, p.y); maxY = std::fmax(maxY, p.y); }
    int inside = 0, covered = 0;
    for (double x = minX; x <= maxX; x += 0.5) {
        for (double y = minY; y <= maxY; y += 0.5) {
            XY p { x, y };
            if (!pointInPoly(p, area)) continue;
            inside++;
            double best = 1e9;
            for (size_t i = 1; i < path.size(); i++) {
                if (!r.legSpray[i - 1]) continue;
                best = std::fmin(best, segDist(p, path[i - 1], path[i]));
            }
            if (best <= swath / 2.0 + 0.05) covered++;
        }
    }
    return inside ? double(covered) / inside : 0;
}

static void run(const char* name, const std::vector<XY>& fieldM, Settings s, bool expectValid,
                double expectAreaM2 = -1, int expectPasses = -1, double minCoverage = 0.985)
{
    std::printf("%s\n", name);
    Result r = generate(fromMetres(fieldM), s);
    CHECK(r.valid == expectValid, "valid=%d expected %d", r.valid, expectValid);
    if (!r.valid) return;
    std::printf("  area %.1f m2, passes %d, waypoints %zu, path %.1f m\n",
                r.sprayAreaM2, r.passCount, r.flightPath.size(), r.flightDistanceM);
    if (expectAreaM2 > 0) CHECK(std::fabs(r.sprayAreaM2 - expectAreaM2) < 1.0, "area %.2f expected %.2f", r.sprayAreaM2, expectAreaM2);
    if (expectPasses >= 0) CHECK(r.passCount == expectPasses, "passes %d expected %d", r.passCount, expectPasses);

    // One spray/transit flag per leg, and the spray length adds up.
    CHECK(r.legSpray.size() + 1 == r.flightPath.size(), "legSpray size %zu vs %zu points", r.legSpray.size(), r.flightPath.size());
    CHECK(!r.legSpray.empty() && r.sprayDistanceM > 0 && r.sprayDistanceM <= r.flightDistanceM + 1e-6, "spray distance %.1f", r.sprayDistanceM);
    int sprayLegs = 0;
    for (bool b : r.legSpray) sprayLegs += b;
    const int headlandLegs = r.headland.empty() ? 0 : int(r.headland.size()) - 1;
    CHECK(sprayLegs == r.passCount + headlandLegs, "spray legs %d expected %d", sprayLegs, r.passCount + headlandLegs);
    std::printf("  spray length %.1f m of %.1f m\n", r.sprayDistanceM, r.flightDistanceM);

    // All waypoints must be inside the original field.
    int outside = 0;
    for (auto& p : r.flightPath) if (!pointInPoly(toM(p), fieldM)) outside++;
    CHECK(outside == 0, "%d waypoints outside the field", outside);

    // Every leg (passes, transits, headland) must stay inside the spray area,
    // so the drone never flies outside the field boundary.
    {
        std::vector<XY> area;
        for (auto& p : r.sprayArea) area.push_back(toM(p));
        auto inside = [&](const XY& p, const std::vector<XY>& poly) {
            if (pointInPoly(p, poly)) return true;
            for (size_t i = 0; i < poly.size(); i++) {
                if (segDist(p, poly[i], poly[(i + 1) % poly.size()]) < 0.05) return true;
            }
            return false;
        };
        int legsOutsideArea = 0, legsOutsideField = 0;
        for (size_t i = 1; i < r.flightPath.size(); i++) {
            const XY a = toM(r.flightPath[i - 1]), b = toM(r.flightPath[i]);
            const int steps = std::max(2, int(std::hypot(b.x - a.x, b.y - a.y) / 0.25));
            bool outArea = false, outField = false;
            for (int k = 0; k <= steps; k++) {
                const double t = double(k) / steps;
                const XY q { a.x + t * (b.x - a.x), a.y + t * (b.y - a.y) };
                outArea  |= !inside(q, area);
                outField |= !inside(q, fieldM);
            }
            legsOutsideArea  += outArea;
            legsOutsideField += outField;
        }
        CHECK(legsOutsideField == 0, "%d legs leave the field boundary", legsOutsideField);
        CHECK(legsOutsideArea == 0, "%d legs leave the spray area", legsOutsideArea);
    }

    const double cov = coverage(r, s.swathWidthM);
    std::printf("  coverage %.1f%%\n", cov * 100);
    CHECK(cov > minCoverage, "coverage %.3f too low", cov);

    if (s.headlandPass) {
        CHECK(r.headland.size() >= 4, "headland missing");
        if (r.headland.size() >= 2) {
            XY a = toM(r.headland.front()), b = toM(r.headland.back());
            CHECK(std::hypot(a.x - b.x, a.y - b.y) < 0.01, "headland not closed");
        }
    }
}

int main()
{
    Settings s;  // 6 m swath, 0 deg, 1 m margin, headland on

    // 100 x 60 m rectangle. Area after 1 m margin = 98 x 58 = 5684 m2.
    // Pass region (inset 6 m more) = 86 x 46; passes run north (angle 0),
    // spaced across 86 m of width -> 14 lines + 1 leftover = 15? Compute: centre-lines
    // from 3 to 83 step 6 -> 3,9,...,81 = 14 lines; covered to 84, leftover 2 m (<1.5? no: 0.25*6=1.5) -> +1 = 15.
    std::vector<XY> rect { {0,0}, {100,0}, {100,60}, {0,60} };
    run("rectangle, headland", rect, s, true, 98.0 * 58.0, 15);

    // Same rectangle, clockwise winding, no headland.
    Settings noHead = s; noHead.headlandPass = false;
    std::vector<XY> rectCW { {0,0}, {0,60}, {100,60}, {100,0} };
    run("rectangle CW, no headland", rectCW, noHead, true, 98.0 * 58.0);

    // Passes east-west (90 deg) and at 37 deg.
    Settings east = s; east.passAngleDeg = 90;
    run("rectangle, 90 deg", rect, east, true, 98.0 * 58.0);
    Settings skew = s; skew.passAngleDeg = 37;
    run("rectangle, 37 deg", rect, skew, true, 98.0 * 58.0);

    // Irregular convex field.
    std::vector<XY> quad { {0,0}, {220,-15}, {260,140}, {-30,170} };
    run("irregular quad", quad, s, true);
    Settings quad2 = s; quad2.passAngleDeg = 120; quad2.swathWidthM = 8.5; quad2.edgeMarginM = 3;
    run("irregular quad, 8.5 m swath, 120 deg, 3 m margin", quad, quad2, true);

    // L-shaped (concave) field, passes cross the notch.
    std::vector<XY> ell { {0,0}, {120,0}, {120,40}, {50,40}, {50,100}, {0,100} };
    run("L-shape, headland", ell, s, true);
    run("L-shape, 90 deg, no headland", ell, [&]{ Settings t = noHead; t.passAngleDeg = 90; return t; }(), true);

    // Field with a bay cut into the top edge (like the pond at Baylands).
    // North-south passes are split by the bay; the drone must fly around it,
    // not across the water.
    std::vector<XY> bay { {0,0}, {200,0}, {200,150}, {130,150}, {120,90}, {100,70}, {80,90}, {70,150}, {0,150} };
    run("field with a bay, headland", bay, s, true);
    run("field with a bay, no headland", bay, noHead, true);
    run("field with a bay, 20 deg", bay, [&]{ Settings t = s; t.passAngleDeg = 20; return t; }(), true);
    run("field with a bay, 90 deg, no headland", bay, [&]{ Settings t = noHead; t.passAngleDeg = 90; return t; }(), true);

    // U-shaped field: two arms joined at the bottom.
    std::vector<XY> uField { {0,0}, {150,0}, {150,120}, {100,120}, {100,40}, {50,40}, {50,120}, {0,120} };
    run("U-shape, headland", uField, s, true);
    run("U-shape, no headland, 7 m swath", uField, [&]{ Settings t = noHead; t.swathWidthM = 7; return t; }(), true);

    // Star-ish field with several concave corners at an odd angle.
    std::vector<XY> star;
    for (int i = 0; i < 14; i++) {
        const double a = 2 * M_PI * i / 14, rr = (i % 2) ? 55 : 110;
        star.push_back({ rr * std::cos(a), rr * std::sin(a) });
    }
    // Sharp star points are narrower than a swath near the tip, so a sliver
    // right at each tip can't be reached (same as before this change).
    run("star field, 33 deg", star, [&]{ Settings t = s; t.passAngleDeg = 33; return t; }(), true, -1, -1, 0.98);
    run("star field, no headland", star, noHead, true);

    // Per-side buffers (side i runs from boundary point i to i+1).
    {
        // CCW rectangle: side 0 bottom, 1 right, 2 top, 3 left. Top side 10 m.
        Settings t = s; t.sideMarginsM = { -1, -1, 10, -1 };
        run("rectangle, 10 m buffer on the top side", rect, t, true, 98.0 * 49.0);
        // Clockwise rectangle: side 0 left, 1 top, 2 right, 3 bottom. Top side 10 m.
        Settings cw = s; cw.sideMarginsM = { -1, 10, -1, -1 };
        run("clockwise rectangle, 10 m buffer on the top side", rectCW, cw, true, 98.0 * 49.0);
        // Left 4 m, right 7 m, top 3 m, bottom uniform 1 m: 89 x 56.
        Settings lr = s; lr.sideMarginsM = { -1, 7, 3, 4 };
        run("rectangle, different buffer on three sides", rect, lr, true, 89.0 * 56.0);
        // Duplicate/collinear points: the bottom is split into sides 0, 1 (zero length)
        // and 2. Joined sides keep the larger buffer: bottom 5 m, top (side 4) 10 m.
        std::vector<XY> messy2 { {0,0}, {50,0}, {50,0}, {100,0}, {100,60}, {0,60} };
        Settings m = s; m.sideMarginsM = { 5, -1, -1, -1, 10, -1 };
        run("split bottom side, per-side buffers", messy2, m, true, 98.0 * 45.0);
        // Wrong number of entries: ignored, uniform buffer used.
        Settings bad = s; bad.sideMarginsM = { 10, 10 };
        run("per-side buffer list of the wrong size is ignored", rect, bad, true, 98.0 * 58.0, 15);
        // Zero uniform buffer with one side buffered.
        Settings zero = noHead; zero.edgeMarginM = 0; zero.sideMarginsM = { -1, -1, -1, 20 };
        run("no uniform buffer, 20 m on the left side", rect, zero, true, 80.0 * 60.0);
    }

    // Duplicate and collinear vertices should be cleaned up.
    std::vector<XY> messy { {0,0}, {50,0}, {50,0}, {100,0}, {100,60}, {0,60} };
    run("rectangle with duplicate/collinear points", messy, s, true, 98.0 * 58.0, 15);

    // Too small for the settings -> invalid, no crash.
    std::vector<XY> tiny { {0,0}, {1.5,0}, {1.5,1.5}, {0,1.5} };
    run("tiny field", tiny, s, false);
    run("two points", { {0,0}, {10,0} }, s, false);

    // Narrow strip: narrower than one swath after the headland -> headland only.
    std::vector<XY> strip { {0,0}, {200,0}, {200,12}, {0,12} };
    run("12 m strip", strip, s, true);

    // KML-style boundary: 240 closely spaced points around a 70 m radius circle,
    // with a little GPS jitter. Short edges used to make the inset fail.
    std::vector<XY> dense;
    for (int i = 0; i < 240; i++) {
        const double a = 2 * M_PI * i / 240;
        const double jitter = 0.15 * std::sin(i * 7.3);
        dense.push_back({ (70 + jitter) * std::cos(a), (70 + jitter) * std::sin(a) });
    }
    run("dense 240-point circle (KML-like)", dense, s, true);
    Settings wide = s; wide.swathWidthM = 9; wide.edgeMarginM = 4; wide.passAngleDeg = 15;
    run("dense circle, 9 m swath, 4 m margin", dense, wide, true);

    // Dense wavy field edge (like a traced creek line) on one side.
    std::vector<XY> wavy { {0,0}, {150,0}, {150,90} };
    for (int i = 0; i <= 150; i++) wavy.push_back({ 150.0 - i, 90 + 4 * std::sin(i * 0.35) });
    run("rectangle with dense wavy edge", wavy, s, true);

    // Offset: passes slide sideways and are refitted inside the spray area.
    {
        for (double off : { 1.0, -1.0, 2.9, 3.0, -2.5, 5.9, 13.0 }) {
            char name[96];
            Settings t = noHead; t.offsetM = off;
            std::snprintf(name, sizeof name, "rectangle, offset %.1f m", off);
            run(name, rect, t, true, 98.0 * 58.0, -1, 0.975);
            Settings h = s; h.offsetM = off; h.passAngleDeg = 37;
            std::snprintf(name, sizeof name, "rectangle, headland, 37 deg, offset %.1f m", off);
            run(name, rect, h, true, 98.0 * 58.0, -1, 0.975);
        }
        const std::vector<XY> bayField { {0,0}, {200,0}, {200,150}, {130,150}, {120,90}, {100,70}, {80,90}, {70,150}, {0,150} };
        Settings bayOff = noHead; bayOff.passAngleDeg = 90; bayOff.offsetM = 2.0;
        run("field with a bay, 90 deg, offset 2 m", bayField, bayOff, true, -1, -1, 0.975);

        std::printf("offset geometry\n");
        // Passes run north (0 deg), so + offset moves them east. Spray area x = 1..99, 6 m swath.
        auto passXs = [&](double off) {
            Settings t = noHead; t.offsetM = off;
            const Result r = generate(fromMetres(rect), t);
            std::vector<double> xs;
            for (size_t i = 1; i < r.flightPath.size(); ++i) {
                if (!r.legSpray[i - 1]) continue;
                const XY a = toM(r.flightPath[i - 1]), b = toM(r.flightPath[i]);
                xs.push_back((a.x + b.x) / 2.0);
            }
            std::sort(xs.begin(), xs.end());
            return xs;
        };
        const auto x0 = passXs(0.0), x1 = passXs(1.0), xm1 = passXs(-1.0), x7 = passXs(7.0), x3 = passXs(3.0);
        CHECK(!x0.empty() && std::fabs(x0.front() - 4.0) < 0.01, "offset 0: first pass at x=%.2f, expected 4", x0.empty() ? -1 : x0.front());
        CHECK(!x1.empty() && std::fabs(x1.front() - 5.0) < 0.01, "offset +1: first pass at x=%.2f, expected 5 (moved east)", x1.empty() ? -1 : x1.front());
        CHECK(xm1.size() >= 2 && std::fabs(xm1[0] - 4.0) < 0.01 && std::fabs(xm1[1] - 9.0) < 0.01,
              "offset -1: passes start %.2f, %.2f, expected 4 (edge fill) then 9", xm1.size() > 0 ? xm1[0] : -1, xm1.size() > 1 ? xm1[1] : -1);
        CHECK(x7.size() == x1.size(), "offset 7 (= 1 + a swath) gives %zu passes, offset 1 gives %zu", x7.size(), x1.size());
        for (size_t i = 0; i < std::min(x7.size(), x1.size()); ++i) {
            CHECK(std::fabs(x7[i] - x1[i]) < 0.01, "offset 7 vs 1, pass %zu: %.2f vs %.2f", i, x7[i], x1[i]);
        }
        CHECK(x3.size() >= 2 && std::fabs(x3[0] - 4.0) < 0.01 && std::fabs(x3[1] - 7.0) < 0.01,
              "offset 3: passes start %.2f, %.2f, expected 4 then 7", x3.size() > 0 ? x3[0] : -1, x3.size() > 1 ? x3[1] : -1);
        for (const auto* xs : { &x0, &x1, &xm1, &x3 }) {
            for (double x : *xs) {
                CHECK(x >= 4.0 - 0.01 && x <= 96.0 + 0.01, "pass at x=%.2f is less than half a swath inside the buffer", x);
            }
        }
    }

    // Start corners: the route can start at any end of a block's first or last pass.
    {
        std::printf("start corners\n");
        struct Case { const char* name; std::vector<XY> field; Settings set; size_t minOptions; };
        Settings bayNoHead = noHead;
        bayNoHead.passAngleDeg = 90;   // across the bay: it splits the passes into blocks
        const std::vector<XY> bayField { {0,0}, {200,0}, {200,150}, {130,150}, {120,90}, {100,70}, {80,90}, {70,150}, {0,150} };
        const std::vector<Case> cases {
            { "rectangle, no headland", rect, noHead, 4 },
            { "rectangle, headland",    rect, s,      4 },
            { "field with a bay",       bayField, bayNoHead, 8 },
        };
        for (const Case& c : cases) {
            const std::vector<LatLon> field = fromMetres(c.field);
            const Result plain = generate(field, c.set);
            CHECK(plain.startOptions.size() >= c.minOptions, "%s: %zu start options, expected >= %zu", c.name, plain.startOptions.size(), c.minOptions);
            // Default start is one of the options.
            bool defaultListed = false;
            for (const auto& o : plain.startOptions) {
                const XY a = toM(o), b = toM(plain.flightPath.front());
                defaultListed = defaultListed || std::hypot(a.x - b.x, a.y - b.y) < 0.01;
            }
            CHECK(defaultListed, "%s: default start not among the options", c.name);
            for (size_t k = 0; k < plain.startOptions.size(); ++k) {
                Settings t = c.set;
                t.hasStartNear = true;
                // Drop a little off the corner, like a finger would.
                const XY near = toM(plain.startOptions[k]);
                t.startNear = fromMetres({ { near.x + 1.5, near.y - 1.0 } }).front();
                const Result r = generate(field, t);
                CHECK(r.valid, "%s option %zu: invalid", c.name, k);
                if (!r.valid) continue;
                const XY got = toM(r.flightPath.front());
                CHECK(std::hypot(got.x - near.x, got.y - near.y) < 0.01, "%s option %zu: starts %.2f m from the chosen corner",
                      c.name, k, std::hypot(got.x - near.x, got.y - near.y));
                CHECK(r.passCount == plain.passCount, "%s option %zu: %d passes, expected %d", c.name, k, r.passCount, plain.passCount);
                CHECK(std::fabs(r.sprayDistanceM - plain.sprayDistanceM) < 0.5, "%s option %zu: spray length %.1f vs %.1f",
                      c.name, k, r.sprayDistanceM, plain.sprayDistanceM);
                CHECK(coverage(r, t.swathWidthM) > 0.99, "%s option %zu: coverage %.3f", c.name, k, coverage(r, t.swathWidthM));
            }
        }
    }

    // Side bearings: passes set to a side's bearing run parallel to that side.
    {
        std::printf("side bearings\n");
        const std::vector<XY> sq { {0,0}, {100,0}, {100,60}, {0,60} };
        const std::vector<double> sqB = sideBearingsDeg(fromMetres(sq));
        CHECK(sqB.size() == 4, "bearing count %zu", sqB.size());
        if (sqB.size() == 4) {
            CHECK(std::fabs(sqB[0] -  90.0) < 1e-6, "east side %.6f", sqB[0]);
            CHECK(std::fabs(sqB[1] -   0.0) < 1e-6, "north side %.6f", sqB[1]);
            CHECK(std::fabs(sqB[2] - 270.0) < 1e-6, "west side %.6f", sqB[2]);
            CHECK(std::fabs(sqB[3] - 180.0) < 1e-6, "south side %.6f", sqB[3]);
        }
        const std::vector<XY> skewQuad { {0,0}, {140,25}, {120,110}, {-15,80} };
        const std::vector<LatLon> field = fromMetres(skewQuad);
        const std::vector<double> bearings = sideBearingsDeg(field);
        for (size_t side = 0; side < bearings.size(); ++side) {
            Settings t = s;
            t.headlandPass = false;
            t.passAngleDeg = bearings[side];
            const Result r = generate(field, t);
            CHECK(r.valid, "side %zu: invalid", side);
            // Longest spray leg = a pass; compare its direction with the side's.
            double bestLen = 0, legDir = 0;
            for (size_t i = 1; i < r.flightPath.size(); ++i) {
                if (!r.legSpray[i - 1]) continue;
                const XY a = toM(r.flightPath[i - 1]), b = toM(r.flightPath[i]);
                const double len = std::hypot(b.x - a.x, b.y - a.y);
                if (len > bestLen) { bestLen = len; legDir = std::atan2(b.x - a.x, b.y - a.y); }
            }
            const XY a = skewQuad[side], b = skewQuad[(side + 1) % skewQuad.size()];
            const double sideDir = std::atan2(b.x - a.x, b.y - a.y);
            const double cross = std::fabs(std::sin(legDir - sideDir));   // 0 when parallel either way
            CHECK(cross < 1e-4, "side %zu: passes %.4f deg off parallel", side, std::asin(cross) * 180 / M_PI);
        }
        CHECK(sideBearingsDeg(fromMetres({ {0,0}, {10,0} })).empty(), "two points should give no bearings");
        const std::vector<double> dup = sideBearingsDeg(fromMetres({ {0,0}, {0,0}, {10,0}, {10,10} }));
        CHECK(dup.size() == 4 && std::isnan(dup[0]), "zero-length side should be NaN");
    }

    // ---- resuming a job: only what's left is planned ----------------------------
    {
        std::printf("resume\n");
        const std::vector<XY> rect { {0,0}, {200,0}, {200,100}, {0,100} };
        const std::vector<LatLon> field = fromMetres(rect);
        Settings base;
        base.swathWidthM = 10.0;
        base.passAngleDeg = 0.0;
        base.edgeMarginM = 1.0;
        base.headlandPass = false;
        const Result full = generate(field, base);
        CHECK(full.valid, "full plan invalid");

        // Strips from the spray legs of a route, up to (not including) leg `upTo`.
        auto flown = [](const Result& r, size_t upTo, double width) {
            std::vector<Strip> strips;
            for (size_t i = 1; i < r.flightPath.size() && i - 1 < upTo; ++i) {
                if (r.legSpray[i - 1]) strips.push_back({ r.flightPath[i - 1], r.flightPath[i], width });
            }
            return strips;
        };
        // Share of the spray area covered by old strips plus the new route's spray legs.
        auto combined = [](const Result& r, const std::vector<Strip>& old, double swath) {
            std::vector<XY> area, path;
            for (auto& p : r.sprayArea) area.push_back(toM(p));
            for (auto& p : r.flightPath) path.push_back(toM(p));
            double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
            for (auto& p : area) { minX = std::fmin(minX, p.x); maxX = std::fmax(maxX, p.x); minY = std::fmin(minY, p.y); maxY = std::fmax(maxY, p.y); }
            int inside = 0, covered = 0;
            for (double x = minX + 0.25; x <= maxX; x += 0.5) {
                for (double y = minY + 0.25; y <= maxY; y += 0.5) {
                    const XY p { x, y };
                    if (!pointInPoly(p, area)) continue;
                    inside++;
                    bool c = false;
                    for (const Strip& st : old) {
                        if (segDist(p, toM(st.a), toM(st.b)) <= st.widthM / 2.0 + 0.05) { c = true; break; }
                    }
                    for (size_t i = 1; !c && i < path.size(); ++i) {
                        if (r.legSpray[i - 1] && segDist(p, path[i - 1], path[i]) <= swath / 2.0 + 0.05) c = true;
                    }
                    covered += c;
                }
            }
            return inside ? double(covered) / inside : 0.0;
        };

        // Half the legs flown, same settings: the rest of the passes, starting where the drone waits.
        const size_t half = full.legSpray.size() / 2;
        const std::vector<Strip> done = flown(full, half, base.swathWidthM);
        Settings resume = base;
        resume.sprayed = done;
        resume.hasResumeFrom = true;
        resume.resumeFrom = full.flightPath[half];
        const Result rest = generate(field, resume);
        CHECK(rest.valid, "resume plan invalid");
        CHECK(!rest.allSprayed, "resume: should not be all sprayed");
        if (rest.valid) {
            const XY first = toM(rest.flightPath.front()), wait = toM(full.flightPath[half]);
            CHECK(std::hypot(first.x - wait.x, first.y - wait.y) < 0.01, "resume: route should start where the drone waits");
            CHECK(!rest.legSpray.front(), "resume: first leg should be transit");
            const int flownPasses = int(done.size());
            CHECK(rest.passCount == full.passCount - flownPasses, "resume: passes %d expected %d", rest.passCount, full.passCount - flownPasses);
            const double cov = combined(rest, done, base.swathWidthM);
            std::printf("  same settings: %d passes left, combined coverage %.4f, done %.0f m2\n", rest.passCount, cov, rest.sprayedDoneM2);
            CHECK(cov > 0.985, "resume same settings coverage %.4f", cov);
            CHECK(rest.sprayedDoneM2 > 0.3 * rest.sprayAreaM2 && rest.sprayedDoneM2 < 0.7 * rest.sprayAreaM2, "done area %.0f", rest.sprayedDoneM2);
        }

        // New swath and pass direction for the rest: still no real gap.
        Settings changed = resume;
        changed.swathWidthM = 7.0;
        changed.passAngleDeg = 90.0;
        const Result turned = generate(field, changed);
        CHECK(turned.valid, "resume turned invalid");
        if (turned.valid) {
            const double cov = combined(turned, done, changed.swathWidthM);
            std::printf("  new swath and angle: %d passes, combined coverage %.4f\n", turned.passCount, cov);
            CHECK(cov > 0.98, "resume turned coverage %.4f", cov);
            // The new passes shouldn't re-fly the sprayed half.
            std::vector<XY> path;
            for (auto& p : turned.flightPath) path.push_back(toM(p));
            double sprayLen = 0, overLen = 0;
            for (size_t i = 1; i < path.size(); ++i) {
                if (!turned.legSpray[i - 1]) continue;
                const double len = std::hypot(path[i].x - path[i-1].x, path[i].y - path[i-1].y);
                const int n = std::max(1, int(len / 0.5));
                for (int k = 0; k < n; ++k) {
                    const double t = (k + 0.5) / n;
                    const XY q { path[i-1].x + t * (path[i].x - path[i-1].x), path[i-1].y + t * (path[i].y - path[i-1].y) };
                    bool in = false;
                    for (const Strip& st : done) if (segDist(q, toM(st.a), toM(st.b)) <= st.widthM / 2.0 - 1.0) { in = true; break; }
                    overLen += in ? len / n : 0;
                }
                sprayLen += len;
            }
            std::printf("  re-sprayed %.1f m of %.1f m\n", overLen, sprayLen);
            CHECK(overLen < 0.05 * sprayLen, "resume turned re-sprays %.1f of %.1f m", overLen, sprayLen);
        }

        // Stopped halfway along a pass: the rest of that pass is still planned.
        {
            size_t leg = half;
            while (leg < full.legSpray.size() && !full.legSpray[leg]) leg++;
            std::vector<Strip> partial = flown(full, leg, base.swathWidthM);
            const XY a = toM(full.flightPath[leg]), b = toM(full.flightPath[leg + 1]);
            const XY mid { (a.x + b.x) / 2, (a.y + b.y) / 2 };
            const LatLon midLL = fromMetres({ mid })[0];
            partial.push_back({ full.flightPath[leg], midLL, base.swathWidthM });
            Settings t = base;
            t.sprayed = partial;
            t.hasResumeFrom = true;
            t.resumeFrom = midLL;
            const Result r = generate(field, t);
            CHECK(r.valid, "partial resume invalid");
            if (r.valid) {
                const double cov = combined(r, partial, base.swathWidthM);
                std::printf("  stopped mid-pass: combined coverage %.4f\n", cov);
                CHECK(cov > 0.985, "partial coverage %.4f", cov);
                // First spray leg continues the interrupted pass from about the stop point.
                for (size_t i = 1; i < r.flightPath.size(); ++i) {
                    if (!r.legSpray[i - 1]) continue;
                    const XY p0 = toM(r.flightPath[i - 1]), p1 = toM(r.flightPath[i]);
                    const bool fromMid = std::hypot(p0.x - mid.x, p0.y - mid.y) < 1.5 || std::hypot(p1.x - mid.x, p1.y - mid.y) < 1.5;
                    CHECK(fromMid, "first pass should pick up at the stop point");
                    break;
                }
            }
        }

        // Everything sprayed: nothing to plan.
        {
            Settings t = base;
            t.sprayed = flown(full, full.legSpray.size(), base.swathWidthM);
            t.hasResumeFrom = true;
            t.resumeFrom = full.flightPath.back();
            const Result r = generate(field, t);
            CHECK(!r.valid && r.allSprayed, "all sprayed: valid=%d allSprayed=%d", r.valid, r.allSprayed);
        }

        // With a headland: passes done and part of the ring: only the rest of the ring.
        {
            Settings h = base;
            h.headlandPass = true;
            const Result fullH = generate(field, h);
            CHECK(fullH.valid, "headland plan invalid");
            size_t ringStart = 0;
            int passesSeen = 0;
            for (size_t i = 0; i < fullH.legSpray.size(); ++i) {
                if (fullH.legSpray[i] && ++passesSeen == fullH.passCount) { ringStart = i + 1; break; }
            }
            const size_t ringLegs = fullH.legSpray.size() - ringStart;
            const size_t upTo = ringStart + ringLegs / 2;
            const std::vector<Strip> doneH = flown(fullH, upTo, h.swathWidthM);
            Settings t = h;
            t.sprayed = doneH;
            t.hasResumeFrom = true;
            t.resumeFrom = fullH.flightPath[upTo];
            const Result r = generate(field, t);
            CHECK(r.valid, "headland resume invalid");
            if (r.valid) {
                const double cov = combined(r, doneH, h.swathWidthM);
                std::printf("  headland half done: %d passes left, %.1f m to spray, combined coverage %.4f\n", r.passCount, r.sprayDistanceM, cov);
                CHECK(r.passCount == 0, "headland resume: %d passes left, expected 0", r.passCount);
                CHECK(cov > 0.985, "headland resume coverage %.4f", cov);
            }
        }
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

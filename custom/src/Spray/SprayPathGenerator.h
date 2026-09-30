#pragma once

// SprayPathGenerator
// -------------------
// Pure geometry for spray-area planning. No Qt dependencies, so it can be unit
// tested on its own.
//
// Given a field boundary (lat/lon) and spray settings it produces:
//   * sprayArea   - the boundary pulled in by the edge margin. This is the polygon
//                   the onboard app uses to decide when the pump is on.
//   * headland    - optional closed track flown half a swath inside sprayArea.
//   * passes      - back-and-forth ("boustrophedon") passes at swath spacing,
//                   in the requested direction, covering the rest of the area.
//   * flightPath  - every waypoint in flight order (passes, then headland).
//
// Geometry is done in a local flat (east/north metres) frame centred on the
// field, which is accurate to well under a metre for field-sized areas.

#include <vector>

namespace spray {

struct LatLon {
    double lat = 0.0;
    double lon = 0.0;
};

// A strip of the field that has already been sprayed: the centre-line from a
// to b, `widthM` wide (the swath it was sprayed with).
struct Strip {
    LatLon a;
    LatLon b;
    double widthM = 0.0;
};

struct Settings {
    double swathWidthM   = 6.0;   // spacing between pass centre-lines
    double passAngleDeg  = 0.0;   // pass direction, degrees clockwise from north
    double offsetM       = 0.0;   // slide the passes sideways (at 90 deg to them), metres;
                                  // + = to the right of the pass direction. Repeats every swath.
    double edgeMarginM   = 1.0;   // uniform buffer inside the boundary
    bool   headlandPass  = true;  // fly one pass around the edge of sprayArea
    // Optional per-side buffers, one per boundary side (side i runs from
    // boundary point i to i+1). A negative value means "use edgeMarginM".
    // Ignored unless it has exactly one entry per boundary point.
    std::vector<double> sideMarginsM;
    // Optional: start the route at the pass corner nearest this point (any end
    // of the first or last pass of any block). Without it the route starts at
    // the low end of the first pass.
    bool   hasStartNear  = false;
    LatLon startNear;
    // Resuming a job: the parts of the spray area already sprayed. Passes (and
    // the headland) are only laid out where there's still something to spray;
    // stretches over sprayed ground are dropped. Empty = a fresh job.
    std::vector<Strip> sprayed;
    // Resuming in the air: the route starts here (where the drone is waiting),
    // then goes inside the field to the nearest remaining pass.
    bool   hasResumeFrom = false;
    LatLon resumeFrom;
};

struct Result {
    bool                valid = false;   // false if the field is too small for the settings
    std::vector<LatLon> sprayArea;       // closed implicitly (last vertex != first)
    std::vector<LatLon> headland;        // closed explicitly (last == first) when present
    std::vector<LatLon> flightPath;      // waypoints in flight order
    std::vector<bool>   legSpray;        // per leg (flightPath[i] -> [i+1]): true = spray pass,
                                         // false = transit (to a pass, between passes)
    int                 passCount = 0;   // number of straight spray passes (excludes headland)
    std::vector<LatLon> startOptions;    // where the route can start: both ends of the first and
                                         // last pass of every block (see Settings::startNear)
    double              sprayAreaM2 = 0.0;
    double              flightDistanceM = 0.0;
    double              sprayDistanceM = 0.0;   // length of the legs marked spray
    double              sprayedDoneM2 = 0.0;    // part of the spray area already sprayed (Settings::sprayed)
    bool                allSprayed = false;     // resuming, and nothing is left to spray
};

Result generate(const std::vector<LatLon>& boundary, const Settings& settings);

// For each leg of `path` (path[i] -> path[i+1]), true if any part of it goes
// outside `boundary`. Used to flag hand-edited route sections.
std::vector<bool> legsOutside(const std::vector<LatLon>& boundary, const std::vector<LatLon>& path);

// ---- transit helpers ------------------------------------------------------
// All take lat/lon; `boundary` is the field boundary as drawn.

// True if p is inside (or on the edge of) the polygon.
bool pointInPolygon(const std::vector<LatLon>& polygon, const LatLon& p);

// Index of the boundary side closest to p (side i runs from point i to i+1),
// or -1 if the boundary has fewer than 3 points.
int nearestSide(const std::vector<LatLon>& boundary, const LatLon& p);

// The side a straight leg from `inside` (in the field) to `outside` leaves the
// field through: the last side it crosses. -1 if it doesn't cross the boundary.
int exitSide(const std::vector<LatLon>& boundary, const LatLon& inside, const LatLon& outside);

// Direction of each side (side i runs from point i to i+1), degrees clockwise
// from north in [0, 360). Measured in the same flat frame the passes are laid
// out in, so passes at this angle run exactly parallel to the side. NaN for a
// zero-length side; empty if the boundary has fewer than 3 points.
std::vector<double> sideBearingsDeg(const std::vector<LatLon>& boundary);

// Where to cross side `side` when flying from `outside` (e.g. takeoff) to
// `inside` (e.g. the spray start): the point that makes the straight leg to it
// plus the in-field leg from it shortest: where the straight line crosses the
// side when it does. Kept 2 m (or a quarter of a short side) off the corners.
LatLon gatePoint(const std::vector<LatLon>& boundary, int side, const LatLon& outside, const LatLon& inside);

// Points to fly from `from` to `to` without leaving the boundary (excluding
// `from`, ending with `to`). Goes around bays and inside corners; falls back to
// the straight leg if there's no way round (e.g. a point is outside).
// Same, choosing among several allowed sides: the crossing on whichever
// side gives the shortest way from `outside` to `inside`. Sides out of range
// are ignored; returns `inside` if none is usable.
LatLon bestGatePoint(const std::vector<LatLon>& boundary, const std::vector<int>& sides,
                     const LatLon& outside, const LatLon& inside);

std::vector<LatLon> routeInside(const std::vector<LatLon>& boundary, const LatLon& from, const LatLon& to);

// Closest point on the polygon's edge to p.
LatLon nearestPointOnPolygon(const std::vector<LatLon>& polygon, const LatLon& p);

// ---- helpers exposed for testing -------------------------------------------

struct XY {
    double x = 0.0;   // east, metres
    double y = 0.0;   // north, metres
};

// Signed area (m^2): positive for counter-clockwise polygons in the east/north frame.
double signedArea(const std::vector<XY>& poly);

// Move every edge of the polygon inward by `distance` metres (miter joins).
// Returns an empty vector if the polygon collapses.
std::vector<XY> insetPolygon(const std::vector<XY>& poly, double distance);
// Same, with each edge (i = vertex i -> i+1) moved in by its own distance.
std::vector<XY> insetPolygon(const std::vector<XY>& poly, const std::vector<double>& edgeDistances);

} // namespace spray

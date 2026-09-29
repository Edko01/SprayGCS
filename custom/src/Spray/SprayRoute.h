#pragma once

// SprayRoute
// ----------
// The editable flight route of a Spray Area. No Qt dependencies.
//
// A route is a list of points; every section between two consecutive points
// is either sprayed or not. It starts as the generated passes, and planners
// can then:
//   * turn the pump on or off for a section
//   * add a point on a section (to divide it) and drag it anywhere
//   * move any point
//   * delete a point (its neighbours are joined)
//   * delete a section (the route is joined around it with a no-spray leg)
//
// toMissionSteps() turns the route into the order of mission commands:
// a waypoint for every point, plus a spray-on/off marker wherever the state
// changes. The onboard app reads those markers; nothing is sent live.

#include <vector>

#include "SprayPathGenerator.h"

namespace spray {

class SprayRoute {
public:
    struct MissionStep {
        enum Kind { Waypoint, SprayOn, SprayOff };
        Kind kind  = Waypoint;
        int  point = -1;   // index into points() for Waypoint steps
    };

    // Start from generated points with default spray flags (one per section).
    // These are also what hasEdits() compares against.
    void reset(const std::vector<LatLon>& points, const std::vector<bool>& segmentSpray);

    // Load an edited route as saved. Returns false (route unchanged) if the
    // data is inconsistent: at least 2 points and one spray flag per section.
    bool load(const std::vector<LatLon>& points, const std::vector<bool>& segmentSpray);

    // Older plans saved only spray on/off edits, with extra points marked as
    // splits on the generated lines. Accepted only if the other points still
    // match the generated route.
    bool restoreLegacy(const std::vector<LatLon>& points, const std::vector<bool>& isSplitPoint,
                       const std::vector<bool>& segmentSpray);

    const std::vector<LatLon>& points()       const { return _points; }
    const std::vector<bool>&   segmentSpray() const { return _spray; }
    int pointCount()   const { return static_cast<int>(_points.size()); }
    int segmentCount() const { return static_cast<int>(_spray.size()); }

    // True if the planner changed anything from the generated route.
    bool hasEdits() const;

    bool toggleSegment(int segment);
    bool setSegmentSpray(int segment, bool spray);

    // Add a point on a section at `at`, dividing it in two. Both parts keep
    // the section's spray state. Returns the new point's index, or -1.
    int  insertPoint(int segment, const LatLon& at);
    // Move any point anywhere (except onto a neighbouring point).
    bool movePoint(int point, const LatLon& to);
    // Delete a point and join its neighbours. The joined section sprays only
    // if both sections it replaces did. The route keeps at least 2 points.
    bool removePoint(int point);
    // Delete a section: its two end points go, and the route is joined around
    // it with a no-spray leg. The route keeps at least 2 points.
    bool removeSegment(int segment);

    double segmentLengthM(int segment) const;
    double sprayLengthM() const;
    double totalLengthM() const;

    // The same route flown from the other end (spray states follow their sections).
    SprayRoute reversed() const;

    std::vector<MissionStep> toMissionSteps() const;
    int markerCount() const;

private:
    std::vector<LatLon> _points;
    std::vector<bool>   _spray;           // one per section
    std::vector<LatLon> _defaultPoints;   // as generated, for hasEdits()
    std::vector<bool>   _defaultSpray;
};

// Great-circle distance in metres (small-distance approximation is fine here).
double distanceM(const LatLon& a, const LatLon& b);

} // namespace spray

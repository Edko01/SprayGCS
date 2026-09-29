#pragma once

// SprayMission
// ------------
// Turns a Spray Area into the full list of mission steps: the transit from the
// takeoff point into the field, the spray route, and the way back. No Qt.
//
// Transit modes (like XAG One):
//   * Gate  (mode A): fly straight from takeoff to an entry side of the field
//     (one or more sides the pilot allows), cross it, and fly inside the field
//     to the start point, at transit height and speed. On the way back: climb
//     at the end point, fly inside the field to an entry side, then straight
//     to the takeoff point. Each way uses whichever allowed side is shortest.
//   * Route (mode B): follow the pilot's planned route from takeoff to its last
//     point (the entry point) at transit height and speed, then fly at spray
//     height and speed to the start point. On the way back: fly at spray height
//     and speed to the entry point, climb, and follow the route back.
//
// Return (until the onboard Field Return app replaces PX4's Return): a
// landing-sequence marker (LandStart, sent as MAV_CMD_DO_LAND_START) sits just
// before the way out: mode A before the exit gate, mode B before the climb at
// the entry point. With PX4's RTL_TYPE = 1, Return flies straight to the
// waypoint after the marker, then follows the plan's way home and lands (the
// plan ends with a Land at the takeoff point, not Return To Launch).
//
// Heights are above ground. PX4 missions hold them relative to the takeoff
// point; following the terrain is left to the onboard app. Transit is never
// flown lower than spray height. Legs flown at spray height stay inside the
// spray area (away from the field edge).
// The pump is only on along spray sections of the route; never in transit.

#include <vector>

#include "SprayPathGenerator.h"
#include "SprayRoute.h"

namespace spray {

enum class TransitMode { None, Gate, Route };

struct MissionInput {
    std::vector<LatLon> boundary;           // field boundary as drawn
    std::vector<LatLon> sprayArea;          // boundary minus buffer; spray-height transit stays inside it
    SprayRoute          route;              // spray route in flying order (start first)
    double              sprayAltM      = 3.0;
    double              spraySpeedMS   = 5.0;
    TransitMode         transit        = TransitMode::None;
    LatLon              takeoff;            // used unless transit is None
    double              transitAltM    = 10.0;
    double              transitSpeedMS = 8.0;
    std::vector<int>    gateSides;          // Gate mode: allowed entry/exit sides; empty = side nearest takeoff
    std::vector<LatLon> transitRoute;       // Route mode: points after takeoff; last = entry point
};

struct PlanStep {
    enum Kind { Speed, Waypoint, SprayOn, SprayOff, LandStart };
    Kind   kind     = Waypoint;
    LatLon pos;               // Waypoint
    double altM     = 0.0;    // Waypoint, above ground
    double speedMS  = 0.0;    // Speed
    bool   transit  = false;  // Waypoint belongs to a transit leg (for display)
};

std::vector<PlanStep> buildMission(const MissionInput& input);

struct MissionStats {
    double distanceM = 0.0;
    double seconds   = 0.0;
};
// Distance flown and time, assuming each leg at the speed in force and
// vertical-only legs at `climbRateMS`.
MissionStats missionStats(const std::vector<PlanStep>& steps, double climbRateMS = 2.0);

} // namespace spray

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
// the entry point. With PX4's RTL_TYPE = 1 (set by SprayGCS on upload), the
// drone's own Return flies straight to the waypoint after the marker, then
// follows the plan's way home and lands (the plan ends with a Land at the
// takeoff point, not Return To Launch). In mode A, Return pressed in SprayGCS
// instead leaves through the entry side nearest the drone (see
// SprayAreaComplexItem::returnViaEntrySide).
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

// Mode A's way back from `from` (e.g. a Return mid-job), mirroring the way in:
// inside the field to the point of the allowed sides best for `from`, across
// it, then straight to takeoff (the last point; `from` isn't included). From
// outside the field, or with no allowed side: straight to takeoff.
std::vector<LatLon> gateReturnPath(const std::vector<LatLon>& boundary, const std::vector<int>& sides,
                                   const LatLon& takeoff, const LatLon& from);

struct MissionStats {
    double distanceM = 0.0;
    double seconds   = 0.0;
};
// Distance flown and time, assuming each leg at the speed in force and
// vertical-only legs at `climbRateMS`.
MissionStats missionStats(const std::vector<PlanStep>& steps, double climbRateMS = 2.0);

// Trips. A spray drone's tank and battery last only part of a job: it sprays
// until one runs out, flies home, and after the refill or battery swap flies
// back to where it stopped. estimateTrips() simulates that along the route
// (straight out from takeoff and straight back, at transit height and speed)
// to tell the planner how many trips the job takes and how much to fill.
struct TripInput {
    SprayRoute route;                 // flying order; no-spray legs after the last sprayed one are ignored
    LatLon     takeoff;
    double     swathM         = 6.0;
    double     volumePerM2    = 0.0;  // application rate, in the tank's volume unit
    double     tankVolume     = 0.0;  // 0 = no limit
    double     batteryS       = 0.0;  // flight time per battery until it must head home; 0 = no limit
    double     spraySpeedMS   = 5.0;
    double     transitSpeedMS = 8.0;
    double     transitAltM    = 10.0; // climbed after takeoff, descended before landing
    double     climbRateMS    = 2.0;
};

struct TripEstimate {
    bool                valid        = false;  // false: a battery can't get to the field and back
    int                 trips        = 0;
    int                 tankTrips    = 0;      // trips (all but the last) ended by an empty tank...
    int                 batteryTrips = 0;      // ...or by the battery
    std::vector<LatLon> stops;                 // where each trip but the last runs out
    double              fullLoad     = 0.0;    // most sprayed on one trip
    double              lastLoad     = 0.0;    // sprayed on the last trip
    double              totalS       = 0.0;    // flying time of all trips, transits included
};

TripEstimate estimateTrips(const TripInput& in);

} // namespace spray

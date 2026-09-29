#include "SprayRoute.h"

#include <algorithm>
#include <cmath>

namespace spray {

namespace {
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kDegToRad     = 3.14159265358979323846 / 180.0;

bool samePoints(const std::vector<LatLon>& a, const std::vector<LatLon>& b, double toleranceM)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (distanceM(a[i], b[i]) > toleranceM) {
            return false;
        }
    }
    return true;
}
} // namespace

double distanceM(const LatLon& a, const LatLon& b)
{
    const double cosLat = std::cos((a.lat + b.lat) / 2.0 * kDegToRad);
    const double dx = (b.lon - a.lon) * kDegToRad * kEarthRadiusM * cosLat;
    const double dy = (b.lat - a.lat) * kDegToRad * kEarthRadiusM;
    return std::hypot(dx, dy);
}

void SprayRoute::reset(const std::vector<LatLon>& points, const std::vector<bool>& segmentSpray)
{
    _points = points;
    _spray  = segmentSpray;
    _spray.resize(points.size() > 1 ? points.size() - 1 : 0, false);
    _defaultPoints = _points;
    _defaultSpray  = _spray;
}

bool SprayRoute::load(const std::vector<LatLon>& points, const std::vector<bool>& segmentSpray)
{
    if (points.size() < 2 || segmentSpray.size() + 1 != points.size()) {
        return false;
    }
    _points = points;
    _spray  = segmentSpray;
    return true;
}

bool SprayRoute::restoreLegacy(const std::vector<LatLon>& points, const std::vector<bool>& isSplitPoint,
                               const std::vector<bool>& segmentSpray)
{
    if (points.size() < 2 || isSplitPoint.size() != points.size() || segmentSpray.size() + 1 != points.size()) {
        return false;
    }
    std::vector<LatLon> savedBase;
    for (size_t i = 0; i < points.size(); ++i) {
        if (!isSplitPoint[i]) {
            savedBase.push_back(points[i]);
        }
    }
    if (!samePoints(savedBase, _defaultPoints, 0.05)) {
        return false;
    }
    _points = points;
    _spray  = segmentSpray;
    return true;
}

bool SprayRoute::hasEdits() const
{
    return _spray != _defaultSpray || !samePoints(_points, _defaultPoints, 0.001);
}

bool SprayRoute::toggleSegment(int segment)
{
    if (segment < 0 || segment >= segmentCount()) {
        return false;
    }
    _spray[static_cast<size_t>(segment)] = !_spray[static_cast<size_t>(segment)];
    return true;
}

bool SprayRoute::setSegmentSpray(int segment, bool spray)
{
    if (segment < 0 || segment >= segmentCount()) {
        return false;
    }
    _spray[static_cast<size_t>(segment)] = spray;
    return true;
}

int SprayRoute::insertPoint(int segment, const LatLon& at)
{
    if (segment < 0 || segment >= segmentCount()) {
        return -1;
    }
    const size_t s = static_cast<size_t>(segment);
    // Refuse a point on top of an existing one; it would make a zero-length section.
    if (distanceM(at, _points[s]) < 0.3 || distanceM(at, _points[s + 1]) < 0.3) {
        return -1;
    }
    _points.insert(_points.begin() + static_cast<long>(s + 1), at);
    _spray.insert(_spray.begin() + static_cast<long>(s + 1), static_cast<bool>(_spray[s]));
    return segment + 1;
}

bool SprayRoute::movePoint(int point, const LatLon& to)
{
    if (point < 0 || point >= pointCount()) {
        return false;
    }
    const size_t p = static_cast<size_t>(point);
    // Refuse dropping a point onto a neighbour; it would make a zero-length section.
    if ((p > 0 && distanceM(to, _points[p - 1]) < 0.3) || (p + 1 < _points.size() && distanceM(to, _points[p + 1]) < 0.3)) {
        return false;
    }
    _points[p] = to;
    return true;
}

bool SprayRoute::removePoint(int point)
{
    if (point < 0 || point >= pointCount() || pointCount() <= 2) {
        return false;
    }
    const size_t p = static_cast<size_t>(point);
    if (p == 0) {
        _spray.erase(_spray.begin());
    } else if (p == _points.size() - 1) {
        _spray.pop_back();
    } else {
        // Sections p-1 and p become one; spray it only if both were sprayed.
        _spray[p - 1] = _spray[p - 1] && _spray[p];
        _spray.erase(_spray.begin() + static_cast<long>(p));
    }
    _points.erase(_points.begin() + static_cast<long>(p));
    return true;
}

bool SprayRoute::removeSegment(int segment)
{
    if (segment < 0 || segment >= segmentCount() || pointCount() < 4) {
        return false;
    }
    const size_t s = static_cast<size_t>(segment);
    const size_t n = _points.size();
    if (s == 0) {
        // First section: the route now starts at what was point 2.
        _spray.erase(_spray.begin(), _spray.begin() + 2);
    } else if (s == n - 2) {
        // Last section: the route now ends at what was point n-3.
        _spray.erase(_spray.end() - 2, _spray.end());
    } else {
        // Sections s-1, s and s+1 become one no-spray leg from point s-1 to s+2.
        _spray.erase(_spray.begin() + static_cast<long>(s), _spray.begin() + static_cast<long>(s + 2));
        _spray[s - 1] = false;
    }
    _points.erase(_points.begin() + static_cast<long>(s), _points.begin() + static_cast<long>(s + 2));
    return true;
}

double SprayRoute::segmentLengthM(int segment) const
{
    if (segment < 0 || segment >= segmentCount()) {
        return 0.0;
    }
    const size_t s = static_cast<size_t>(segment);
    return distanceM(_points[s], _points[s + 1]);
}

double SprayRoute::sprayLengthM() const
{
    double total = 0.0;
    for (int i = 0; i < segmentCount(); ++i) {
        if (_spray[static_cast<size_t>(i)]) {
            total += segmentLengthM(i);
        }
    }
    return total;
}

double SprayRoute::totalLengthM() const
{
    double total = 0.0;
    for (int i = 0; i < segmentCount(); ++i) {
        total += segmentLengthM(i);
    }
    return total;
}

SprayRoute SprayRoute::reversed() const
{
    std::vector<LatLon> points(_points.rbegin(), _points.rend());
    std::vector<bool>   spray(_spray.rbegin(), _spray.rend());
    SprayRoute r;
    r.reset(points, spray);
    return r;
}

std::vector<SprayRoute::MissionStep> SprayRoute::toMissionSteps() const
{
    std::vector<MissionStep> steps;
    bool on = false;   // the pump is off when the route starts
    for (size_t i = 0; i < _points.size(); ++i) {
        steps.push_back({ MissionStep::Waypoint, static_cast<int>(i) });
        // State of the section leaving this point (off after the last point).
        const bool next = i < _spray.size() ? static_cast<bool>(_spray[i]) : false;
        if (next != on) {
            steps.push_back({ next ? MissionStep::SprayOn : MissionStep::SprayOff, -1 });
            on = next;
        }
    }
    return steps;
}

int SprayRoute::markerCount() const
{
    int count = 0;
    for (const auto& step : toMissionSteps()) {
        if (step.kind != MissionStep::Waypoint) {
            ++count;
        }
    }
    return count;
}

} // namespace spray

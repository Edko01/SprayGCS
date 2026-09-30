#include "SprayPathGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace spray {

namespace {

constexpr double kEarthRadiusM = 6371000.0;
constexpr double kPi           = 3.14159265358979323846;
constexpr double kDegToRad     = kPi / 180.0;
constexpr double kEps          = 1e-9;
constexpr double kGateCornerMarginM = 2.0;   // how close the transit may cross the boundary to a corner

// ---- local flat projection -------------------------------------------------

struct Projection {
    double lat0 = 0.0;
    double lon0 = 0.0;
    double cosLat0 = 1.0;

    XY toXY(const LatLon& p) const {
        return { (p.lon - lon0) * kDegToRad * kEarthRadiusM * cosLat0,
                 (p.lat - lat0) * kDegToRad * kEarthRadiusM };
    }
    LatLon toLatLon(const XY& p) const {
        return { lat0 + p.y / (kEarthRadiusM * kDegToRad),
                 lon0 + p.x / (kEarthRadiusM * kDegToRad * cosLat0) };
    }
};

Projection makeProjection(const std::vector<LatLon>& pts)
{
    Projection proj;
    for (const auto& p : pts) {
        proj.lat0 += p.lat;
        proj.lon0 += p.lon;
    }
    proj.lat0 /= static_cast<double>(pts.size());
    proj.lon0 /= static_cast<double>(pts.size());
    proj.cosLat0 = std::cos(proj.lat0 * kDegToRad);
    return proj;
}

double dist(const XY& a, const XY& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// Remove consecutive duplicates and (near-)collinear vertices; these break miter joins.
// `edgeValues` (optional, one per edge i = vertex i -> i+1) is kept in step:
// when two edges are joined the larger value is kept.
std::vector<XY> cleanPolygon(std::vector<XY> poly, std::vector<double>* edgeValues = nullptr)
{
    bool changed = true;
    while (changed && poly.size() >= 3) {
        changed = false;
        for (size_t i = 0; i < poly.size() && poly.size() >= 3; ++i) {
            const XY& prev = poly[(i + poly.size() - 1) % poly.size()];
            const XY& cur  = poly[i];
            const XY& next = poly[(i + 1) % poly.size()];
            const double cross = (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
            const double scale = dist(prev, cur) * dist(cur, next);
            if (dist(prev, cur) < 1e-3 || scale < kEps || std::fabs(cross) / scale < 1e-6) {
                if (edgeValues && edgeValues->size() == poly.size()) {
                    // Edges i-1 (prev->cur) and i (cur->next) become one edge prev->next.
                    auto& v = *edgeValues;
                    const size_t before = (i + v.size() - 1) % v.size();
                    v[before] = std::max(v[before], v[i]);
                    v.erase(v.begin() + static_cast<long>(i));
                }
                poly.erase(poly.begin() + static_cast<long>(i));
                changed = true;
                break;
            }
        }
    }
    return poly;
}

// Douglas-Peucker simplification of a closed ring: drops points that sit within
// `tolerance` metres of the straight line between their neighbours.
std::vector<XY> simplifyRing(const std::vector<XY>& ring, double tolerance)
{
    if (ring.size() < 4) {
        return ring;
    }
    auto segDist = [](const XY& p, const XY& a, const XY& b) {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
    };
    // Split the ring at vertex 0 and its farthest vertex, simplify both halves.
    size_t far = 0;
    double farDist = 0.0;
    for (size_t i = 1; i < ring.size(); ++i) {
        const double d = dist(ring[0], ring[i]);
        if (d > farDist) { farDist = d; far = i; }
    }
    std::vector<bool> keep(ring.size(), false);
    keep[0] = keep[far] = true;
    std::vector<std::pair<size_t, size_t>> stack { { 0, far }, { far, ring.size() } };
    while (!stack.empty()) {
        auto [s, e] = stack.back();
        stack.pop_back();
        const XY& a = ring[s];
        const XY& b = ring[e % ring.size()];
        double maxD = 0.0;
        size_t idx = s;
        for (size_t i = s + 1; i < e; ++i) {
            const double d = segDist(ring[i], a, b);
            if (d > maxD) { maxD = d; idx = i; }
        }
        if (maxD > tolerance) {
            keep[idx] = true;
            stack.push_back({ s, idx });
            stack.push_back({ idx, e });
        }
    }
    std::vector<XY> out;
    for (size_t i = 0; i < ring.size(); ++i) {
        if (keep[i]) out.push_back(ring[i]);
    }
    return out.size() >= 3 ? out : ring;
}

// Make the polygon counter-clockwise (interior on the left of every edge).
// `edgeValues` (optional, one per edge) is reordered to match.
std::vector<XY> toCCW(std::vector<XY> poly, std::vector<double>* edgeValues = nullptr)
{
    if (signedArea(poly) < 0) {
        std::reverse(poly.begin(), poly.end());
        if (edgeValues && edgeValues->size() == poly.size()) {
            // Reversed edge k runs along old edge n-2-k.
            const size_t n = poly.size();
            std::vector<double> reordered(n);
            for (size_t k = 0; k < n; ++k) {
                reordered[k] = (*edgeValues)[(2 * n - 2 - k) % n];
            }
            *edgeValues = reordered;
        }
    }
    return poly;
}

// ---- staying inside the field -----------------------------------------------
//
// Transit legs (to the next pass, around a bay, to the headland) must stay
// inside the spray area. InsideRouter checks whether a straight leg stays
// inside, and if not, finds the shortest way round: in a simple polygon that
// path only ever bends at concave (reflex) corners, so a small visibility
// graph over those corners is enough.

class InsideRouter
{
public:
    explicit InsideRouter(const std::vector<XY>& ccwPolygon)
        : _poly(ccwPolygon)
    {
        const size_t n = _poly.size();
        for (size_t i = 0; i < n; ++i) {
            const XY& prev = _poly[(i + n - 1) % n];
            const XY& cur  = _poly[i];
            const XY& next = _poly[(i + 1) % n];
            const double cross = (cur.x - prev.x) * (next.y - cur.y) - (cur.y - prev.y) * (next.x - cur.x);
            if (cross < 0.0) {
                _reflex.push_back(cur);
            }
        }
    }

    /// True if the straight leg a-b stays inside (or on the edge of) the polygon.
    bool segmentInside(const XY& a, const XY& b) const
    {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double len2 = dx * dx + dy * dy;
        if (len2 < kTol * kTol) {
            return pointInside(a);
        }

        // Every place the leg meets the boundary splits it into pieces; each
        // piece is wholly inside or wholly outside, so test one point of each.
        std::vector<double> cuts { 0.0, 1.0 };
        const size_t n = _poly.size();
        for (size_t i = 0; i < n; ++i) {
            const XY& p = _poly[i];
            const XY& q = _poly[(i + 1) % n];

            // Polygon corner lying on the leg.
            const double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
            if (t > 0.0 && t < 1.0 && std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y) < kTol) {
                cuts.push_back(t);
            }

            // Edge crossing the leg.
            const double ex = q.x - p.x;
            const double ey = q.y - p.y;
            const double denom = dx * ey - dy * ex;
            if (std::fabs(denom) < kEps) {
                continue;   // parallel; overlaps are caught by the corner test
            }
            const double tl = ((p.x - a.x) * ey - (p.y - a.y) * ex) / denom;
            const double te = ((p.x - a.x) * dy - (p.y - a.y) * dx) / denom;
            if (tl > 0.0 && tl < 1.0 && te >= -kEps && te <= 1.0 + kEps) {
                cuts.push_back(tl);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            if (cuts[i + 1] - cuts[i] < 1e-12) {
                continue;
            }
            const double t = (cuts[i] + cuts[i + 1]) / 2.0;
            if (!pointInside({ a.x + t * dx, a.y + t * dy })) {
                return false;
            }
        }
        return true;
    }

    /// Points to fly through from `from` to `to` without leaving the polygon
    /// (excluding `from`, ending with `to`). Falls back to the straight leg if
    /// no way round is found.
    std::vector<XY> route(const XY& from, const XY& to)
    {
        if (segmentInside(from, to)) {
            return { to };
        }
        _buildGraph();

        // Nodes: reflex corners 0..r-1, then from (r) and to (r+1).
        const size_t r = _reflex.size();
        const size_t fromNode = r;
        const size_t toNode   = r + 1;
        auto nodePoint = [&](size_t i) -> const XY& { return i < r ? _reflex[i] : (i == fromNode ? from : to); };

        std::vector<std::vector<size_t>> extra(r + 2);   // edges touching from / to
        for (size_t i = 0; i < r; ++i) {
            if (segmentInside(from, _reflex[i])) {
                extra[fromNode].push_back(i);
            }
            if (segmentInside(_reflex[i], to)) {
                extra[i].push_back(toNode);
            }
        }

        std::vector<double> best(r + 2, std::numeric_limits<double>::max());
        std::vector<size_t> prev(r + 2, SIZE_MAX);
        std::vector<bool>   done(r + 2, false);
        best[fromNode] = 0.0;
        for (;;) {
            size_t u = SIZE_MAX;
            for (size_t i = 0; i < r + 2; ++i) {
                if (!done[i] && best[i] < std::numeric_limits<double>::max() && (u == SIZE_MAX || best[i] < best[u])) {
                    u = i;
                }
            }
            if (u == SIZE_MAX || u == toNode) {
                break;
            }
            done[u] = true;
            auto relax = [&](size_t w) {
                const double d = best[u] + dist(nodePoint(u), nodePoint(w));
                if (d < best[w]) {
                    best[w] = d;
                    prev[w] = u;
                }
            };
            if (u < r) {
                for (size_t w : _visible[u]) {
                    relax(w);
                }
            }
            for (size_t w : extra[u]) {
                relax(w);
            }
        }
        if (prev[toNode] == SIZE_MAX) {
            return { to };
        }
        std::vector<XY> out;
        for (size_t i = toNode; i != fromNode; i = prev[i]) {
            out.push_back(nodePoint(i));
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

private:
    static constexpr double kTol = 1e-6;   // metres

    bool pointInside(const XY& p) const
    {
        bool in = false;
        const size_t n = _poly.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const XY& a = _poly[i];
            const XY& b = _poly[j];
            // On the boundary counts as inside.
            const double ex = b.x - a.x;
            const double ey = b.y - a.y;
            const double l2 = ex * ex + ey * ey;
            double t = l2 > 0.0 ? ((p.x - a.x) * ex + (p.y - a.y) * ey) / l2 : 0.0;
            t = std::max(0.0, std::min(1.0, t));
            if (std::hypot(p.x - (a.x + t * ex), p.y - (a.y + t * ey)) < 1e-4) {
                return true;
            }
            if (((a.y > p.y) != (b.y > p.y)) && (p.x < ex * (p.y - a.y) / ey + a.x)) {
                in = !in;
            }
        }
        return in;
    }

    void _buildGraph()
    {
        if (_graphBuilt) {
            return;
        }
        _graphBuilt = true;
        _visible.assign(_reflex.size(), {});
        for (size_t i = 0; i < _reflex.size(); ++i) {
            for (size_t j = i + 1; j < _reflex.size(); ++j) {
                if (segmentInside(_reflex[i], _reflex[j])) {
                    _visible[i].push_back(j);
                    _visible[j].push_back(i);
                }
            }
        }
    }

    std::vector<XY>                  _poly;
    std::vector<XY>                  _reflex;
    std::vector<std::vector<size_t>> _visible;
    bool                             _graphBuilt = false;
};

// ---- resuming a job: what's already sprayed ----------------------------------
//
// The spray area on a fine grid, each cell outside, still to spray, or already
// sprayed (inside the rectangle, a swath wide, of a leg sprayed earlier). Lookups are O(1), so
// the new passes can be checked densely without polygon clipping.

class CoverageGrid
{
public:
    enum State : std::uint8_t { Outside = 0, Open = 1, Done = 2 };

    CoverageGrid(const std::vector<XY>& area, const std::vector<XY>& stripA,
                 const std::vector<XY>& stripB, const std::vector<double>& widths)
    {
        _minX = _minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();
        for (const XY& p : area) {
            _minX = std::min(_minX, p.x);
            _minY = std::min(_minY, p.y);
            maxX  = std::max(maxX, p.x);
            maxY  = std::max(maxY, p.y);
        }
        const double w = maxX - _minX;
        const double h = maxY - _minY;
        _res = std::max(0.25, std::sqrt(std::max(w * h, 1.0) / 4.0e6));   // at most ~4 million cells
        _nx  = static_cast<int>(std::ceil(w / _res)) + 1;
        _ny  = static_cast<int>(std::ceil(h / _res)) + 1;
        _cells.assign(static_cast<size_t>(_nx) * static_cast<size_t>(_ny), Outside);

        // The area, row by row (even-odd crossings of the row's centre line).
        const size_t n = area.size();
        std::vector<double> xs;
        for (int j = 0; j < _ny; ++j) {
            const double y = _minY + (j + 0.5) * _res;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                const XY& a = area[i];
                const XY& b = area[(i + 1) % n];
                if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                    xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int i0 = std::max(0, static_cast<int>(std::ceil((xs[k] - _minX) / _res - 0.5)));
                const int i1 = std::min(_nx - 1, static_cast<int>(std::floor((xs[k + 1] - _minX) / _res - 0.5)));
                for (int i = i0; i <= i1; ++i) {
                    _cells[_index(i, j)] = Open;
                }
            }
        }

        // The strips already sprayed.
        for (size_t k = 0; k < stripA.size(); ++k) {
            const XY&    a    = stripA[k];
            const XY&    b    = stripB[k];
            const double half = widths[k] / 2.0;
            if (half <= 0.0) {
                continue;
            }
            const double dx   = b.x - a.x;
            const double dy   = b.y - a.y;
            const double len  = std::hypot(dx, dy);
            if (len < kEps) {
                continue;
            }
            // The strip's rectangle, filled row by row (only the cells it touches).
            const XY nrm { -dy / len * half, dx / len * half };
            const XY corners[4] = { { a.x + nrm.x, a.y + nrm.y }, { b.x + nrm.x, b.y + nrm.y },
                                    { b.x - nrm.x, b.y - nrm.y }, { a.x - nrm.x, a.y - nrm.y } };
            double loY = corners[0].y;
            double hiY = corners[0].y;
            for (const XY& c : corners) {
                loY = std::min(loY, c.y);
                hiY = std::max(hiY, c.y);
            }
            const int j0 = std::max(0,       static_cast<int>(std::floor((loY - _minY) / _res)));
            const int j1 = std::min(_ny - 1, static_cast<int>(std::ceil ((hiY - _minY) / _res)));
            for (int j = j0; j <= j1; ++j) {
                const double y = _minY + (j + 0.5) * _res;
                double lo = std::numeric_limits<double>::max();
                double hi = std::numeric_limits<double>::lowest();
                for (int e = 0; e < 4; ++e) {
                    const XY& p = corners[e];
                    const XY& q = corners[(e + 1) % 4];
                    if ((p.y <= y && q.y >= y) || (q.y <= y && p.y >= y)) {
                        const double x = std::fabs(q.y - p.y) < kEps ? std::min(p.x, q.x) : p.x + (y - p.y) / (q.y - p.y) * (q.x - p.x);
                        const double x2 = std::fabs(q.y - p.y) < kEps ? std::max(p.x, q.x) : x;
                        lo = std::min(lo, x);
                        hi = std::max(hi, x2);
                    }
                }
                if (lo > hi) {
                    continue;
                }
                const int i0 = std::max(0,       static_cast<int>(std::ceil ((lo - _minX) / _res - 0.5)));
                const int i1 = std::min(_nx - 1, static_cast<int>(std::floor((hi - _minX) / _res - 0.5)));
                for (int i = i0; i <= i1; ++i) {
                    std::uint8_t& cell = _cells[_index(i, j)];
                    if (cell == Open) {
                        cell = Done;
                    }
                }
            }
        }
    }

    State at(const XY& p) const
    {
        const int i = static_cast<int>(std::floor((p.x - _minX) / _res));
        const int j = static_cast<int>(std::floor((p.y - _minY) / _res));
        if (i < 0 || j < 0 || i >= _nx || j >= _ny) {
            return Outside;
        }
        return static_cast<State>(_cells[_index(i, j)]);
    }

    double resolution() const { return _res; }

    double doneM2() const
    {
        return static_cast<double>(std::count(_cells.begin(), _cells.end(), static_cast<std::uint8_t>(Done))) * _res * _res;
    }

    /// Does a swath centred on p (across = unit vector across the flight
    /// direction) still have ground to spray? True when at least a quarter of
    /// the swath (of the part inside the spray area) hasn't been sprayed: the
    /// same allowance the generator uses for strips left along the edges.
    bool needsSpray(const XY& p, const XY& across, double swath) const
    {
        constexpr int kSamples = 9;
        int inside = 0;
        int open   = 0;
        for (int k = 0; k < kSamples; ++k) {
            const double o = swath * ((k + 0.5) / kSamples - 0.5);
            const State  st = at({ p.x + across.x * o, p.y + across.y * o });
            if (st != Outside) {
                inside++;
                if (st == Open) {
                    open++;
                }
            }
        }
        if (inside == 0) {
            return at(p) == Open;
        }
        return open >= 0.25 * inside;
    }

private:
    size_t _index(int i, int j) const { return static_cast<size_t>(j) * static_cast<size_t>(_nx) + static_cast<size_t>(i); }

    double                    _minX = 0.0;
    double                    _minY = 0.0;
    double                    _res  = 1.0;
    int                       _nx   = 0;
    int                       _ny   = 0;
    std::vector<std::uint8_t> _cells;
};

// Douglas-Peucker for an open polyline (keeps both ends).
std::vector<XY> simplifyLine(const std::vector<XY>& pts, double tolerance)
{
    if (pts.size() < 3) {
        return pts;
    }
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack { { 0, pts.size() - 1 } };
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        const XY& a = pts[first];
        const XY& b = pts[last];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double len = std::hypot(dx, dy);
        double worst = -1.0;
        size_t worstAt = first;
        for (size_t i = first + 1; i < last; ++i) {
            const double d = len > kEps ? std::fabs((pts[i].x - a.x) * dy - (pts[i].y - a.y) * dx) / len
                                        : dist(pts[i], a);
            if (d > worst) {
                worst   = d;
                worstAt = i;
            }
        }
        if (worst > tolerance) {
            keep[worstAt] = true;
            stack.push_back({ first, worstAt });
            stack.push_back({ worstAt, last });
        }
    }
    std::vector<XY> out;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (keep[i]) {
            out.push_back(pts[i]);
        }
    }
    return out;
}

} // namespace

// ---- public helpers --------------------------------------------------------

double signedArea(const std::vector<XY>& poly)
{
    double a = 0.0;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const XY& p = poly[i];
        const XY& q = poly[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2.0;
}

std::vector<XY> insetPolygon(const std::vector<XY>& input, double distance)
{
    return insetPolygon(input, std::vector<double>(input.size(), distance));
}

std::vector<XY> insetPolygon(const std::vector<XY>& input, const std::vector<double>& edgeDistances)
{
    std::vector<double> distances = edgeDistances;
    distances.resize(input.size(), 0.0);
    for (double& d : distances) {
        d = std::max(0.0, d);
    }
    std::vector<XY> poly = toCCW(cleanPolygon(input, &distances), &distances);
    const size_t n = poly.size();
    if (n < 3) {
        return {};
    }
    if (std::all_of(distances.begin(), distances.end(), [](double d) { return d <= 0.0; })) {
        return poly;
    }

    // Offset line for each edge (from vertex i to i+1): shifted along the
    // left-hand normal, which points into the polygon for CCW winding.
    struct Line { XY p; XY d; };
    std::vector<Line> lines;
    lines.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const XY& a = poly[i];
        const XY& b = poly[(i + 1) % n];
        const double len = dist(a, b);
        const XY dir { (b.x - a.x) / len, (b.y - a.y) / len };
        const XY normal { -dir.y, dir.x };
        lines.push_back({ { a.x + normal.x * distances[i], a.y + normal.y * distances[i] }, dir });
    }

    auto intersect = [](const Line& l1, const Line& l2) {
        const double denom = l1.d.x * l2.d.y - l1.d.y * l2.d.x;
        if (std::fabs(denom) < kEps) {
            return l2.p;  // parallel edges: keep the shifted point
        }
        const double t = ((l2.p.x - l1.p.x) * l2.d.y - (l2.p.y - l1.p.y) * l2.d.x) / denom;
        return XY { l1.p.x + l1.d.x * t, l1.p.y + l1.d.y * t };
    };

    // Short edges on curved or jagged boundaries can shrink past zero and flip
    // direction. When that happens the edge has vanished at this inset
    // distance, so drop it and join its neighbours directly. Repeat until stable.
    std::vector<XY> out;
    for (size_t pass = 0; pass <= n && lines.size() >= 3; ++pass) {
        const size_t m = lines.size();
        out.assign(m, XY{});
        for (size_t i = 0; i < m; ++i) {
            out[i] = intersect(lines[(i + m - 1) % m], lines[i]);  // start of edge i
        }
        std::vector<bool> flipped(m, false);
        bool anyFlipped = false;
        for (size_t i = 0; i < m; ++i) {
            const XY& a = out[i];
            const XY& b = out[(i + 1) % m];
            const double along = (b.x - a.x) * lines[i].d.x + (b.y - a.y) * lines[i].d.y;
            if (along <= 0.0) {
                flipped[i] = true;
                anyFlipped = true;
            }
        }
        if (!anyFlipped) {
            break;
        }
        std::vector<Line> kept;
        for (size_t i = 0; i < m; ++i) {
            if (!flipped[i]) {
                kept.push_back(lines[i]);
            }
        }
        if (kept.size() == m) {
            break;
        }
        lines.swap(kept);
    }
    if (lines.size() < 3 || out.size() != lines.size()) {
        return {};
    }

    // If the inset flipped over or shrank to (almost) nothing, the polygon collapsed.
    const double newArea = signedArea(out);
    if (newArea <= 1.0 || newArea >= signedArea(poly)) {
        return {};
    }
    return cleanPolygon(out);
}

// ---- generator -------------------------------------------------------------

Result generate(const std::vector<LatLon>& boundary, const Settings& s)
{
    Result result;
    if (boundary.size() < 3 || s.swathWidthM <= 0.1) {
        return result;
    }

    const Projection proj = makeProjection(boundary);
    std::vector<XY> field;
    field.reserve(boundary.size());
    for (const auto& p : boundary) {
        field.push_back(proj.toXY(p));
    }

    // 1. Spray area = field pulled in by the buffer: the uniform one, or a
    //    side's own buffer where it has one.
    std::vector<double> margins(field.size(), std::max(0.0, s.edgeMarginM));
    if (s.sideMarginsM.size() == field.size()) {
        for (size_t i = 0; i < margins.size(); ++i) {
            if (s.sideMarginsM[i] >= 0.0) {
                margins[i] = s.sideMarginsM[i];
            }
        }
    }
    const std::vector<XY> area = insetPolygon(field, margins);
    if (area.size() < 3) {
        return result;
    }

    InsideRouter router(toCCW(area));

    // Resuming a job: which parts of the spray area are already sprayed.
    std::optional<CoverageGrid> coverage;
    if (!s.sprayed.empty()) {
        std::vector<XY>     stripA;
        std::vector<XY>     stripB;
        std::vector<double> widths;
        for (const Strip& strip : s.sprayed) {
            stripA.push_back(proj.toXY(strip.a));
            stripB.push_back(proj.toXY(strip.b));
            widths.push_back(strip.widthM);
        }
        coverage.emplace(area, stripA, stripB, widths);
        result.sprayedDoneM2 = coverage->doneM2();
    }
    bool somethingToSpray = false;   // before leaving out what's already sprayed

    // 2. Optional headland track, half a swath inside the spray area.
    std::vector<XY> headlandTrack;
    if (s.headlandPass) {
        // Simplified so a dense (KML-traced) edge doesn't turn into hundreds of waypoints.
        headlandTrack = simplifyRing(insetPolygon(area, s.swathWidthM / 2.0), 0.5);
    }

    // 3. Region the straight passes cover. With a headland, the passes start
    //    where the headland's swath ends (one full swath in from the area edge).
    const std::vector<XY> passRegion = s.headlandPass ? insetPolygon(area, s.swathWidthM) : area;

    // 4. Straight passes. Work in a rotated frame: u = along the pass, v = across.
    const double theta = s.passAngleDeg * kDegToRad;
    const XY along  { std::sin(theta),  std::cos(theta) };   // pass direction (east, north)
    const XY across { std::cos(theta), -std::sin(theta) };   // 90 deg to the right
    auto u = [&](const XY& p) { return p.x * along.x + p.y * along.y; };
    auto v = [&](const XY& p) { return p.x * across.x + p.y * across.y; };
    auto fromUV = [&](double pu, double pv) {
        return XY { pu * along.x + pv * across.x, pu * along.y + pv * across.y };
    };

    std::vector<XY>   path;
    std::vector<bool> legs;   // legs[i] describes the leg from path[i-1] to path[i]; legs[0] unused
    auto addPoint = [&](const XY& p, bool sprayLegToHere) {
        path.push_back(p);
        legs.push_back(sprayLegToHere);
    };

    // Resuming in the air: start where the drone is waiting.
    if (s.hasResumeFrom) {
        addPoint(proj.toXY(s.resumeFrom), false);
    }

    if (passRegion.size() >= 3) {
        double vMin = std::numeric_limits<double>::max();
        double vMax = std::numeric_limits<double>::lowest();
        for (const auto& p : passRegion) {
            vMin = std::min(vMin, v(p));
            vMax = std::max(vMax, v(p));
        }

        // Pass centre-lines, half a swath in from each side, slid sideways by
        // the offset. Lines are kept at least half a swath inside the pass
        // region (so nothing is sprayed past the buffer); a strip left at
        // either side is covered by one extra (overlapping) pass.
        std::vector<double> lines;
        const double swath = s.swathWidthM;
        const double width = vMax - vMin;
        if (width <= swath) {
            lines.push_back((vMin + vMax) / 2.0);
        } else {
            // Only the offset within one swath matters: a whole swath is the same pattern.
            double phase = std::fmod(s.offsetM, swath);
            if (phase < 0.0) {
                phase += swath;
            }
            if (phase > swath - 1e-6) {
                phase = 0.0;
            }
            const double lo = vMin + swath / 2.0;   // first and last allowed centre-lines
            const double hi = vMax - swath / 2.0;
            if (phase > 0.25 * swath) {
                lines.push_back(lo);                // near-side strip
            }
            for (double c = lo + phase; c <= hi + 1e-6; c += swath) {
                lines.push_back(c);
            }
            if (lines.empty()) {
                lines.push_back((vMin + vMax) / 2.0);
            }
            const double covered = lines.back() + swath / 2.0;
            if (vMax - covered > 0.25 * swath) {
                lines.push_back(hi);                // far-side strip
            }
        }

        // Where each line is inside the pass region: one or more stretches.
        struct Stretch { double lo; double hi; };
        const size_t n = passRegion.size();
        std::vector<std::vector<Stretch>> stretches(lines.size());
        for (size_t li = 0; li < lines.size(); ++li) {
            const double c = lines[li];
            std::vector<double> hits;
            for (size_t i = 0; i < n; ++i) {
                const XY& a = passRegion[i];
                const XY& b = passRegion[(i + 1) % n];
                const double va = v(a);
                const double vb = v(b);
                if ((va <= c && vb > c) || (vb <= c && va > c)) {
                    const double t = (c - va) / (vb - va);
                    hits.push_back(u(a) + t * (u(b) - u(a)));
                }
            }
            std::sort(hits.begin(), hits.end());
            // Inside stretches are between hit pairs (0-1, 2-3, ...).
            for (size_t k = 0; k + 1 < hits.size(); k += 2) {
                if (hits[k + 1] - hits[k] > 0.5) {
                    stretches[li].push_back({ hits[k], hits[k + 1] });
                }
            }
            somethingToSpray = somethingToSpray || !stretches[li].empty();
        }

        // Resuming: keep only the parts of each stretch where the swath still
        // has ground to spray. Short sprayed gaps inside a stretch are flown
        // through (sprayed again) rather than split; tiny leftovers are dropped.
        if (coverage) {
            const double ds       = std::max(0.5, coverage->resolution());
            const double mergeGap = 3.0;
            const double minRun   = 2.0;
            for (size_t li = 0; li < lines.size(); ++li) {
                std::vector<Stretch> kept;
                for (const Stretch& st : stretches[li]) {
                    const int    samples = std::max(1, static_cast<int>(std::ceil((st.hi - st.lo) / ds)));
                    const double step    = (st.hi - st.lo) / samples;
                    std::vector<Stretch> runs;
                    bool   inRun = false;
                    double runLo = 0.0;
                    for (int k = 0; k < samples; ++k) {
                        const double pu   = st.lo + (k + 0.5) * step;
                        const bool   need = coverage->needsSpray(fromUV(pu, lines[li]), across, swath);
                        if (need && !inRun) {
                            inRun = true;
                            runLo = st.lo + k * step;
                        } else if (!need && inRun) {
                            inRun = false;
                            runs.push_back({ runLo, st.lo + k * step });
                        }
                    }
                    if (inRun) {
                        runs.push_back({ runLo, st.hi });
                    }
                    std::vector<Stretch> merged;
                    for (const Stretch& r : runs) {
                        if (!merged.empty() && r.lo - merged.back().hi < mergeGap) {
                            merged.back().hi = r.hi;
                        } else {
                            merged.push_back(r);
                        }
                    }
                    for (const Stretch& r : merged) {
                        if (r.hi - r.lo >= minRun) {
                            kept.push_back(r);
                        }
                    }
                }
                stretches[li] = kept;
            }
        }

        // Split the field into blocks ("cells") that can each be sprayed
        // back-and-forth without leaving it. A block continues from one line
        // to the next while the stretches overlap one-to-one; where a bay or
        // an inside corner splits or joins the stretches, new blocks start.
        struct Pass  { double c; Stretch s; };
        std::vector<std::vector<Pass>> cells;
        std::vector<int> prevCell;   // block index of each stretch on the previous line
        for (size_t li = 0; li < lines.size(); ++li) {
            const auto& cur = stretches[li];
            std::vector<int> curCell(cur.size(), -1);
            if (li > 0) {
                const auto& prv = stretches[li - 1];
                auto overlaps = [](const Stretch& a, const Stretch& b) { return std::max(a.lo, b.lo) < std::min(a.hi, b.hi); };
                for (size_t i = 0; i < cur.size(); ++i) {
                    int match = -1;
                    int count = 0;
                    for (size_t j = 0; j < prv.size(); ++j) {
                        if (overlaps(cur[i], prv[j])) {
                            match = static_cast<int>(j);
                            count++;
                        }
                    }
                    if (count != 1) {
                        continue;
                    }
                    int back = 0;
                    for (size_t k = 0; k < cur.size(); ++k) {
                        if (overlaps(cur[k], prv[static_cast<size_t>(match)])) {
                            back++;
                        }
                    }
                    if (back == 1) {
                        curCell[i] = prevCell[static_cast<size_t>(match)];
                    }
                }
            }
            for (size_t i = 0; i < cur.size(); ++i) {
                if (curCell[i] < 0) {
                    curCell[i] = static_cast<int>(cells.size());
                    cells.emplace_back();
                }
                cells[static_cast<size_t>(curCell[i])].push_back({ lines[li], cur[i] });
            }
            prevCell = curCell;
        }

        // Every block can be started from either end of its first or last pass.
        std::vector<XY> startCorners;
        for (const auto& cell : cells) {
            for (const Pass* pass : { &cell.front(), &cell.back() }) {
                for (const XY& corner : { fromUV(pass->s.lo, pass->c), fromUV(pass->s.hi, pass->c) }) {
                    const bool dup = std::any_of(startCorners.cbegin(), startCorners.cend(),
                                                 [&](const XY& q) { return dist(q, corner) < 0.01; });
                    if (!dup) {
                        startCorners.push_back(corner);
                    }
                }
            }
        }
        for (const XY& corner : startCorners) {
            result.startOptions.push_back(proj.toLatLon(corner));
        }
        const bool hasStartNear = s.hasStartNear && !cells.empty();
        const XY   startNear    = hasStartNear ? proj.toXY(s.startNear) : XY {};

        // Fly the blocks one after another, each time picking the block whose
        // nearest corner is closest. Transits are routed round bays and inside
        // corners so they never leave the spray area.
        std::vector<bool> cellDone(cells.size(), false);
        for (size_t done = 0; done < cells.size(); ++done) {
            size_t bestCell   = 0;
            bool   bestRev    = false;   // fly the block's lines in reverse order
            bool   bestFwd    = true;    // first pass runs low u -> high u
            double bestCost   = std::numeric_limits<double>::max();
            for (size_t ci = 0; ci < cells.size(); ++ci) {
                if (cellDone[ci]) {
                    continue;
                }
                for (int option = 0; option < 4; ++option) {
                    const bool rev = option & 1;
                    const bool fwd = option & 2;
                    const Pass& first = rev ? cells[ci].back() : cells[ci].front();
                    const XY    entry = fromUV(fwd ? first.s.lo : first.s.hi, first.c);
                    // First block: the corner nearest the pilot's chosen start, or
                    // like a plain field (first line, low end).
                    double cost = 0.0;
                    if (!path.empty()) {
                        cost = dist(path.back(), entry);
                    } else if (hasStartNear) {
                        cost = dist(startNear, entry);
                    } else {
                        cost = (rev ? 1.0 : 0.0) + (fwd ? 0.0 : 2.0) + static_cast<double>(ci) * 4.0;
                    }
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestCell = ci;
                        bestRev  = rev;
                        bestFwd  = fwd;
                    }
                }
            }
            cellDone[bestCell] = true;

            std::vector<Pass> passes = cells[bestCell];
            if (bestRev) {
                std::reverse(passes.begin(), passes.end());
            }
            bool forward = bestFwd;
            for (const Pass& pass : passes) {
                const XY start = fromUV(forward ? pass.s.lo : pass.s.hi, pass.c);
                const XY end   = fromUV(forward ? pass.s.hi : pass.s.lo, pass.c);
                if (path.empty()) {
                    addPoint(start, false);
                } else {
                    for (const XY& p : router.route(path.back(), start)) {
                        addPoint(p, false);   // transit to the start of the pass
                    }
                }
                addPoint(end, true);          // the pass itself
                result.passCount++;
                forward = !forward;
            }
        }
    }

    // 5. Headland after the passes, starting at the corner nearest the last pass.
    std::vector<XY> headlandRing;
    if (headlandTrack.size() >= 3) {
        size_t startIdx = 0;
        if (!path.empty()) {
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i < headlandTrack.size(); ++i) {
                const double d = dist(headlandTrack[i], path.back());
                if (d < best) {
                    best = d;
                    startIdx = i;
                }
            }
        }
        for (size_t k = 0; k <= headlandTrack.size(); ++k) {
            headlandRing.push_back(headlandTrack[(startIdx + k) % headlandTrack.size()]);
        }
        somethingToSpray = true;

        // Resuming: only the parts of the ring that still need spraying
        // (arcs); all of it is flown as a ring as before.
        std::vector<std::vector<XY>> arcs;
        bool wholeRing = true;
        if (coverage) {
            const double ds = std::max(0.5, coverage->resolution());
            std::vector<XY>   samples;
            std::vector<bool> need;
            for (size_t k = 0; k + 1 < headlandRing.size(); ++k) {
                const XY&    a   = headlandRing[k];
                const XY&    b   = headlandRing[k + 1];
                const double len = dist(a, b);
                if (len < kEps) {
                    continue;
                }
                const XY  dir    { (b.x - a.x) / len, (b.y - a.y) / len };
                const XY  across { dir.y, -dir.x };
                const int steps  = std::max(1, static_cast<int>(std::ceil(len / ds)));
                for (int i = 0; i < steps; ++i) {
                    const double t = static_cast<double>(i) / steps;
                    const XY     p { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
                    samples.push_back(p);
                    need.push_back(coverage->needsSpray(p, across, s.swathWidthM));
                }
            }
            const size_t m = samples.size();
            const size_t openCount = static_cast<size_t>(std::count(need.begin(), need.end(), true));
            if (openCount < m) {
                wholeRing = false;
                if (openCount > 0) {
                    // Walk the ring once from a sample that's already sprayed.
                    size_t first = 0;
                    while (need[first]) {
                        first++;
                    }
                    std::vector<XY> arc;
                    for (size_t n = 1; n <= m; ++n) {
                        const size_t i = (first + n) % m;
                        if (need[i]) {
                            arc.push_back(samples[i]);
                        }
                        if ((!need[i] || n == m) && !arc.empty()) {
                            if (!need[i]) {
                                arc.push_back(samples[i]);   // up to where it's sprayed
                            }
                            double length = 0.0;
                            for (size_t q = 1; q < arc.size(); ++q) {
                                length += dist(arc[q - 1], arc[q]);
                            }
                            if (length >= 2.0) {
                                arcs.push_back(simplifyLine(arc, 0.2));
                            }
                            arc.clear();
                        }
                    }
                }
            }
        }

        if (wholeRing) {
            if (path.empty()) {
                addPoint(headlandRing.front(), false);
            } else {
                for (const XY& p : router.route(path.back(), headlandRing.front())) {
                    addPoint(p, false);   // transit to the ring
                }
            }
            for (size_t k = 1; k < headlandRing.size(); ++k) {
                addPoint(headlandRing[k], true);   // spray around it
            }
        } else {
            // Fly the arcs nearest-first, each from whichever end is closer.
            std::vector<bool> arcDone(arcs.size(), false);
            for (size_t done = 0; done < arcs.size(); ++done) {
                size_t best    = 0;
                bool   bestRev = false;
                double bestD   = std::numeric_limits<double>::max();
                for (size_t a = 0; a < arcs.size(); ++a) {
                    if (arcDone[a]) {
                        continue;
                    }
                    for (bool rev : { false, true }) {
                        const XY&    end = rev ? arcs[a].back() : arcs[a].front();
                        const double d   = path.empty() ? static_cast<double>(a) : dist(path.back(), end);
                        if (d < bestD) {
                            bestD   = d;
                            best    = a;
                            bestRev = rev;
                        }
                    }
                }
                arcDone[best] = true;
                std::vector<XY> arc = arcs[best];
                if (bestRev) {
                    std::reverse(arc.begin(), arc.end());
                }
                if (path.empty()) {
                    addPoint(arc.front(), false);
                } else {
                    for (const XY& p : router.route(path.back(), arc.front())) {
                        addPoint(p, false);
                    }
                }
                for (size_t k = 1; k < arc.size(); ++k) {
                    addPoint(arc[k], true);
                }
            }
        }
    }

    const bool anySpray = std::find(legs.begin() + (legs.empty() ? 0 : 1), legs.end(), true) != legs.end();
    if (path.size() < 2 || !anySpray) {
        result.allSprayed = coverage.has_value() && somethingToSpray;
        return result;
    }

    // 6. Convert back to lat/lon and fill in the stats.
    for (const auto& p : area) {
        result.sprayArea.push_back(proj.toLatLon(p));
    }
    for (const auto& p : headlandRing) {
        result.headland.push_back(proj.toLatLon(p));
    }
    for (size_t i = 0; i < path.size(); ++i) {
        result.flightPath.push_back(proj.toLatLon(path[i]));
        if (i > 0) {
            const double legLength = dist(path[i - 1], path[i]);
            result.flightDistanceM += legLength;
            result.legSpray.push_back(legs[i]);
            if (legs[i]) {
                result.sprayDistanceM += legLength;
            }
        }
    }
    result.sprayAreaM2 = std::fabs(signedArea(area));
    result.valid = true;
    return result;
}

// ---- checking an edited route ----------------------------------------------

std::vector<bool> legsOutside(const std::vector<LatLon>& boundary, const std::vector<LatLon>& path)
{
    std::vector<bool> outside(path.size() > 1 ? path.size() - 1 : 0, false);
    if (boundary.size() < 3 || outside.empty()) {
        return outside;
    }
    const Projection proj = makeProjection(boundary);
    std::vector<XY> field;
    field.reserve(boundary.size());
    for (const auto& p : boundary) {
        field.push_back(proj.toXY(p));
    }
    field = toCCW(cleanPolygon(field));
    if (field.size() < 3) {
        return outside;
    }
    InsideRouter router(field);
    for (size_t i = 0; i < outside.size(); ++i) {
        outside[i] = !router.segmentInside(proj.toXY(path[i]), proj.toXY(path[i + 1]));
    }
    return outside;
}

// ---- transit helpers ---------------------------------------------------------

namespace {

struct FieldFrame {
    Projection      proj;
    std::vector<XY> raw;     // boundary as drawn, projected (side indices match)
    std::vector<XY> ccw;     // cleaned, counter-clockwise (for routing)
};

FieldFrame makeFrame(const std::vector<LatLon>& boundary)
{
    FieldFrame f;
    f.proj = makeProjection(boundary);
    for (const auto& p : boundary) {
        f.raw.push_back(f.proj.toXY(p));
    }
    f.ccw = toCCW(cleanPolygon(f.raw));
    return f;
}

XY closestOnSegment(const XY& p, const XY& a, const XY& b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0.0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    return { a.x + t * dx, a.y + t * dy };
}

double pathLength(const XY& from, const std::vector<XY>& pts)
{
    double total = 0.0;
    XY prev = from;
    for (const auto& p : pts) {
        total += dist(prev, p);
        prev = p;
    }
    return total;
}

} // namespace

bool pointInPolygon(const std::vector<LatLon>& polygon, const LatLon& p)
{
    if (polygon.size() < 3) {
        return false;
    }
    const FieldFrame f = makeFrame(polygon);
    const XY q = f.proj.toXY(p);
    bool in = false;
    const size_t n = f.raw.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const XY& a = f.raw[i];
        const XY& b = f.raw[j];
        if (dist(closestOnSegment(q, a, b), q) < 1e-4) {
            return true;
        }
        if (((a.y > q.y) != (b.y > q.y)) && (q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)) {
            in = !in;
        }
    }
    return in;
}

int nearestSide(const std::vector<LatLon>& boundary, const LatLon& p)
{
    if (boundary.size() < 3) {
        return -1;
    }
    const FieldFrame f = makeFrame(boundary);
    const XY q = f.proj.toXY(p);
    int best = -1;
    double bestDist = std::numeric_limits<double>::max();
    for (size_t i = 0; i < f.raw.size(); ++i) {
        const XY& a = f.raw[i];
        const XY& b = f.raw[(i + 1) % f.raw.size()];
        if (dist(a, b) < 1e-3) {
            continue;   // zero-length side
        }
        const double d = dist(closestOnSegment(q, a, b), q);
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::vector<double> sideBearingsDeg(const std::vector<LatLon>& boundary)
{
    std::vector<double> bearings;
    if (boundary.size() < 3) {
        return bearings;
    }
    // Same projection as generate(), so "parallel" means parallel on the passes' grid.
    const Projection proj = makeProjection(boundary);
    const size_t n = boundary.size();
    bearings.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const XY a = proj.toXY(boundary[i]);
        const XY b = proj.toXY(boundary[(i + 1) % n]);
        if (dist(a, b) < 1e-3) {
            bearings.push_back(std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        double deg = std::atan2(b.x - a.x, b.y - a.y) / kDegToRad;   // east over north: clockwise from north
        if (deg < 0.0) {
            deg += 360.0;
        }
        if (deg >= 360.0) {
            deg -= 360.0;
        }
        bearings.push_back(deg);
    }
    return bearings;
}

LatLon gatePoint(const std::vector<LatLon>& boundary, int side, const LatLon& outside, const LatLon& inside)
{
    if (boundary.size() < 3 || side < 0 || side >= static_cast<int>(boundary.size())) {
        return inside;
    }
    const FieldFrame f = makeFrame(boundary);
    const XY a  = f.raw[static_cast<size_t>(side)];
    const XY b  = f.raw[(static_cast<size_t>(side) + 1) % f.raw.size()];
    const XY po = f.proj.toXY(outside);
    const XY pi = f.proj.toXY(inside);
    const double len = dist(a, b);
    if (len < 1e-3) {
        return f.proj.toLatLon(a);
    }

    // Stay a little way off the corners (a fixed distance, so a long side
    // doesn't push the crossing far from where it should be).
    const double margin = std::min(kGateCornerMarginM, 0.25 * len);
    const double tMin = margin / len;
    const double tMax = 1.0 - tMin;

    InsideRouter router(f.ccw);
    auto cost = [&](double t) {
        const XY g { a.x + t * (b.x - a.x), a.y + t * (b.y - a.y) };
        return dist(po, g) + pathLength(g, router.route(g, pi));
    };

    // Candidates: where the straight line to the inside point crosses the
    // side (the ideal crossing when it's usable), plus points every metre or
    // so along the side for when the straight line misses it.
    std::vector<double> candidates;
    {
        const double dx = pi.x - po.x, dy = pi.y - po.y;
        const double ex = b.x - a.x,   ey = b.y - a.y;
        const double denom = dx * ey - dy * ex;
        if (std::fabs(denom) > kEps) {
            const double t = ((a.x - po.x) * dy - (a.y - po.y) * dx) / denom;
            candidates.push_back(std::max(tMin, std::min(tMax, t)));
        }
    }
    const int samples = std::max(20, std::min(400, static_cast<int>(len)));
    for (int k = 0; k <= samples; ++k) {
        candidates.push_back(tMin + (tMax - tMin) * k / samples);
    }

    double bestT = candidates.front();
    double bestCost = std::numeric_limits<double>::max();
    for (double t : candidates) {
        const double c = cost(t);
        if (c < bestCost - 1e-6) {
            bestCost = c;
            bestT = t;
        }
    }
    return f.proj.toLatLon({ a.x + bestT * (b.x - a.x), a.y + bestT * (b.y - a.y) });
}

LatLon bestGatePoint(const std::vector<LatLon>& boundary, const std::vector<int>& sides,
                     const LatLon& outside, const LatLon& inside)
{
    if (boundary.size() < 3) {
        return inside;
    }
    const FieldFrame f = makeFrame(boundary);
    InsideRouter router(f.ccw);
    const XY po = f.proj.toXY(outside);
    const XY pi = f.proj.toXY(inside);

    LatLon best     = inside;
    double bestCost = std::numeric_limits<double>::max();
    for (int side : sides) {
        if (side < 0 || side >= static_cast<int>(boundary.size())) {
            continue;
        }
        const LatLon g  = gatePoint(boundary, side, outside, inside);
        const XY     gx = f.proj.toXY(g);
        const double c  = dist(po, gx) + pathLength(gx, router.route(gx, pi));
        if (c < bestCost) {
            bestCost = c;
            best     = g;
        }
    }
    return best;
}

std::vector<LatLon> routeInside(const std::vector<LatLon>& boundary, const LatLon& from, const LatLon& to)
{
    if (boundary.size() < 3) {
        return { to };
    }
    const FieldFrame f = makeFrame(boundary);
    if (f.ccw.size() < 3) {
        return { to };
    }
    InsideRouter router(f.ccw);
    std::vector<LatLon> out;
    for (const XY& p : router.route(f.proj.toXY(from), f.proj.toXY(to))) {
        out.push_back(f.proj.toLatLon(p));
    }
    if (!out.empty()) {
        out.back() = to;   // exact, not round-tripped through the projection
    }
    return out;
}

LatLon nearestPointOnPolygon(const std::vector<LatLon>& polygon, const LatLon& p)
{
    if (polygon.size() < 2) {
        return polygon.empty() ? p : polygon.front();
    }
    const FieldFrame f = makeFrame(polygon);
    const XY q = f.proj.toXY(p);
    XY best = f.raw.front();
    double bestDist = std::numeric_limits<double>::max();
    for (size_t i = 0; i < f.raw.size(); ++i) {
        const XY c = closestOnSegment(q, f.raw[i], f.raw[(i + 1) % f.raw.size()]);
        const double d = dist(c, q);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return f.proj.toLatLon(best);
}

} // namespace spray

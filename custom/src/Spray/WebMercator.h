#pragma once

// WebMercator
// -----------
// The map's tile grid: normalised Web Mercator coordinates (x and y run 0..1
// from the top-left of the world) and XYZ tile numbers at a zoom level, where
// the world is 2^level tiles across. No Qt dependencies.

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spray::webmercator {

constexpr double MaxLat = 85.05112878;

inline double xFromLon(double lon)
{
    return (lon + 180.0) / 360.0;
}

inline double yFromLat(double lat)
{
    const double s = std::sin(std::clamp(lat, -MaxLat, MaxLat) * std::numbers::pi / 180.0);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * std::numbers::pi);
}

inline double lonFromX(double x)
{
    return x * 360.0 - 180.0;
}

inline double latFromY(double y)
{
    return std::atan(std::sinh(std::numbers::pi * (1.0 - 2.0 * y))) * 180.0 / std::numbers::pi;
}

// Tile column or row holding normalised coordinate v at `level`.
inline int tileIndex(double v, int level)
{
    const int count = 1 << level;
    return std::clamp(static_cast<int>(std::floor(v * count)), 0, count - 1);
}

// Inclusive range of tiles at `level` covering normalised [minX, maxX] x [minY, maxY].
struct TileRange {
    int x0 = 0;
    int y0 = 0;
    int x1 = -1;
    int y1 = -1;

    long long count() const { return x1 < x0 || y1 < y0 ? 0 : static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1); }
};

inline TileRange tileRange(double minX, double minY, double maxX, double maxY, int level)
{
    TileRange range;
    range.x0 = tileIndex(minX, level);
    range.y0 = tileIndex(minY, level);
    range.x1 = tileIndex(maxX, level);
    range.y1 = tileIndex(maxY, level);
    return range;
}

inline TileRange intersect(const TileRange& a, const TileRange& b)
{
    TileRange range;
    range.x0 = std::max(a.x0, b.x0);
    range.y0 = std::max(a.y0, b.y0);
    range.x1 = std::min(a.x1, b.x1);
    range.y1 = std::min(a.y1, b.y1);
    return range;
}

} // namespace spray::webmercator

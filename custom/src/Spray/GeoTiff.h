#pragma once

// GeoTiff
// -------
// Reads where a GeoTIFF sits on the earth: the size of each image in the file
// (the full image and any reduced-resolution overviews), the pixel-to-map
// transform and the coordinate system. No Qt dependencies, so it can be unit
// tested on its own; the pixels themselves are decoded elsewhere.
//
// Coordinate systems understood: WGS84 / NAD83 latitude and longitude, Web
// Mercator, and UTM (WGS84, NAD83, ETRS89) in metres. That covers what drone
// mapping software exports by default.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "SprayPathGenerator.h"   // LatLon

namespace spray {

// Random-access bytes of the file (the caller opens it, so paths work on every platform).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads `size` bytes at `offset`; false if the file is shorter.
    virtual bool read(uint64_t offset, void* data, size_t size) = 0;
};

struct GeoTiffImage {
    uint32_t width       = 0;
    uint32_t height      = 0;
    uint32_t subfileType = 0;   // bit 0: reduced-resolution overview, bit 2: transparency mask

    bool isMask() const { return (subfileType & 4u) != 0; }
};

enum class GeoTiffError {
    None,
    NotTiff,          // not a TIFF file, or damaged
    NoGeoreference,   // a TIFF, but nothing places it on the map
    UnsupportedCrs,   // placed with a coordinate system we can't convert
};

class GeoTiff {
public:
    // Reads the file's structure and georeferencing.
    GeoTiffError read(ByteSource& source);

    // Every image in the file, in file order (the order TIFF readers number them).
    const std::vector<GeoTiffImage>& images() const { return _images; }

    // Map position of a point of the full-resolution image, in pixel units from
    // its top-left corner (so the image spans 0..width, 0..height). False if the
    // coordinate system isn't supported.
    bool pixelToLatLon(double column, double row, LatLon& result) const;

    // EPSG code of the coordinate system (0: not stated), for messages.
    int epsgCode() const;

    // GDAL's "no data" value, for images without an alpha band.
    bool hasNoData() const { return _hasNoData; }
    double noData() const { return _noData; }

private:
    bool _modelToLatLon(double x, double y, LatLon& result) const;

    std::vector<GeoTiffImage> _images;

    // model = (a[0] + a[1]*column + a[2]*row,  a[3] + a[4]*column + a[5]*row)
    double _affine[6]    = { 0, 1, 0, 0, 0, -1 };
    bool   _pixelIsPoint = false;

    int _modelType      = 0;   // 1 projected, 2 geographic
    int _geographicType = 0;
    int _projectedType  = 0;
    int _linearUnits    = 0;   // 9001 metre

    bool   _hasNoData = false;
    double _noData    = 0.0;
};

// Web Mercator (EPSG:3857) metres to latitude / longitude.
LatLon webMercatorToLatLon(double x, double y);

// UTM metres (WGS84 ellipsoid) to latitude / longitude.
LatLon utmToLatLon(double easting, double northing, int zone, bool south);

} // namespace spray

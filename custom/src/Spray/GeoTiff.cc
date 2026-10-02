#include "GeoTiff.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

namespace spray {

namespace {

constexpr uint16_t TagNewSubfileType      = 254;
constexpr uint16_t TagImageWidth          = 256;
constexpr uint16_t TagImageLength         = 257;
constexpr uint16_t TagModelPixelScale     = 33550;
constexpr uint16_t TagModelTiepoint       = 33922;
constexpr uint16_t TagModelTransformation = 34264;
constexpr uint16_t TagGeoKeyDirectory     = 34735;
constexpr uint16_t TagGdalNoData          = 42113;

constexpr uint16_t KeyModelType       = 1024;
constexpr uint16_t KeyRasterType      = 1025;
constexpr uint16_t KeyGeographicType  = 2048;
constexpr uint16_t KeyProjectedType   = 3072;
constexpr uint16_t KeyProjLinearUnits = 3076;

constexpr int RasterPixelIsPoint = 2;
constexpr int LinearUnitMetre    = 9001;

// Limits that keep a damaged file from looping or allocating without end.
constexpr size_t   MaxImages  = 64;
constexpr uint64_t MaxEntries = 4096;
constexpr uint64_t MaxValues  = 65536;

constexpr double Pi = 3.14159265358979323846;
constexpr double DegPerRad = 180.0 / Pi;

struct Entry {
    uint16_t tag        = 0;
    uint16_t type       = 0;
    uint64_t count      = 0;
    uint64_t dataOffset = 0;   // where the values are (inside the entry when they fit)
};

size_t typeSize(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7:   return 1;   // BYTE ASCII SBYTE UNDEFINED
    case 3: case 8:                   return 2;   // SHORT SSHORT
    case 4: case 9: case 11: case 13: return 4;   // LONG SLONG FLOAT IFD
    case 5: case 10: case 12:
    case 16: case 17: case 18:        return 8;   // RATIONAL SRATIONAL DOUBLE LONG8 SLONG8 IFD8
    default:                          return 0;
    }
}

class Reader {
public:
    Reader(ByteSource& source, bool bigEndian)
        : _source(source)
        , _bigEndian(bigEndian)
    {
    }

    // Unsigned integer of `size` bytes (1, 2, 4 or 8) in the file's byte order.
    bool uint(uint64_t offset, size_t size, uint64_t& value)
    {
        unsigned char bytes[8];
        if (size > sizeof(bytes) || !_source.read(offset, bytes, size)) {
            return false;
        }
        value = 0;
        for (size_t i = 0; i < size; i++) {
            const size_t shift = (_bigEndian ? size - 1 - i : i) * 8;
            value |= static_cast<uint64_t>(bytes[i]) << shift;
        }
        return true;
    }

    bool number(uint64_t offset, uint16_t type, double& value)
    {
        uint64_t raw = 0;
        switch (type) {
        case 1: case 3: case 4: case 13: case 16: case 18:
            if (!uint(offset, typeSize(type), raw)) {
                return false;
            }
            value = static_cast<double>(raw);
            return true;
        case 6:
            if (!uint(offset, 1, raw)) {
                return false;
            }
            value = static_cast<int8_t>(raw);
            return true;
        case 8:
            if (!uint(offset, 2, raw)) {
                return false;
            }
            value = static_cast<int16_t>(raw);
            return true;
        case 9:
            if (!uint(offset, 4, raw)) {
                return false;
            }
            value = static_cast<int32_t>(raw);
            return true;
        case 17:
            if (!uint(offset, 8, raw)) {
                return false;
            }
            value = static_cast<double>(static_cast<int64_t>(raw));
            return true;
        case 11: {
            if (!uint(offset, 4, raw)) {
                return false;
            }
            const uint32_t bits = static_cast<uint32_t>(raw);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            value = f;
            return true;
        }
        case 12:
            if (!uint(offset, 8, raw)) {
                return false;
            }
            std::memcpy(&value, &raw, sizeof(value));
            return true;
        case 5: case 10: {
            uint64_t numerator = 0;
            uint64_t denominator = 0;
            if (!uint(offset, 4, numerator) || !uint(offset + 4, 4, denominator) || denominator == 0) {
                return false;
            }
            value = type == 5 ? static_cast<double>(numerator) / static_cast<double>(denominator)
                              : static_cast<double>(static_cast<int32_t>(numerator)) / static_cast<int32_t>(denominator);
            return true;
        }
        default:
            return false;
        }
    }

    bool numbers(const Entry& entry, std::vector<double>& values)
    {
        const size_t size = typeSize(entry.type);
        if (size == 0 || entry.count > MaxValues) {
            return false;
        }
        values.resize(static_cast<size_t>(entry.count));
        for (size_t i = 0; i < values.size(); i++) {
            if (!number(entry.dataOffset + i * size, entry.type, values[i])) {
                return false;
            }
        }
        return true;
    }

    bool ascii(const Entry& entry, std::string& text)
    {
        if (entry.type != 2 || entry.count > MaxValues) {
            return false;
        }
        text.assign(static_cast<size_t>(entry.count), '\0');
        if (!text.empty() && !_source.read(entry.dataOffset, &text[0], text.size())) {
            return false;
        }
        const size_t end = text.find('\0');
        if (end != std::string::npos) {
            text.resize(end);
        }
        return true;
    }

private:
    ByteSource& _source;
    bool        _bigEndian;
};

bool isWebMercator(int code)
{
    return code == 3857 || code == 3785 || code == 900913 || code == 102100 || code == 102113;
}

// UTM zone of a projected EPSG code (WGS84, NAD83, ETRS89); 0 if it isn't UTM.
int utmZone(int code, bool& south)
{
    south = false;
    if (code >= 32601 && code <= 32660) {
        return code - 32600;
    }
    if (code >= 32701 && code <= 32760) {
        south = true;
        return code - 32700;
    }
    if (code >= 26901 && code <= 26923) {   // NAD83 / UTM zone 1N..23N
        return code - 26900;
    }
    if (code >= 25828 && code <= 25838) {   // ETRS89 / UTM zone 28N..38N
        return code - 25800;
    }
    return 0;
}

// Latitude / longitude systems close enough to WGS84 for a map overlay.
bool isWgs84Like(int code)
{
    return code == 4326 || code == 4269 || code == 4258 || code == 4283 || code == 4617
        || code == 4152 || code == 6318 || code == 4167;
}

} // namespace

GeoTiffError GeoTiff::read(ByteSource& source)
{
    *this = GeoTiff();

    unsigned char order[2];
    if (!source.read(0, order, sizeof(order))) {
        return GeoTiffError::NotTiff;
    }
    bool bigEndian = false;
    if (order[0] == 'I' && order[1] == 'I') {
        bigEndian = false;
    } else if (order[0] == 'M' && order[1] == 'M') {
        bigEndian = true;
    } else {
        return GeoTiffError::NotTiff;
    }

    Reader reader(source, bigEndian);
    uint64_t version = 0;
    if (!reader.uint(2, 2, version) || (version != 42 && version != 43)) {
        return GeoTiffError::NotTiff;
    }
    const bool bigTiff = version == 43;
    uint64_t ifdOffset = 0;
    if (bigTiff) {
        uint64_t offsetSize = 0;
        if (!reader.uint(4, 2, offsetSize) || offsetSize != 8 || !reader.uint(8, 8, ifdOffset)) {
            return GeoTiffError::NotTiff;
        }
    } else if (!reader.uint(4, 4, ifdOffset)) {
        return GeoTiffError::NotTiff;
    }

    const size_t countSize  = bigTiff ? 8 : 2;
    const size_t entrySize  = bigTiff ? 20 : 12;
    const size_t fieldSize  = bigTiff ? 8 : 4;
    const size_t offsetSize = bigTiff ? 8 : 4;

    std::vector<double> scale;
    std::vector<double> tiepoints;
    std::vector<double> transformation;
    std::vector<double> geoKeys;
    std::set<uint64_t>  visited;

    while (ifdOffset != 0 && _images.size() < MaxImages && visited.insert(ifdOffset).second) {
        uint64_t entryCount = 0;
        if (!reader.uint(ifdOffset, countSize, entryCount) || entryCount > MaxEntries) {
            return GeoTiffError::NotTiff;
        }
        const uint64_t firstEntry = ifdOffset + countSize;
        const bool     firstImage = _images.empty();
        GeoTiffImage   image;

        for (uint64_t i = 0; i < entryCount; i++) {
            const uint64_t base = firstEntry + i * entrySize;
            uint64_t tag = 0;
            uint64_t type = 0;
            Entry entry;
            if (!reader.uint(base, 2, tag) || !reader.uint(base + 2, 2, type)
                || !reader.uint(base + 4, bigTiff ? 8 : 4, entry.count)) {
                return GeoTiffError::NotTiff;
            }
            entry.tag  = static_cast<uint16_t>(tag);
            entry.type = static_cast<uint16_t>(type);
            const uint64_t field = base + (bigTiff ? 12 : 8);
            const size_t   size  = typeSize(entry.type);
            if (size != 0 && entry.count <= MaxValues && size * entry.count <= fieldSize) {
                entry.dataOffset = field;
            } else if (!reader.uint(field, offsetSize, entry.dataOffset)) {
                return GeoTiffError::NotTiff;
            }

            std::vector<double> values;
            switch (entry.tag) {
            case TagImageWidth:
            case TagImageLength:
            case TagNewSubfileType:
                if (reader.numbers(entry, values) && !values.empty()) {
                    const uint32_t value = static_cast<uint32_t>(values[0]);
                    if (entry.tag == TagImageWidth) {
                        image.width = value;
                    } else if (entry.tag == TagImageLength) {
                        image.height = value;
                    } else {
                        image.subfileType = value;
                    }
                }
                break;
            case TagModelPixelScale:
                if (firstImage) {
                    reader.numbers(entry, scale);
                }
                break;
            case TagModelTiepoint:
                if (firstImage) {
                    reader.numbers(entry, tiepoints);
                }
                break;
            case TagModelTransformation:
                if (firstImage) {
                    reader.numbers(entry, transformation);
                }
                break;
            case TagGeoKeyDirectory:
                if (firstImage) {
                    reader.numbers(entry, geoKeys);
                }
                break;
            case TagGdalNoData:
                if (firstImage) {
                    std::string text;
                    if (reader.ascii(entry, text) && !text.empty()) {
                        char* end = nullptr;
                        const double value = std::strtod(text.c_str(), &end);
                        if (end != text.c_str()) {
                            _hasNoData = true;
                            _noData    = value;
                        }
                    }
                }
                break;
            default:
                break;
            }
        }
        _images.push_back(image);

        if (!reader.uint(firstEntry + entryCount * entrySize, offsetSize, ifdOffset)) {
            ifdOffset = 0;   // a missing next-image pointer just ends the list
        }
    }

    if (_images.empty() || _images[0].width == 0 || _images[0].height == 0) {
        return GeoTiffError::NotTiff;
    }

    if (transformation.size() >= 16) {
        _affine[0] = transformation[3];
        _affine[1] = transformation[0];
        _affine[2] = transformation[1];
        _affine[3] = transformation[7];
        _affine[4] = transformation[4];
        _affine[5] = transformation[5];
    } else if (tiepoints.size() >= 6 && scale.size() >= 2 && scale[0] != 0.0 && scale[1] != 0.0) {
        // The first tie point pins raster (I, J) to model (X, Y); rows go down, Y goes up.
        const double i0 = tiepoints[0];
        const double j0 = tiepoints[1];
        _affine[1] = scale[0];
        _affine[2] = 0.0;
        _affine[0] = tiepoints[3] - i0 * scale[0];
        _affine[4] = 0.0;
        _affine[5] = -scale[1];
        _affine[3] = tiepoints[4] + j0 * scale[1];
    } else {
        return GeoTiffError::NoGeoreference;
    }

    // GeoKeyDirectory: a 4-value header, then (key, location, count, value) per key.
    // Location 0 means the value is the short itself; the keys we need all are.
    if (geoKeys.size() >= 4) {
        const size_t keyCount = static_cast<size_t>(geoKeys[3]);
        for (size_t k = 0; k < keyCount && 4 + k * 4 + 3 < geoKeys.size(); k++) {
            const int key      = static_cast<int>(geoKeys[4 + k * 4]);
            const int location = static_cast<int>(geoKeys[4 + k * 4 + 1]);
            const int value    = static_cast<int>(geoKeys[4 + k * 4 + 3]);
            if (location != 0) {
                continue;
            }
            switch (key) {
            case KeyModelType:       _modelType      = value; break;
            case KeyRasterType:      _pixelIsPoint   = value == RasterPixelIsPoint; break;
            case KeyGeographicType:  _geographicType = value; break;
            case KeyProjectedType:   _projectedType  = value; break;
            case KeyProjLinearUnits: _linearUnits    = value; break;
            default: break;
            }
        }
    }

    LatLon centre;
    if (!pixelToLatLon(_images[0].width / 2.0, _images[0].height / 2.0, centre)) {
        return GeoTiffError::UnsupportedCrs;
    }
    return GeoTiffError::None;
}

bool GeoTiff::pixelToLatLon(double column, double row, LatLon& result) const
{
    if (_pixelIsPoint) {
        // Raster coordinates name pixel centres: the top-left corner is at (-0.5, -0.5).
        column -= 0.5;
        row    -= 0.5;
    }
    const double x = _affine[0] + _affine[1] * column + _affine[2] * row;
    const double y = _affine[3] + _affine[4] * column + _affine[5] * row;
    return _modelToLatLon(x, y, result);
}

int GeoTiff::epsgCode() const
{
    return (_modelType != 2 && _projectedType != 0) ? _projectedType : _geographicType;
}

bool GeoTiff::_modelToLatLon(double x, double y, LatLon& result) const
{
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    const bool projected = _modelType == 1 || (_modelType == 0 && _projectedType != 0);
    if (projected) {
        if (_linearUnits != 0 && _linearUnits != LinearUnitMetre) {
            return false;
        }
        if (isWebMercator(_projectedType)) {
            result = webMercatorToLatLon(x, y);
            return true;
        }
        bool south = false;
        const int zone = utmZone(_projectedType, south);
        if (zone == 0) {
            return false;
        }
        result = utmToLatLon(x, y, zone, south);
        return true;
    }

    // Geographic: X is longitude, Y latitude. Without any keys, accept values that can only be degrees.
    if (_modelType == 2 || _geographicType != 0 || _modelType == 0) {
        if (_geographicType != 0 && !isWgs84Like(_geographicType)) {
            return false;
        }
        if (std::fabs(y) > 90.0 || std::fabs(x) > 180.0) {
            return false;
        }
        result.lat = y;
        result.lon = x;
        return true;
    }
    return false;
}

LatLon webMercatorToLatLon(double x, double y)
{
    constexpr double R = 6378137.0;
    LatLon result;
    result.lon = x / R * DegPerRad;
    result.lat = (2.0 * std::atan(std::exp(y / R)) - Pi / 2.0) * DegPerRad;
    return result;
}

LatLon utmToLatLon(double easting, double northing, int zone, bool south)
{
    // Snyder, "Map Projections: A Working Manual", inverse transverse Mercator (8-18..8-25).
    constexpr double a  = 6378137.0;
    constexpr double f  = 1.0 / 298.257223563;
    constexpr double k0 = 0.9996;
    const double e2   = f * (2.0 - f);
    const double ep2  = e2 / (1.0 - e2);
    const double x    = easting - 500000.0;
    const double yN   = south ? northing - 10000000.0 : northing;
    const double lon0 = ((zone - 1) * 6 - 180 + 3) / DegPerRad;

    const double m  = yN / k0;
    const double mu = m / (a * (1.0 - e2 / 4.0 - 3.0 * e2 * e2 / 64.0 - 5.0 * e2 * e2 * e2 / 256.0));
    const double e1 = (1.0 - std::sqrt(1.0 - e2)) / (1.0 + std::sqrt(1.0 - e2));
    const double phi1 = mu
        + (3.0 * e1 / 2.0 - 27.0 * std::pow(e1, 3) / 32.0) * std::sin(2.0 * mu)
        + (21.0 * e1 * e1 / 16.0 - 55.0 * std::pow(e1, 4) / 32.0) * std::sin(4.0 * mu)
        + (151.0 * std::pow(e1, 3) / 96.0) * std::sin(6.0 * mu)
        + (1097.0 * std::pow(e1, 4) / 512.0) * std::sin(8.0 * mu);

    const double sinPhi = std::sin(phi1);
    const double cosPhi = std::cos(phi1);
    const double tanPhi = std::tan(phi1);
    const double n1 = a / std::sqrt(1.0 - e2 * sinPhi * sinPhi);
    const double t1 = tanPhi * tanPhi;
    const double c1 = ep2 * cosPhi * cosPhi;
    const double r1 = a * (1.0 - e2) / std::pow(1.0 - e2 * sinPhi * sinPhi, 1.5);
    const double d  = x / (n1 * k0);

    LatLon result;
    result.lat = (phi1 - (n1 * tanPhi / r1)
                  * (d * d / 2.0
                     - (5.0 + 3.0 * t1 + 10.0 * c1 - 4.0 * c1 * c1 - 9.0 * ep2) * std::pow(d, 4) / 24.0
                     + (61.0 + 90.0 * t1 + 298.0 * c1 + 45.0 * t1 * t1 - 252.0 * ep2 - 3.0 * c1 * c1) * std::pow(d, 6) / 720.0))
                 * DegPerRad;
    result.lon = (lon0 + (d
                          - (1.0 + 2.0 * t1 + c1) * std::pow(d, 3) / 6.0
                          + (5.0 - 2.0 * c1 + 28.0 * t1 - 3.0 * c1 * c1 + 8.0 * ep2 + 24.0 * t1 * t1) * std::pow(d, 5) / 120.0)
                         / cosPhi)
                 * DegPerRad;
    return result;
}

} // namespace spray

// Unit tests for GeoTiff (GeoTIFF structure, georeferencing and coordinate
// conversions) and the WebMercator tile grid. No Qt needed. Build and run from the custom/src/Spray folder:
//   g++ -std=c++17 -O2 -Wall -Wextra -Wshadow -I. tests/test_geotiff.cc GeoTiff.cc -o /tmp/test_geotiff && /tmp/test_geotiff
// Optionally pass GeoTIFF files to print what is read from them.
#include "GeoTiff.h"
#include "WebMercator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace spray;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static constexpr double kPi = 3.14159265358979323846;

class MemorySource : public ByteSource {
public:
    explicit MemorySource(std::vector<unsigned char> bytes) : _bytes(std::move(bytes)) {}
    bool read(uint64_t offset, void* data, size_t size) override
    {
        if (offset > _bytes.size() || size > _bytes.size() - offset) {
            return false;
        }
        std::memcpy(data, _bytes.data() + offset, size);
        return true;
    }

private:
    std::vector<unsigned char> _bytes;
};

class FileSource : public ByteSource {
public:
    explicit FileSource(const char* path) : _file(std::fopen(path, "rb")) {}
    ~FileSource() override
    {
        if (_file) {
            std::fclose(_file);
        }
    }
    bool ok() const { return _file != nullptr; }
    bool read(uint64_t offset, void* data, size_t size) override
    {
        return _file && std::fseek(_file, static_cast<long>(offset), SEEK_SET) == 0
            && std::fread(data, 1, size, _file) == size;
    }

private:
    std::FILE* _file;
};

// ---- a minimal TIFF writer: tags only, no pixels (GeoTiff doesn't read them) ----

struct Tag {
    uint16_t tag;
    uint16_t type;                  // 3 SHORT, 4 LONG, 12 DOUBLE, 2 ASCII
    std::vector<double> values;     // SHORT, LONG, DOUBLE
    std::string text;               // ASCII
};

class TiffWriter {
public:
    TiffWriter(bool bigEndian, bool bigTiff) : _bigEndian(bigEndian), _bigTiff(bigTiff) {}

    std::vector<unsigned char> build(const std::vector<std::vector<Tag>>& images)
    {
        _out.clear();
        _out.push_back(_bigEndian ? 'M' : 'I');
        _out.push_back(_bigEndian ? 'M' : 'I');
        put(_bigTiff ? 43 : 42, 2);
        size_t firstOffsetAt;
        if (_bigTiff) {
            put(8, 2);
            put(0, 2);
            firstOffsetAt = _out.size();
            put(0, 8);
        } else {
            firstOffsetAt = _out.size();
            put(0, 4);
        }
        size_t linkAt = firstOffsetAt;
        for (const auto& tags : images) {
            // Values that don't fit in an entry go before the IFD.
            std::vector<uint64_t> dataOffsets;
            const size_t field = _bigTiff ? 8 : 4;
            for (const auto& t : tags) {
                const size_t size = valuesSize(t);
                if (size > field) {
                    dataOffsets.push_back(_out.size());
                    putValues(t);
                    if (_out.size() % 2) {
                        _out.push_back(0);
                    }
                } else {
                    dataOffsets.push_back(0);
                }
            }
            const uint64_t ifd = _out.size();
            patch(linkAt, ifd, _bigTiff ? 8 : 4);
            put(tags.size(), _bigTiff ? 8 : 2);
            for (size_t i = 0; i < tags.size(); i++) {
                const Tag& t = tags[i];
                put(t.tag, 2);
                put(t.type, 2);
                put(t.type == 2 ? t.text.size() + 1 : t.values.size(), _bigTiff ? 8 : 4);
                const size_t start = _out.size();
                if (dataOffsets[i]) {
                    put(dataOffsets[i], field);
                } else {
                    putValues(t);
                }
                while (_out.size() < start + field) {
                    _out.push_back(0);
                }
            }
            linkAt = _out.size();
            put(0, _bigTiff ? 8 : 4);
        }
        return _out;
    }

private:
    static size_t typeBytes(uint16_t type) { return type == 3 ? 2 : (type == 4 ? 4 : (type == 12 ? 8 : 1)); }
    static size_t valuesSize(const Tag& t) { return t.type == 2 ? t.text.size() + 1 : t.values.size() * typeBytes(t.type); }

    void put(uint64_t value, size_t size)
    {
        for (size_t i = 0; i < size; i++) {
            const size_t shift = (_bigEndian ? size - 1 - i : i) * 8;
            _out.push_back(static_cast<unsigned char>(value >> shift));
        }
    }
    void patch(size_t at, uint64_t value, size_t size)
    {
        for (size_t i = 0; i < size; i++) {
            const size_t shift = (_bigEndian ? size - 1 - i : i) * 8;
            _out[at + i] = static_cast<unsigned char>(value >> shift);
        }
    }
    void putValues(const Tag& t)
    {
        if (t.type == 2) {
            for (char c : t.text) {
                _out.push_back(static_cast<unsigned char>(c));
            }
            _out.push_back(0);
            return;
        }
        for (double v : t.values) {
            if (t.type == 12) {
                uint64_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                put(bits, 8);
            } else {
                put(static_cast<uint64_t>(v), typeBytes(t.type));
            }
        }
    }

    bool _bigEndian;
    bool _bigTiff;
    std::vector<unsigned char> _out;
};

static std::vector<Tag> imageTags(uint32_t width, uint32_t height, uint32_t subfile = 0)
{
    return { { 254, 4, { double(subfile) }, "" }, { 256, 4, { double(width) }, "" }, { 257, 4, { double(height) }, "" } };
}

static std::vector<double> geoKeys(std::vector<std::vector<double>> keys)
{
    std::vector<double> v = { 1, 1, 0, double(keys.size()) };
    for (const auto& k : keys) {
        v.push_back(k[0]);
        v.push_back(0);
        v.push_back(1);
        v.push_back(k[1]);
    }
    return v;
}

// ---- forward projections, to check the inverse ones against -----------------

static void latLonToUtm(double latDeg, double lonDeg, int zone, double& e, double& n)
{
    const double a = 6378137.0, f = 1.0 / 298.257223563, k0 = 0.9996;
    const double e2 = f * (2 - f), ep2 = e2 / (1 - e2);
    const double lat = latDeg * kPi / 180, lon = lonDeg * kPi / 180;
    const double lon0 = ((zone - 1) * 6 - 180 + 3) * kPi / 180;
    const double N = a / std::sqrt(1 - e2 * std::sin(lat) * std::sin(lat));
    const double T = std::tan(lat) * std::tan(lat), C = ep2 * std::cos(lat) * std::cos(lat);
    const double A = std::cos(lat) * (lon - lon0);
    const double M = a * ((1 - e2 / 4 - 3 * e2 * e2 / 64 - 5 * e2 * e2 * e2 / 256) * lat
                          - (3 * e2 / 8 + 3 * e2 * e2 / 32 + 45 * e2 * e2 * e2 / 1024) * std::sin(2 * lat)
                          + (15 * e2 * e2 / 256 + 45 * e2 * e2 * e2 / 1024) * std::sin(4 * lat)
                          - (35 * e2 * e2 * e2 / 3072) * std::sin(6 * lat));
    e = 500000 + k0 * N * (A + (1 - T + C) * std::pow(A, 3) / 6 + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * std::pow(A, 5) / 120);
    n = k0 * (M + N * std::tan(lat) * (A * A / 2 + (5 - T + 9 * C + 4 * C * C) * std::pow(A, 4) / 24
                                        + (61 - 58 * T + T * T + 600 * C - 330 * ep2) * std::pow(A, 6) / 720));
    if (latDeg < 0) {
        n += 10000000;
    }
}

static bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// ---- tests --------------------------------------------------------------------

static void testUtm()
{
    // Published: CN Tower, 43.6426 N 79.3871 W = UTM 17T 630084 E 4833438 N (1 m / 1e-4 deg rounding).
    LatLon p = utmToLatLon(630084, 4833438, 17, false);
    CHECK(near(p.lat, 43.6426, 2e-4) && near(p.lon, -79.3871, 2e-4), "CN Tower: %.6f %.6f", p.lat, p.lon);

    // Round trips across a zone, both hemispheres (field-sized offsets from the central meridian and beyond).
    const double pts[][3] = { { 37.4128, -121.9990, 10 }, { 37.4128, -119.5, 11 }, { -33.8568, 151.2153, 56 },
                              { 52.0, 2.5, 31 }, { -1.0, 36.8, 37 }, { 60.0, -150.4, 5 } };
    for (const auto& q : pts) {
        double e, n;
        latLonToUtm(q[0], q[1], int(q[2]), e, n);
        LatLon r = utmToLatLon(e, n, int(q[2]), q[0] < 0);
        // 1e-7 deg is about 1 cm.
        CHECK(near(r.lat, q[0], 1e-7) && near(r.lon, q[1], 1e-7), "UTM round trip %.4f %.4f -> %.8f %.8f", q[0], q[1], r.lat, r.lon);
    }
}

static void testWebMercator()
{
    LatLon p = webMercatorToLatLon(0, 0);
    CHECK(near(p.lat, 0, 1e-12) && near(p.lon, 0, 1e-12), "origin");
    const double R = 6378137.0, lat = 37.4128, lon = -121.999;
    const double x = R * lon * kPi / 180, y = R * std::log(std::tan(kPi / 4 + lat * kPi / 360));
    p = webMercatorToLatLon(x, y);
    CHECK(near(p.lat, lat, 1e-9) && near(p.lon, lon, 1e-9), "mercator round trip %.9f %.9f", p.lat, p.lon);
}

static void testWgs84WithOverviews(bool bigEndian, bool bigTiff)
{
    auto full = imageTags(4000, 3000);
    full.push_back({ 33550, 12, { 0.00001, 0.00001, 0 }, "" });
    full.push_back({ 33922, 12, { 0, 0, 0, -121.9990, 37.4140, 0 }, "" });
    full.push_back({ 34735, 3, geoKeys({ { 1024, 2 }, { 1025, 1 }, { 2048, 4326 } }), "" });
    auto overview = imageTags(1000, 750, 1);
    auto mask     = imageTags(4000, 3000, 4);
    MemorySource src(TiffWriter(bigEndian, bigTiff).build({ full, overview, mask }));

    GeoTiff g;
    const GeoTiffError err = g.read(src);
    CHECK(err == GeoTiffError::None, "wgs84 be=%d big=%d: error %d", bigEndian, bigTiff, int(err));
    CHECK(g.images().size() == 3, "image count %zu", g.images().size());
    if (g.images().size() == 3) {
        CHECK(g.images()[1].width == 1000 && g.images()[1].height == 750 && !g.images()[1].isMask(), "overview");
        CHECK(g.images()[2].isMask(), "mask");
    }
    LatLon tl, br;
    CHECK(g.pixelToLatLon(0, 0, tl) && near(tl.lat, 37.4140, 1e-12) && near(tl.lon, -121.9990, 1e-12), "top-left %.8f %.8f", tl.lat, tl.lon);
    CHECK(g.pixelToLatLon(4000, 3000, br) && near(br.lat, 37.3840, 1e-9) && near(br.lon, -121.9590, 1e-9), "bottom-right %.8f %.8f", br.lat, br.lon);
    CHECK(g.epsgCode() == 4326, "epsg %d", g.epsgCode());
    CHECK(!g.hasNoData(), "no nodata");
}

static void testUtmPixelIsPointAndNoData()
{
    auto full = imageTags(800, 600);
    full.push_back({ 33550, 12, { 0.5, 0.5, 0 }, "" });
    full.push_back({ 33922, 12, { 0, 0, 0, 590000.0, 4141000.0, 0 }, "" });
    full.push_back({ 34735, 3, geoKeys({ { 1024, 1 }, { 1025, 2 }, { 3072, 32610 }, { 3076, 9001 } }), "" });
    full.push_back({ 42113, 2, {}, "0" });
    MemorySource src(TiffWriter(false, false).build({ full }));

    GeoTiff g;
    CHECK(g.read(src) == GeoTiffError::None, "utm read");
    // Pixel-is-point: the tie point is the centre of the top-left pixel, so the corner is half a pixel up-left.
    LatLon corner, expected;
    g.pixelToLatLon(0, 0, corner);
    expected = utmToLatLon(590000.0 - 0.25, 4141000.0 + 0.25, 10, false);
    CHECK(near(corner.lat, expected.lat, 1e-10) && near(corner.lon, expected.lon, 1e-10), "pixel-is-point corner");
    CHECK(near(corner.lat, 37.41, 0.01) && near(corner.lon, -121.98, 0.01), "near Baylands: %.5f %.5f", corner.lat, corner.lon);
    CHECK(g.hasNoData() && g.noData() == 0.0, "nodata");
    CHECK(g.epsgCode() == 32610, "epsg %d", g.epsgCode());
}

static void testTransformationMatrix()
{
    // Rotated 3857 image: model = M * (col, row).
    auto full = imageTags(100, 100);
    const double sx = 2.0, x0 = -13580000.0, y0 = 4497000.0;
    full.push_back({ 34264, 12, { sx, 0.5, 0, x0, 0.5, -sx, 0, y0, 0, 0, 0, 0, 0, 0, 0, 1 }, "" });
    full.push_back({ 34735, 3, geoKeys({ { 1024, 1 }, { 3072, 3857 } }), "" });
    MemorySource src(TiffWriter(true, true).build({ full }));
    GeoTiff g;
    CHECK(g.read(src) == GeoTiffError::None, "transformation read");
    LatLon p;
    g.pixelToLatLon(10, 20, p);
    const LatLon expected = webMercatorToLatLon(x0 + sx * 10 + 0.5 * 20, y0 + 0.5 * 10 - sx * 20);
    CHECK(near(p.lat, expected.lat, 1e-10) && near(p.lon, expected.lon, 1e-10), "matrix point");
}

static void testTileGrid()
{
    namespace wm = spray::webmercator;
    CHECK(near(wm::xFromLon(-180), 0, 1e-15) && near(wm::xFromLon(180), 1, 1e-15) && near(wm::yFromLat(0), 0.5, 1e-15), "grid edges");
    const double lat = 41.3, lon = -83.07;
    CHECK(near(wm::latFromY(wm::yFromLat(lat)), lat, 1e-10) && near(wm::lonFromX(wm::xFromLon(lon)), lon, 1e-12), "round trip");
    CHECK(wm::yFromLat(89.9) > -1e-9 && wm::yFromLat(-89.9) < 1 + 1e-9, "clamped poles: %.12f %.12f", wm::yFromLat(89.9), wm::yFromLat(-89.9));

    // Slippy-map tile numbers for a known place: Bellevue, Ohio area at zoom 18.
    // x = floor((lon + 180) / 360 * 2^z), y = floor((1 - asinh(tan(lat)) / pi) / 2 * 2^z)
    const int z = 18;
    const double n = std::pow(2.0, z);
    const int ex = int(std::floor((lon + 180) / 360 * n));
    const int ey = int(std::floor((1 - std::asinh(std::tan(lat * kPi / 180)) / kPi) / 2 * n));
    CHECK(wm::tileIndex(wm::xFromLon(lon), z) == ex && wm::tileIndex(wm::yFromLat(lat), z) == ey,
          "tile %d,%d vs %d,%d", wm::tileIndex(wm::xFromLon(lon), z), wm::tileIndex(wm::yFromLat(lat), z), ex, ey);

    // A tile's top-left corner maps back into that tile.
    const double north = wm::latFromY(ey / n), west = wm::lonFromX(ex / n);
    CHECK(wm::tileIndex(wm::xFromLon(west + 1e-9), z) == ex && wm::tileIndex(wm::yFromLat(north - 1e-9), z) == ey, "corner in tile");

    // Ranges: a 400 m square at zoom 20 (about 0.1 m tiles' pixels) spans roughly 400 / (40075016 * cos(41.3) / 2^20 ) / 256 tiles.
    const double x0 = wm::xFromLon(-83.075), x1 = wm::xFromLon(-83.0702), y0 = wm::yFromLat(41.302), y1 = wm::yFromLat(41.2984);
    const wm::TileRange r = wm::tileRange(x0, y0, x1, y1, 20);
    CHECK(r.count() > 100 && r.count() < 400, "range count %lld", r.count());
    CHECK(wm::intersect(r, wm::TileRange{ r.x1 + 1, r.y0, r.x1 + 5, r.y1 }).count() == 0, "disjoint ranges");
    CHECK(wm::tileIndex(1.0, 3) == 7 && wm::tileIndex(-0.1, 3) == 0, "index clamped to the world");
}

static void testErrors()
{
    MemorySource garbage(std::vector<unsigned char>{ 'h', 'e', 'l', 'l', 'o', 0, 0, 0, 0, 0 });
    GeoTiff g;
    CHECK(g.read(garbage) == GeoTiffError::NotTiff, "garbage");

    MemorySource empty(std::vector<unsigned char>{});
    CHECK(g.read(empty) == GeoTiffError::NotTiff, "empty");

    MemorySource plain(TiffWriter(false, false).build({ imageTags(10, 10) }));
    CHECK(g.read(plain) == GeoTiffError::NoGeoreference, "plain tiff");

    auto statePlane = imageTags(10, 10);
    statePlane.push_back({ 33550, 12, { 1, 1, 0 }, "" });
    statePlane.push_back({ 33922, 12, { 0, 0, 0, 6000000.0, 2000000.0, 0 }, "" });
    statePlane.push_back({ 34735, 3, geoKeys({ { 1024, 1 }, { 3072, 2227 } }), "" });
    MemorySource sp(TiffWriter(false, false).build({ statePlane }));
    CHECK(g.read(sp) == GeoTiffError::UnsupportedCrs, "state plane");
    CHECK(g.epsgCode() == 2227, "state plane epsg %d", g.epsgCode());

    auto feetUtm = imageTags(10, 10);
    feetUtm.push_back({ 33550, 12, { 1, 1, 0 }, "" });
    feetUtm.push_back({ 33922, 12, { 0, 0, 0, 590000.0, 4141000.0, 0 }, "" });
    feetUtm.push_back({ 34735, 3, geoKeys({ { 1024, 1 }, { 3072, 32610 }, { 3076, 9002 } }), "" });
    MemorySource fu(TiffWriter(false, false).build({ feetUtm }));
    CHECK(g.read(fu) == GeoTiffError::UnsupportedCrs, "UTM in feet");

    // A next-image pointer looping back to the first image must not hang.
    auto bytes = TiffWriter(false, false).build({ imageTags(10, 10) });
    const uint32_t first = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (uint32_t(bytes[7]) << 24);
    const size_t nextAt = first + 2 + 3 * 12;
    for (int i = 0; i < 4; i++) {
        bytes[nextAt + i] = bytes[4 + i];
    }
    MemorySource loop(bytes);
    CHECK(g.read(loop) == GeoTiffError::NoGeoreference && g.images().size() == 1, "IFD loop");
}

int main(int argc, char** argv)
{
    testUtm();
    testWebMercator();
    testWgs84WithOverviews(false, false);
    testWgs84WithOverviews(true, false);
    testWgs84WithOverviews(false, true);
    testWgs84WithOverviews(true, true);
    testUtmPixelIsPointAndNoData();
    testTransformationMatrix();
    testTileGrid();
    testErrors();

    for (int i = 1; i < argc; i++) {
        FileSource file(argv[i]);
        GeoTiff g;
        const GeoTiffError err = file.ok() ? g.read(file) : GeoTiffError::NotTiff;
        std::printf("%s: error %d, EPSG %d, %zu image(s)", argv[i], int(err), g.epsgCode(), g.images().size());
        for (const auto& im : g.images()) {
            std::printf(" %ux%u%s", im.width, im.height, im.isMask() ? "(mask)" : "");
        }
        LatLon tl, br;
        if (err == GeoTiffError::None && g.pixelToLatLon(0, 0, tl)
            && g.pixelToLatLon(g.images()[0].width, g.images()[0].height, br)) {
            std::printf(", corners %.6f %.6f / %.6f %.6f", tl.lat, tl.lon, br.lat, br.lon);
        }
        std::printf("%s\n", g.hasNoData() ? ", nodata" : "");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

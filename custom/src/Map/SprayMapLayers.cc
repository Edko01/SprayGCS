#include "SprayMapLayers.h"

#include "AppMessages.h"
#include "GeoTiff.h"
#include "QGCLoggingCategory.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointF>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtCore/QVariantMap>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>
#include <QtGui/QTransform>

#include <algorithm>
#include <cmath>
#include <numbers>

QGC_LOGGING_CATEGORY(SprayMapLayersLog, "Custom.SprayMapLayers")

namespace {

constexpr double MaxMercatorLat = 85.05112878;
constexpr int    MapTileSize    = 256;    // QGC's map tiles: world width at zoom z is 256 * 2^z px
constexpr int    MaxDecodeMB    = 2048;   // refuse to decode a full image bigger than this

class FileByteSource : public spray::ByteSource {
public:
    explicit FileByteSource(QFile &file)
        : _file(file)
    {
    }

    bool read(uint64_t offset, void *data, size_t size) override
    {
        return _file.seek(static_cast<qint64>(offset))
            && _file.read(static_cast<char *>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
    }

private:
    QFile &_file;
};

// Normalised Web Mercator: x and y run 0..1 from the top-left of the world.
QPointF toMercator(const spray::LatLon &point)
{
    const double lat = std::clamp(point.lat, -MaxMercatorLat, MaxMercatorLat) * std::numbers::pi / 180.0;
    const double s   = std::sin(lat);
    return QPointF((point.lon + 180.0) / 360.0, 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * std::numbers::pi));
}

double mercatorYToLat(double y)
{
    return std::atan(std::sinh(std::numbers::pi * (1.0 - 2.0 * y))) * 180.0 / std::numbers::pi;
}

} // namespace

SprayMapLayers::SprayMapLayers(QObject *parent)
    : QObject(parent)
{
    (void) connect(&_watcher, &QFutureWatcher<LoadResult>::finished, this, &SprayMapLayers::_loaded);
    _restore();
}

QVariantList SprayMapLayers::layers() const
{
    QVariantList list;
    for (const Layer &layer : _layers) {
        QVariantMap map;
        map[QStringLiteral("name")]      = layer.name;
        map[QStringLiteral("url")]       = QUrl::fromLocalFile(_imagePath(layer)).toString();
        map[QStringLiteral("north")]     = layer.north;
        map[QStringLiteral("south")]     = layer.south;
        map[QStringLiteral("east")]      = layer.east;
        map[QStringLiteral("west")]      = layer.west;
        map[QStringLiteral("width")]     = layer.width;
        map[QStringLiteral("height")]    = layer.height;
        map[QStringLiteral("zoomLevel")] = layer.zoomLevel;
        map[QStringLiteral("visible")]   = layer.visible;
        map[QStringLiteral("opacity")]   = layer.opacity;
        list.append(map);
    }
    return list;
}

void SprayMapLayers::addGeoTiff(const QString &path)
{
    if (_watcher.isRunning()) {
        QGC::showAppMessage(tr("Still reading the last image. Add this one when it's on the map."));
        return;
    }
    _watcher.setFuture(QtConcurrent::run(&SprayMapLayers::loadGeoTiff, path, _folder()));
    emit loadingChanged();
}

void SprayMapLayers::removeLayer(int index)
{
    if (index < 0 || index >= _layers.count()) {
        return;
    }
    (void) QFile::remove(_imagePath(_layers[index]));
    _layers.removeAt(index);
    _save();
    emit layersChanged();
}

void SprayMapLayers::setLayerVisible(int index, bool visible)
{
    if (index < 0 || index >= _layers.count() || _layers[index].visible == visible) {
        return;
    }
    _layers[index].visible = visible;
    _save();
    emit layersChanged();
}

void SprayMapLayers::setLayerOpacity(int index, double opacity)
{
    opacity = std::clamp(opacity, 0.1, 1.0);
    if (index < 0 || index >= _layers.count() || qFuzzyCompare(_layers[index].opacity, opacity)) {
        return;
    }
    _layers[index].opacity = opacity;
    _save();
    emit layersChanged();
}

SprayMapLayers::LoadResult SprayMapLayers::loadGeoTiff(const QString &path, const QString &folder)
{
    LoadResult result;
    const QString fileName = QFileInfo(path).fileName();

    // ---- where it is ----
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = tr("Couldn't open %1.").arg(fileName);
        return result;
    }
    spray::GeoTiff geo;
    FileByteSource source(file);
    switch (geo.read(source)) {
    case spray::GeoTiffError::None:
        break;
    case spray::GeoTiffError::NotTiff:
        result.error = tr("%1 isn't a TIFF image, or it's damaged.").arg(fileName);
        return result;
    case spray::GeoTiffError::NoGeoreference:
        result.error = tr("%1 has no map position. Export it from the mapping software as a GeoTIFF.").arg(fileName);
        return result;
    case spray::GeoTiffError::UnsupportedCrs:
        result.error = tr("%1 uses a coordinate system SprayGCS can't place (EPSG:%2). Export it in WGS84 (EPSG:4326), "
                          "Web Mercator (EPSG:3857) or UTM.").arg(fileName).arg(geo.epsgCode());
        return result;
    }
    file.close();

    // ---- the pixels: the smallest overview still MaxImageSide across, else the full image ----
    const std::vector<spray::GeoTiffImage> &images = geo.images();
    const spray::GeoTiffImage &full = images[0];
    int pick = 0;
    uint32_t pickSide = 0;
    for (size_t i = 0; i < images.size(); i++) {
        const uint32_t side = std::max(images[i].width, images[i].height);
        if (!images[i].isMask() && side >= static_cast<uint32_t>(MaxImageSide) && (pickSide == 0 || side < pickSide)) {
            pick     = static_cast<int>(i);
            pickSide = side;
        }
    }

    QImageReader reader(path, "tiff");
    if (pick != 0 && !reader.jumpToImage(pick)) {
        qCDebug(SprayMapLayersLog) << "Couldn't jump to overview" << pick << "of" << path;
        pick = 0;
        reader.setFileName(path);
    }
    const qint64 decodeMB = static_cast<qint64>(images[pick].width) * images[pick].height * 8 / (1024 * 1024);
    if (decodeMB > MaxDecodeMB) {
        result.error = tr("%1 is too big to load (%2 × %3 pixels) and has no smaller overviews. Export it at a lower "
                          "resolution, or with overviews (pyramids), and add it again.")
                           .arg(fileName).arg(full.width).arg(full.height);
        return result;
    }
    if (QImageReader::allocationLimit() > 0 && decodeMB + 64 > QImageReader::allocationLimit()) {
        QImageReader::setAllocationLimit(static_cast<int>(decodeMB + 64));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        result.error = tr("Couldn't read the pixels of %1: %2").arg(fileName, reader.errorString());
        return result;
    }

    // GDAL's "no data" colour (often black around the field) becomes transparent.
    const bool hadAlpha = image.hasAlphaChannel();
    image = image.convertToFormat(QImage::Format_ARGB32);
    if (!hadAlpha && geo.hasNoData() && geo.noData() >= 0.0 && geo.noData() <= 255.0) {
        const int noData = static_cast<int>(std::lround(geo.noData()));
        for (int y = 0; y < image.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                if (qRed(line[x]) == noData && qGreen(line[x]) == noData && qBlue(line[x]) == noData) {
                    line[x] = 0;
                }
            }
        }
    }

    // ---- onto the map: corners in Web Mercator, then an affine fit (exact for Web Mercator
    // and lat/lon images, centimetres off for a field-sized UTM image) ----
    const double toFullX = static_cast<double>(full.width) / image.width();
    const double toFullY = static_cast<double>(full.height) / image.height();
    QPointF corners[4];
    const double cornerPixels[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        spray::LatLon latLon;
        if (!geo.pixelToLatLon(cornerPixels[i][0] * image.width() * toFullX, cornerPixels[i][1] * image.height() * toFullY, latLon)) {
            result.error = tr("%1 has a corner off the map.").arg(fileName);
            return result;
        }
        corners[i] = toMercator(latLon);
    }
    const QPointF &topLeft    = corners[0];
    const QPointF &topRight   = corners[1];
    const QPointF &bottomLeft = corners[2];

    double minX = corners[0].x(), maxX = minX, minY = corners[0].y(), maxY = minY;
    for (const QPointF &corner : corners) {
        minX = std::min(minX, corner.x());
        maxX = std::max(maxX, corner.x());
        minY = std::min(minY, corner.y());
        maxY = std::max(maxY, corner.y());
    }
    const double edgePerPixel = std::hypot(topRight.x() - topLeft.x(), topRight.y() - topLeft.y()) / image.width();
    if (!(edgePerPixel > 0.0) || !(maxX > minX) || !(maxY > minY)) {
        result.error = tr("%1 has no size on the map.").arg(fileName);
        return result;
    }
    // Map-copy pixels per Mercator unit: the image's own resolution, shrunk to fit MaxImageSide.
    const double fit = std::min(1.0, MaxImageSide / (std::max(maxX - minX, maxY - minY) / edgePerPixel));
    const double scale = fit / edgePerPixel;
    const int outWidth  = std::clamp(static_cast<int>(std::ceil((maxX - minX) * scale)), 1, MaxImageSide);
    const int outHeight = std::clamp(static_cast<int>(std::ceil((maxY - minY) * scale)), 1, MaxImageSide);

    // A big reduction looks better done by smooth scaling than by the painter's filter.
    if (fit < 0.5) {
        image = image.scaled(std::max(1, static_cast<int>(std::lround(image.width() * fit))),
                             std::max(1, static_cast<int>(std::lround(image.height() * fit))),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    const auto toOut = [&](const QPointF &mercator) {
        return QPointF((mercator.x() - minX) * scale, (mercator.y() - minY) * scale);
    };
    const QPointF o00 = toOut(topLeft);
    const QPointF o10 = toOut(topRight);
    const QPointF o01 = toOut(bottomLeft);
    const QTransform transform((o10.x() - o00.x()) / image.width(), (o10.y() - o00.y()) / image.width(),
                               (o01.x() - o00.x()) / image.height(), (o01.y() - o00.y()) / image.height(),
                               o00.x(), o00.y());

    QImage out(outWidth, outHeight, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(transform);
        painter.drawImage(QPointF(0, 0), image);
    }

    Layer &layer     = result.layer;
    layer.id         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name       = QFileInfo(path).completeBaseName();
    layer.sourcePath = path;
    layer.north      = mercatorYToLat(minY);
    layer.south      = mercatorYToLat(minY + outHeight / scale);
    layer.west       = minX * 360.0 - 180.0;
    layer.east       = (minX + outWidth / scale) * 360.0 - 180.0;
    layer.width      = outWidth;
    layer.height     = outHeight;
    layer.zoomLevel  = std::log2(scale / MapTileSize);

    if (!QDir().mkpath(folder) || !out.save(folder + QStringLiteral("/") + layer.id + QStringLiteral(".png"), "PNG")) {
        result.error = tr("Couldn't save the map copy of %1.").arg(fileName);
        return result;
    }
    qCDebug(SprayMapLayersLog) << "Loaded" << path << "image" << pick << image.size() << "->" << out.size()
                               << "zoom" << layer.zoomLevel;
    return result;
}

void SprayMapLayers::_loaded()
{
    const LoadResult result = _watcher.result();
    emit loadingChanged();
    if (!result.error.isEmpty()) {
        QGC::showAppMessage(result.error);
        return;
    }
    _layers.append(result.layer);
    _save();
    emit layersChanged();
    emit layerAdded(static_cast<int>(_layers.count() - 1));
}

QString SprayMapLayers::_folder() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/map-layers");
}

QString SprayMapLayers::_imagePath(const Layer &layer) const
{
    return _folder() + QStringLiteral("/") + layer.id + QStringLiteral(".png");
}

void SprayMapLayers::_save() const
{
    QJsonArray array;
    for (const Layer &layer : _layers) {
        QJsonObject object;
        object[QStringLiteral("id")]         = layer.id;
        object[QStringLiteral("name")]       = layer.name;
        object[QStringLiteral("sourcePath")] = layer.sourcePath;
        object[QStringLiteral("north")]      = layer.north;
        object[QStringLiteral("south")]      = layer.south;
        object[QStringLiteral("east")]       = layer.east;
        object[QStringLiteral("west")]       = layer.west;
        object[QStringLiteral("width")]      = layer.width;
        object[QStringLiteral("height")]     = layer.height;
        object[QStringLiteral("zoomLevel")]  = layer.zoomLevel;
        object[QStringLiteral("visible")]    = layer.visible;
        object[QStringLiteral("opacity")]    = layer.opacity;
        array.append(object);
    }
    QFile file(_folder() + QStringLiteral("/layers.json"));
    if (!QDir().mkpath(_folder()) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(QJsonDocument(array).toJson()) < 0) {
        qCWarning(SprayMapLayersLog) << "Couldn't save the map layer list" << file.fileName();
    }
}

void SprayMapLayers::_restore()
{
    QFile file(_folder() + QStringLiteral("/layers.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Layer layer;
        layer.id         = object[QStringLiteral("id")].toString();
        layer.name       = object[QStringLiteral("name")].toString();
        layer.sourcePath = object[QStringLiteral("sourcePath")].toString();
        layer.north      = object[QStringLiteral("north")].toDouble();
        layer.south      = object[QStringLiteral("south")].toDouble();
        layer.east       = object[QStringLiteral("east")].toDouble();
        layer.west       = object[QStringLiteral("west")].toDouble();
        layer.width      = object[QStringLiteral("width")].toInt();
        layer.height     = object[QStringLiteral("height")].toInt();
        layer.zoomLevel  = object[QStringLiteral("zoomLevel")].toDouble();
        layer.visible    = object[QStringLiteral("visible")].toBool(true);
        layer.opacity    = object[QStringLiteral("opacity")].toDouble(1.0);
        if (!layer.id.isEmpty() && layer.width > 0 && layer.height > 0 && QFile::exists(_imagePath(layer))) {
            _layers.append(layer);
        }
    }
}

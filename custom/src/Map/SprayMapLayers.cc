#include "SprayMapLayers.h"

#include "AppMessages.h"
#include "GeoTiff.h"
#include "QGCLoggingCategory.h"
#include "WebMercator.h"

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
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <climits>
#include <cmath>

QGC_LOGGING_CATEGORY(SprayMapLayersLog, "Custom.SprayMapLayers")

namespace wm = spray::webmercator;

namespace {

constexpr int    TileSize       = 256;      // also QGC's map tiles: the world is 256 * 2^z px wide at zoom z
constexpr int    MaxTileLevel   = 22;       // about 2 cm per pixel
constexpr qint64 MaxTopTiles    = 40000;    // at the most detailed level; beyond it the level drops
constexpr int    JpegQuality    = 85;
constexpr int    MaxDownloads   = 6;        // Save Offline: tiles fetched at once
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
constexpr qint64 DecodeBudgetMB = 384;      // largest image decoded at once
#else
constexpr qint64 DecodeBudgetMB = 1024;
#endif

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

QPointF toMercator(const spray::LatLon &point)
{
    return QPointF(wm::xFromLon(point.lon), wm::yFromLat(point.lat));
}

enum class Fill { Empty, Opaque, Partial };

Fill fillOf(const QImage &image)
{
    bool anyClear = false;
    bool anyInk   = false;
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const int alpha = qAlpha(line[x]);
            anyInk   = anyInk || alpha != 0;
            anyClear = anyClear || alpha != 255;
            if (anyInk && anyClear) {
                return Fill::Partial;
            }
        }
    }
    return anyInk ? Fill::Opaque : Fill::Empty;
}

QString tileFile(const QString &tileFolder, int level, int x, int y, bool jpeg)
{
    return QStringLiteral("%1/%2/%3_%4.%5").arg(tileFolder).arg(level).arg(x).arg(y).arg(jpeg ? QStringLiteral("jpg") : QStringLiteral("png"));
}

// Saves a tile unless it's empty: JPEG when fully opaque (small), PNG at the image's edges.
bool saveTile(const QImage &tile, const QString &tileFolder, int level, int x, int y,
              SprayMapLayers::TileIndex &index)
{
    const Fill fill = fillOf(tile);
    if (fill == Fill::Empty) {
        return true;
    }
    const bool jpeg = fill == Fill::Opaque;
    const QString path = tileFile(tileFolder, level, x, y, jpeg);
    const bool saved = jpeg ? tile.convertToFormat(QImage::Format_RGB32).save(path, "JPG", JpegQuality)
                            : tile.save(path, "PNG");
    if (saved) {
        index.insert(SprayMapLayers::tileKey(level, x, y), jpeg);
    }
    return saved;
}

QJsonObject indexToJson(int minLevel, int maxLevel, const SprayMapLayers::TileIndex &index)
{
    // Flat list of level, x, y, jpeg (1/0) per tile.
    QJsonArray tiles;
    for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
        const quint64 key = it.key();
        tiles.append(static_cast<int>(key >> 56));
        tiles.append(static_cast<int>((key >> 28) & 0xFFFFFFF));
        tiles.append(static_cast<int>(key & 0xFFFFFFF));
        tiles.append(it.value() ? 1 : 0);
    }
    QJsonObject object;
    object[QStringLiteral("minLevel")] = minLevel;
    object[QStringLiteral("maxLevel")] = maxLevel;
    object[QStringLiteral("tiles")]    = tiles;
    return object;
}

} // namespace

quint64 SprayMapLayers::tileKey(int level, int x, int y)
{
    return (static_cast<quint64>(level) << 56) | (static_cast<quint64>(static_cast<quint32>(x)) << 28)
           | static_cast<quint64>(static_cast<quint32>(y));
}

SprayMapLayers::SprayMapLayers(QObject *parent)
    : QObject(parent)
    , _network(new QNetworkAccessManager(this))
{
    (void) connect(&_watcher, &QFutureWatcher<LoadResult>::finished, this, &SprayMapLayers::_loaded);
    (void) connect(&_watcher, &QFutureWatcher<LoadResult>::progressValueChanged, this, &SprayMapLayers::progressChanged);
    _restore();
}

SprayMapLayers::~SprayMapLayers()
{
    // Don't leave a tile job running past the app.
    _watcher.cancel();
    _watcher.waitForFinished();
}

QVariantList SprayMapLayers::layers() const
{
    QVariantList list;
    for (const Layer &layer : _layers) {
        QVariantMap map;
        map[QStringLiteral("name")]         = layer.name;
        // A QUrl: the folder name can have spaces. Link layers have no overview.
        map[QStringLiteral("url")]          = layer.width > 0 ? QUrl::fromLocalFile(_imagePath(layer)) : QUrl();
        map[QStringLiteral("north")]        = layer.north;
        map[QStringLiteral("south")]        = layer.south;
        map[QStringLiteral("east")]         = layer.east;
        map[QStringLiteral("west")]         = layer.west;
        map[QStringLiteral("width")]        = layer.width;
        map[QStringLiteral("height")]       = layer.height;
        map[QStringLiteral("zoomLevel")]    = layer.zoomLevel;
        map[QStringLiteral("tileMinLevel")] = layer.tiles ? layer.tileMinLevel : -1;
        map[QStringLiteral("tileMaxLevel")] = layer.tiles ? layer.tileMaxLevel : -1;
        map[QStringLiteral("visible")]      = layer.visible;
        map[QStringLiteral("remote")]       = !layer.urlTemplate.isEmpty();
        map[QStringLiteral("offline")]      = layer.offline;
        list.append(map);
    }
    return list;
}

QVariantList SprayMapLayers::tilesInView(int index, int level, double north, double south, double west, double east,
                                         int maxCount) const
{
    QVariantList list;
    if (index < 0 || index >= _layers.count()) {
        return list;
    }
    const Layer &layer = _layers[index];
    if (!layer.tiles || level < layer.tileMinLevel || level > layer.tileMaxLevel) {
        return list;
    }
    const wm::TileRange view  = wm::tileRange(wm::xFromLon(west), wm::yFromLat(north), wm::xFromLon(east), wm::yFromLat(south), level);
    const wm::TileRange image = wm::tileRange(wm::xFromLon(layer.west), wm::yFromLat(layer.north),
                                              wm::xFromLon(layer.east), wm::yFromLat(layer.south), level);
    const wm::TileRange range = wm::intersect(view, image);
    const double tiles = static_cast<double>(1 << level);
    for (int y = range.y0; y <= range.y1; y++) {
        for (int x = range.x0; x <= range.x1; x++) {
            const auto it = layer.tiles->constFind(tileKey(level, x, y));
            if (it == layer.tiles->constEnd()) {
                continue;
            }
            if (list.count() >= maxCount) {
                return list;
            }
            QVariantMap tile;
            tile[QStringLiteral("key")]   = QStringLiteral("%1/%2/%3").arg(level).arg(x).arg(y);
            // Link layers: the saved copy if there is one, else from the link.
            const QString path = _tilePath(layer, level, x, y, it.value());
            if (layer.urlTemplate.isEmpty() || QFile::exists(path)) {
                tile[QStringLiteral("url")] = QUrl::fromLocalFile(path);
            } else {
                tile[QStringLiteral("url")] = QUrl(QString(layer.urlTemplate)
                                                       .replace(QStringLiteral("{z}"), QString::number(level))
                                                       .replace(QStringLiteral("{x}"), QString::number(x))
                                                       .replace(QStringLiteral("{y}"), QString::number(y)));
            }
            tile[QStringLiteral("north")] = wm::latFromY(y / tiles);
            tile[QStringLiteral("west")]  = wm::lonFromX(x / tiles);
            list.append(tile);
        }
    }
    return list;
}

void SprayMapLayers::addGeoTiff(const QString &path)
{
    if (loading()) {
        QGC::showAppMessage(tr("Still working on the last layer. Add this one when it's done."));
        return;
    }
    _setBusy(tr("Making map tiles"));
    _watcher.setFuture(QtConcurrent::run(&SprayMapLayers::loadGeoTiff, path, _folder()));
    emit progressChanged();
}

void SprayMapLayers::addTileUrl(const QString &urlTemplate)
{
    if (loading()) {
        QGC::showAppMessage(tr("Still working on the last layer. Add this one when it's done."));
        return;
    }
    // https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<path with {z}/{x}/{y}>
    static const QRegularExpression github(QStringLiteral("^https://raw\\.githubusercontent\\.com/([^/]+)/([^/]+)/([^/]+)/(.+)$"));
    const QString link = urlTemplate.trimmed();
    const QRegularExpressionMatch match = github.match(link);
    if (!match.hasMatch() || !link.contains(QStringLiteral("{z}")) || !link.contains(QStringLiteral("{x}"))
        || !link.contains(QStringLiteral("{y}"))) {
        QGC::showAppMessage(tr("Paste a GitHub tile link with {z}, {x} and {y} in it, like "
                               "https://raw.githubusercontent.com/you/repo/main/{z}/{x}/{y}.png"));
        return;
    }
    const QString owner        = match.captured(1);
    const QString repo         = match.captured(2);
    const QString branch       = match.captured(3);
    const QString pathTemplate = match.captured(4);

    // Which files in the repository are tiles: the path with {z}, {x}, {y} as numbers.
    QString pattern = QRegularExpression::escape(pathTemplate);
    pattern.replace(QStringLiteral("\\{z\\}"), QStringLiteral("(?<z>\\d+)"))
           .replace(QStringLiteral("\\{x\\}"), QStringLiteral("(?<x>\\d+)"))
           .replace(QStringLiteral("\\{y\\}"), QStringLiteral("(?<y>\\d+)"));
    const QRegularExpression tilePath(QStringLiteral("^") + pattern + QStringLiteral("$"));
    const bool jpeg = pathTemplate.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
                      || pathTemplate.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive);

    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/%1/%2/git/trees/%3?recursive=1").arg(owner, repo, branch)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "SprayGCS");
    QNetworkReply *reply = _network->get(request);
    _setBusy(tr("Reading the tile list"));
    _setProgress(0);

    (void) connect(reply, &QNetworkReply::finished, this, [this, reply, link, repo, tilePath, jpeg]() {
        reply->deleteLater();
        _setBusy(QString());
        if (reply->error() != QNetworkReply::NoError) {
            QGC::showAppMessage(tr("Couldn't get the tile list from GitHub (%1). Check the link and the internet connection.")
                                    .arg(reply->errorString()));
            return;
        }
        const QJsonObject tree = QJsonDocument::fromJson(reply->readAll()).object();
        auto index = std::make_shared<TileIndex>();
        int minLevel = MaxTileLevel + 1;
        int maxLevel = -1;
        for (const QJsonValue &entry : tree[QStringLiteral("tree")].toArray()) {
            const QJsonObject file = entry.toObject();
            if (file[QStringLiteral("type")].toString() != QStringLiteral("blob")) {
                continue;
            }
            const QRegularExpressionMatch tile = tilePath.match(file[QStringLiteral("path")].toString());
            if (!tile.hasMatch()) {
                continue;
            }
            const int level = tile.captured(QStringLiteral("z")).toInt();
            if (level < 0 || level > MaxTileLevel) {
                continue;
            }
            index->insert(tileKey(level, tile.captured(QStringLiteral("x")).toInt(), tile.captured(QStringLiteral("y")).toInt()), jpeg);
            minLevel = std::min(minLevel, level);
            maxLevel = std::max(maxLevel, level);
        }
        if (index->isEmpty()) {
            QGC::showAppMessage(tr("No tiles found at that link. Check the branch and the {z}/{x}/{y} part of the path."));
            return;
        }
        if (tree[QStringLiteral("truncated")].toBool()) {
            QGC::showAppMessage(tr("The repository is too big for GitHub to list in full; some tiles may be missing."));
        }

        // Where it is: the extent of the most detailed level.
        int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
        for (auto it = index->constBegin(); it != index->constEnd(); ++it) {
            if (static_cast<int>(it.key() >> 56) != maxLevel) {
                continue;
            }
            const int x = static_cast<int>((it.key() >> 28) & 0xFFFFFFF);
            const int y = static_cast<int>(it.key() & 0xFFFFFFF);
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        const double tiles = static_cast<double>(1 << maxLevel);

        Layer layer;
        layer.id           = QUuid::createUuid().toString(QUuid::WithoutBraces);
        layer.name         = repo;
        layer.sourcePath   = link;
        layer.urlTemplate  = link;
        layer.north        = wm::latFromY(y0 / tiles);
        layer.south        = wm::latFromY((y1 + 1) / tiles);
        layer.west         = wm::lonFromX(x0 / tiles);
        layer.east         = wm::lonFromX((x1 + 1) / tiles);
        layer.zoomLevel    = minLevel - 1;   // no overview: the tiles show at every zoom level
        layer.tileMinLevel = minLevel;
        layer.tileMaxLevel = maxLevel;
        layer.tiles        = index;
        _saveIndex(layer);
        _layers.append(layer);
        _save();
        emit layersChanged();
        emit layerAdded(static_cast<int>(_layers.count() - 1));
        QGC::showAppMessage(tr("Added %1 (%2 tiles, zoom %3 to %4). It loads from GitHub; Save Offline keeps it on this "
                               "device for fields without internet.").arg(repo).arg(index->count()).arg(minLevel).arg(maxLevel));
    });
}

void SprayMapLayers::saveOffline(int index)
{
    if (index < 0 || index >= _layers.count() || _layers[index].urlTemplate.isEmpty() || !_layers[index].tiles) {
        return;
    }
    if (loading()) {
        QGC::showAppMessage(tr("Still working on the last layer. Save this one when it's done."));
        return;
    }
    const Layer &layer = _layers[index];
    _downloadQueue.clear();
    for (auto it = layer.tiles->constBegin(); it != layer.tiles->constEnd(); ++it) {
        const int level = static_cast<int>(it.key() >> 56);
        const int x = static_cast<int>((it.key() >> 28) & 0xFFFFFFF);
        const int y = static_cast<int>(it.key() & 0xFFFFFFF);
        if (!QFile::exists(_tilePath(layer, level, x, y, it.value()))) {
            _downloadQueue.append(std::make_tuple(level, x, y));
        }
    }
    _downloadId     = layer.id;
    _downloadTotal  = static_cast<int>(_downloadQueue.count());
    _downloadDone   = 0;
    _downloadFailed = 0;
    _inFlight       = 0;
    _setBusy(tr("Saving tiles"));
    _setProgress(0);
    for (int i = 0; i < MaxDownloads; i++) {
        _downloadNext();
    }
    if (_downloadTotal == 0) {
        _downloadFinished();
    }
}

void SprayMapLayers::_downloadNext()
{
    const auto layerIt = std::find_if(_layers.begin(), _layers.end(), [this](const Layer &l) { return l.id == _downloadId; });
    if (_downloadQueue.isEmpty() || layerIt == _layers.end()) {
        return;
    }
    const auto [level, x, y] = _downloadQueue.takeFirst();
    const Layer layer = *layerIt;
    const bool jpeg = layer.tiles->value(tileKey(level, x, y));
    const QString path = _tilePath(layer, level, x, y, jpeg);
    QNetworkRequest request(QUrl(QString(layer.urlTemplate)
                                     .replace(QStringLiteral("{z}"), QString::number(level))
                                     .replace(QStringLiteral("{x}"), QString::number(x))
                                     .replace(QStringLiteral("{y}"), QString::number(y))));
    request.setRawHeader("User-Agent", "SprayGCS");
    QNetworkReply *reply = _network->get(request);
    _inFlight++;

    (void) connect(reply, &QNetworkReply::finished, this, [this, reply, path, id = layer.id]() {
        reply->deleteLater();
        _inFlight--;
        if (id == _downloadId) {
            bool saved = false;
            if (reply->error() == QNetworkReply::NoError) {
                (void) QDir().mkpath(QFileInfo(path).absolutePath());
                QSaveFile file(path);
                saved = file.open(QIODevice::WriteOnly) && file.write(reply->readAll()) >= 0 && file.commit();
            }
            if (!saved) {
                _downloadFailed++;
            }
            _downloadDone++;
            _setProgress(_downloadTotal > 0 ? 100 * _downloadDone / _downloadTotal : 100);
        }
        if (_downloadQueue.isEmpty() && _inFlight == 0) {
            _downloadFinished();
        } else {
            _downloadNext();
        }
    });
}

void SprayMapLayers::_downloadFinished()
{
    const QString id = _downloadId;
    _downloadId.clear();
    _setBusy(QString());
    for (Layer &layer : _layers) {
        if (layer.id != id) {
            continue;
        }
        if (_downloadFailed == 0) {
            layer.offline = true;
            _save();
            emit layersChanged();
            QGC::showAppMessage(tr("%1 is saved on this device and works without internet.").arg(layer.name));
        } else {
            QGC::showAppMessage(tr("%1 of %2 tiles of %3 didn't download. Tap Save Offline again to retry them.")
                                    .arg(_downloadFailed).arg(_downloadTotal).arg(layer.name));
        }
    }
}

void SprayMapLayers::_setBusy(const QString &text)
{
    if (_busyText != text) {
        _busyText = text;
        emit loadingChanged();
    }
}

void SprayMapLayers::_setProgress(int value)
{
    if (_progress != value) {
        _progress = value;
        emit progressChanged();
    }
}

void SprayMapLayers::removeLayer(int index)
{
    if (index < 0 || index >= _layers.count()) {
        return;
    }
    if (_layers[index].id == _downloadId) {
        // Stop saving it; tiles still on their way are dropped.
        _downloadQueue.clear();
        _downloadId.clear();
        if (_inFlight == 0) {
            _setBusy(QString());
        }
    }
    (void) QFile::remove(_imagePath(_layers[index]));
    (void) QDir(_folder() + QStringLiteral("/") + _layers[index].id).removeRecursively();
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

void SprayMapLayers::loadGeoTiff(QPromise<LoadResult> &promise, const QString &path, const QString &folder)
{
    LoadResult result;
    const QString fileName = QFileInfo(path).fileName();
    const auto fail = [&](const QString &error) {
        result.error = error;
        promise.addResult(result);
    };
    promise.setProgressRange(0, 100);

    // ---- where it is ----
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(tr("Couldn't open %1.").arg(fileName));
        return;
    }
    spray::GeoTiff geo;
    FileByteSource source(file);
    switch (geo.read(source)) {
    case spray::GeoTiffError::None:
        break;
    case spray::GeoTiffError::NotTiff:
        fail(tr("%1 isn't a TIFF image, or it's damaged.").arg(fileName));
        return;
    case spray::GeoTiffError::NoGeoreference:
        fail(tr("%1 has no map position. Export it from the mapping software as a GeoTIFF.").arg(fileName));
        return;
    case spray::GeoTiffError::UnsupportedCrs:
        fail(tr("%1 uses a coordinate system SprayGCS can't place (EPSG:%2). Export it in WGS84 (EPSG:4326), "
                "Web Mercator (EPSG:3857) or UTM.").arg(fileName).arg(geo.epsgCode()));
        return;
    }
    file.close();

    // ---- the pixels: the most detailed image in the file that fits the memory budget ----
    const std::vector<spray::GeoTiffImage> &images = geo.images();
    const spray::GeoTiffImage &full = images[0];
    int pick = -1;
    qint64 pickPixels = 0;
    for (size_t i = 0; i < images.size(); i++) {
        const qint64 pixels = static_cast<qint64>(images[i].width) * images[i].height;
        if (!images[i].isMask() && pixels * 4 / (1024 * 1024) <= DecodeBudgetMB && pixels > pickPixels) {
            pick       = static_cast<int>(i);
            pickPixels = pixels;
        }
    }
    if (pick < 0) {
        fail(tr("%1 is too big to load (%2 × %3 pixels) and has no smaller overviews. Export it at a lower "
                "resolution, or with overviews (pyramids), and add it again.").arg(fileName).arg(full.width).arg(full.height));
        return;
    }

    QImageReader reader(path, "tiff");
    if (pick != 0 && !reader.jumpToImage(pick)) {
        fail(tr("Couldn't read the pixels of %1: %2").arg(fileName, reader.errorString()));
        return;
    }
    const qint64 decodeMB = pickPixels * 8 / (1024 * 1024) + 64;   // 16-bit images decode at 8 bytes a pixel
    if (QImageReader::allocationLimit() > 0 && decodeMB > QImageReader::allocationLimit()) {
        QImageReader::setAllocationLimit(static_cast<int>(decodeMB));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        fail(tr("Couldn't read the pixels of %1: %2").arg(fileName, reader.errorString()));
        return;
    }
    promise.setProgressValue(15);
    if (promise.isCanceled()) {
        return;
    }

    // GDAL's "no data" colour (often black around the field) becomes transparent.
    const bool hadAlpha = image.hasAlphaChannel();
    image = std::move(image).convertToFormat(QImage::Format_ARGB32);
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
    image = std::move(image).convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // ---- onto the map: corners in Web Mercator, then an affine fit (exact for Web Mercator
    // and lat/lon images, centimetres off for a field-sized UTM image) ----
    const double toFullX = static_cast<double>(full.width) / image.width();
    const double toFullY = static_cast<double>(full.height) / image.height();
    QPointF corners[4];
    const int cornerPixels[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        spray::LatLon latLon;
        if (!geo.pixelToLatLon(cornerPixels[i][0] * image.width() * toFullX, cornerPixels[i][1] * image.height() * toFullY, latLon)) {
            fail(tr("%1 has a corner off the map.").arg(fileName));
            return;
        }
        corners[i] = toMercator(latLon);
    }
    double minX = corners[0].x(), maxX = minX, minY = corners[0].y(), maxY = minY;
    for (const QPointF &corner : corners) {
        minX = std::min(minX, corner.x());
        maxX = std::max(maxX, corner.x());
        minY = std::min(minY, corner.y());
        maxY = std::max(maxY, corner.y());
    }
    const double edgePerPixel = std::hypot(corners[1].x() - corners[0].x(), corners[1].y() - corners[0].y()) / image.width();
    if (!(edgePerPixel > 0.0) || !(maxX > minX) || !(maxY > minY)) {
        fail(tr("%1 has no size on the map.").arg(fileName));
        return;
    }
    // Image pixel -> normalised Web Mercator.
    const QTransform toMap((corners[1].x() - corners[0].x()) / image.width(), (corners[1].y() - corners[0].y()) / image.width(),
                           (corners[2].x() - corners[0].x()) / image.height(), (corners[2].y() - corners[0].y()) / image.height(),
                           corners[0].x(), corners[0].y());

    Layer &layer     = result.layer;
    layer.id         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name       = QFileInfo(path).completeBaseName();
    layer.sourcePath = path;

    // ---- the overview: the whole image at up to MaxImageSide px ----
    {
        const double fit   = std::min(1.0, MaxImageSide / (std::max(maxX - minX, maxY - minY) / edgePerPixel));
        const double scale = fit / edgePerPixel;   // overview pixels per Mercator unit
        const int outWidth  = std::clamp(static_cast<int>(std::ceil((maxX - minX) * scale)), 1, MaxImageSide);
        const int outHeight = std::clamp(static_cast<int>(std::ceil((maxY - minY) * scale)), 1, MaxImageSide);

        // A big reduction looks better done by smooth scaling than by the painter's filter.
        QImage small;
        QTransform smallToMap = toMap;
        if (fit < 0.5) {
            small = image.scaled(std::max(1, static_cast<int>(std::lround(image.width() * fit))),
                                 std::max(1, static_cast<int>(std::lround(image.height() * fit))),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            smallToMap = QTransform::fromScale(static_cast<double>(image.width()) / small.width(),
                                               static_cast<double>(image.height()) / small.height()) * toMap;
        }
        const QImage &drawn = small.isNull() ? image : small;

        QImage out(outWidth, outHeight, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        {
            QPainter painter(&out);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.setTransform(smallToMap * QTransform::fromScale(scale, scale) * QTransform::fromTranslate(-minX * scale, -minY * scale));
            painter.drawImage(QPointF(0, 0), drawn);
        }
        layer.north     = wm::latFromY(minY);
        layer.south     = wm::latFromY(minY + outHeight / scale);
        layer.west      = wm::lonFromX(minX);
        layer.east      = wm::lonFromX(minX + outWidth / scale);
        layer.width     = outWidth;
        layer.height    = outHeight;
        layer.zoomLevel = std::log2(scale / TileSize);

        if (!QDir().mkpath(folder) || !out.save(folder + QStringLiteral("/") + layer.id + QStringLiteral(".png"), "PNG")) {
            fail(tr("Couldn't save the map copy of %1.").arg(fileName));
            return;
        }
    }
    promise.setProgressValue(25);

    // ---- tiles, from the overview's zoom down to the photo's own detail ----
    const int minLevel = std::max(0, static_cast<int>(std::floor(layer.zoomLevel)));
    int maxLevel = std::min(MaxTileLevel, static_cast<int>(std::lround(std::log2(1.0 / (edgePerPixel * TileSize)))));
    while (maxLevel > minLevel && wm::tileRange(minX, minY, maxX, maxY, maxLevel).count() > MaxTopTiles) {
        maxLevel--;
    }
    if (maxLevel <= minLevel) {
        // The overview already holds all the detail there is.
        promise.addResult(result);
        return;
    }

    const QString tileFolder = folder + QStringLiteral("/") + layer.id;
    auto index = std::make_shared<TileIndex>();
    for (int level = minLevel; level <= maxLevel; level++) {
        (void) QDir().mkpath(QStringLiteral("%1/%2").arg(tileFolder).arg(level));
    }
    const auto cancel = [&]() {
        (void) QFile::remove(folder + QStringLiteral("/") + layer.id + QStringLiteral(".png"));
        (void) QDir(tileFolder).removeRecursively();
    };

    // The most detailed level, straight from the image.
    const wm::TileRange top = wm::tileRange(minX, minY, maxX, maxY, maxLevel);
    const double topScale = static_cast<double>(TileSize) * (1 << maxLevel);   // pixels per Mercator unit
    long long done = 0;
    QImage tile(TileSize, TileSize, QImage::Format_ARGB32_Premultiplied);
    for (int ty = top.y0; ty <= top.y1; ty++) {
        for (int tx = top.x0; tx <= top.x1; tx++) {
            tile.fill(Qt::transparent);
            {
                QPainter painter(&tile);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setTransform(toMap * QTransform::fromScale(topScale, topScale)
                                     * QTransform::fromTranslate(-tx * static_cast<double>(TileSize), -ty * static_cast<double>(TileSize)));
                painter.drawImage(QPointF(0, 0), image);
            }
            if (!saveTile(tile, tileFolder, maxLevel, tx, ty, *index)) {
                cancel();
                fail(tr("Couldn't save the map tiles of %1.").arg(fileName));
                return;
            }
            done++;
        }
        if (promise.isCanceled()) {
            cancel();
            return;
        }
        promise.setProgressValue(25 + static_cast<int>(60 * done / std::max<long long>(1, top.count())));
    }
    image = QImage();   // free the full image before the smaller levels

    // Each less detailed level from four tiles of the one below it.
    QImage quad(TileSize * 2, TileSize * 2, QImage::Format_ARGB32_Premultiplied);
    for (int level = maxLevel - 1; level >= minLevel; level--) {
        const wm::TileRange range = wm::tileRange(minX, minY, maxX, maxY, level);
        for (int ty = range.y0; ty <= range.y1; ty++) {
            for (int tx = range.x0; tx <= range.x1; tx++) {
                quad.fill(Qt::transparent);
                bool any = false;
                {
                    QPainter painter(&quad);
                    for (int child = 0; child < 4; child++) {
                        const int cx = tx * 2 + (child & 1);
                        const int cy = ty * 2 + (child >> 1);
                        const auto it = index->constFind(tileKey(level + 1, cx, cy));
                        if (it == index->constEnd()) {
                            continue;
                        }
                        const QImage childImage(tileFile(tileFolder, level + 1, cx, cy, it.value()));
                        if (!childImage.isNull()) {
                            painter.drawImage((child & 1) * TileSize, (child >> 1) * TileSize, childImage);
                            any = true;
                        }
                    }
                }
                if (any && !saveTile(quad.scaled(TileSize, TileSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                         .convertToFormat(QImage::Format_ARGB32_Premultiplied),
                                     tileFolder, level, tx, ty, *index)) {
                    cancel();
                    fail(tr("Couldn't save the map tiles of %1.").arg(fileName));
                    return;
                }
            }
            if (promise.isCanceled()) {
                cancel();
                return;
            }
        }
        promise.setProgressValue(85 + 15 * (maxLevel - level) / std::max(1, maxLevel - minLevel));
    }

    QFile indexFile(tileFolder + QStringLiteral("/tiles.json"));
    if (!indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || indexFile.write(QJsonDocument(indexToJson(minLevel, maxLevel, *index)).toJson(QJsonDocument::Compact)) < 0) {
        cancel();
        fail(tr("Couldn't save the map tiles of %1.").arg(fileName));
        return;
    }
    layer.tileMinLevel = minLevel;
    layer.tileMaxLevel = maxLevel;
    layer.tiles        = index;
    qCDebug(SprayMapLayersLog) << "Loaded" << path << "image" << pick << "overview" << layer.width << "x" << layer.height
                               << "tiles" << index->count() << "levels" << minLevel << "-" << maxLevel;
    promise.addResult(result);
}

void SprayMapLayers::_loaded()
{
    _setBusy(QString());
    emit progressChanged();
    if (_watcher.isCanceled() || _watcher.future().resultCount() == 0) {
        return;
    }
    const LoadResult result = _watcher.result();
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

QString SprayMapLayers::_tilePath(const Layer &layer, int level, int x, int y, bool jpeg) const
{
    return tileFile(_folder() + QStringLiteral("/") + layer.id, level, x, y, jpeg);
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
        if (!layer.urlTemplate.isEmpty()) {
            object[QStringLiteral("urlTemplate")] = layer.urlTemplate;
            object[QStringLiteral("offline")]     = layer.offline;
        }
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
        layer.urlTemplate = object[QStringLiteral("urlTemplate")].toString();
        layer.offline     = object[QStringLiteral("offline")].toBool(false);
        const bool hasOverview = layer.width > 0 && layer.height > 0 && QFile::exists(_imagePath(layer));
        if (layer.id.isEmpty() || (layer.urlTemplate.isEmpty() && !hasOverview)) {
            continue;
        }

        // Its tiles, if it has them (images added before tiles existed have only the overview).
        QFile indexFile(_folder() + QStringLiteral("/") + layer.id + QStringLiteral("/tiles.json"));
        if (indexFile.open(QIODevice::ReadOnly)) {
            const QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
            const QJsonArray tiles = index[QStringLiteral("tiles")].toArray();
            auto tileIndex = std::make_shared<TileIndex>();
            tileIndex->reserve(tiles.count() / 4);
            for (qsizetype i = 0; i + 3 < tiles.count(); i += 4) {
                tileIndex->insert(tileKey(tiles[i].toInt(), tiles[i + 1].toInt(), tiles[i + 2].toInt()), tiles[i + 3].toInt() != 0);
            }
            if (!tileIndex->isEmpty()) {
                layer.tileMinLevel = index[QStringLiteral("minLevel")].toInt(-1);
                layer.tileMaxLevel = index[QStringLiteral("maxLevel")].toInt(-1);
                layer.tiles        = tileIndex;
            }
        }
        if (!layer.urlTemplate.isEmpty() && !layer.tiles) {
            continue;   // a link layer is its tile list
        }
        _layers.append(layer);
    }
}

void SprayMapLayers::_saveIndex(const Layer &layer) const
{
    if (!layer.tiles) {
        return;
    }
    const QString tileFolder = _folder() + QStringLiteral("/") + layer.id;
    QFile file(tileFolder + QStringLiteral("/tiles.json"));
    if (!QDir().mkpath(tileFolder) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(QJsonDocument(indexToJson(layer.tileMinLevel, layer.tileMaxLevel, *layer.tiles)).toJson(QJsonDocument::Compact)) < 0) {
        qCWarning(SprayMapLayersLog) << "Couldn't save the tile list" << file.fileName();
    }
}

#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPromise>
#include <QtCore/QString>
#include <QtCore/QVariantList>

#include <memory>
#include <tuple>

class QNetworkAccessManager;

/// Field images (GeoTIFF orthomosaics) shown on the Plan and Fly maps under the
/// plan. Adding one reads the GeoTIFF in the background and keeps, in the app's
/// data folder:
///   * an overview: the whole image on the map's Web Mercator grid at up to
///     MaxImageSide px, drawn when zoomed out;
///   * map tiles (XYZ, 256 px) from the overview's zoom level down to the
///     photo's own detail, of which the maps draw only those in view.
/// So the layers come back quickly after a restart.
///
/// A layer can also be map tiles hosted on GitHub (a raw.githubusercontent.com
/// link with {z}, {x} and {y}): the repository's file list says which tiles
/// exist; they load from GitHub until Save Offline keeps a copy here.
class SprayMapLayers : public QObject
{
    Q_OBJECT

    /// One map per layer: name, url (the overview), north, south, east, west,
    /// width, height (overview pixels), zoomLevel (where the overview shows at
    /// natural size), tileMinLevel, tileMaxLevel (-1: no tiles), visible,
    /// remote (tiles from a link), offline (a remote layer saved here).
    Q_PROPERTY(QVariantList layers   READ layers   NOTIFY layersChanged)
    Q_PROPERTY(bool         loading  READ loading  NOTIFY loadingChanged)    ///< a GeoTIFF or tile link is being worked on
    Q_PROPERTY(QString      busyText READ busyText NOTIFY loadingChanged)    ///< what, for the button
    Q_PROPERTY(int          progress READ progress NOTIFY progressChanged)   ///< of that, 0..100

public:
    /// Which tiles exist (key: level, x, y), and whether each is a JPEG (else PNG).
    using TileIndex = QHash<quint64, bool>;

    struct Layer {
        QString id;          ///< also the name of its overview file and tile folder
        QString name;
        QString sourcePath;
        double  north        = 0.0;
        double  south        = 0.0;
        double  east         = 0.0;
        double  west         = 0.0;
        int     width        = 0;
        int     height       = 0;
        double  zoomLevel    = 0.0;
        int     tileMinLevel = -1;
        int     tileMaxLevel = -1;
        bool    visible      = true;
        QString urlTemplate;          ///< tiles from this link ({z}, {x}, {y}); empty for a GeoTIFF
        bool    offline      = false; ///< all of a link's tiles are saved here
        std::shared_ptr<const TileIndex> tiles;
    };

    struct LoadResult {
        Layer   layer;
        QString error;       ///< empty on success
    };

    /// Largest side of the overview, in pixels.
    static constexpr int MaxImageSide = 4096;

    explicit SprayMapLayers(QObject *parent = nullptr);
    ~SprayMapLayers() override;

    QVariantList layers() const;
    bool loading() const { return !_busyText.isEmpty(); }
    QString busyText() const { return _busyText; }
    int progress() const { return _watcher.isRunning() ? _watcher.progressValue() : _progress; }

    Q_INVOKABLE void addGeoTiff(const QString &path);
    /// Adds map tiles hosted on GitHub, e.g.
    /// https://raw.githubusercontent.com/owner/repo/main/{z}/{x}/{y}.png
    Q_INVOKABLE void addTileUrl(const QString &urlTemplate);
    /// Downloads all of a link layer's tiles, so it works without internet.
    Q_INVOKABLE void saveOffline(int index);
    Q_INVOKABLE void removeLayer(int index);
    Q_INVOKABLE void setLayerVisible(int index, bool visible);

    /// Tiles of layer `index` at `level` within the given edges (at most `maxCount`):
    /// each a map of key ("level/x/y"), url, north, west (its top-left corner).
    Q_INVOKABLE QVariantList tilesInView(int index, int level, double north, double south, double west, double east,
                                         int maxCount) const;

    /// Reads a GeoTIFF and writes its overview and tiles into `folder` (runs off the GUI thread).
    static void loadGeoTiff(QPromise<LoadResult> &promise, const QString &path, const QString &folder);

    static quint64 tileKey(int level, int x, int y);

signals:
    void layersChanged();
    void loadingChanged();
    void progressChanged();
    /// A GeoTIFF was read and added as layer `index` (the maps can show it).
    void layerAdded(int index);

private:
    void _loaded();
    void _save() const;
    void _restore();
    QString _folder() const;
    QString _imagePath(const Layer &layer) const;
    QString _tilePath(const Layer &layer, int level, int x, int y, bool jpeg) const;
    void _setBusy(const QString &text);
    void _setProgress(int value);
    void _saveIndex(const Layer &layer) const;
    void _downloadNext();
    void _downloadFinished();

    QList<Layer>               _layers;
    QFutureWatcher<LoadResult> _watcher;
    QNetworkAccessManager     *_network  = nullptr;
    QString                    _busyText;
    int                        _progress = 0;

    // Save Offline of one link layer.
    QString                              _downloadId;
    QList<std::tuple<int, int, int>>     _downloadQueue;   ///< level, x, y
    int                                  _downloadTotal  = 0;
    int                                  _downloadDone   = 0;
    int                                  _downloadFailed = 0;
    int                                  _inFlight       = 0;
};

#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPromise>
#include <QtCore/QString>
#include <QtCore/QVariantList>

#include <memory>

/// Field images (GeoTIFF orthomosaics) shown on the Plan and Fly maps under the
/// plan. Adding one reads the GeoTIFF in the background and keeps, in the app's
/// data folder:
///   * an overview: the whole image on the map's Web Mercator grid at up to
///     MaxImageSide px, drawn when zoomed out;
///   * map tiles (XYZ, 256 px) from the overview's zoom level down to the
///     photo's own detail, of which the maps draw only those in view.
/// So the layers come back quickly after a restart.
class SprayMapLayers : public QObject
{
    Q_OBJECT

    /// One map per layer: name, url (the overview), north, south, east, west,
    /// width, height (overview pixels), zoomLevel (where the overview shows at
    /// natural size), tileMinLevel, tileMaxLevel (-1: no tiles), visible, opacity.
    Q_PROPERTY(QVariantList layers   READ layers   NOTIFY layersChanged)
    Q_PROPERTY(bool         loading  READ loading  NOTIFY loadingChanged)    ///< a GeoTIFF is being read
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
        double  opacity      = 1.0;
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
    bool loading() const { return _watcher.isRunning(); }
    int progress() const { return _watcher.isRunning() ? _watcher.progressValue() : 0; }

    Q_INVOKABLE void addGeoTiff(const QString &path);
    Q_INVOKABLE void removeLayer(int index);
    Q_INVOKABLE void setLayerVisible(int index, bool visible);
    Q_INVOKABLE void setLayerOpacity(int index, double opacity);

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

    QList<Layer>               _layers;
    QFutureWatcher<LoadResult> _watcher;
};

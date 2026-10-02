#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>

/// Field images (GeoTIFF orthomosaics) shown on the Plan and Fly maps under the
/// plan. Adding one reads the GeoTIFF in the background, reprojects it onto the
/// map's Web Mercator grid at up to MaxImageSide px, and keeps that copy in the
/// app's data folder, so the layers come back quickly after a restart.
class SprayMapLayers : public QObject
{
    Q_OBJECT

    /// One map per layer: name, url (the map copy), north, west (top-left corner),
    /// width, height (pixels), zoomLevel (where the copy shows at natural size),
    /// visible, opacity.
    Q_PROPERTY(QVariantList layers  READ layers  NOTIFY layersChanged)
    Q_PROPERTY(bool         loading READ loading NOTIFY loadingChanged)   ///< a GeoTIFF is being read

public:
    struct Layer {
        QString id;          ///< also the map copy's file name
        QString name;
        QString sourcePath;
        double  north     = 0.0;
        double  south     = 0.0;
        double  east      = 0.0;
        double  west      = 0.0;
        int     width     = 0;
        int     height    = 0;
        double  zoomLevel = 0.0;
        bool    visible   = true;
        double  opacity   = 1.0;
    };

    struct LoadResult {
        Layer   layer;
        QString error;       ///< empty on success
    };

    /// Largest side of the map copy, in pixels.
    static constexpr int MaxImageSide = 4096;

    explicit SprayMapLayers(QObject *parent = nullptr);

    QVariantList layers() const;
    bool loading() const { return _watcher.isRunning(); }

    Q_INVOKABLE void addGeoTiff(const QString &path);
    Q_INVOKABLE void removeLayer(int index);
    Q_INVOKABLE void setLayerVisible(int index, bool visible);
    Q_INVOKABLE void setLayerOpacity(int index, double opacity);

    /// Reads a GeoTIFF and writes its map copy into `folder` (runs off the GUI thread).
    static LoadResult loadGeoTiff(const QString &path, const QString &folder);

signals:
    void layersChanged();
    void loadingChanged();

private:
    void _loaded();
    void _save() const;
    void _restore();
    QString _folder() const;
    QString _imagePath(const Layer &layer) const;

    QList<Layer>               _layers;
    QFutureWatcher<LoadResult> _watcher;
};

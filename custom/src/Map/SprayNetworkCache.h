#pragma once

#include <QtQml/QQmlNetworkAccessManagerFactory>

/// Network access for QML (images loaded from the internet, such as field image
/// tiles hosted on GitHub) through a disk cache that is preferred over the
/// network: a tile seen once loads from the cache after that, also offline.
class SprayNetworkCacheFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override;
};

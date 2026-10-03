#include "SprayNetworkCache.h"

#include <QtCore/QStandardPaths>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkDiskCache>
#include <QtNetwork/QNetworkRequest>

namespace {

constexpr qint64 CacheBytes = 1024LL * 1024 * 1024;

class PreferCacheAccessManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        QNetworkRequest cached(request);
        cached.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
        return QNetworkAccessManager::createRequest(op, cached, outgoingData);
    }
};

} // namespace

// Called by the QML engine, possibly from its loader threads: makes a new manager each time.
QNetworkAccessManager *SprayNetworkCacheFactory::create(QObject *parent)
{
    auto *manager = new PreferCacheAccessManager(parent);
    auto *cache   = new QNetworkDiskCache(manager);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/qml-network"));
    cache->setMaximumCacheSize(CacheBytes);
    manager->setCache(cache);
    return manager;
}

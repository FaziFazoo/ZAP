#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QGeoPositionInfoSource>
#include <QGeoPositionInfo>
#include <QImage>
#include <QDir>
#include <QMutex>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug> // Add this include at the top

/**
 * @brief Provides real-time OpenStreetCam/KartaView landscape images for Zap Telescope.
 * Usage: Instantiate, call start().
 */
class StreetViewLandscape : public QObject
{
    Q_OBJECT
public:
    explicit StreetViewLandscape(QObject* parent = nullptr)
        : QObject(parent),
          netManager(new QNetworkAccessManager(this)),
          gpsSource(QGeoPositionInfoSource::createDefaultSource(this)),
          cacheDir(QDir::homePath() + "/.zap_streetview_cache"),
          lastLat(0.0),
          lastLon(0.0),
          minDistance(100.0)
    {
        QDir().mkpath(cacheDir);
        connect(gpsSource, &QGeoPositionInfoSource::positionUpdated, this, &StreetViewLandscape::onPositionUpdated);
    }

    void start() {
        if (gpsSource) {
            gpsSource->setUpdateInterval(5000); // 5 seconds
            gpsSource->startUpdates();
        }
    }

    void stop() {
        if (gpsSource) gpsSource->stopUpdates();
    }

    void setCacheDir(const QString& dir) {
        cacheDir = dir;
        QDir().mkpath(cacheDir);
    }

    void setMinDistance(double meters) {
        minDistance = meters;
    }

signals:
    void imageReady(const QImage& image);
    void loadingStarted();
    void loadingFailed(const QString& error);

private slots:
    void onPositionUpdated(const QGeoPositionInfo& info) {
        double lat = info.coordinate().latitude();
        double lon = info.coordinate().longitude();

        // Print GPS coordinates to terminal
        qDebug() << "[StreetView] GPS updated: Latitude =" << lat << ", Longitude =" << lon;

        if (distanceMoved(lat, lon) < minDistance)
            return;

        lastLat = lat;
        lastLon = lon;

        if (isImageCached(lat, lon)) {
            qDebug() << "[StreetView] Using cached image for:" << lat << lon;
            emit imageReady(getCachedImage(lat, lon));
        } else {
            emit loadingStarted();
            fetchImage(lat, lon);
        }
    }

private:
    QNetworkAccessManager* netManager;
    QGeoPositionInfoSource* gpsSource;
    QString cacheDir;
    double lastLat, lastLon;
    double minDistance;
    QMutex cacheMutex;

    double distanceMoved(double lat, double lon) const {
        // Simple haversine formula
        constexpr double R = 6371000.0;
        double dLat = qDegreesToRadians(lat - lastLat);
        double dLon = qDegreesToRadians(lon - lastLon);
        double a = qSin(dLat/2)*qSin(dLat/2) +
                   qCos(qDegreesToRadians(lastLat)) * qCos(qDegreesToRadians(lat)) *
                   qSin(dLon/2)*qSin(dLon/2);
        double c = 2 * qAtan2(qSqrt(a), qSqrt(1-a));
        return R * c;
    }

    QString buildApiUrl(double latitude, double longitude, int zoomLevel = 15) const {
        return QString("https://api.openstreetcam.org/2.0/photo/?lat=%1&lng=%2&zoomLevel=%3")
            .arg(latitude, 0, 'f', 6)
            .arg(longitude, 0, 'f', 6)
            .arg(zoomLevel);
    }

    QString cacheFilePath(double latitude, double longitude) const {
        return QString("%1/%2_%3.jpg").arg(cacheDir)
            .arg(latitude, 0, 'f', 6).arg(longitude, 0, 'f', 6);
    }

    bool isImageCached(double latitude, double longitude) const {
        QFile file(cacheFilePath(latitude, longitude));
        return file.exists();
    }

    QImage getCachedImage(double latitude, double longitude) const {
        QImage img;
        img.load(cacheFilePath(latitude, longitude));
        return img;
    }

    void compressAndStoreImage(const QImage& image, double latitude, double longitude) {
        QMutexLocker locker(&cacheMutex);
        image.save(cacheFilePath(latitude, longitude), "JPG", 75);
    }

    void fetchImage(double latitude, double longitude) {
        QUrl url(buildApiUrl(latitude, longitude));
        QNetworkRequest request(url);
        auto reply = netManager->get(request);
        connect(reply, &QNetworkReply::finished, [=]() {
            if (reply->error() != QNetworkReply::NoError) {
                emit loadingFailed(reply->errorString());
                reply->deleteLater();
                return;
            }
            // Parse JSON response
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonObject obj = doc.object();
            QJsonArray photos = obj["photos"].toArray();
            if (photos.isEmpty()) {
                emit loadingFailed("No nearby OpenStreetCam image found.");
                reply->deleteLater();
                return;
            }
            QJsonObject photo = photos.at(0).toObject();
            QString photoUrl = photo["thumbnailUrl"].toString();
            if (photoUrl.isEmpty()) {
                emit loadingFailed("No valid OpenStreetCam image URL found.");
                reply->deleteLater();
                return;
            }
            // Download the image
            QNetworkRequest imgReq(QUrl(photoUrl));
            auto imgReply = netManager->get(imgReq);
            connect(imgReply, &QNetworkReply::finished, [=]() {
                if (imgReply->error() != QNetworkReply::NoError) {
                    emit loadingFailed(imgReply->errorString());
                    imgReply->deleteLater();
                    return;
                }
                QByteArray imgData = imgReply->readAll();
                QImage img;
                img.loadFromData(imgData);
                if (!img.isNull()) {
                    compressAndStoreImage(img, latitude, longitude);
                    qDebug() << "[StreetView] Downloaded and cached image for:" << latitude << longitude;
                    emit imageReady(img);
                } else {
                    emit loadingFailed("Failed to load OpenStreetCam image.");
                }
                imgReply->deleteLater();
            });
            reply->deleteLater();
        });
    }
};

#include "StreetViewLandscape.moc"
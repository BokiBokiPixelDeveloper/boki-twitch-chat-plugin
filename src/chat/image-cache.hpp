#pragma once

#include "chat/chat-types.hpp"
#include <QCache>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <deque>
#include <functional>

DecodedImage decodeChatImage(const QByteArray &bytes);
bool advanceAnimation(const DecodedImage &image, int &frame, double &elapsedMs, double deltaMs);

// Lives on the Qt thread. Owns only CPU images; callers upload them on the OBS graphics path.
class ImageCache : public QObject {
public:
    using Callback = std::function<void(ImageAsset)>;
    explicit ImageCache(QNetworkAccessManager *transport = nullptr);
    ~ImageCache() override;
    void request(const QUrl &url, Callback callback, std::optional<EmoteProvider> provider = {});
    void clear();

private:
    void pump();
    void finish(const QString &key, ImageAsset image);
    QNetworkAccessManager network_;
    QNetworkAccessManager *transport_;
    QCache<QString, ImageAsset> cache_{64 * 1024}; // KiB; active messages retain shared ownership
    QCache<QString, qint64> failures_{256};
    QHash<QString, std::vector<Callback>> waiting_;
    std::deque<QUrl> queued_;
    int active_ = 0;
    QSet<QNetworkReply *> replies_;
};

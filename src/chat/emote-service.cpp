#include "chat/emote-service.hpp"
#include <QNetworkReply>

EmoteService::EmoteService(MessageCallback callback, QNetworkAccessManager *transport)
    : callback_(std::move(callback)), images_(transport), transport_(transport ? transport : &network_)
{
    refreshTimer_.setInterval(5 * 60 * 1000);
    QObject::connect(&refreshTimer_, &QTimer::timeout, this, [this]() { refresh(); });
    flushTimer_.setInterval(50);
    QObject::connect(&flushTimer_, &QTimer::timeout, this, [this]() { flush(); });
}

EmoteService::~EmoteService()
{
    clear();
}

void EmoteService::clear()
{
    ++generation_;
    refreshTimer_.stop();
    flushTimer_.stop();
    pending_.clear();
    catalog_.clear();
    channelId_.clear();
    loading_ = 0;
    initialLoad_ = false;
    const auto replies = std::exchange(replies_, {});
    for (auto *reply : replies) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    images_.clear();
}

void EmoteService::setChannel(const QString &twitchId)
{
    if (channelId_ == twitchId)
        return;
    clear();
    channelId_ = twitchId;
    if (channelId_.isEmpty())
        return;
    initialLoad_ = true;
    refresh();
    refreshTimer_.start();
}

void EmoteService::refresh()
{
    if (loading_ || channelId_.isEmpty())
        return;
    loading_ = 6;
    const QString id = QString::fromLatin1(QUrl::toPercentEncoding(channelId_));
    fetchCatalog(EmoteProvider::SevenTV, false, QUrl(QStringLiteral("https://7tv.io/v3/emote-sets/global")), generation_);
    fetchCatalog(EmoteProvider::SevenTV, true, QUrl(QStringLiteral("https://7tv.io/v3/users/twitch/") + id), generation_);
    fetchCatalog(EmoteProvider::BetterTTV, false, QUrl(QStringLiteral("https://api.betterttv.net/3/cached/emotes/global")), generation_);
    fetchCatalog(EmoteProvider::BetterTTV, true, QUrl(QStringLiteral("https://api.betterttv.net/3/cached/users/twitch/") + id), generation_);
    fetchCatalog(EmoteProvider::FrankerFaceZ, false, QUrl(QStringLiteral("https://api.frankerfacez.com/v1/set/global")), generation_);
    fetchCatalog(EmoteProvider::FrankerFaceZ, true, QUrl(QStringLiteral("https://api.frankerfacez.com/v1/room/id/") + id), generation_);
}

void EmoteService::fetchCatalog(EmoteProvider provider, bool channel, const QUrl &url, quint64 generation)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(5000);
    auto *reply = transport_->get(request);
    reply->setParent(this); // Cancel deferred reply/timer work with this owner.
    replies_.insert(reply);
    auto bytes = std::make_shared<QByteArray>();
    QObject::connect(reply, &QNetworkReply::readyRead, this, [reply, bytes]() {
        constexpr qint64 maxBytes = 8 * 1024 * 1024;
        if (reply->bytesAvailable() > maxBytes - bytes->size())
            reply->abort();
        else
            bytes->append(reply->readAll());
    });
    QTimer::singleShot(6000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, provider, channel, generation]() {
        replies_.remove(reply);
        reply->deleteLater();
        if (generation != generation_)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            bytes->append(reply->readAll());
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(*bytes, &error);
            if (error.error == QJsonParseError::NoError)
                catalog_.replace(provider, channel, parseEmoteCatalog(provider, document));
        } else if (status == 404) {
            catalog_.replace(provider, channel, {}); // channel has no provider account/set
        }
        if (--loading_ == 0) {
            initialLoad_ = false;
            // start() can synchronously finish cached images; iterate a snapshot.
            const auto jobs = pending_;
            for (const auto &job : jobs)
                start(job);
            flush();
        }
    });
}

void EmoteService::resolve(ChatMessage message)
{
    resolve(std::move(message), callback_);
}

void EmoteService::resolve(ChatMessage message, MessageCallback completion)
{
    if (pending_.size() >= 64) {
        const auto oldest = pending_.front();
        pending_.pop_front();
        oldest->delivered = true;
        if (oldest->completion)
            oldest->completion(std::move(oldest->message));
    }
    auto pending = std::make_shared<Pending>();
    pending->message = std::move(message);
    pending->completion = std::move(completion);
    pending_.push_back(pending);
    flushTimer_.start();
    if (!initialLoad_)
        start(pending);
    flush();
}

void EmoteService::start(const std::shared_ptr<Pending> &pending)
{
    if (pending->started || pending->delivered)
        return;
    pending->started = true;
    pending->assembling = true;
    catalog_.apply(pending->message);
    const std::weak_ptr<Pending> weak = pending;
    for (size_t i = 0; i < pending->message.fragments.size(); ++i) {
        const auto &fragment = pending->message.fragments[i];
        if (fragment.imageUrl.isEmpty() || fragment.image)
            continue;
        ++pending->remaining;
        images_.request(fragment.imageUrl, [this, weak, i, fallback = fragment.fallbackUrl, url = fragment.imageUrl,
                                           provider = fragment.provider](ImageAsset image) {
            const auto job = weak.lock();
            if (!job || job->delivered)
                return;
            auto complete = [this, weak, i](ImageAsset decoded) {
                const auto job = weak.lock();
                if (!job || job->delivered)
                    return;
                job->message.fragments[i].image = std::move(decoded);
                --job->remaining;
                if (!job->assembling)
                    flush();
            };
            if (!image && !fallback.isEmpty() && fallback != url)
                images_.request(fallback, std::move(complete), provider);
            else
                complete(std::move(image));
        }, fragment.provider);
    }
    for (size_t i = 0; i < pending->message.media.size(); ++i) {
        if (pending->message.media[i].imageUrl.isEmpty())
            continue;
        ++pending->remaining;
        images_.request(pending->message.media[i].imageUrl, [this, weak, i](ImageAsset image) {
            const auto job = weak.lock();
            if (!job || job->delivered)
                return;
            job->message.media[i].image = std::move(image);
            --job->remaining;
            if (!job->assembling)
                flush();
        });
    }
    pending->assembling = false;
}

void EmoteService::flush()
{
    while (!pending_.empty()) {
        const auto job = pending_.front();
        if (job->assembling || (!job->deadline.hasExpired() && (!job->started || job->remaining)))
            break;
        pending_.pop_front();
        job->delivered = true;
        if (job->completion)
            job->completion(std::move(job->message));
    }
    if (pending_.empty())
        flushTimer_.stop();
}

void EmoteService::loadImage(const QUrl &url, ImageCache::Callback callback)
{
    images_.request(url, std::move(callback));
}

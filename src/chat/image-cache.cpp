#include "chat/image-cache.hpp"
#include "core/event-validation.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QNetworkReply>
#include <QTimer>
#include <webp/demux.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 maxDownloadBytes = 8 * 1024 * 1024;
constexpr qint64 maxDecodedBytes = 16 * 1024 * 1024;
constexpr int maxFrames = 512;
constexpr int maxSourceDimension = 2048;
constexpr int maxFrameDimension = 256;

bool validSize(const QSize &size)
{
    return size.width() > 0 && size.height() > 0 &&
        size.width() <= maxSourceDimension && size.height() <= maxSourceDimension;
}

QSize frameSize(const QSize &source, int frameCount)
{
    QSize target = source;
    if (target.width() > maxFrameDimension || target.height() > maxFrameDimension)
        target.scale(maxFrameDimension, maxFrameDimension, Qt::KeepAspectRatio);
    const int count = frameCount > 0 ? std::min(frameCount, maxFrames) : maxFrames;
    const double expectedBytes = static_cast<double>(target.width()) * target.height() * 4 * count;
    if (expectedBytes > maxDecodedBytes) {
        const double scale = std::sqrt(maxDecodedBytes / expectedBytes);
        target = QSize(std::max(1, static_cast<int>(target.width() * scale)),
                       std::max(1, static_cast<int>(target.height() * scale)));
    }
    return target;
}

bool appendFrame(DecodedImage &out, QImage frame, const QSize &target, int delay, qint64 &bytes)
{
    if (frame.isNull() || !validSize(frame.size()))
        return false;
    if (frame.size() != target)
        frame = frame.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    frame = frame.convertToFormat(QImage::Format_RGBA8888);
    if ((!out.frames.empty() && frame.size() != out.frames.front().size()) ||
        bytes + frame.sizeInBytes() > maxDecodedBytes)
        return false;
    bytes += frame.sizeInBytes();
    out.frames.push_back(std::move(frame));
    out.delaysMs.push_back(delay <= 0 ? 100 : std::max(20, delay));
    return true;
}
} // namespace

DecodedImage decodeChatImage(const QByteArray &bytes)
{
    DecodedImage out;
    if (bytes.isEmpty() || bytes.size() > maxDownloadBytes)
        return out;
    qint64 decodedBytes = 0;
    if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP") {
        // libwebp guarantees animated WebP support even without Qt's optional imageformats plugin.
        WebPData data{reinterpret_cast<const uint8_t *>(bytes.constData()), static_cast<size_t>(bytes.size())};
        std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data), WebPDemuxDelete);
        if (!demux || !validSize(QSize(WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH),
                                      WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT))))
            return {};
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options))
            return {};
        options.color_mode = MODE_RGBA;
        std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
            WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
        WebPAnimInfo info{};
        if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info))
            return {};
        const QSize target = frameSize(QSize(info.canvas_width, info.canvas_height), static_cast<int>(info.frame_count));
        int previousTimestamp = 0;
        while (WebPAnimDecoderHasMoreFrames(decoder.get()) && out.frames.size() < maxFrames) {
            uint8_t *pixels = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp))
                return {}; // allow the caller to try the static fallback
            QImage frame(pixels, static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height), QImage::Format_RGBA8888);
            if (!appendFrame(out, frame.copy(), target, timestamp - previousTimestamp, decodedBytes))
                break;
            previousTimestamp = timestamp;
        }
    } else {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const auto format = reader.format().toLower();
        if ((format != "png" && format != "gif" && format != "jpeg") || !validSize(reader.size()))
            return {};
        const QSize target = frameSize(reader.size(), reader.supportsAnimation() ? reader.imageCount() : 1);
        reader.setAutoTransform(true);
        while (reader.canRead() && out.frames.size() < maxFrames) {
            const QImage frame = reader.read();
            if (frame.isNull())
                return {};
            if (!appendFrame(out, frame, target, reader.nextImageDelay(), decodedBytes))
                break;
            if (!reader.supportsAnimation())
                break;
        }
    }
    return out;
}

bool advanceAnimation(const DecodedImage &image, int &frame, double &elapsedMs, double deltaMs)
{
    if (image.frames.size() < 2 || !std::isfinite(deltaMs) || deltaMs <= 0)
        return false;
    const int count = static_cast<int>(image.frames.size());
    frame = std::clamp(frame, 0, count - 1);
    auto delayAt = [&](int index) {
        return index < static_cast<int>(image.delaysMs.size()) ? std::max(20, image.delaysMs[static_cast<size_t>(index)]) : 100;
    };
    double duration = 0;
    for (int i = 0; i < count; ++i)
        duration += delayAt(i);
    elapsedMs = std::fmod(std::max(0.0, elapsedMs) + deltaMs, duration);
    const int previousFrame = frame;
    while (elapsedMs >= delayAt(frame)) {
        elapsedMs -= delayAt(frame);
        frame = (frame + 1) % count;
    }
    return frame != previousFrame;
}

ImageCache::ImageCache(QNetworkAccessManager *transport) : transport_(transport ? transport : &network_) {}

ImageCache::~ImageCache()
{
    clear();
}

void ImageCache::clear()
{
    const auto replies = std::exchange(replies_, {});
    for (auto *reply : replies) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    waiting_.clear();
    queued_.clear();
    cache_.clear();
    failures_.clear();
    active_ = 0;
}

void ImageCache::request(const QUrl &url, Callback callback, std::optional<EmoteProvider> provider)
{
    if (!isAllowedAssetUrl(url, provider)) {
        callback({});
        return;
    }
    const QString key = url.toString();
    if (auto *cached = cache_.object(key)) {
        callback(*cached);
        return;
    }
    const auto *failedUntil = failures_.object(key);
    if (failedUntil && *failedUntil > QDateTime::currentMSecsSinceEpoch()) {
        callback({});
        return;
    }
    auto pending = waiting_.find(key);
    if (pending != waiting_.end()) {
        if (pending->size() < 512)
            pending->push_back(std::move(callback));
        else
            callback({});
        return;
    }
    if (waiting_.size() >= 128) {
        callback({});
        return;
    }
    waiting_.insert(key, {std::move(callback)});
    queued_.push_back(url);
    pump();
}

void ImageCache::pump()
{
    while (active_ < 6 && !queued_.empty()) {
        const QUrl url = queued_.front();
        queued_.pop_front();
        ++active_;
        QNetworkRequest request(url);
        request.setTransferTimeout(5000);
        // A redirect could cross the provider allowlist after the original URL
        // was validated. Provider CDN URLs are fetched without redirects.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = transport_->get(request); // never attach Twitch credentials to image requests
        reply->setParent(this); // Cancel deferred reply/timer work with this owner.
        replies_.insert(reply);
        auto bytes = std::make_shared<QByteArray>();
        QObject::connect(reply, &QNetworkReply::readyRead, this, [reply, bytes]() {
            if (reply->bytesAvailable() > maxDownloadBytes - bytes->size())
                reply->abort();
            else
                bytes->append(reply->readAll());
        });
        QTimer::singleShot(6000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, key = url.toString()]() {
            replies_.remove(reply);
            ImageAsset asset;
            if (reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                bytes->append(reply->readAll());
                auto decoded = decodeChatImage(*bytes);
                if (!decoded.frames.empty())
                    asset = std::make_shared<const DecodedImage>(std::move(decoded));
            }
            reply->deleteLater();
            --active_;
            finish(key, std::move(asset));
            pump();
        });
    }
}

void ImageCache::finish(const QString &key, ImageAsset image)
{
    if (image) {
        qint64 cost = 0;
        for (const auto &frame : image->frames)
            cost += frame.sizeInBytes();
        cache_.insert(key, new ImageAsset(image), static_cast<int>((cost + 1023) / 1024));
    } else {
        failures_.insert(key, new qint64(QDateTime::currentMSecsSinceEpoch() + 30000));
    }
    const auto callbacks = waiting_.take(key);
    for (const auto &callback : callbacks)
        callback(image);
}

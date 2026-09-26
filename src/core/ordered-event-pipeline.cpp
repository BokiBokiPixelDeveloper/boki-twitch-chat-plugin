#include "core/ordered-event-pipeline.hpp"
#include "twitch/event-normalizer.hpp"
#include <QPointer>
#include <QThread>

namespace {
ChatMessage *chatContent(EventPayload &payload)
{
    return std::visit([](auto &value) -> ChatMessage * {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ChatMessage>) return &value;
        else if constexpr (requires { value.notice; }) return &value.notice;
        else return nullptr;
    }, payload);
}

void discardDecodedAssets(PluginEvent &event)
{
    if (auto *chat = chatContent(event.payload)) {
        for (auto &fragment : chat->fragments) fragment.image.reset();
        for (auto &media : chat->media) media.image.reset();
    }
}
} // namespace

OrderedEventPipeline::OrderedEventPipeline(EventDispatcher &dispatcher, LogCallback log, QNetworkAccessManager *assets)
    : dispatcher_(dispatcher), log_(std::move(log)), emotes_({}, assets)
{
    clock_.start();
    flushTimer_.setInterval(50);
    QObject::connect(&flushTimer_, &QTimer::timeout, this, [this] { flush(); });
}

OrderedEventPipeline::~OrderedEventPipeline() { stop(); }

void OrderedEventPipeline::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    channelId_.clear();
    flushTimer_.stop();
    pending_.clear();
    seen_.clear();
    seenOrder_.clear();
    emotes_.clear();
}

void OrderedEventPipeline::setChannel(QString channelId, std::uint64_t generation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (channelId_ == channelId && generation_ == generation)
        return;
    stop();
    generation_ = generation;
    channelId_ = std::move(channelId);
    emotes_.setChannel(channelId_);
}

IngestResult OrderedEventPipeline::ingest(const QByteArray &envelope)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (channelId_.isEmpty()) return IngestResult::Stopped;
    auto normalized = normalizeTwitchEvent(envelope, QDateTime::currentDateTimeUtc(), nextSequence_, generation_);
    if (normalized.status == NormalizationStatus::Ignored) return IngestResult::Ignored;
    if (!normalized.event) return IngestResult::Invalid;
    if (normalized.event->header.channelId != channelId_) return IngestResult::WrongChannel;
    const auto now = clock_.elapsed();
    const auto id = normalized.event->header.eventId;
    if (seen_.contains(id) && now - seen_.value(id) < 10 * 60 * 1000) return IngestResult::Duplicate;
    while (!seenOrder_.empty() && (now - seenOrder_.front().second >= 10 * 60 * 1000 || seenOrder_.size() >= 16384)) {
        seen_.remove(seenOrder_.front().first);
        seenOrder_.pop_front();
    }
    seen_.insert(id, now);
    seenOrder_.emplace_back(id, now);
    ++nextSequence_;
    // Insert the ticket before asset resolution: a cached image can finish inline.
    auto job = std::make_shared<Pending>();
    job->event = std::move(*normalized.event);
    pending_.push_back(job);
    if (auto *chat = chatContent(job->event.payload)) {
        const std::weak_ptr<Pending> weak = job;
        const QPointer<OrderedEventPipeline> guard(this);
        emotes_.resolve(*chat, [guard, weak](ChatMessage message) {
            const auto ticket = weak.lock();
            if (!guard || !ticket || ticket->ready) return;
            *chatContent(ticket->event.payload) = std::move(message);
            ticket->ready = true;
            guard->enforceBudget();
            guard->flush();
        });
    } else {
        job->ready = true;
    }
    enforceBudget();
    flush();
    if (!pending_.empty()) flushTimer_.start();
    return IngestResult::Accepted;
}

void OrderedEventPipeline::enforceBudget()
{
    size_t bytes = 0;
    for (const auto &job : pending_) {
        auto cost = eventRetainedBytes(job->event);
        if (cost > 32 * 1024 * 1024 || bytes + cost > 64 * 1024 * 1024) {
            discardDecodedAssets(job->event);
            cost = eventRetainedBytes(job->event);
        }
        bytes += cost;
    }
    while (pending_.size() > 128 || bytes > 64 * 1024 * 1024) {
        pending_.front()->ready = true; // Pressure uses existing text/URL fallback.
        flush();
        bytes = 0;
        for (const auto &job : pending_) bytes += eventRetainedBytes(job->event);
    }
}

void OrderedEventPipeline::flush()
{
    while (!pending_.empty()) {
        const auto job = pending_.front();
        if (!job->ready && !job->deadline.hasExpired()) break;
        job->ready = true;
        pending_.pop_front();
        const auto result = dispatcher_.publish(std::make_shared<const PluginEvent>(std::move(job->event)));
        if (result != PublishResult::Published && log_)
            log_(QStringLiteral("Event dispatcher rejected a producer publication"));
    }
    if (pending_.empty()) flushTimer_.stop();
}

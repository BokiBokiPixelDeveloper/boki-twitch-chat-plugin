#include "renderer/native-event-adapter.hpp"
#include <QMutexLocker>
#include <algorithm>
#include <cmath>

void NativeEventAdapter::configure(NativeLayoutSettings settings)
{
    QMutexLocker lock(&mutex);
    settings_ = std::move(settings);
}
void NativeEventAdapter::close()
{
    QMutexLocker lock(&mutex);
    active_ = false;
    ++queueRevision;
    messages.clear(); gifs.clear(); controls.clear(); recentControls_.clear();
}
bool NativeEventAdapter::removes(const PluginEvent &control, const MessageIdentity &identity)
{
    if (identity.header.channelId.isEmpty() || identity.header.channelId != control.header.channelId) return false;
    if (const auto *deleted = std::get_if<MessageDeleted>(&control.payload))
        return !identity.messageId.isEmpty() && deleted->messageId == identity.messageId;
    if (const auto *clear = std::get_if<ChatCleared>(&control.payload)) {
        if (clear->user && clear->user->id != identity.userId) return false;
        // A clear can arrive after a newer message. Server time takes priority
        // over arrival order, including messages retained across reauthorization.
        if (identity.header.timestamp.isValid() && control.header.timestamp.isValid())
            return identity.header.timestamp <= control.header.timestamp;
        return identity.header.generation == control.header.generation &&
            identity.header.sequence <= control.header.sequence;
    }
    return false;
}
bool NativeEventAdapter::removed(const MessageIdentity &identity) const
{
    return std::any_of(recentControls_.begin(), recentControls_.end(), [&](const auto &event) { return removes(*event, identity); });
}
void NativeEventAdapter::accept(BackendAttachment::Delivery delivery)
{
    if (delivery.reset || delivery.batch.state != ConsumerState::Active) {
        QMutexLocker lock(&mutex);
        if (!active_) return;
        ++queueRevision;
        reset = true; messages.clear(); gifs.clear();
    }
    for (const auto &event : delivery.batch.events) {
        if (std::holds_alternative<MessageDeleted>(event->payload) || std::holds_alternative<ChatCleared>(event->payload)) {
            QMutexLocker lock(&mutex);
            if (!active_) return;
            if (controls.size() >= 256) {
                ++queueRevision;
                controls.clear(); messages.clear(); gifs.clear(); reset = true;
            }
            controls.push_back(event);
            recentControls_.push_back(event);
            if (recentControls_.size() > 256) recentControls_.pop_front();
            std::erase_if(messages, [&](const auto &message) { return removes(*event, message.identity); });
            std::erase_if(gifs, [&](const auto &gif) { return removes(*event, gif.identity); });
        } else if (const auto *message = std::get_if<ChatMessage>(&event->payload)) {
            enqueue(*message, event->header);
        }
        // Other event types remain available in the dispatcher. Floating currently
        // has no alert presentation, so this adapter does not invent alert text.
    }
}
void NativeEventAdapter::enqueue(ChatMessage message, EventHeader header)
{
    NativeLayoutSettings settings;
    float pressure;
    std::uint64_t revision;
    MessageIdentity identity{std::move(header), message.messageId, message.user.id};
    {
        QMutexLocker lock(&mutex);
        if (!active_ || removed(identity)) return;
        settings = settings_;
        revision = queueRevision;
        pressure = std::clamp((static_cast<float>(activeCount.load()) + messages.size()) / 18.0f, 0.0f, 1.0f);
    }
    const float lengthPressure = std::clamp((message.text.size() - 70.0f) / 180.0f, 0.0f, 1.0f);
    const float density = std::max(pressure, lengthPressure * 0.85f);
    const int fontPx = static_cast<int>(std::round(settings.maxFont - density * (settings.maxFont - settings.minFont)));
    PreparedMessage prepared;
    prepared.identity = identity;
    prepared.speed = settings.minSpeed + std::max(pressure, lengthPressure) * (settings.maxSpeed - settings.minSpeed);
    prepared.layout = layoutMessage(message, settings.fontFamily, settings.fontWeight, fontPx, settings.outlineWidth);
    QMutexLocker lock(&mutex);
    if (!active_ || revision != queueRevision || removed(identity)) return;
    if (messages.size() >= 128) messages.pop_front();
    messages.push_back(std::move(prepared));
    for (const auto &media : message.media) if (media.image && !media.image->frames.empty()) {
        if (gifs.size() >= 30) gifs.pop_front();
        gifs.push_back({*media.image, identity});
    }
}
void NativeEventAdapter::enqueueGif(DecodedGif image, MessageIdentity identity)
{
    QMutexLocker lock(&mutex);
    if (!active_ || removed(identity)) return;
    if (gifs.size() >= 30) gifs.pop_front();
    gifs.push_back({std::move(image), std::move(identity)});
}

void NativeEventAdapter::returnPending(std::deque<PreparedMessage> pendingMessages,
                                      std::deque<PreparedGif> pendingGifs, std::uint64_t revision)
{
    QMutexLocker lock(&mutex);
    if (!active_ || revision != queueRevision) return;
    // Prefer older waiting objects while retaining the original queue limits.
    // Recheck moderation that may have arrived while OBS processed the batch.
    auto restore = [this](auto &destination, auto &pending, size_t limit) {
        while (!pending.empty()) {
            if (!removed(pending.back().identity)) {
                if (destination.size() >= limit) destination.pop_back();
                destination.push_front(std::move(pending.back()));
            }
            pending.pop_back();
        }
    };
    restore(messages, pendingMessages, 128);
    restore(gifs, pendingGifs, 30);
}

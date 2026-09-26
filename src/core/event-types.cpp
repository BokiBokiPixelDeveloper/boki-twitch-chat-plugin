#include "core/event-types.hpp"

#include <limits>
#include <type_traits>

namespace {
template<class> constexpr bool unsupportedPayload = false;

class ByteCounter {
public:
    size_t total = sizeof(PluginEvent);

    void bytes(size_t value)
    {
        total = value > std::numeric_limits<size_t>::max() - total
            ? std::numeric_limits<size_t>::max() : total + value;
    }
    void add(const QString &value) { bytes(static_cast<size_t>(value.capacity()) * sizeof(QChar)); }
    void add(const QUrl &value) { add(value.toString()); }
    void add(const ChatUser &user) { add(user.id); add(user.login); add(user.displayName); }
    template<class T> void add(const std::optional<T> &value) { if (value) add(*value); }
    template<class T> void add(const std::vector<T> &values)
    {
        bytes(values.capacity() * sizeof(T));
        for (const auto &value : values)
            add(value);
    }
    void add(const ImageAsset &image)
    {
        if (!image)
            return;
        bytes(sizeof(DecodedImage));
        bytes(image->frames.capacity() * sizeof(QImage));
        bytes(image->delaysMs.capacity() * sizeof(int));
        for (const auto &frame : image->frames)
            bytes(static_cast<size_t>(frame.sizeInBytes()));
    }
    void add(const ChatFragment &fragment)
    {
        add(fragment.text); add(fragment.emoteId);
        add(fragment.imageUrl); add(fragment.fallbackUrl); add(fragment.image);
        add(fragment.mention);
        if (fragment.twitch) { add(fragment.twitch->setId); add(fragment.twitch->ownerId); }
        if (fragment.cheermote) add(fragment.cheermote->prefix);
    }
    void add(const ChatBadge &badge)
    {
        add(badge.type); add(badge.version); add(badge.info); add(badge.imageUrl);
    }
    void add(const ChatMedia &media) { add(media.imageUrl); add(media.image); }
    void add(const ChatMessage &message)
    {
        add(message.messageId); add(message.user); add(message.text);
        add(message.fragments); add(message.badges); add(message.media);
        add(message.metadata.messageType); add(message.metadata.systemText); add(message.metadata.rewardId);
        if (message.metadata.reply) {
            add(message.metadata.reply->parentMessageId);
            add(message.metadata.reply->threadMessageId);
        }
    }
    void add(const MessageDeleted &event) { add(event.messageId); add(event.user); }
    void add(const ChatCleared &event) { add(event.user); }
    void add(const Follow &event) { add(event.user); }
    void add(const Subscription &event) { add(event.notice); }
    void add(const GiftActor &gifter) { add(gifter.user); }
    void add(const Resubscription &event) { add(event.notice); add(event.gifter); }
    void add(const GiftSubscription &event)
    {
        add(event.notice); add(event.gifter); add(event.recipient); add(event.communityGiftId);
    }
    void add(const CommunityGiftSubscription &event)
    {
        add(event.notice); add(event.gifter); add(event.communityGiftId);
    }
    void add(const Cheer &event) { add(event.user); add(event.text); }
    void add(const Raid &event) { add(event.from); add(event.to); }
};
} // namespace

EventKind eventKind(const EventPayload &payload)
{
    return std::visit([](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ChatMessage>) return EventKind::ChatMessage;
        else if constexpr (std::is_same_v<T, MessageDeleted>) return EventKind::MessageDeleted;
        else if constexpr (std::is_same_v<T, ChatCleared>) return EventKind::ChatCleared;
        else if constexpr (std::is_same_v<T, Follow>) return EventKind::Follow;
        else if constexpr (std::is_same_v<T, Subscription>) return EventKind::Subscription;
        else if constexpr (std::is_same_v<T, Resubscription>) return EventKind::Resubscription;
        else if constexpr (std::is_same_v<T, GiftSubscription>) return EventKind::GiftSubscription;
        else if constexpr (std::is_same_v<T, CommunityGiftSubscription>) return EventKind::CommunityGiftSubscription;
        else if constexpr (std::is_same_v<T, Cheer>) return EventKind::Cheer;
        else if constexpr (std::is_same_v<T, Raid>) return EventKind::Raid;
        else static_assert(unsupportedPayload<T>, "Every event payload needs a kind");
    }, payload);
}

size_t eventRetainedBytes(const PluginEvent &event)
{
    ByteCounter counter;
    counter.add(event.header.eventId);
    counter.add(event.header.channelId);
    counter.add(event.header.originChannelId);
    counter.add(event.header.originMessageId);
    std::visit([&](const auto &payload) { counter.add(payload); }, event.payload);
    return counter.total;
}

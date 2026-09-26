#pragma once

#include "chat/chat-types.hpp"

#include <QDateTime>
#include <cstddef>
#include <variant>

struct EventHeader {
    QString eventId; // Transport notification ID, distinct from a chat message ID.
    QDateTime timestamp; // UTC, millisecond precision.
    QDateTime receivedAt;
    QString channelId;
    QString originChannelId;
    QString originMessageId;
    std::uint64_t sequence = 0; // Assigned by the producer before enrichment.
    std::uint64_t generation = 0;
};

struct MessageDeleted { QString messageId; ChatUser user; };
struct ChatCleared { std::optional<ChatUser> user; }; // Absent: entire channel.
struct Follow { ChatUser user; QDateTime followedAt; };

enum class SubscriptionTier { Unknown, Tier1, Tier2, Tier3 };
struct SubscriptionTerms {
    SubscriptionTier tier = SubscriptionTier::Unknown;
    std::optional<bool> isPrime;
    int durationMonths = 0;
};
struct GiftActor {
    bool anonymous = false;
    std::optional<ChatUser> user;
};
struct Subscription { ChatMessage notice; SubscriptionTerms terms; };
struct Resubscription {
    ChatMessage notice;
    SubscriptionTerms terms;
    int cumulativeMonths = 0;
    std::optional<int> streakMonths;
    bool isGift = false;
    std::optional<GiftActor> gifter;
};
struct GiftSubscription {
    ChatMessage notice;
    GiftActor gifter;
    ChatUser recipient;
    SubscriptionTerms terms;
    QString communityGiftId;
    std::optional<int> cumulativeTotal;
};
struct CommunityGiftSubscription {
    ChatMessage notice;
    GiftActor gifter;
    SubscriptionTier tier = SubscriptionTier::Unknown;
    QString communityGiftId;
    int count = 0;
    std::optional<int> cumulativeTotal;
};
struct Cheer { std::optional<ChatUser> user; int bits = 0; QString text; };
struct Raid { ChatUser from; ChatUser to; int viewers = 0; };

using EventPayload = std::variant<ChatMessage, MessageDeleted, ChatCleared, Follow,
    Subscription, Resubscription, GiftSubscription, CommunityGiftSubscription, Cheer, Raid>;
struct PluginEvent { EventHeader header; EventPayload payload; };
using EventPtr = std::shared_ptr<const PluginEvent>;

enum class EventKind {
    ChatMessage, MessageDeleted, ChatCleared, Follow, Subscription,
    Resubscription, GiftSubscription, CommunityGiftSubscription, Cheer, Raid
};

[[nodiscard]] EventKind eventKind(const EventPayload &payload);

// Queue accounting includes retained string/vector capacity and decoded pixels.
// Shared assets are charged per reference. This is a budget, not a heap profiler.
[[nodiscard]] size_t eventRetainedBytes(const PluginEvent &event);

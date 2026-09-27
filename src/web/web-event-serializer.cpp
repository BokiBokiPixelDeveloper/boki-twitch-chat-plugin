#include "web/web-event-serializer.hpp"
#include "core/event-validation.hpp"

#include <QJsonArray>

namespace {
QJsonValue optionalString(const QString &value) { return value.isEmpty() ? QJsonValue::Null : QJsonValue(value); }
QJsonValue url(const QUrl &value, std::optional<EmoteProvider> provider = {})
{
    return isAllowedAssetUrl(value, provider) ? QJsonValue(value.toString(QUrl::FullyEncoded)) : QJsonValue::Null;
}
QString timestamp(const QDateTime &value) { return value.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")); }

QJsonObject user(const ChatUser &value)
{
    return {{"id", optionalString(value.id)}, {"login", value.login}, {"displayName", value.displayName},
            {"color", value.color.isValid() ? QJsonValue(value.color.name(QColor::HexRgb)) : QJsonValue::Null}};
}
QString provider(EmoteProvider value)
{
    switch (value) {
    case EmoteProvider::Twitch: return QStringLiteral("twitch");
    case EmoteProvider::FrankerFaceZ: return QStringLiteral("frankerfacez");
    case EmoteProvider::BetterTTV: return QStringLiteral("betterttv");
    case EmoteProvider::SevenTV: return QStringLiteral("seventv");
    }
    return {};
}
QString tier(SubscriptionTier value)
{
    switch (value) {
    case SubscriptionTier::Tier1: return QStringLiteral("tier1");
    case SubscriptionTier::Tier2: return QStringLiteral("tier2");
    case SubscriptionTier::Tier3: return QStringLiteral("tier3");
    default: return QStringLiteral("unknown");
    }
}
QJsonObject terms(const SubscriptionTerms &value)
{
    return {{"tier", tier(value.tier)}, {"isPrime", value.isPrime ? QJsonValue(*value.isPrime) : QJsonValue::Null},
            {"durationMonths", value.durationMonths}};
}
QJsonObject giftActor(const GiftActor &value)
{
    return {{"anonymous", value.anonymous}, {"user", value.user ? QJsonValue(user(*value.user)) : QJsonValue::Null}};
}
QJsonObject message(const ChatMessage &value)
{
    QJsonArray fragments;
    for (const auto &fragment : value.fragments) {
        QJsonObject item{{"type", fragment.type == ChatFragment::Type::Emote ? "emote" : "text"}, {"text", fragment.text},
                         {"emoteId", optionalString(fragment.emoteId)},
                         {"provider", fragment.provider ? QJsonValue(provider(*fragment.provider)) : QJsonValue::Null},
                         {"imageUrl", url(fragment.imageUrl, fragment.provider)}, {"fallbackUrl", url(fragment.fallbackUrl, fragment.provider)},
                         {"zeroWidth", fragment.zeroWidth}};
        item["sourceRange"] = fragment.sourceRange ? QJsonValue(QJsonObject{{"offset", fragment.sourceRange->offset},
                                                                             {"length", fragment.sourceRange->length}}) : QJsonValue::Null;
        item["twitch"] = fragment.twitch ? QJsonValue(QJsonObject{{"setId", optionalString(fragment.twitch->setId)},
                                                                    {"ownerId", optionalString(fragment.twitch->ownerId)},
                                                                    {"supportsStatic", fragment.twitch->supportsStatic},
                                                                    {"supportsAnimated", fragment.twitch->supportsAnimated}}) : QJsonValue::Null;
        item["mention"] = fragment.mention ? QJsonValue(user(*fragment.mention)) : QJsonValue::Null;
        item["cheermote"] = fragment.cheermote ? QJsonValue(QJsonObject{{"prefix", fragment.cheermote->prefix},
                                                                          {"bits", fragment.cheermote->bits},
                                                                          {"tier", fragment.cheermote->tier}}) : QJsonValue::Null;
        fragments.append(item);
    }
    QJsonArray badges;
    for (const auto &badge : value.badges)
        badges.append(QJsonObject{{"provider", "twitch"}, {"type", badge.type}, {"version", badge.version},
                                  {"info", badge.info}, {"imageUrl", url(badge.imageUrl, EmoteProvider::Twitch)}});
    QJsonArray media;
    for (const auto &asset : value.media) media.append(QJsonObject{{"imageUrl", url(asset.imageUrl)}});
    QJsonObject metadata{{"messageType", value.metadata.messageType}, {"systemText", value.metadata.systemText},
                         {"cheerBits", value.metadata.cheerBits ? QJsonValue(*value.metadata.cheerBits) : QJsonValue::Null},
                         {"rewardId", optionalString(value.metadata.rewardId)}};
    metadata["reply"] = value.metadata.reply ? QJsonValue(QJsonObject{{"parentMessageId", optionalString(value.metadata.reply->parentMessageId)},
                                                                        {"threadMessageId", optionalString(value.metadata.reply->threadMessageId)}}) : QJsonValue::Null;
    return {{"messageId", value.messageId}, {"user", user(value.user)}, {"text", value.text}, {"fragments", fragments},
            {"badges", badges}, {"media", media}, {"metadata", metadata}};
}
}

QJsonObject WebEventSerializer::serialize(const PluginEvent &event)
{
    const auto &h = event.header;
    QJsonObject header{{"eventId", h.eventId}, {"timestamp", timestamp(h.timestamp)}, {"receivedAt", timestamp(h.receivedAt)},
                       {"channelId", h.channelId}, {"originChannelId", optionalString(h.originChannelId)},
                       {"originMessageId", optionalString(h.originMessageId)}, {"sequence", QString::number(h.sequence)},
                       {"generation", QString::number(h.generation)},
                       {"origin", h.origin == EventOrigin::Production ? "production" :
                          h.origin == EventOrigin::LocalTransportTest ? "localTransportTest" : "syntheticTest"}};
    QJsonObject data;
    QString type;
    std::visit([&](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, ChatMessage>) { type = "ChatMessage"; data = message(payload); }
        else if constexpr (std::is_same_v<T, MessageDeleted>) { type = "MessageDeleted"; data = {{"messageId", payload.messageId}, {"user", user(payload.user)}}; }
        else if constexpr (std::is_same_v<T, ChatCleared>) { type = "ChatCleared"; data = {{"user", payload.user ? QJsonValue(user(*payload.user)) : QJsonValue::Null}}; }
        else if constexpr (std::is_same_v<T, Follow>) { type = "Follow"; data = {{"user", user(payload.user)}, {"followedAt", timestamp(payload.followedAt)}}; }
        else if constexpr (std::is_same_v<T, Subscription>) { type = "Subscription"; data = {{"notice", message(payload.notice)}, {"terms", terms(payload.terms)}}; }
        else if constexpr (std::is_same_v<T, Resubscription>) { type = "Resubscription"; data = {{"notice", message(payload.notice)}, {"terms", terms(payload.terms)}, {"cumulativeMonths", payload.cumulativeMonths}, {"streakMonths", payload.streakMonths ? QJsonValue(*payload.streakMonths) : QJsonValue::Null}, {"isGift", payload.isGift}, {"gifter", payload.gifter ? QJsonValue(giftActor(*payload.gifter)) : QJsonValue::Null}}; }
        else if constexpr (std::is_same_v<T, GiftSubscription>) { type = "GiftSubscription"; data = {{"notice", message(payload.notice)}, {"gifter", giftActor(payload.gifter)}, {"recipient", user(payload.recipient)}, {"terms", terms(payload.terms)}, {"communityGiftId", optionalString(payload.communityGiftId)}, {"cumulativeTotal", payload.cumulativeTotal ? QJsonValue(*payload.cumulativeTotal) : QJsonValue::Null}}; }
        else if constexpr (std::is_same_v<T, CommunityGiftSubscription>) { type = "CommunityGiftSubscription"; data = {{"notice", message(payload.notice)}, {"gifter", giftActor(payload.gifter)}, {"tier", tier(payload.tier)}, {"communityGiftId", optionalString(payload.communityGiftId)}, {"count", payload.count}, {"cumulativeTotal", payload.cumulativeTotal ? QJsonValue(*payload.cumulativeTotal) : QJsonValue::Null}}; }
        else if constexpr (std::is_same_v<T, Cheer>) { type = "Cheer"; data = {{"user", payload.user ? QJsonValue(user(*payload.user)) : QJsonValue::Null}, {"bits", payload.bits}, {"text", payload.text}}; }
        else if constexpr (std::is_same_v<T, Raid>) { type = "Raid"; data = {{"from", user(payload.from)}, {"to", user(payload.to)}, {"viewers", payload.viewers}}; }
    }, event.payload);
    return {{"schemaVersion", 1}, {"header", header}, {"type", type}, {"data", data}};
}

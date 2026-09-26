#include "twitch/event-normalizer.hpp"
#include "twitch/chat-message-parser.hpp"

#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace {
QDateTime timestamp(const QString &text)
{
    static const QRegularExpression zone(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})$"));
    if (!zone.match(text).hasMatch())
        return {};
    return QDateTime::fromString(text, Qt::ISODateWithMs).toUTC();
}

NormalizationResult invalid(const char *reason)
{
    return {NormalizationStatus::Invalid, {}, QString::fromLatin1(reason)};
}

struct Reader {
    bool valid = true;
    void require(bool condition) { valid = valid && condition; }
    int count(const QJsonObject &object, const char *key, int minimum = 0)
    {
        const auto value = object.value(QLatin1String(key));
        const double number = value.toDouble(-1);
        if (!value.isDouble() || !std::isfinite(number) || number < minimum ||
            number > std::numeric_limits<int>::max() || std::floor(number) != number) {
            valid = false;
            return 0;
        }
        return static_cast<int>(number);
    }
    std::optional<int> optionalCount(const QJsonObject &object, const char *key)
    {
        const auto value = object.value(QLatin1String(key));
        return value.isNull() || value.isUndefined() ? std::nullopt : std::optional<int>{count(object, key)};
    }
    bool boolean(const QJsonObject &object, const char *key)
    {
        const auto value = object.value(QLatin1String(key));
        require(value.isBool());
        return value.toBool();
    }
    std::optional<bool> optionalBool(const QJsonObject &object, const char *key)
    {
        const auto value = object.value(QLatin1String(key));
        return value.isNull() || value.isUndefined() ? std::nullopt : std::optional<bool>{boolean(object, key)};
    }
    ChatUser user(const QJsonObject &object, const QString &prefix)
    {
        auto result = parseTwitchUser(object, prefix);
        require(!result.id.isEmpty());
        return result;
    }
    SubscriptionTier tier(const QJsonObject &object)
    {
        const auto value = object.value(QStringLiteral("sub_tier"));
        require(value.isString() && !value.toString().isEmpty());
        const auto text = value.toString();
        if (text == QStringLiteral("1000")) return SubscriptionTier::Tier1;
        if (text == QStringLiteral("2000")) return SubscriptionTier::Tier2;
        if (text == QStringLiteral("3000")) return SubscriptionTier::Tier3;
        return SubscriptionTier::Unknown;
    }
    SubscriptionTerms terms(const QJsonObject &object)
    {
        return {tier(object), optionalBool(object, "is_prime"), count(object, "duration_months", 1)};
    }
    ChatMessage message(const QJsonObject &object, bool anonymous = false)
    {
        require(object.value(QStringLiteral("message")).isObject());
        require(object.value(QStringLiteral("message")).toObject().value(QStringLiteral("text")).isString());
        auto result = parseTwitchMessage(object);
        require(!result.messageId.isEmpty());
        require(anonymous || !result.user.id.isEmpty());
        return result;
    }
};
} // namespace

NormalizationResult normalizeTwitchEvent(const QByteArray &envelope, QDateTime receivedAt,
                                       std::uint64_t sequence, std::uint64_t generation)
{
    constexpr qsizetype maxEnvelopeBytes = 1024 * 1024;
    if (envelope.isEmpty() || envelope.size() > maxEnvelopeBytes)
        return invalid("EventSub envelope size is invalid");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(envelope, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return invalid("EventSub envelope is not a JSON object");
    const auto root = document.object();
    const auto metadata = root.value(QStringLiteral("metadata")).toObject();
    const auto messageType = metadata.value(QStringLiteral("message_type")).toString();
    if (messageType.isEmpty())
        return invalid("EventSub message type is missing");
    if (messageType != QStringLiteral("notification"))
        return {NormalizationStatus::Ignored, {}, {}};

    const auto type = metadata.value(QStringLiteral("subscription_type")).toString();
    const auto version = metadata.value(QStringLiteral("subscription_version")).toString();
    if (type.isEmpty() || version.isEmpty())
        return invalid("EventSub subscription metadata is missing");
    const QStringList supported{QStringLiteral("channel.chat.message"), QStringLiteral("channel.chat.message_delete"),
        QStringLiteral("channel.chat.clear"), QStringLiteral("channel.chat.clear_user_messages"),
        QStringLiteral("channel.follow"), QStringLiteral("channel.chat.notification"),
        QStringLiteral("channel.cheer"), QStringLiteral("channel.raid")};
    if (!supported.contains(type) || version != (type == QStringLiteral("channel.follow") ? QStringLiteral("2") : QStringLiteral("1")))
        return {NormalizationStatus::Ignored, {}, {}};

    const auto payload = root.value(QStringLiteral("payload")).toObject();
    const auto subscription = payload.value(QStringLiteral("subscription")).toObject();
    if (subscription.value(QStringLiteral("type")).toString() != type ||
        subscription.value(QStringLiteral("version")).toString() != version ||
        !payload.value(QStringLiteral("event")).isObject())
        return invalid("EventSub payload does not match its subscription metadata");
    const auto input = payload.value(QStringLiteral("event")).toObject();
    PluginEvent output;
    output.header.eventId = metadata.value(QStringLiteral("message_id")).toString();
    output.header.timestamp = timestamp(metadata.value(QStringLiteral("message_timestamp")).toString());
    output.header.receivedAt = receivedAt.toUTC();
    output.header.sequence = sequence;
    output.header.generation = generation;
    output.header.channelId = input.value(type == QStringLiteral("channel.raid")
        ? QStringLiteral("to_broadcaster_user_id") : QStringLiteral("broadcaster_user_id")).toString();
    output.header.originChannelId = input.value(QStringLiteral("source_broadcaster_user_id")).toString();
    if (output.header.originChannelId == output.header.channelId)
        output.header.originChannelId.clear();
    output.header.originMessageId = input.value(QStringLiteral("source_message_id")).toString();
    if (output.header.eventId.isEmpty() || !output.header.timestamp.isValid() ||
        !output.header.receivedAt.isValid() || output.header.channelId.isEmpty())
        return invalid("EventSub event identity or timestamp is invalid");

    Reader reader;
    if (type == QStringLiteral("channel.chat.message")) {
        output.payload = reader.message(input);
    } else if (type == QStringLiteral("channel.chat.message_delete")) {
        auto id = input.value(QStringLiteral("message_id")).toString();
        reader.require(!id.isEmpty());
        output.payload = MessageDeleted{std::move(id), reader.user(input, QStringLiteral("target_"))};
    } else if (type == QStringLiteral("channel.chat.clear")) {
        output.payload = ChatCleared{};
    } else if (type == QStringLiteral("channel.chat.clear_user_messages")) {
        output.payload = ChatCleared{reader.user(input, QStringLiteral("target_"))};
    } else if (type == QStringLiteral("channel.follow")) {
        const auto followedAt = timestamp(input.value(QStringLiteral("followed_at")).toString());
        reader.require(followedAt.isValid());
        output.payload = Follow{reader.user(input, {}), followedAt};
    } else if (type == QStringLiteral("channel.cheer")) {
        const bool anonymous = reader.boolean(input, "is_anonymous");
        Cheer cheer;
        if (!anonymous)
            cheer.user = reader.user(input, {});
        cheer.bits = reader.count(input, "bits", 1);
        reader.require(input.value(QStringLiteral("message")).isString());
        cheer.text = input.value(QStringLiteral("message")).toString();
        output.payload = std::move(cheer);
    } else if (type == QStringLiteral("channel.raid")) {
        output.payload = Raid{reader.user(input, QStringLiteral("from_broadcaster_")),
                              reader.user(input, QStringLiteral("to_broadcaster_")), reader.count(input, "viewers")};
    } else {
        const auto noticeType = input.value(QStringLiteral("notice_type")).toString();
        if (noticeType.isEmpty())
            return invalid("EventSub notice type is missing");
        QString kind = noticeType;
        if (kind.startsWith(QStringLiteral("shared_chat_"))) {
            kind.remove(0, QStringLiteral("shared_chat_").size());
            reader.require(!output.header.originChannelId.isEmpty());
        }
        if (kind != QStringLiteral("sub") && kind != QStringLiteral("resub") &&
            kind != QStringLiteral("sub_gift") && kind != QStringLiteral("community_sub_gift"))
            return {NormalizationStatus::Ignored, {}, {}};
        reader.require(input.value(noticeType).isObject());
        const auto detail = input.value(noticeType).toObject();
        const bool anonymous = reader.boolean(input, "chatter_is_anonymous");
        auto notice = reader.message(input, anonymous);
        GiftActor actor{anonymous, {}};
        if (!anonymous)
            actor.user = notice.user;
        if (kind == QStringLiteral("sub")) {
            reader.require(!anonymous);
            auto terms = reader.terms(detail);
            terms.isPrime = reader.boolean(detail, "is_prime");
            output.payload = Subscription{std::move(notice), terms};
        } else if (kind == QStringLiteral("resub")) {
            reader.require(!anonymous);
            Resubscription resub;
            resub.notice = std::move(notice);
            resub.terms = reader.terms(detail);
            resub.cumulativeMonths = reader.count(detail, "cumulative_months", 1);
            resub.streakMonths = reader.optionalCount(detail, "streak_months");
            resub.isGift = reader.boolean(detail, "is_gift");
            const auto gifterAnonymous = reader.optionalBool(detail, "gifter_is_anonymous");
            if (resub.isGift && (gifterAnonymous || !detail.value(QStringLiteral("gifter_user_id")).toString().isEmpty())) {
                GiftActor gifter{gifterAnonymous.value_or(false), {}};
                if (!gifter.anonymous)
                    gifter.user = reader.user(detail, QStringLiteral("gifter_"));
                resub.gifter = std::move(gifter);
            }
            output.payload = std::move(resub);
        } else if (kind == QStringLiteral("sub_gift")) {
            output.payload = GiftSubscription{std::move(notice), std::move(actor),
                reader.user(detail, QStringLiteral("recipient_")), reader.terms(detail),
                detail.value(QStringLiteral("community_gift_id")).toString(), reader.optionalCount(detail, "cumulative_total")};
        } else {
            const auto id = detail.value(QStringLiteral("id")).toString();
            reader.require(!id.isEmpty());
            output.payload = CommunityGiftSubscription{std::move(notice), std::move(actor), reader.tier(detail),
                id, reader.count(detail, "total", 1), reader.optionalCount(detail, "cumulative_total")};
        }
    }
    if (!reader.valid)
        return invalid("EventSub event has missing or invalid required fields");
    return {NormalizationStatus::Event, std::move(output), {}};
}

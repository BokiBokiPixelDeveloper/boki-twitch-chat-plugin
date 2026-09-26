#include "twitch/event-normalizer.hpp"
#include "core/event-dispatcher.hpp"
#include "core/event-validation.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace {
QDateTime receivedAt() { return QDateTime::fromString(QStringLiteral("2026-09-26T12:00:01.000Z"), Qt::ISODateWithMs); }

QJsonObject chat()
{
    return {{"broadcaster_user_id", "channel"}, {"message_id", "chat-id"},
        {"chatter_user_id", "author"}, {"chatter_user_login", "login"}, {"chatter_user_name", "Display"},
        {"chatter_is_anonymous", false}, {"color", "#123456"}, {"message", QJsonObject{{"text", "Hello"}}}};
}

QJsonObject notice(const QString &kind)
{
    auto input = chat();
    input.insert("notice_type", kind);
    input.insert("system_message", "A synthetic subscription notice");
    QJsonObject detail{{"sub_tier", "1000"}, {"duration_months", 1}, {"is_prime", true},
        {"is_gift", false}, {"cumulative_months", 12}, {"streak_months", QJsonValue::Null},
        {"recipient_user_id", "recipient"}, {"recipient_user_login", "recipient_login"},
        {"recipient_user_name", "Recipient"}, {"community_gift_id", "community"},
        {"id", "community"}, {"total", 5}, {"cumulative_total", 20}};
    input.insert(kind, detail);
    if (kind.startsWith(QStringLiteral("shared_chat_"))) {
        input.insert("source_broadcaster_user_id", "origin");
        input.insert("source_message_id", "origin-message");
    }
    return input;
}

QJsonObject envelope(const QString &type, QJsonObject input)
{
    const QString version = type == QStringLiteral("channel.follow") ? QStringLiteral("2") : QStringLiteral("1");
    return {{"metadata", QJsonObject{{"message_type", "notification"}, {"message_id", "transport-id"},
                {"message_timestamp", "2026-09-26T12:00:00.123456789Z"},
                {"subscription_type", type}, {"subscription_version", version}}},
        {"payload", QJsonObject{{"subscription", QJsonObject{{"type", type}, {"version", version}}},
                               {"event", input}}}};
}

NormalizationResult normalize(const QJsonObject &root)
{
    return normalizeTwitchEvent(QJsonDocument(root).toJson(QJsonDocument::Compact), receivedAt(), 42, 7);
}

NormalizationResult normalize(const QString &type, const QJsonObject &input)
{
    return normalize(envelope(type, input));
}
} // namespace

class EventNormalizerTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void supportedEvents_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QJsonObject>("input");
        QTest::addColumn<int>("kind");
        QTest::newRow("chat") << QStringLiteral("channel.chat.message") << chat() << int(EventKind::ChatMessage);
        QTest::newRow("delete") << QStringLiteral("channel.chat.message_delete")
            << QJsonObject{{"broadcaster_user_id", "channel"}, {"message_id", "deleted"}, {"target_user_id", "author"}}
            << int(EventKind::MessageDeleted);
        QTest::newRow("clear-channel") << QStringLiteral("channel.chat.clear")
            << QJsonObject{{"broadcaster_user_id", "channel"}} << int(EventKind::ChatCleared);
        QTest::newRow("clear-user") << QStringLiteral("channel.chat.clear_user_messages")
            << QJsonObject{{"broadcaster_user_id", "channel"}, {"target_user_id", "author"}} << int(EventKind::ChatCleared);
        QTest::newRow("follow") << QStringLiteral("channel.follow")
            << QJsonObject{{"broadcaster_user_id", "channel"}, {"user_id", "follower"}, {"followed_at", "2026-09-26T11:59:00Z"}}
            << int(EventKind::Follow);
        QTest::newRow("sub") << QStringLiteral("channel.chat.notification") << notice("sub") << int(EventKind::Subscription);
        QTest::newRow("resub") << QStringLiteral("channel.chat.notification") << notice("resub") << int(EventKind::Resubscription);
        QTest::newRow("gift") << QStringLiteral("channel.chat.notification") << notice("sub_gift") << int(EventKind::GiftSubscription);
        QTest::newRow("community-gift") << QStringLiteral("channel.chat.notification") << notice("community_sub_gift")
            << int(EventKind::CommunityGiftSubscription);
        QTest::newRow("cheer") << QStringLiteral("channel.cheer")
            << QJsonObject{{"broadcaster_user_id", "channel"}, {"is_anonymous", false}, {"user_id", "cheerer"}, {"bits", 100}, {"message", "Hi"}}
            << int(EventKind::Cheer);
        QTest::newRow("raid") << QStringLiteral("channel.raid")
            << QJsonObject{{"from_broadcaster_user_id", "raider"}, {"to_broadcaster_user_id", "channel"}, {"viewers", 12}}
            << int(EventKind::Raid);
        QTest::newRow("shared-sub") << QStringLiteral("channel.chat.notification") << notice("shared_chat_sub") << int(EventKind::Subscription);
        QTest::newRow("shared-resub") << QStringLiteral("channel.chat.notification") << notice("shared_chat_resub") << int(EventKind::Resubscription);
        QTest::newRow("shared-gift") << QStringLiteral("channel.chat.notification") << notice("shared_chat_sub_gift") << int(EventKind::GiftSubscription);
        QTest::newRow("shared-community") << QStringLiteral("channel.chat.notification") << notice("shared_chat_community_sub_gift")
            << int(EventKind::CommunityGiftSubscription);
    }

    void supportedEvents()
    {
        QFETCH(QString, type);
        QFETCH(QJsonObject, input);
        QFETCH(int, kind);
        auto result = normalize(type, input);
        QCOMPARE(result.status, NormalizationStatus::Event);
        QVERIFY(result.event);
        QCOMPARE(int(eventKind(result.event->payload)), kind);
        QCOMPARE(result.event->header.eventId, QStringLiteral("transport-id"));
        QCOMPARE(result.event->header.channelId, QStringLiteral("channel"));
        QCOMPARE(result.event->header.sequence, std::uint64_t(42));
        QCOMPARE(result.event->header.generation, std::uint64_t(7));
        QCOMPARE(result.event->header.timestamp.toString(Qt::ISODateWithMs), QStringLiteral("2026-09-26T12:00:00.123Z"));
        QCOMPARE(result.event->header.receivedAt, receivedAt());
        EventDispatcher dispatcher;
        auto a = dispatcher.subscribe();
        auto b = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(std::make_shared<const PluginEvent>(std::move(*result.event))), PublishResult::Published);
        QCOMPARE(a.takeBatch().events.front(), b.takeBatch().events.front());
    }

    void structuredChatFields()
    {
        auto input = chat();
        input.insert("badges", QJsonArray{QJsonObject{{"set_id", "subscriber"}, {"id", "12"}, {"info", "14"}}});
        input.insert("source_broadcaster_user_id", "origin");
        input.insert("source_message_id", "original");
        input.insert("message_type", "channel_points_highlighted");
        input.insert("channel_points_custom_reward_id", "reward");
        input.insert("reply", QJsonObject{{"parent_message_id", "parent"}, {"thread_message_id", "thread"}});
        input.insert("cheer", QJsonObject{{"bits", 10}});
        input.insert("message", QJsonObject{{"text", QStringLiteral("👀 Kappa @friend Cheer10")}, {"fragments", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", QStringLiteral("👀 ")}},
            QJsonObject{{"type", "emote"}, {"text", "Kappa"}, {"emote", QJsonObject{{"id", "25"}, {"emote_set_id", "set"},
                {"owner_id", "owner"}, {"format", QJsonArray{"static", "animated"}}}}},
            QJsonObject{{"type", "text"}, {"text", " "}},
            QJsonObject{{"type", "mention"}, {"text", "@friend"}, {"mention", QJsonObject{{"user_id", "friend"}, {"user_login", "friend_login"}, {"user_name", "Friend"}}}},
            QJsonObject{{"type", "text"}, {"text", " "}},
            QJsonObject{{"type", "cheermote"}, {"text", "Cheer10"}, {"cheermote", QJsonObject{{"prefix", "cheer"}, {"bits", 10}, {"tier", 1}}}}
        }}});
        const auto result = normalize("channel.chat.message", input);
        QVERIFY(result.event);
        const auto &message = std::get<ChatMessage>(result.event->payload);
        QCOMPARE(message.messageId, QStringLiteral("chat-id"));
        QCOMPARE(message.user.id, QStringLiteral("author"));
        QCOMPARE(message.user.login, QStringLiteral("login"));
        QCOMPARE(message.user.displayName, QStringLiteral("Display"));
        QCOMPARE(message.user.color, QColor("#123456"));
        QCOMPARE(message.badges.size(), size_t(1));
        QCOMPARE(message.badges[0].provider, BadgeProvider::Twitch);
        QCOMPARE(message.badges[0].type, QStringLiteral("subscriber"));
        QCOMPARE(message.badges[0].version, QStringLiteral("12"));
        QCOMPARE(message.badges[0].info, QStringLiteral("14"));
        QVERIFY(message.badges[0].imageUrl.isEmpty());
        QCOMPARE(message.fragments.size(), size_t(6));
        const auto &emote = message.fragments[1];
        QCOMPARE(emote.provider, std::optional{EmoteProvider::Twitch});
        QCOMPARE(emote.sourceRange->offset, qsizetype(3));
        QCOMPARE(emote.sourceRange->length, qsizetype(5));
        QCOMPARE(emote.twitch->setId, QStringLiteral("set"));
        QCOMPARE(emote.twitch->ownerId, QStringLiteral("owner"));
        QVERIFY(emote.twitch->supportsAnimated && emote.twitch->supportsStatic);
        QCOMPARE(message.fragments[3].mention->id, QStringLiteral("friend"));
        QCOMPARE(message.fragments[5].cheermote->bits, 10);
        QCOMPARE(message.metadata.reply->threadMessageId, QStringLiteral("thread"));
        QCOMPARE(message.metadata.rewardId, QStringLiteral("reward"));
        QCOMPARE(message.metadata.cheerBits, std::optional{10});
        QCOMPARE(result.event->header.originChannelId, QStringLiteral("origin"));
        QCOMPARE(result.event->header.originMessageId, QStringLiteral("original"));
    }

    void giftAndResubscriptionSemantics()
    {
        auto input = notice("sub_gift");
        input.insert("chatter_is_anonymous", true);
        // An anonymous flag wins even if a sender supplied an identity.
        auto result = normalize("channel.chat.notification", input);
        QVERIFY(result.event);
        const auto &gift = std::get<GiftSubscription>(result.event->payload);
        QVERIFY(gift.gifter.anonymous);
        QVERIFY(!gift.gifter.user);
        QVERIFY(gift.notice.user.id.isEmpty());
        QCOMPARE(gift.recipient.id, QStringLiteral("recipient"));
        QCOMPARE(gift.communityGiftId, QStringLiteral("community"));
        QCOMPARE(gift.notice.metadata.systemText, QStringLiteral("A synthetic subscription notice"));

        result = normalize("channel.chat.notification", notice("community_sub_gift"));
        QVERIFY(result.event);
        const auto &community = std::get<CommunityGiftSubscription>(result.event->payload);
        QCOMPARE(community.count, 5);
        QCOMPARE(community.communityGiftId, QStringLiteral("community"));
        QCOMPARE(community.cumulativeTotal, std::optional{20});

        input = notice("resub");
        auto detail = input["resub"].toObject();
        detail.insert("is_gift", true);
        detail.insert("gifter_is_anonymous", true);
        detail.insert("gifter_user_id", "must-not-leak");
        input.insert("resub", detail);
        result = normalize("channel.chat.notification", input);
        QVERIFY(result.event);
        const auto &resub = std::get<Resubscription>(result.event->payload);
        QCOMPARE(resub.cumulativeMonths, 12);
        QVERIFY(!resub.streakMonths);
        QVERIFY(resub.gifter && resub.gifter->anonymous && !resub.gifter->user);
        QCOMPARE(resub.terms.tier, SubscriptionTier::Tier1);
        QCOMPARE(resub.terms.isPrime, std::optional{true});

        input = notice("sub");
        detail = input["sub"].toObject();
        detail.insert("sub_tier", "future-tier");
        input.insert("sub", detail);
        result = normalize("channel.chat.notification", input);
        QVERIFY(result.event);
        QCOMPARE(std::get<Subscription>(result.event->payload).terms.tier, SubscriptionTier::Unknown);
    }

    void anonymousCheerAndClearScope()
    {
        const auto cheer = normalize("channel.cheer", {{"broadcaster_user_id", "channel"}, {"is_anonymous", true},
            {"user_id", "must-not-leak"}, {"bits", 50}, {"message", "Anonymous cheer"}});
        QVERIFY(cheer.event);
        QVERIFY(!std::get<Cheer>(cheer.event->payload).user);
        const auto all = normalize("channel.chat.clear", {{"broadcaster_user_id", "channel"}});
        QVERIFY(all.event);
        QVERIFY(!std::get<ChatCleared>(all.event->payload).user);
        const auto user = normalize("channel.chat.clear_user_messages", {{"broadcaster_user_id", "channel"}, {"target_user_id", "target"}});
        QVERIFY(user.event);
        QCOMPARE(std::get<ChatCleared>(user.event->payload).user->id, QStringLiteral("target"));
    }

    void controlsUnknownTypesVersionsAndNoticesAreIgnored()
    {
        for (const auto *control : {"session_welcome", "session_keepalive", "session_reconnect", "revocation"})
            QCOMPARE(normalize(QJsonObject{{"metadata", QJsonObject{{"message_type", control}}}}).status, NormalizationStatus::Ignored);
        QCOMPARE(normalize("channel.future", chat()).status, NormalizationStatus::Ignored);
        auto root = envelope("channel.chat.message", chat());
        auto metadata = root["metadata"].toObject();
        metadata.insert("subscription_version", "99");
        root.insert("metadata", metadata);
        QCOMPARE(normalize(root).status, NormalizationStatus::Ignored);
        for (const auto *kind : {"raid", "gift_paid_upgrade", "announcement", "unknown"})
            QCOMPARE(normalize("channel.chat.notification", notice(kind)).status, NormalizationStatus::Ignored);
    }

    void invalidKnownEventsDoNotBecomeDefaultPayloads()
    {
        auto input = chat();
        for (const auto *key : {"broadcaster_user_id", "message_id", "chatter_user_id", "message"}) {
            auto broken = input;
            broken.remove(key);
            const auto result = normalize("channel.chat.message", broken);
            QCOMPARE(result.status, NormalizationStatus::Invalid);
            QVERIFY(!result.event);
            QVERIFY(!result.error.isEmpty());
        }
        QCOMPARE(normalize("channel.chat.clear_user_messages", {{"broadcaster_user_id", "channel"}}).status, NormalizationStatus::Invalid);
        QCOMPARE(normalize("channel.chat.message_delete", {{"broadcaster_user_id", "channel"}, {"message_id", "deleted"}}).status, NormalizationStatus::Invalid);
        QCOMPARE(normalize("channel.follow", {{"broadcaster_user_id", "channel"}, {"user_id", "follower"}, {"followed_at", "invalid"}}).status, NormalizationStatus::Invalid);
        input = notice("sub");
        input.remove("sub");
        QCOMPARE(normalize("channel.chat.notification", input).status, NormalizationStatus::Invalid);
        input = notice("shared_chat_sub");
        input.remove("source_broadcaster_user_id");
        QCOMPARE(normalize("channel.chat.notification", input).status, NormalizationStatus::Invalid);
        const std::vector<QJsonValue> badCounts{QJsonValue::Null, -1, 0, 1.5, 2147483648.0, "100"};
        for (const auto &bits : badCounts) {
            QCOMPARE(normalize("channel.cheer", {{"broadcaster_user_id", "channel"}, {"is_anonymous", true},
                {"bits", bits}, {"message", "test"}}).status, NormalizationStatus::Invalid);
        }
    }

    void invalidEnvelopesAndTimestampZones()
    {
        for (const auto &input : {QByteArray{}, QByteArray("[]"), QByteArray("{"), QByteArray(1024 * 1024 + 1, 'x')})
            QCOMPARE(normalizeTwitchEvent(input, receivedAt()).status, NormalizationStatus::Invalid);
        auto root = envelope("channel.chat.message", chat());
        auto metadata = root["metadata"].toObject();
        metadata.insert("message_timestamp", "2026-09-26T12:00:00");
        root.insert("metadata", metadata);
        QCOMPARE(normalize(root).status, NormalizationStatus::Invalid);
        metadata.insert("message_timestamp", "2026-09-26T14:00:00.123+02:00");
        root.insert("metadata", metadata);
        const auto result = normalize(root);
        QVERIFY(result.event);
        QCOMPARE(result.event->header.timestamp.toString(Qt::ISODateWithMs), QStringLiteral("2026-09-26T12:00:00.123Z"));
        metadata.insert("message_id", QJsonValue::Null);
        root.insert("metadata", metadata);
        QCOMPARE(normalize(root).status, NormalizationStatus::Invalid);
        root = envelope("channel.chat.message", chat());
        auto payload = root["payload"].toObject();
        payload.insert("subscription", QJsonObject{{"type", "channel.cheer"}, {"version", "1"}});
        root.insert("payload", payload);
        QCOMPARE(normalize(root).status, NormalizationStatus::Invalid);
    }

    void securityBoundaryPreservesSemanticTextAndDropsOptionalAssets()
    {
        auto result = normalize("channel.chat.message", chat());
        QVERIFY(result.event);
        auto event = *result.event;
        auto &message = std::get<ChatMessage>(event.payload);
        message.text = QStringLiteral("<script>alert(1)</script> <img src=x onerror=alert(1)> 😀\r\nnext");
        message.fragments = {{ChatFragment::Type::Text, message.text}};
        message.fragments.front().sourceRange = TextRange{0, message.text.size()};
        message.media = {{QUrl(QStringLiteral("javascript:alert(1)")), {}},
                         {QUrl(QStringLiteral("https://static-cdn.jtvnw.net/emote.png")), {}}};
        message.badges = {{BadgeProvider::Twitch, QStringLiteral("subscriber"), QStringLiteral("1"), {},
                           QUrl(QStringLiteral("file:///tmp/badge.png"))}};
        const auto validation = validateEvent(event);
        QCOMPARE(validation.disposition, ValidationDisposition::Repaired);
        const auto &validated = std::get<ChatMessage>(event.payload);
        QCOMPARE(validated.text, QStringLiteral("<script>alert(1)</script> <img src=x onerror=alert(1)> 😀\nnext"));
        QCOMPARE(validated.media.size(), size_t(1));
        QVERIFY(validated.badges.front().imageUrl.isEmpty());
    }

    void securityBoundaryRejectsRequiredIdsAndInvalidRanges()
    {
        auto result = normalize("channel.chat.message", chat());
        QVERIFY(result.event);
        auto event = *result.event;
        event.header.eventId = QStringLiteral("bad id");
        QCOMPARE(validateEvent(event).disposition, ValidationDisposition::Rejected);

        event = *result.event;
        auto &message = std::get<ChatMessage>(event.payload);
        message.fragments = {{ChatFragment::Type::Emote, QStringLiteral("Hello")}};
        message.fragments.front().provider = EmoteProvider::Twitch;
        message.fragments.front().emoteId = QStringLiteral("25");
        message.fragments.front().imageUrl = QUrl(QStringLiteral("data:image/svg+xml,x"));
        QCOMPARE(validateEvent(event).disposition, ValidationDisposition::Repaired);
        QCOMPARE(std::get<ChatMessage>(event.payload).fragments.front().type, ChatFragment::Type::Text);
    }

    void securityBoundaryAcceptsOnlyApprovedHttpsAssetsAndBoundsText()
    {
        QVERIFY(isAllowedAssetUrl(QUrl(QStringLiteral("https://static-cdn.jtvnw.net/emote.png")), EmoteProvider::Twitch));
        QVERIFY(isAllowedAssetUrl(QUrl(QStringLiteral("HTTPS://cdn.7tv.app/emote.png")), EmoteProvider::SevenTV));
        for (const auto &url : {QStringLiteral("http://static-cdn.jtvnw.net/a"),
                                QStringLiteral("javascript:alert(1)"), QStringLiteral("file:///tmp/a"),
                                QStringLiteral("data:image/png,x"), QStringLiteral("ws://127.0.0.1:8080/ws"),
                                QStringLiteral("wss://eventsub.wss.twitch.tv/ws"), QStringLiteral("https://static-cdn.jtvnw.net/%00"),
                                QStringLiteral("https://static-cdn.jtvnw.net/%zz"), QStringLiteral(" https://static-cdn.jtvnw.net/a"),
                                QStringLiteral("https://static-cdn.jtvnw.net/a ")})
            QVERIFY(!isAllowedAssetUrl(QUrl(url, QUrl::StrictMode)));
        QVERIFY(!isAllowedAssetUrl(QUrl(QStringLiteral("https://cdn.7tv.app/emote.png")), EmoteProvider::Twitch));

        auto result = normalize("channel.chat.message", chat());
        QVERIFY(result.event);
        auto event = *result.event;
        auto &message = std::get<ChatMessage>(event.payload);
        message.text = QString(9000, QLatin1Char('x')) + QString::fromUtf8("😀");
        message.fragments = {{ChatFragment::Type::Text, message.text}};
        message.fragments.front().sourceRange = TextRange{0, message.text.size()};
        QCOMPARE(validateEvent(event).disposition, ValidationDisposition::Repaired);
        QCOMPARE(std::get<ChatMessage>(event.payload).text.size(), qsizetype(8192));
    }

    void securityBoundaryRepairsNestedMetadataAndInvalidColor()
    {
        auto input = chat();
        input["color"] = QStringLiteral("red");
        auto result = normalize("channel.chat.message", input);
        QVERIFY(result.event);
        QVERIFY(!std::get<ChatMessage>(result.event->payload).user.color.isValid());

        auto event = *result.event;
        auto &message = std::get<ChatMessage>(event.payload);
        message.badges.push_back({BadgeProvider::Twitch, QStringLiteral("subscriber"), QStringLiteral("1"), {},
                                  QUrl(QStringLiteral("https://cdn.7tv.app/badge.png"))});
        message.fragments.front().mention = ChatUser{QStringLiteral("bad id"), {}, {}, {}};
        message.fragments.front().cheermote = CheermoteMetadata{QStringLiteral("Cheer"), -1, 10};
        QCOMPARE(validateEvent(event).disposition, ValidationDisposition::Repaired);
        const auto &validated = std::get<ChatMessage>(event.payload);
        QVERIFY(validated.badges.back().imageUrl.isEmpty());
        QVERIFY(!validated.fragments.front().mention);
        QVERIFY(!validated.fragments.front().cheermote);

        Cheer cheer{{}, 1, QString(QChar(0xD800))};
        event.payload = cheer;
        QCOMPARE(validateEvent(event).disposition, ValidationDisposition::Rejected);
    }
};

QTEST_GUILESS_MAIN(EventNormalizerTests)
#include "event-normalizer-tests.moc"

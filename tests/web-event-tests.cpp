#include "web/web-event-serializer.hpp"
#include "core/event-dispatcher.hpp"
#include "renderer/renderer-mode.hpp"
#include "web/widget-resource-request.hpp"
#include <QJsonDocument>
#include <QTimeZone>
#include <QtTest>

namespace {
ChatUser user(QString name = QStringLiteral("Viewer")) { return {QStringLiteral("user-1"), QStringLiteral("viewer"), std::move(name), QColor(QStringLiteral("#123456"))}; }
ChatMessage message(QString value = QStringLiteral("Hello 👋"))
{
    ChatMessage result;
    result.messageId = QStringLiteral("message-1"); result.user = user(); result.text = value;
    ChatFragment emote{ChatFragment::Type::Emote, QStringLiteral("Kappa")};
    emote.emoteId = QStringLiteral("25"); emote.provider = EmoteProvider::Twitch;
    emote.imageUrl = QUrl(QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/25/static/dark/3.0"));
    emote.sourceRange = TextRange{0, 5}; emote.twitch = TwitchEmoteMetadata{QStringLiteral("set-1"), QStringLiteral("owner-1"), true, false};
    result.fragments = {{ChatFragment::Type::Text, value}, emote};
    result.badges = {{BadgeProvider::Twitch, QStringLiteral("moderator"), QStringLiteral("1"), QStringLiteral("Moderator"), {}}};
    return result;
}
PluginEvent makeEvent(EventPayload payload)
{
    return {{QStringLiteral("event-1"), QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC), QDateTime::fromMSecsSinceEpoch(2000, QTimeZone::UTC),
             QStringLiteral("channel-1"), {}, {}, 9007199254740993ULL, 2, EventOrigin::SyntheticTest}, std::move(payload)};
}
}

class WebEventTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void serializesAllEventTypes()
    {
        const auto actor = user();
        const auto notice = message();
        const SubscriptionTerms subscriptionTerms{SubscriptionTier::Tier1, false, 3};
        const GiftActor gifter{false, actor};
        const std::vector<EventPayload> payloads{
            notice, MessageDeleted{notice.messageId, actor}, ChatCleared{}, Follow{actor, QDateTime::fromMSecsSinceEpoch(3000, QTimeZone::UTC)},
            Subscription{notice, subscriptionTerms}, Resubscription{notice, subscriptionTerms, 12, 4, true, gifter},
            GiftSubscription{notice, gifter, user(QStringLiteral("Recipient")), subscriptionTerms, QStringLiteral("community-1"), 9},
            CommunityGiftSubscription{notice, gifter, SubscriptionTier::Tier2, QStringLiteral("community-1"), 5, 20},
            Cheer{actor, 100, QStringLiteral("Great stream")}, Raid{actor, user(QStringLiteral("Channel")), 42}};
        const QStringList expected{"ChatMessage", "MessageDeleted", "ChatCleared", "Follow", "Subscription", "Resubscription",
                                   "GiftSubscription", "CommunityGiftSubscription", "Cheer", "Raid"};
        for (size_t i = 0; i < payloads.size(); ++i) {
            const auto object = WebEventSerializer::serialize(makeEvent(payloads[i]));
            QCOMPARE(object.value("type").toString(), expected.at(qsizetype(i)));
            QCOMPARE(object.value("schemaVersion").toInt(), 1);
            QCOMPARE(object.value("header").toObject().value("sequence").toString(), QStringLiteral("9007199254740993"));
            QVERIFY(object.value("data").isObject());
        }
    }
    void semanticTextStaysJsonData()
    {
        const QString hostile = QStringLiteral("</script><img onerror=alert(1)> \\\" 👨‍👩‍👧‍👦");
        const QByteArray encoded = QJsonDocument(WebEventSerializer::serialize(makeEvent(message(hostile)))).toJson(QJsonDocument::Compact);
        const auto parsed = QJsonDocument::fromJson(encoded).object();
        QCOMPARE(parsed.value("data").toObject().value("text").toString(), hostile);
        QVERIFY(!encoded.contains("innerHTML"));
    }
    void embeddedNullAndControlsStayJsonData()
    {
        QString value = QStringLiteral("prefix");
        value.append(QChar::Null);
        value.append(QChar(0x01));
        value.append(QStringLiteral("suffix 👋"));
        const QByteArray encoded = QJsonDocument(WebEventSerializer::serialize(makeEvent(message(value)))).toJson(QJsonDocument::Compact);
        QCOMPARE(QJsonDocument::fromJson(encoded).object().value("data").toObject().value("text").toString(), value);
        QVERIFY(encoded.contains("\\u0000"));
        QVERIFY(encoded.contains("\\u0001"));
    }
    void serializesStructuredEmotesAndBadges()
    {
        const auto data = WebEventSerializer::serialize(makeEvent(message())).value("data").toObject();
        const auto fragments = data.value("fragments").toArray();
        QCOMPARE(fragments.at(1).toObject().value("emoteId").toString(), QStringLiteral("25"));
        QCOMPARE(fragments.at(1).toObject().value("provider").toString(), QStringLiteral("twitch"));
        QCOMPARE(data.value("badges").toArray().at(0).toObject().value("type").toString(), QStringLiteral("moderator"));
        QVERIFY(data.value("badges").toArray().at(0).toObject().value("imageUrl").isNull());
    }
    void independentSubscriptionsAndDestruction()
    {
        EventDispatcher dispatcher;
        auto first = dispatcher.subscribe();
        auto second = dispatcher.subscribe();
        auto value = std::make_shared<const PluginEvent>(makeEvent(message()));
        QCOMPARE(dispatcher.publish(value), PublishResult::Published);
        QCOMPARE(first.takeBatch().events.size(), size_t(1));
        QCOMPARE(second.takeBatch().events.size(), size_t(1));
        first.close();
        QCOMPARE(dispatcher.publish(value), PublishResult::Published);
        QCOMPARE(first.takeBatch().state, ConsumerState::Closed);
        QCOMPARE(second.takeBatch().events.size(), size_t(1));
        second.close();
        auto refreshed = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(value), PublishResult::Published);
        QCOMPARE(refreshed.takeBatch().events.size(), size_t(1));
    }
    void rendererModeConfiguration()
    {
        QCOMPARE(rendererModeFromSetting(QStringLiteral("native")), RendererMode::Native);
        QCOMPARE(rendererModeFromSetting(QStringLiteral("web_widget")), RendererMode::WebWidget);
        QCOMPARE(rendererModeFromSetting(QStringLiteral("future_mode")), RendererMode::Native);
        QCOMPARE(rendererModeSetting(RendererMode::WebWidget), QStringLiteral("web_widget"));
    }
    void rejectsUnvalidatedAssetUrls()
    {
        auto value = message();
        value.fragments.at(1).imageUrl = QUrl(QStringLiteral("https://example.invalid/emote.png"));
        value.badges.at(0).imageUrl = QUrl(QStringLiteral("file:///tmp/badge.png"));
        const auto data = WebEventSerializer::serialize(makeEvent(value)).value("data").toObject();
        QVERIFY(data.value("fragments").toArray().at(1).toObject().value("imageUrl").isNull());
        QVERIFY(data.value("badges").toArray().at(0).toObject().value("imageUrl").isNull());
    }
    void parsesWidgetResourceRequestsExactly()
    {
        const QByteArray capability = "0123456789abcdef";
        const QByteArray request = "GET /" + capability + "/index.html HTTP/1.1\r\nHost: 127.0.0.1:4567\r\nAccept: */*\r\n\r\n";
        const auto parsed = WidgetResourceRequest::resourceName(request, capability, 4567);
        QVERIFY(parsed);
        QCOMPARE(*parsed, QByteArray("index.html"));
        QVERIFY(!WidgetResourceRequest::resourceName(request.chopped(1), capability, 4567));
        QVERIFY(!WidgetResourceRequest::resourceName(
            "GET /" + capability + "/../secret HTTP/1.1\r\nHost: 127.0.0.1:4567\r\n\r\n", capability, 4567));
        QVERIFY(!WidgetResourceRequest::resourceName(
            "GET /" + capability + "/index.html HTTP/1.1\r\nHost: example.com\r\n\r\n", capability, 4567));
        QVERIFY(!WidgetResourceRequest::resourceName(
            "GET /" + capability + "/index.html HTTP/1.1\r\nHost: 127.0.0.1:4567\r\nHost: 127.0.0.1:4567\r\n\r\n", capability, 4567));
    }
};

QTEST_GUILESS_MAIN(WebEventTests)
#include "web-event-tests.moc"

#include "twitch-producer-tests.hpp"
#include "core/plugin-runtime.hpp"
#include "renderer/native-event-adapter.hpp"
#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTest>
#include <thread>

namespace {
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonArray fixtures()
{
    QFile file(QStringLiteral(FIXTURE_DIR "/eventsub-events.json"));
    if (!file.open(QIODevice::ReadOnly)) qFatal("Missing EventSub fixtures");
    return QJsonDocument::fromJson(file.readAll()).array();
}
QByteArray eventAt(int index, const QString &id = {})
{
    auto root = fixtures()[index].toObject()["envelope"].toObject();
    if (!id.isEmpty()) { auto meta = root["metadata"].toObject(); meta["message_id"] = id; root["metadata"] = meta; }
    return json(root);
}
QByteArray welcome(const QString &id = QStringLiteral("session-one"))
{
    return json({{"metadata", QJsonObject{{"message_type", "session_welcome"}}},
        {"payload", QJsonObject{{"session", QJsonObject{{"id", id}, {"keepalive_timeout_seconds", 60}}}}}});
}
struct Response { QByteArray bytes = "{}"; int status = 200; int delay = 0; };
class FakeReply final : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest &request, Response response, QObject *parent)
        : QNetworkReply(parent), bytes_(std::move(response.bytes))
    {
        setRequest(request); setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(response.delay, this, [this] {
            if (isFinished()) return;
            ready_ = true; Q_EMIT readyRead();
            if (!isFinished()) { setFinished(true); Q_EMIT finished(); }
        });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError, QStringLiteral("Cancelled")); setFinished(true); Q_EMIT finished(); } }
    qint64 bytesAvailable() const override { return (ready_ ? bytes_.size() - offset_ : 0) + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const auto count = std::min(maximum, ready_ ? bytes_.size() - offset_ : qint64(0));
        if (count <= 0) return -1;
        memcpy(data, bytes_.constData() + offset_, static_cast<size_t>(count)); offset_ += count; return count;
    }
private:
    QByteArray bytes_;
    qint64 offset_ = 0;
    bool ready_ = false;
};
class FakeNetwork final : public QNetworkAccessManager {
public:
    QJsonArray scopes{"user:read:chat", "moderator:read:followers", "bits:read"};
    QList<QJsonObject> subscriptions;
    QList<QNetworkRequest> requests;
    QList<QByteArray> forms;
    int validations = 0, lookups = 0, deviceRequests = 0, tokenRequests = 0;
    int imageDelay = 0, catalogDelay = 0, followFailures = 0, followStatus = 403;
    int invalidValidations = 0;
    bool allInvalid = false;
    QString userId = QStringLiteral("100");
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *outgoing) override
    {
        requests.push_back(request);
        const auto path = request.url().path();
        const auto host = request.url().host();
        Response response;
        if (path == QStringLiteral("/oauth2/validate")) {
            ++validations;
            if (allInvalid || invalidValidations-- > 0) response.status = 401;
            else response.bytes = json({{"client_id", "test-client"}, {"user_id", userId}, {"login", "broadcaster"}, {"scopes", scopes}});
        } else if (path == QStringLiteral("/helix/users")) {
            ++lookups; response.bytes = R"({"data":[{"id":"100","login":"broadcaster"}]})";
        } else if (path == QStringLiteral("/helix/eventsub/subscriptions")) {
            const auto body = QJsonDocument::fromJson(outgoing->readAll()).object();
            subscriptions.push_back(body); response.status = 202;
            if (body["type"] == QStringLiteral("channel.follow") && followFailures-- > 0) response.status = followStatus;
        } else if (path == QStringLiteral("/oauth2/device")) {
            ++deviceRequests; forms.push_back(outgoing->readAll());
            response.bytes = R"({"device_code":"synthetic-device","user_code":"TEST-ONLY","verification_uri":"https://www.twitch.tv/activate","interval":1})";
        } else if (path == QStringLiteral("/oauth2/token")) {
            ++tokenRequests; forms.push_back(outgoing->readAll());
            response.bytes = R"({"access_token":"synthetic-new-access","refresh_token":"synthetic-new-refresh"})";
        } else if (host == QStringLiteral("api.betterttv.net")) {
            response.delay = catalogDelay;
            response.bytes = path.endsWith(QStringLiteral("/100")) ? R"({"channelEmotes":[{"id":"123","code":"Dance"}]})" : "[]";
        } else if (host == QStringLiteral("api.frankerfacez.com")) {
            response.delay = catalogDelay;
            response.bytes = R"({"sets":{"1":{"emoticons":[{"id":42,"name":"Hello","urls":{"4":"https://cdn.frankerfacez.com/42"}}]}}})";
        } else if (host == QStringLiteral("7tv.io")) {
            response.delay = catalogDelay;
            response.bytes = R"({"emotes":[{"id":"abc","name":"Alias","data":{"host":{"url":"https://cdn.7tv.app/emote/abc","files":[{"name":"3x.png","format":"PNG","height":112}]}}}]})";
        } else {
            response.delay = imageDelay;
            QImage image(24, 16, QImage::Format_RGBA8888); image.fill(Qt::green);
            QBuffer buffer(&response.bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
        }
        return new FakeReply(request, std::move(response), this);
    }
};
class FakeSocket final : public EventSubSocket {
public:
    QUrl url;
    bool closed = false;
    void open(const QUrl &value) override { url = value; }
    void close() override { closed = true; }
    void deliver(const QByteArray &bytes) { Q_EMIT messageReceived(QString::fromUtf8(bytes)); }
    void lose() { Q_EMIT connectionLost(); }
};
struct Harness {
    FakeNetwork network;
    QList<QPointer<FakeSocket>> sockets;
    QStringList logs;
    QList<TwitchTokens> tokens;
    int browsers = 0;
    TwitchClient::Dependencies dependencies()
    {
        return {&network, [this] {
            auto socket = std::make_unique<FakeSocket>(); sockets.push_back(socket.get()); return socket;
        }, [this](const QUrl &) { ++browsers; }};
    }
};
TwitchConfiguration configuration() { return {QStringLiteral("test-client"), QStringLiteral("broadcaster"), QStringLiteral("synthetic-access"), QStringLiteral("synthetic-refresh")}; }
void append(EventSubscription &subscription, std::vector<EventPtr> &events)
{
    auto batch = subscription.takeBatch(256);
    events.insert(events.end(), batch.events.begin(), batch.events.end());
}
} // namespace

void TwitchProducerTests::allEventsThroughLiveProducer()
{
    Harness h; EventDispatcher dispatcher;
    auto a = dispatcher.subscribe(), b = dispatcher.subscribe();
    TwitchClient client(dispatcher, [&](QString log) { h.logs.push_back(log); }, {}, h.dependencies());
    client.configure(configuration()); client.startOrResume(); client.startOrResume();
    QTRY_COMPARE(h.sockets.size(), 1);
    h.sockets[0]->deliver(welcome()); h.sockets[0]->deliver(welcome());
    QTRY_COMPARE(h.network.subscriptions.size(), 8);
    QTRY_COMPARE(client.subscriptions().value(QStringLiteral("channel.follow")), TwitchSubscriptionState::Enabled);
    QCOMPARE(h.network.validations, 1); QCOMPARE(h.network.lookups, 1);
    QSet<QString> types;
    for (const auto &sub : h.network.subscriptions) {
        const auto type = sub["type"].toString(); types.insert(type);
        QCOMPARE(sub["transport"].toObject()["session_id"].toString(), QStringLiteral("session-one"));
        if (type == QStringLiteral("channel.follow")) {
            QCOMPARE(sub["version"].toString(), QStringLiteral("2"));
            QCOMPARE(sub["condition"].toObject()["moderator_user_id"].toString(), QStringLiteral("100"));
        }
    }
    QCOMPARE(types.size(), 8);
    QVERIFY(!types.contains(QStringLiteral("channel.subscribe")));
    QVERIFY(!types.contains(QStringLiteral("channel.subscription.gift")));
    for (int i = 0; i < fixtures().size(); ++i) { h.sockets[0]->deliver(eventAt(i)); h.sockets[0]->deliver(eventAt(i)); }
    std::vector<EventPtr> received, other;
    QTRY_VERIFY_WITH_TIMEOUT((append(a, received), received.size() == 12), 4000);
    append(b, other); QCOMPARE(other, received);
    const std::vector<EventKind> expected{EventKind::ChatMessage, EventKind::MessageDeleted, EventKind::ChatCleared,
        EventKind::ChatCleared, EventKind::Follow, EventKind::Subscription, EventKind::Resubscription,
        EventKind::GiftSubscription, EventKind::CommunityGiftSubscription, EventKind::Cheer, EventKind::Cheer, EventKind::Raid};
    for (size_t i = 0; i < expected.size(); ++i) {
        QCOMPARE(eventKind(received[i]->payload), expected[i]);
        QCOMPARE(received[i]->header.sequence, i + 1);
        QCOMPARE(received[i]->header.channelId, QStringLiteral("100"));
        QVERIFY(received[i]->header.timestamp.isValid());
    }
    const auto &chat = std::get<ChatMessage>(received[0]->payload);
    QCOMPARE(chat.messageId, QStringLiteral("chat-001"));
    QCOMPARE(chat.user.displayName, QStringLiteral("雪Viewer👋"));
    QCOMPARE(chat.user.login, QStringLiteral("viewer")); QCOMPARE(chat.user.id, QStringLiteral("200"));
    QCOMPARE(chat.user.color, QColor(QStringLiteral("#AB12EF")));
    QCOMPARE(chat.badges[0].type, QStringLiteral("subscriber")); QCOMPARE(chat.badges[0].info, QStringLiteral("14"));
    QVERIFY(chat.metadata.reply); QCOMPARE(chat.metadata.reply->parentMessageId, QStringLiteral("parent-001"));
    QSet<int> providers;
    for (const auto &fragment : chat.fragments) if (fragment.provider) {
        providers.insert(int(*fragment.provider)); QVERIFY(fragment.image); QVERIFY(fragment.sourceRange);
        QCOMPARE(chat.text.mid(fragment.sourceRange->offset, fragment.sourceRange->length), fragment.text);
    }
    QCOMPARE(providers.size(), 4);
    QCOMPARE(std::get<MessageDeleted>(received[1]->payload).messageId, chat.messageId);
    QVERIFY(!std::get<ChatCleared>(received[2]->payload).user);
    QCOMPARE(std::get<ChatCleared>(received[3]->payload).user->id, QStringLiteral("201"));
    QVERIFY(std::get<Follow>(received[4]->payload).followedAt.isValid());
    QCOMPARE(std::get<Subscription>(received[5]->payload).terms.tier, SubscriptionTier::Tier1);
    const auto &resub = std::get<Resubscription>(received[6]->payload);
    QCOMPARE(resub.cumulativeMonths, 24); QVERIFY(!resub.streakMonths); QCOMPARE(resub.notice.text, QStringLiteral("Thanks 💜"));
    const auto &gift = std::get<GiftSubscription>(received[7]->payload);
    QVERIFY(gift.gifter.anonymous); QVERIFY(!gift.gifter.user); QVERIFY(!gift.cumulativeTotal);
    QCOMPARE(gift.recipient.id, QStringLiteral("203"));
    QCOMPARE(std::get<CommunityGiftSubscription>(received[8]->payload).count, 5);
    QCOMPARE(std::get<Cheer>(received[9]->payload).bits, 100);
    QVERIFY(!std::get<Cheer>(received[10]->payload).user);
    QCOMPARE(std::get<Raid>(received[11]->payload).from.displayName, QStringLiteral("Raid友"));
    QCOMPARE(std::get<Raid>(received[11]->payload).viewers, 42);
    for (const auto &request : h.network.requests)
        if (request.url().host() != QStringLiteral("api.twitch.tv") && request.url().host() != QStringLiteral("id.twitch.tv"))
            QVERIFY(request.rawHeader("Authorization").isEmpty());
    QVERIFY(!h.logs.join(' ').contains(QStringLiteral("synthetic-access")));
    QVERIFY(a.takeBatch().events.empty());
}

void TwitchProducerTests::handoffAndFreshReconnect()
{
    Harness h; EventDispatcher dispatcher; auto consumer = dispatcher.subscribe();
    TwitchClient client(dispatcher, {}, {}, h.dependencies()); client.configure(configuration()); client.startOrResume();
    QTRY_COMPARE(h.sockets.size(), 1); h.sockets[0]->deliver(welcome());
    QTRY_COMPARE(h.network.subscriptions.size(), 8);
    h.sockets[0]->deliver(eventAt(4));
    h.sockets[0]->deliver(R"({"metadata":{"message_type":"session_reconnect"},"payload":{"session":{"reconnect_url":"wss://eventsub.wss.twitch.tv/reconnect?test=1"}}})");
    QCOMPARE(h.sockets.size(), 2); QVERIFY(!h.sockets[0]->closed);
    h.sockets[0]->deliver(eventAt(4)); h.sockets[1]->deliver(welcome(QStringLiteral("session-two")));
    QVERIFY(h.sockets[0]->closed); QCOMPARE(h.network.subscriptions.size(), 8);
    h.sockets[1]->deliver(eventAt(4)); h.sockets[1]->deliver(eventAt(4, QStringLiteral("next-follow")));
    QCOMPARE(consumer.takeBatch().events.size(), size_t(2));
    h.sockets[1]->lose();
    QTRY_COMPARE_WITH_TIMEOUT(h.sockets.size(), 3, 2000);
    h.sockets[2]->deliver(welcome(QStringLiteral("session-three")));
    QTRY_COMPARE(h.network.subscriptions.size(), 16);
    h.sockets[2]->deliver(eventAt(4)); QVERIFY(consumer.takeBatch().events.empty());
    h.sockets[2]->deliver(eventAt(4, QStringLiteral("after-reconnect")));
    QCOMPARE(consumer.takeBatch().events.size(), size_t(1));
}

void TwitchProducerTests::missingScopesAndRejectedFollowDoNotBreakChat()
{
    Harness h; h.network.scopes = {"user:read:chat"}; EventDispatcher dispatcher;
    auto consumer = dispatcher.subscribe(); TwitchClient client(dispatcher, {}, {}, h.dependencies());
    client.configure(configuration()); client.startOrResume(); QTRY_COMPARE(h.sockets.size(), 1);
    h.sockets[0]->deliver(welcome()); QTRY_COMPARE(h.network.subscriptions.size(), 6);
    QCOMPARE(client.subscriptions()[QStringLiteral("channel.follow")], TwitchSubscriptionState::Unavailable);
    QCOMPARE(client.subscriptions()[QStringLiteral("channel.cheer")], TwitchSubscriptionState::Unavailable);
    client.stop(); h.network.scopes.append(QStringLiteral("moderator:read:followers")); h.network.followFailures = 1;
    client.startOrResume(); QTRY_COMPARE(h.sockets.size(), 2); h.sockets[1]->deliver(welcome());
    QTRY_COMPARE(client.subscriptions().value(QStringLiteral("channel.follow")), TwitchSubscriptionState::Failed);
    h.sockets[1]->deliver(eventAt(0)); std::vector<EventPtr> events;
    QTRY_VERIFY((append(consumer, events), events.size() == 1));
    QVERIFY(client.status().contains(QStringLiteral("channel.follow")));
    h.sockets[1]->deliver(R"({"metadata":{"message_type":"revocation"},"payload":{"subscription":{"type":"channel.chat.message"}}})");
    QCOMPARE(client.subscriptions().value(QStringLiteral("channel.chat.message")), TwitchSubscriptionState::Revoked);
}

void TwitchProducerTests::transientSubscriptionRetryAndStop()
{
    Harness h; h.network.followFailures = 1; h.network.followStatus = 503;
    EventDispatcher dispatcher; TwitchClient client(dispatcher, {}, {}, h.dependencies());
    client.configure(configuration()); client.startOrResume(); QTRY_COMPARE(h.sockets.size(), 1);
    h.sockets[0]->deliver(welcome());
    QTRY_COMPARE_WITH_TIMEOUT(client.subscriptions().value(QStringLiteral("channel.follow")), TwitchSubscriptionState::Enabled, 2500);
    QCOMPARE(h.network.subscriptions.size(), 9);
    client.stop(); h.network.followFailures = 10; client.startOrResume(); QTRY_COMPARE(h.sockets.size(), 2);
    h.sockets[1]->deliver(welcome()); QTRY_COMPARE(h.network.subscriptions.size(), 17);
    client.stop(); QTest::qWait(1100); QCOMPARE(h.network.subscriptions.size(), 17);
}

void TwitchProducerTests::delayedEnrichmentOrderingAndCancellation()
{
    Harness h; h.network.imageDelay = 200; EventDispatcher dispatcher; auto consumer = dispatcher.subscribe();
    OrderedEventPipeline pipeline(dispatcher, {}, &h.network); pipeline.setChannel(QStringLiteral("100"), 7);
    QCOMPARE(pipeline.ingest(eventAt(0)), IngestResult::Accepted);
    QCOMPARE(pipeline.ingest(eventAt(1)), IngestResult::Accepted);
    QCOMPARE(pipeline.ingest(eventAt(4)), IngestResult::Accepted);
    QVERIFY(consumer.takeBatch().events.empty()); std::vector<EventPtr> events;
    QTRY_VERIFY((append(consumer, events), events.size() == 3));
    QCOMPARE(eventKind(events[0]->payload), EventKind::ChatMessage);
    QCOMPARE(eventKind(events[1]->payload), EventKind::MessageDeleted);
    QCOMPARE(eventKind(events[2]->payload), EventKind::Follow);
    pipeline.stop(); pipeline.setChannel(QStringLiteral("100"), 8);
    QCOMPARE(pipeline.ingest(eventAt(0)), IngestResult::Accepted);
    pipeline.stop(); QTest::qWait(300); QVERIFY(consumer.takeBatch().events.empty());
    QCOMPARE(pipeline.ingest(eventAt(4)), IngestResult::Stopped);
    pipeline.setChannel(QStringLiteral("other"), 9);
    QCOMPARE(pipeline.ingest(eventAt(4)), IngestResult::WrongChannel);
}

void TwitchProducerTests::sharedRuntimeAndDetachedConsumers()
{
    Harness h; PluginRuntime runtime(h.dependencies());
    std::vector<EventPtr> one, two;
    auto a = runtime.attach([&](BackendAttachment::Delivery delivery) { one.insert(one.end(), delivery.batch.events.begin(), delivery.batch.events.end()); });
    auto b = runtime.attach([&](BackendAttachment::Delivery delivery) { two.insert(two.end(), delivery.batch.events.begin(), delivery.batch.events.end()); });
    a->configure(configuration()); b->configure(configuration());
    QTRY_COMPARE(h.sockets.size(), 1); h.sockets[0]->deliver(welcome()); QTRY_COMPARE(h.network.subscriptions.size(), 8);
    auto future = runtime.subscribe(); h.sockets[0]->deliver(eventAt(4));
    QTRY_COMPARE(one.size(), size_t(1)); QTRY_COMPARE(two.size(), size_t(1));
    QCOMPARE(one.front(), two.front()); QCOMPARE(future.takeBatch().events.front(), one.front());
    QCOMPARE(h.network.validations, 1);
    auto conflict = runtime.attach([](auto delivery) { QVERIFY(delivery.batch.events.empty()); });
    auto other = configuration(); other.channel = QStringLiteral("another-channel"); conflict->configure(other);
    QTRY_VERIFY(conflict->status().contains(QStringLiteral("another channel")));
    QCOMPARE(h.sockets.size(), 1);
    std::thread detach([&] { a->close(); }); detach.join();
    h.sockets[0]->deliver(eventAt(4, QStringLiteral("second")));
    QTRY_COMPARE(two.size(), size_t(2)); QCOMPARE(one.size(), size_t(1));
    auto socket = h.sockets[0]; b->close();
    QTRY_VERIFY(socket.isNull() || socket->closed);
    conflict->close(); runtime.shutdown();
    QCOMPARE(future.takeBatch().state, ConsumerState::RuntimeStopped);
    auto late = runtime.attach([](auto) { qFatal("Stopped runtime delivered an event"); });
    QCOMPARE(late->status(), QStringLiteral("Runtime stopped"));
}

void TwitchProducerTests::runtimeTokensAndQueuedDetach()
{
    Harness h; PluginRuntime runtime(h.dependencies(), [&](QString log) { h.logs.push_back(log); });
    int deliveries = 0;
    auto cancelled = runtime.attach([&](auto) { ++deliveries; }); cancelled->configure(configuration()); cancelled->close();
    auto a = runtime.attach([&](BackendAttachment::Delivery delivery) { if (delivery.tokens) h.tokens.push_back(*delivery.tokens); });
    auto config = configuration(); config.accessToken.clear(); config.refreshToken.clear(); a->configure(config);
    a->connect(); a->connect(); QTRY_COMPARE(h.sockets.size(), 1);
    QTRY_COMPARE(h.tokens.size(), 1); QCOMPARE(h.network.deviceRequests, 1); QCOMPARE(h.network.tokenRequests, 1);
    QCOMPARE(h.browsers, 1); QCOMPARE(deliveries, 0);
    a->configure(config); // A stale source settings snapshot cannot roll back shared credentials.
    QTest::qWait(60); QCOMPARE(h.sockets.size(), 1); QCOMPARE(h.network.validations, 1);
    QVERIFY(!h.logs.join(' ').contains(QStringLiteral("TEST-ONLY")));
    QVERIFY(!h.logs.join(' ').contains(QStringLiteral("synthetic-new-access")));
    runtime.shutdown(); runtime.shutdown();
}

void TwitchProducerTests::refreshBoundedAndFormEncoding()
{
    Harness h; h.network.allInvalid = true; EventDispatcher dispatcher;
    TwitchClient client(dispatcher, {}, {}, h.dependencies());
    auto config = configuration(); config.refreshToken = QStringLiteral("synthetic+refresh&token");
    client.configure(config); client.startOrResume();
    QTRY_COMPARE(h.network.validations, 2); QTRY_VERIFY(client.status().contains(QStringLiteral("expired")));
    QCOMPARE(h.network.tokenRequests, 1); QVERIFY(h.sockets.empty());
    QVERIFY(h.network.forms.front().contains("synthetic%2Brefresh%26token"));
}

void TwitchProducerTests::nativeAdapterModerationAndLifetime()
{
    Harness h; EventDispatcher dispatcher; auto consumer = dispatcher.subscribe();
    OrderedEventPipeline pipeline(dispatcher, {}, &h.network); pipeline.setChannel(QStringLiteral("100"), 1);
    auto adapter = std::make_shared<NativeEventAdapter>();
    pipeline.ingest(eventAt(0)); std::vector<EventPtr> events;
    QTRY_VERIFY((append(consumer, events), events.size() == 1));
    adapter->accept({{events, ConsumerState::Active}, {}, {}, {}});
    QCOMPARE(adapter->messages.size(), size_t(1));
    const auto identity = adapter->messages.front().identity;
    pipeline.ingest(eventAt(1)); auto deleted = consumer.takeBatch();
    QVERIFY(NativeEventAdapter::removes(*deleted.events.front(), identity));
    adapter->accept({std::move(deleted), {}, {}, {}}); QVERIFY(adapter->messages.empty());
    adapter->accept({{events, ConsumerState::Active}, {}, {}, {}}); QVERIFY(adapter->messages.empty());
    auto other = std::make_shared<NativeEventAdapter>(); other->configure({{}, 400, 2, 16, 16, 50, 50});
    other->accept({{events, ConsumerState::Active}, {}, {}, {}});
    QCOMPARE(other->messages.size(), size_t(1)); QCOMPARE(other->messages.front().speed, 50.0f);
    other->close(); other->accept({{events, ConsumerState::Active}, {}, {}, {}}); QVERIFY(other->messages.empty());
    pipeline.ingest(eventAt(2)); auto cleared = consumer.takeBatch();
    QVERIFY(NativeEventAdapter::removes(*cleared.events.front(), identity));
    auto newer = identity; newer.header.sequence = 999; newer.header.timestamp = newer.header.timestamp.addDays(1);
    QVERIFY(!NativeEventAdapter::removes(*cleared.events.front(), newer));
    pipeline.ingest(eventAt(3)); auto userClear = consumer.takeBatch();
    QVERIFY(!NativeEventAdapter::removes(*userClear.events.front(), identity));
}
void TwitchProducerTests::shutdownFromAnotherThread()
{
    Harness h; h.network.catalogDelay = 4000;
    PluginRuntime runtime(h.dependencies());
    auto source = runtime.attach([](auto) {}); source->configure(configuration());
    QTRY_COMPARE(h.sockets.size(), 1);
    auto consumer = runtime.subscribe();
    h.sockets[0]->deliver(welcome()); h.sockets[0]->deliver(eventAt(0));
    std::atomic<bool> done{false};
    std::jthread worker([&] { source->close(); runtime.shutdown(); done.store(true); });
    QTRY_VERIFY_WITH_TIMEOUT(done.load(), 3000);
    worker.join();
    QCOMPARE(consumer.takeBatch().state, ConsumerState::RuntimeStopped);
    // Deferred children and their timeout callbacks cannot survive module teardown.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(h.sockets[0].isNull());
    QVERIFY(h.network.findChildren<QNetworkReply *>().isEmpty());
}

void TwitchProducerTests::enrichmentPressureAndDeadline()
{
    Harness h; h.network.catalogDelay = 4000;
    EventDispatcher dispatcher; auto consumer = dispatcher.subscribe({}, {512, 64 * 1024 * 1024});
    OrderedEventPipeline pipeline(dispatcher, {}, &h.network); pipeline.setChannel(QStringLiteral("100"), 1);
    pipeline.ingest(eventAt(0));
    for (int i = 0; i < 200; ++i) pipeline.ingest(eventAt(4, QStringLiteral("pressure-%1").arg(i)));
    auto batch = consumer.takeBatch(512); QCOMPARE(batch.events.size(), size_t(201));
    QCOMPARE(std::get<ChatMessage>(batch.events[0]->payload).text, QStringLiteral("Hi 👋 Kappa Dance Hello Alias"));
    for (size_t i = 1; i < batch.events.size(); ++i) QVERIFY(batch.events[i]->header.sequence > batch.events[i - 1]->header.sequence);
    pipeline.stop(); pipeline.setChannel(QStringLiteral("100"), 2);
    pipeline.ingest(eventAt(0)); pipeline.ingest(eventAt(1));
    std::vector<EventPtr> delayed;
    QTRY_VERIFY_WITH_TIMEOUT((append(consumer, delayed), delayed.size() == 2), 3500);
    const auto &chat = std::get<ChatMessage>(delayed[0]->payload);
    QVERIFY(!chat.fragments[1].image); // Text and Twitch URL survive the deadline.
    QVERIFY(!chat.fragments[1].imageUrl.isEmpty());
    pipeline.stop(); QVERIFY(consumer.takeBatch().events.empty());
}

void TwitchProducerTests::gifUsesTheSameDispatcherEvent()
{
    Harness h; EventDispatcher dispatcher; auto consumer = dispatcher.subscribe();
    OrderedEventPipeline pipeline(dispatcher, {}, &h.network); pipeline.setChannel(QStringLiteral("100"), 1);
    auto root = QJsonDocument::fromJson(eventAt(0)).object(); auto payload = root["payload"].toObject(); auto event = payload["event"].toObject();
    event["message"] = QJsonObject{{"text", "GIF"}, {"fragments", QJsonArray{QJsonObject{{"type", "gif"}, {"text", "GIF"},
        {"gif", QJsonObject{{"url", "https://example.test/legacy.gif"}}}}}}};
    payload["event"] = event; root["payload"] = payload;
    pipeline.ingest(json(root)); std::vector<EventPtr> events;
    QTRY_VERIFY((append(consumer, events), events.size() == 1));
    const auto &message = std::get<ChatMessage>(events[0]->payload);
    QCOMPARE(message.media.size(), size_t(1)); QVERIFY(message.media[0].image);
    NativeEventAdapter adapter; adapter.accept({{events, ConsumerState::Active}, {}, {}, {}});
    QCOMPARE(adapter.gifs.size(), size_t(1)); QCOMPARE(adapter.messages.size(), size_t(1));
    pipeline.ingest(eventAt(1)); adapter.accept({consumer.takeBatch(), {}, {}, {}});
    QVERIFY(adapter.gifs.empty()); QVERIFY(adapter.messages.empty());
}

void TwitchProducerTests::changedSettingsReuseTheRuntime()
{
    Harness h; PluginRuntime runtime(h.dependencies());
    std::vector<EventPtr> received;
    int resets = 0;
    auto source = runtime.attach([&](BackendAttachment::Delivery delivery) {
        if (delivery.reset) ++resets;
        received.insert(received.end(), delivery.batch.events.begin(), delivery.batch.events.end());
    });
    source->configure(configuration());
    QTRY_COMPARE(h.sockets.size(), 1);
    auto oldSocket = h.sockets[0];
    oldSocket->deliver(welcome()); oldSocket->deliver(eventAt(4));
    auto changed = configuration(); changed.channel = QStringLiteral("new-channel"); source->configure(changed);
    QTRY_COMPARE(h.sockets.size(), 2);
    QVERIFY(oldSocket.isNull() || oldSocket->closed);
    QCOMPARE(h.network.validations, 2); QVERIFY(received.empty()); QCOMPARE(resets, 2);
    auto other = runtime.attach([](auto) {}); other->configure(changed);
    QTest::qWait(60); QCOMPARE(h.sockets.size(), 2);
    h.sockets[1]->deliver(welcome(QStringLiteral("new-settings")));
    h.sockets[1]->deliver(eventAt(4, QStringLiteral("new-generation")));
    QTRY_COMPARE(received.size(), size_t(1));
    QCOMPARE(received[0]->header.eventId, QStringLiteral("new-generation"));
    runtime.shutdown();
}

void TwitchProducerTests::slowSourceResubscribesWithoutASecondConnection()
{
    Harness h; PluginRuntime runtime(h.dependencies());
    int overflows = 0; std::vector<EventPtr> received;
    auto source = runtime.attach([&](BackendAttachment::Delivery delivery) {
        if (delivery.batch.state == ConsumerState::Overflowed) ++overflows;
        received.insert(received.end(), delivery.batch.events.begin(), delivery.batch.events.end());
    });
    source->configure(configuration()); QTRY_COMPARE(h.sockets.size(), 1);
    h.sockets[0]->deliver(welcome());
    auto largeConsumer = runtime.subscribe({}, {512, 64 * 1024 * 1024});
    for (int i = 0; i < 300; ++i) h.sockets[0]->deliver(eventAt(4, QStringLiteral("overflow-%1").arg(i)));
    QTRY_COMPARE(overflows, 1); QVERIFY(received.empty());
    QCOMPARE(largeConsumer.takeBatch(512).events.size(), size_t(300));
    h.sockets[0]->deliver(eventAt(4, QStringLiteral("after-overflow")));
    QTRY_COMPARE(received.size(), size_t(1));
    QCOMPARE(received.front()->header.eventId, QStringLiteral("after-overflow"));
    QCOMPARE(h.sockets.size(), 1); QCOMPARE(h.network.validations, 1);
    runtime.shutdown();
}

void TwitchProducerTests::delayedClearUsesServerTime()
{
    PluginEvent clear;
    clear.header.channelId = QStringLiteral("100");
    clear.header.timestamp = QDateTime::fromString(QStringLiteral("2026-09-26T12:00:00Z"), Qt::ISODate);
    clear.header.sequence = 10; clear.header.generation = 2;
    clear.payload = ChatCleared{};
    MessageIdentity message{clear.header, QStringLiteral("message"), QStringLiteral("user")};
    message.header.timestamp = clear.header.timestamp.addSecs(1);
    message.header.sequence = 9; // Newer message arrived before a delayed clear.
    QVERIFY(!NativeEventAdapter::removes(clear, message));
    message.header.timestamp = clear.header.timestamp.addSecs(-1);
    message.header.sequence = 11; // Late old message must still be removed.
    QVERIFY(NativeEventAdapter::removes(clear, message));
    message.header.generation = 1; // Visible message retained across reauthorization.
    QVERIFY(NativeEventAdapter::removes(clear, message));
    clear.payload = ChatCleared{ChatUser{QStringLiteral("other"), {}, {}, {}}};
    QVERIFY(!NativeEventAdapter::removes(clear, message));
}

void TwitchProducerTests::pendingReturnIsBoundedAndInvalidated()
{
    NativeEventAdapter adapter;
    std::deque<PreparedMessage> oldMessages(128);
    std::deque<PreparedGif> oldGifs(30);
    oldMessages.front().identity.messageId = QStringLiteral("oldest");
    adapter.messages.resize(128); adapter.gifs.resize(30); // Arrived after the OBS drain.
    adapter.returnPending(std::move(oldMessages), std::move(oldGifs), adapter.queueRevision);
    QCOMPARE(adapter.messages.size(), size_t(128)); QCOMPARE(adapter.gifs.size(), size_t(30));
    QCOMPARE(adapter.messages.front().identity.messageId, QStringLiteral("oldest"));
    const auto revision = adapter.queueRevision;
    oldMessages.swap(adapter.messages); oldGifs.swap(adapter.gifs);
    BackendAttachment::Delivery reset; reset.reset = true;
    adapter.accept(std::move(reset));
    adapter.returnPending(std::move(oldMessages), std::move(oldGifs), revision);
    QVERIFY(adapter.messages.empty()); QVERIFY(adapter.gifs.empty());

    PreparedMessage deleted; deleted.identity.messageId = QStringLiteral("deleted");
    deleted.identity.header.channelId = QStringLiteral("100");
    PreparedGif gif; gif.identity = deleted.identity;
    PluginEvent control; control.header.channelId = QStringLiteral("100");
    control.payload = MessageDeleted{QStringLiteral("deleted"), {}};
    BackendAttachment::Delivery delivery;
    delivery.batch.events.push_back(std::make_shared<const PluginEvent>(control));
    adapter.accept(std::move(delivery));
    adapter.returnPending({deleted}, {gif}, adapter.queueRevision);
    QVERIFY(adapter.messages.empty()); QVERIFY(adapter.gifs.empty());
    adapter.close();
    adapter.returnPending({PreparedMessage{}}, {PreparedGif{}}, adapter.queueRevision);
    QVERIFY(adapter.messages.empty()); QVERIFY(adapter.gifs.empty());
}

void TwitchProducerTests::closeReleasesCallbackOutsideLock()
{
    Harness h; PluginRuntime runtime(h.dependencies());
    std::shared_ptr<BackendAttachment> attachment;
    bool destroyed = false;
    auto owner = std::shared_ptr<int>(new int(0), [&](int *value) {
        QVERIFY(!attachment->status().isEmpty()); // Reenters the attachment mutex.
        destroyed = true; delete value;
    });
    attachment = runtime.attach([owner](auto) {});
    owner.reset();
    attachment->close();
    QVERIFY(destroyed);
    runtime.shutdown();
}

void TwitchProducerTests::runtimeLogCanReadAttachmentStatus()
{
    Harness h;
    std::shared_ptr<BackendAttachment> source;
    int statusReads = 0;
    PluginRuntime runtime(h.dependencies(), [&](QString) {
        if (source) { QVERIFY(!source->status().isEmpty()); ++statusReads; }
    });
    source = runtime.attach([](auto) {});
    source->configure(configuration());
    QTRY_COMPARE(h.sockets.size(), 1);
    QVERIFY(statusReads > 0);
    source->connect();
    QTRY_COMPARE(h.network.deviceRequests, 1);
    runtime.shutdown();
}

QTEST_MAIN(TwitchProducerTests)

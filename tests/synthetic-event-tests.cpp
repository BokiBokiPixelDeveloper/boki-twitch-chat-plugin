#include "core/synthetic-event-producer.hpp"
#include "core/event-validation.hpp"

#include <QTest>

class SyntheticEventTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void disabledByDefaultAndGateCannotBeBypassed()
    {
        EventDispatcher dispatcher;
        SyntheticEventProducer producer(dispatcher);
        auto consumer = dispatcher.subscribe();
        QVERIFY(!producer.isEnabled());
        QCOMPARE(producer.inject(SyntheticEventKind::ChatMessage), IngestResult::Stopped);
        QVERIFY(consumer.takeBatch().events.empty());
        producer.setEnabled(true);
        QCOMPARE(producer.inject(SyntheticEventKind::ChatMessage), IngestResult::Accepted);
        const auto batch = consumer.takeBatch();
        QCOMPARE(batch.events.size(), size_t(1));
        QCOMPARE(batch.events.front()->header.origin, EventOrigin::SyntheticTest);
        producer.setEnabled(false);
        QCOMPARE(producer.inject(SyntheticEventKind::Follow), IngestResult::Stopped);
    }

    void everyKindUsesValidatedPipeline_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("expected");
        const std::pair<const char *, SyntheticEventKind> kinds[] = {
            {"chat", SyntheticEventKind::ChatMessage}, {"delete", SyntheticEventKind::MessageDeleted},
            {"clear", SyntheticEventKind::ChatCleared}, {"follow", SyntheticEventKind::Follow},
            {"subscription", SyntheticEventKind::Subscription}, {"resubscription", SyntheticEventKind::Resubscription},
            {"gift", SyntheticEventKind::GiftSubscription}, {"community-gift", SyntheticEventKind::CommunityGiftSubscription},
            {"cheer", SyntheticEventKind::Cheer}, {"raid", SyntheticEventKind::Raid}};
        for (const auto &[name, kind] : kinds)
            QTest::newRow(name) << int(kind) << int(kind);
    }

    void everyKindUsesValidatedPipeline()
    {
        QFETCH(int, kind);
        QFETCH(int, expected);
        EventDispatcher dispatcher;
        SyntheticEventProducer producer(dispatcher);
        auto consumer = dispatcher.subscribe();
        producer.setEnabled(true);
        QCOMPARE(producer.inject(SyntheticEventKind(kind)), IngestResult::Accepted);
        const auto batch = consumer.takeBatch();
        QCOMPARE(batch.events.size(), size_t(1));
        const auto event = batch.events.front();
        QCOMPARE(int(eventKind(event->payload)), expected);
        QCOMPARE(event->header.origin, EventOrigin::SyntheticTest);
        auto copy = *event;
        QCOMPARE(validateEvent(copy).disposition, ValidationDisposition::Accepted);
    }

    void oneInjectionFansOutOnce()
    {
        EventDispatcher dispatcher;
        SyntheticEventProducer producer(dispatcher);
        auto first = dispatcher.subscribe();
        auto second = dispatcher.subscribe();
        producer.setEnabled(true);
        QCOMPARE(producer.inject(SyntheticEventKind::Subscription), IngestResult::Accepted);
        const auto firstBatch = first.takeBatch();
        const auto secondBatch = second.takeBatch();
        QCOMPARE(firstBatch.events.size(), size_t(1));
        QCOMPARE(secondBatch.events.size(), size_t(1));
        QCOMPARE(firstBatch.events.front(), secondBatch.events.front());
    }

    void maliciousLookingTextRemainsSemantic()
    {
        EventDispatcher dispatcher;
        SyntheticEventProducer producer(dispatcher);
        auto consumer = dispatcher.subscribe();
        producer.setEnabled(true);
        SyntheticEventValues values;
        values.chatText = QStringLiteral("<script>alert(1)</script><img src=x onerror=alert(1)>&quot; 😀");
        QCOMPARE(producer.inject(SyntheticEventKind::ChatMessage, values), IngestResult::Accepted);
        const auto batch = consumer.takeBatch();
        QCOMPARE(batch.events.size(), size_t(1));
        const auto &message = std::get<ChatMessage>(batch.events.front()->payload);
        QCOMPARE(message.text, values.chatText);
        QCOMPARE(message.fragments.size(), size_t(1));
        QCOMPARE(message.fragments.front().text, values.chatText);
    }
};

QTEST_GUILESS_MAIN(SyntheticEventTests)
#include "synthetic-event-tests.moc"

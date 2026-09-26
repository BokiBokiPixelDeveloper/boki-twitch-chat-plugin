#include "core/event-dispatcher.hpp"

#include <QTest>
#include <atomic>
#include <barrier>
#include <thread>
#include <type_traits>

namespace {
EventPtr makeEvent(std::uint64_t sequence = 1, EventPayload payload = ChatMessage{}, QString channel = QStringLiteral("channel"))
{
    PluginEvent event;
    event.header.sequence = sequence;
    event.header.channelId = std::move(channel);
    event.payload = std::move(payload);
    return std::make_shared<const PluginEvent>(std::move(event));
}
} // namespace

class EventDispatcherTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void allPayloadsReachIndependentConsumers()
    {
        static_assert(!std::is_copy_constructible_v<EventSubscription>);
        static_assert(std::is_nothrow_move_constructible_v<EventSubscription>);
        EventDispatcher dispatcher;
        auto first = dispatcher.subscribe();
        auto second = dispatcher.subscribe();
        auto third = dispatcher.subscribe();
        const std::vector<EventPayload> payloads{ChatMessage{}, MessageDeleted{}, ChatCleared{}, Follow{},
            Subscription{}, Resubscription{}, GiftSubscription{}, CommunityGiftSubscription{}, Cheer{}, Raid{}};
        const std::vector<EventKind> kinds{EventKind::ChatMessage, EventKind::MessageDeleted, EventKind::ChatCleared,
            EventKind::Follow, EventKind::Subscription, EventKind::Resubscription, EventKind::GiftSubscription,
            EventKind::CommunityGiftSubscription, EventKind::Cheer, EventKind::Raid};
        for (size_t i = 0; i < payloads.size(); ++i)
            QCOMPARE(dispatcher.publish(makeEvent(i, payloads[i])), PublishResult::Published);
        const auto a = first.takeBatch();
        QCOMPARE(a.events.size(), payloads.size());
        QCOMPARE(first.takeBatch().events.size(), size_t(0));
        const auto b = second.takeBatch();
        const auto c = third.takeBatch();
        QCOMPARE(b.events.size(), a.events.size());
        QCOMPARE(c.events.size(), a.events.size());
        for (size_t i = 0; i < a.events.size(); ++i) {
            QCOMPARE(eventKind(a.events[i]->payload), kinds[i]);
            QCOMPARE(a.events[i], b.events[i]);
            QCOMPARE(a.events[i], c.events[i]);
        }
    }

    void closeDestructionAndMoves()
    {
        EventDispatcher dispatcher;
        auto surviving = dispatcher.subscribe();
        EventSubscription empty;
        QCOMPARE(empty.takeBatch().state, ConsumerState::Closed);
        empty.close();
        std::weak_ptr<const PluginEvent> pending;
        {
            auto original = dispatcher.subscribe();
            auto event = makeEvent();
            pending = event;
            QCOMPARE(dispatcher.publish(event), PublishResult::Published);
            auto moved = std::move(original);
            QCOMPARE(original.takeBatch().state, ConsumerState::Closed);
            auto replaced = dispatcher.subscribe();
            replaced = std::move(moved);
            QCOMPARE(replaced.takeBatch().events.size(), size_t(1));
            replaced.close();
            replaced.close();
            QCOMPARE(dispatcher.publish(makeEvent(2)), PublishResult::Published);
            QCOMPARE(replaced.takeBatch().state, ConsumerState::Closed);
            QVERIFY(replaced.takeBatch().events.empty());
        }
        QVERIFY(!pending.expired()); // The other consumer still owns its pending event.
        auto batch = surviving.takeBatch();
        QCOMPARE(batch.events.size(), size_t(2));
        surviving.close();
        QCOMPARE(batch.events[0]->header.sequence, std::uint64_t(1));
        batch.events.clear();
        QVERIFY(pending.expired());
        QCOMPARE(dispatcher.publish(makeEvent(3)), PublishResult::Published);
    }

    void filteringOrderingAndNoReplay()
    {
        EventDispatcher dispatcher;
        QCOMPARE(dispatcher.publish(makeEvent(100)), PublishResult::Published);
        auto filtered = dispatcher.subscribe({QStringLiteral("channel"), {EventKind::Follow}});
        auto all = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(makeEvent(30)), PublishResult::Published);
        QCOMPARE(dispatcher.publish(makeEvent(20, Follow{})), PublishResult::Published);
        QCOMPARE(dispatcher.publish(makeEvent(10, Follow{}, QStringLiteral("other"))), PublishResult::Published);
        QVERIFY(all.takeBatch(0).events.empty());
        const auto first = all.takeBatch(2);
        QCOMPARE(first.events.size(), size_t(2));
        QCOMPARE(first.events[0]->header.sequence, std::uint64_t(30));
        QCOMPARE(first.events[1]->header.sequence, std::uint64_t(20));
        QCOMPARE(all.takeBatch().events.front()->header.sequence, std::uint64_t(10));
        const auto selected = filtered.takeBatch();
        QCOMPARE(selected.events.size(), size_t(1));
        QCOMPARE(selected.events.front()->header.sequence, std::uint64_t(20));
    }

    void overflowIsLocalAndTerminal()
    {
        EventDispatcher dispatcher;
        auto slow = dispatcher.subscribe({}, {1, 1024 * 1024});
        auto fast = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(makeEvent()), PublishResult::Published);
        QCOMPARE(fast.takeBatch().events.size(), size_t(1));
        QCOMPARE(dispatcher.publish(makeEvent(2)), PublishResult::Published);
        QCOMPARE(fast.takeBatch().events.size(), size_t(1));
        const auto overflow = slow.takeBatch();
        QCOMPARE(overflow.state, ConsumerState::Overflowed);
        QVERIFY(overflow.events.empty());
        slow.close();
        dispatcher.shutdown();
        QCOMPARE(slow.takeBatch().state, ConsumerState::Overflowed);
    }

    void byteBudgetIncludesAssetsAndReleasesOnDrain()
    {
        EventDispatcher dispatcher;
        auto decoded = std::make_shared<DecodedImage>();
        decoded->frames.emplace_back(64, 64, QImage::Format_RGBA8888);
        ChatMessage chat{QStringLiteral("Name"), QStringLiteral("Emote")};
        chat.fragments.emplace_back(ChatFragment::Type::Emote, chat.text);
        chat.fragments.back().image = decoded;
        const auto event = makeEvent(1, chat);
        const auto bytes = eventRetainedBytes(*event);
        QVERIFY(bytes >= static_cast<size_t>(decoded->frames.front().sizeInBytes()));
        auto fits = dispatcher.subscribe({}, {5, bytes});
        auto tooSmall = dispatcher.subscribe({}, {5, bytes - 1});
        QCOMPARE(dispatcher.publish(event), PublishResult::Published);
        QCOMPARE(fits.takeBatch().events.size(), size_t(1));
        QCOMPARE(tooSmall.takeBatch().state, ConsumerState::Overflowed);
        QCOMPARE(dispatcher.publish(event), PublishResult::Published);
        QCOMPARE(fits.takeBatch().events.size(), size_t(1));
        auto zero = dispatcher.subscribe({}, {0, 0});
        QCOMPARE(dispatcher.publish(event), PublishResult::Published);
        QCOMPARE(zero.takeBatch().state, ConsumerState::Overflowed);
    }

    void dispatcherCanDieBeforeSubscriptions()
    {
        EventSubscription subscription;
        EventBatch retained;
        std::weak_ptr<const PluginEvent> pending;
        {
            EventDispatcher dispatcher;
            subscription = dispatcher.subscribe();
            QCOMPARE(dispatcher.publish(makeEvent()), PublishResult::Published);
            retained = subscription.takeBatch();
            auto event = makeEvent(2);
            pending = event;
            QCOMPARE(dispatcher.publish(event), PublishResult::Published);
        }
        QVERIFY(pending.expired());
        QCOMPARE(subscription.takeBatch().state, ConsumerState::RuntimeStopped);
        QVERIFY(subscription.takeBatch().events.empty());
        QCOMPARE(retained.events.front()->header.sequence, std::uint64_t(1));
        subscription.close();
        QCOMPARE(subscription.takeBatch().state, ConsumerState::RuntimeStopped);
    }

    void shutdownRejectsNewWorkAndInvalidEvents()
    {
        EventDispatcher dispatcher;
        QCOMPARE(dispatcher.publish({}), PublishResult::InvalidEvent);
        auto consumer = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(makeEvent()), PublishResult::Published);
        dispatcher.shutdown();
        dispatcher.shutdown();
        QVERIFY(consumer.takeBatch().events.empty());
        QCOMPARE(consumer.takeBatch().state, ConsumerState::RuntimeStopped);
        auto late = dispatcher.subscribe();
        QCOMPARE(late.takeBatch().state, ConsumerState::RuntimeStopped);
        QCOMPARE(dispatcher.publish(makeEvent()), PublishResult::RuntimeStopped);
    }

    void eventDeletersRunOutsideLocks()
    {
        EventDispatcher dispatcher;
        auto consumer = dispatcher.subscribe();
        bool released = false;
        EventPtr event(new PluginEvent{}, [&](const PluginEvent *value) {
            delete value;
            auto reentrant = dispatcher.subscribe();
            reentrant.close();
            consumer.close();
            released = true;
        });
        QCOMPARE(dispatcher.publish(std::move(event)), PublishResult::Published);
        dispatcher.shutdown();
        QVERIFY(released);
    }

    void producerThreadContractIsEnforced()
    {
        EventDispatcher dispatcher;
        auto consumer = dispatcher.subscribe();
        QCOMPARE(dispatcher.publish(makeEvent()), PublishResult::Published);
        PublishResult result = PublishResult::Published;
        std::thread other([&] { result = dispatcher.publish(makeEvent(2)); });
        other.join();
        QCOMPARE(result, PublishResult::WrongProducerThread);
        QCOMPARE(consumer.takeBatch().events.size(), size_t(1));
    }

    void concurrentDeliveryRegistrationAndClose()
    {
        EventDispatcher dispatcher;
        auto first = dispatcher.subscribe({}, {4096, 32 * 1024 * 1024});
        auto closing = dispatcher.subscribe({}, {4096, 32 * 1024 * 1024});
        std::barrier start(3);
        std::atomic<bool> done = false;
        std::atomic<bool> publishFailed = false;
        std::thread producer([&] {
            start.arrive_and_wait();
            for (std::uint64_t i = 1; i <= 2000; ++i)
                if (dispatcher.publish(makeEvent(i)) != PublishResult::Published)
                    publishFailed = true;
            done = true;
        });
        std::thread registrar([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 300; ++i) {
                auto temporary = dispatcher.subscribe();
                if (i % 2 == 0) temporary.close();
                (void)temporary.takeBatch();
            }
            closing.close();
        });
        start.arrive_and_wait();
        std::vector<std::uint64_t> seen;
        while (!done.load()) {
            for (const auto &event : first.takeBatch().events)
                seen.push_back(event->header.sequence);
            (void)closing.takeBatch();
            std::this_thread::yield();
        }
        producer.join();
        registrar.join();
        for (const auto &event : first.takeBatch(4096).events)
            seen.push_back(event->header.sequence);
        QVERIFY(!publishFailed.load());
        QCOMPARE(seen.size(), size_t(2000));
        for (size_t i = 0; i < seen.size(); ++i)
            QCOMPARE(seen[i], static_cast<std::uint64_t>(i + 1));
        QCOMPARE(closing.takeBatch().state, ConsumerState::Closed);
        QVERIFY(closing.takeBatch().events.empty());
    }

    void shutdownRacesPublicationAndSubscription()
    {
        EventDispatcher dispatcher;
        auto consumer = dispatcher.subscribe({}, {4096, 32 * 1024 * 1024});
        std::barrier start(3);
        std::atomic<bool> invalidResult = false;
        std::thread producer([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 1000; ++i) {
                const auto result = dispatcher.publish(makeEvent());
                if (result != PublishResult::Published && result != PublishResult::RuntimeStopped)
                    invalidResult = true;
            }
        });
        std::thread registrar([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 300; ++i) {
                auto subscription = dispatcher.subscribe();
                (void)subscription.takeBatch();
            }
        });
        start.arrive_and_wait();
        dispatcher.shutdown();
        producer.join();
        registrar.join();
        QVERIFY(!invalidResult.load());
        QCOMPARE(consumer.takeBatch().state, ConsumerState::RuntimeStopped);
        QVERIFY(consumer.takeBatch().events.empty());
    }
};

QTEST_GUILESS_MAIN(EventDispatcherTests)
#include "event-dispatcher-tests.moc"

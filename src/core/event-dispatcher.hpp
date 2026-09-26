#pragma once

#include "core/event-types.hpp"

namespace event_detail { struct Mailbox; struct DispatcherState; }

struct EventFilter {
    QString channelId; // Empty: every channel.
    std::vector<EventKind> kinds; // Empty: every payload type.
};

struct QueueLimits {
    size_t maxEvents = 256;
    size_t maxBytes = 32 * 1024 * 1024;
};

enum class ConsumerState { Active, Closed, Overflowed, RuntimeStopped };
struct EventBatch {
    std::vector<EventPtr> events;
    ConsumerState state = ConsumerState::Active;
};

// No callbacks: consumers drain their own mailbox on their own thread.
// close/takeBatch may race. Moving/destroying a handle requires exclusive access
// to that C++ handle, as with other standard-library objects.
class EventSubscription {
public:
    EventSubscription() noexcept = default;
    EventSubscription(EventSubscription &&other) noexcept;
    EventSubscription &operator=(EventSubscription &&other) noexcept;
    EventSubscription(const EventSubscription &) = delete;
    EventSubscription &operator=(const EventSubscription &) = delete;
    ~EventSubscription();

    [[nodiscard]] EventBatch takeBatch(size_t limit = 32);
    void close() noexcept;

private:
    friend class EventDispatcher;
    explicit EventSubscription(std::shared_ptr<event_detail::Mailbox> mailbox);
    std::shared_ptr<event_detail::Mailbox> mailbox_;
};

enum class PublishResult { Published, RuntimeStopped, InvalidEvent, WrongProducerThread };

class EventDispatcher {
public:
    EventDispatcher();
    ~EventDispatcher();
    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;

    [[nodiscard]] EventSubscription subscribe(EventFilter filter = {}, QueueLimits limits = {});
    // First valid publish binds the producer thread. All matching consumers see
    // publication order; sequence/timestamp are producer data, never sorted here.
    // The producer must relinquish all mutable aliases before publishing.
    [[nodiscard]] PublishResult publish(EventPtr event);
    // Thread-safe, idempotent. Clears queued events; already drained values survive.
    void shutdown() noexcept;

private:
    std::unique_ptr<event_detail::DispatcherState> state_;
};

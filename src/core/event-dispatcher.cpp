#include "core/event-dispatcher.hpp"

#include <algorithm>
#include <list>
#include <mutex>
#include <thread>

namespace event_detail {
struct QueuedEvent { EventPtr event; size_t bytes = 0; };
struct Mailbox {
    std::mutex mutex;
    EventFilter filter;
    QueueLimits limits;
    ConsumerState state = ConsumerState::Active;
    std::list<QueuedEvent> events;
    size_t bytes = 0;
};
struct DispatcherState {
    std::mutex mutex;
    bool stopped = false;
    std::optional<std::thread::id> producer;
    std::vector<std::weak_ptr<Mailbox>> mailboxes;
};
} // namespace event_detail

namespace {
void clearMailbox(event_detail::Mailbox &mailbox, ConsumerState state,
                  std::list<event_detail::QueuedEvent> &retired)
{
    // Caller holds mailbox.mutex. Terminal reasons survive subsequent close/stop.
    if (mailbox.state != ConsumerState::Active)
        return;
    mailbox.state = state;
    retired.splice(retired.end(), mailbox.events);
    mailbox.bytes = 0;
}
} // namespace

EventSubscription::EventSubscription(std::shared_ptr<event_detail::Mailbox> mailbox)
    : mailbox_(std::move(mailbox)) {}

EventSubscription::EventSubscription(EventSubscription &&other) noexcept
    : mailbox_(std::move(other.mailbox_)) {}

EventSubscription &EventSubscription::operator=(EventSubscription &&other) noexcept
{
    if (this != &other) {
        close();
        mailbox_ = std::move(other.mailbox_);
    }
    return *this;
}

EventSubscription::~EventSubscription() { close(); }

void EventSubscription::close() noexcept
{
    if (!mailbox_)
        return;
    std::list<event_detail::QueuedEvent> retired;
    const std::lock_guard lock(mailbox_->mutex);
    clearMailbox(*mailbox_, ConsumerState::Closed, retired);
}

EventBatch EventSubscription::takeBatch(size_t limit)
{
    if (!mailbox_)
        return {{}, ConsumerState::Closed};
    const std::lock_guard lock(mailbox_->mutex);
    EventBatch batch{{}, mailbox_->state};
    const size_t count = std::min(limit, mailbox_->events.size());
    batch.events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto &entry = mailbox_->events.front();
        mailbox_->bytes -= entry.bytes;
        batch.events.push_back(std::move(entry.event));
        mailbox_->events.pop_front();
    }
    return batch;
}

EventDispatcher::EventDispatcher() : state_(std::make_unique<event_detail::DispatcherState>()) {}
EventDispatcher::~EventDispatcher() { shutdown(); }

EventSubscription EventDispatcher::subscribe(EventFilter filter, QueueLimits limits)
{
    auto mailbox = std::make_shared<event_detail::Mailbox>();
    mailbox->filter = std::move(filter);
    mailbox->limits = limits;
    const std::lock_guard lock(state_->mutex);
    if (state_->stopped) {
        mailbox->state = ConsumerState::RuntimeStopped;
    } else {
        std::erase_if(state_->mailboxes, [](const auto &entry) { return entry.expired(); });
        state_->mailboxes.push_back(mailbox);
    }
    return EventSubscription(std::move(mailbox));
}

PublishResult EventDispatcher::publish(EventPtr event)
{
    if (!event || event->payload.valueless_by_exception())
        return PublishResult::InvalidEvent;
    std::vector<std::shared_ptr<event_detail::Mailbox>> mailboxes;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->stopped)
            return PublishResult::RuntimeStopped;
        const auto thread = std::this_thread::get_id();
        if (state_->producer && *state_->producer != thread)
            return PublishResult::WrongProducerThread;
        state_->producer = thread;
        std::erase_if(state_->mailboxes, [](const auto &entry) { return entry.expired(); });
        for (const auto &entry : state_->mailboxes)
            if (auto mailbox = entry.lock())
                mailboxes.push_back(std::move(mailbox));
    }
    const auto kind = eventKind(event->payload);
    const size_t bytes = eventRetainedBytes(*event);
    std::list<event_detail::QueuedEvent> retired;
    for (const auto &mailbox : mailboxes) {
        const auto &filter = mailbox->filter; // Immutable after registration.
        if ((!filter.channelId.isEmpty() && filter.channelId != event->header.channelId) ||
            (!filter.kinds.empty() && std::find(filter.kinds.begin(), filter.kinds.end(), kind) == filter.kinds.end()))
            continue;
        const std::lock_guard lock(mailbox->mutex);
        if (mailbox->state != ConsumerState::Active)
            continue;
        if (mailbox->events.size() >= mailbox->limits.maxEvents ||
            bytes > mailbox->limits.maxBytes - mailbox->bytes) {
            clearMailbox(*mailbox, ConsumerState::Overflowed, retired);
            continue;
        }
        mailbox->events.push_back({event, bytes});
        mailbox->bytes += bytes;
    }
    return PublishResult::Published;
}

void EventDispatcher::shutdown() noexcept
{
    // Release event/asset owners only after all registry/mailbox locks are gone.
    std::list<event_detail::QueuedEvent> retired;
    const std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    for (const auto &entry : state_->mailboxes) {
        if (const auto mailbox = entry.lock()) {
            const std::lock_guard mailboxLock(mailbox->mutex);
            clearMailbox(*mailbox, ConsumerState::RuntimeStopped, retired);
        }
    }
    state_->mailboxes.clear();
}

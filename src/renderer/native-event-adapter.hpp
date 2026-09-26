#pragma once

#include "renderer/render-types.hpp"
#include "core/plugin-runtime.hpp"
#include <QMutex>
#include <atomic>
#include <deque>

struct NativeLayoutSettings {
    QString fontFamily;
    int fontWeight = 400;
    float outlineWidth = 2.5f;
    int minFont = 28, maxFont = 52;
    float minSpeed = 340, maxSpeed = 1000;
};

// Shared CPU-only consumer state. No OBS pointer or GPU resources are reachable
// from a network callback or queued runtime delivery.
class NativeEventAdapter {
public:
    void configure(NativeLayoutSettings settings);
    void accept(BackendAttachment::Delivery delivery);
    void enqueue(ChatMessage message, EventHeader header = {});
    void enqueueGif(DecodedGif image, MessageIdentity identity = {});
    void close();
    void returnPending(std::deque<PreparedMessage> pendingMessages, std::deque<PreparedGif> pendingGifs,
                       std::uint64_t revision);
    static bool removes(const PluginEvent &control, const MessageIdentity &identity);

    QMutex mutex;
    std::deque<PreparedMessage> messages;
    std::deque<PreparedGif> gifs;
    std::deque<EventPtr> controls;
    bool reset = false;
    std::uint64_t queueRevision = 0; // Protected by mutex; invalidates drained/in-flight work.
    std::atomic<int> activeCount{0};
private:
    bool active_ = true;
    NativeLayoutSettings settings_;
    std::deque<EventPtr> recentControls_;
    bool removed(const MessageIdentity &identity) const;
};

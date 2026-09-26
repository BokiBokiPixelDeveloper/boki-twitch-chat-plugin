#pragma once

#include "twitch/twitch-client.hpp"
#include "core/synthetic-event-producer.hpp"
#include <mutex>
#include <condition_variable>

// Source handles contain no QObject. Configuration and detach are safe on OBS threads.
class BackendAttachment {
public:
    struct Delivery {
        EventBatch batch;
        QString status;
        std::optional<TwitchTokens> tokens;
        TwitchConfiguration tokenSettings; // Expected saved settings before token replacement.
        bool reset = false;
    };
    using Consumer = std::function<void(Delivery)>;
    ~BackendAttachment();
    void configure(TwitchConfiguration configuration);
    void connect();
    void close();
    void setEventTestEnabled(bool enabled);
    void injectSyntheticEvent(SyntheticEventKind kind, SyntheticEventValues values = {});
    QString status() const;
private:
    friend class PluginRuntime;
    struct State;
    explicit BackendAttachment(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

// Plugin-owned facade. The service and all network work live on the Qt application
// thread. shutdown is a module lifecycle operation, never a render-path operation.
class PluginRuntime {
public:
    explicit PluginRuntime(TwitchClient::Dependencies dependencies = {}, TwitchClient::LogCallback log = {});
    ~PluginRuntime();
    std::shared_ptr<BackendAttachment> attach(BackendAttachment::Consumer consumer);
    EventSubscription subscribe(EventFilter filter = {}, QueueLimits limits = {});
    void shutdown();
private:
    struct Service;
    std::shared_ptr<EventDispatcher> dispatcher_;
    std::mutex gate_;
    Service *service_ = nullptr;
    QThread *ownerThread_ = nullptr;
    bool stopped_ = false;
    bool shutdownComplete_ = false;
    std::condition_variable shutdownDone_;
    QMetaObject::Connection quitConnection_;
};

#include "core/plugin-runtime.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QThread>
#include <QPointer>
#include <QTimer>

struct BackendAttachment::State {
    mutable std::mutex mutex;
    TwitchConfiguration configuration;
    Consumer consumer;
    EventSubscription subscription;
    QString status = QStringLiteral("Waiting for shared backend");
    bool active = true;
    bool accepted = false;
    bool connect = false;
    bool configured = false;
    bool eventTestEnabled = false;
    std::deque<std::pair<SyntheticEventKind, SyntheticEventValues>> syntheticRequests;
};
BackendAttachment::BackendAttachment(std::shared_ptr<State> state) : state_(std::move(state)) {}
BackendAttachment::~BackendAttachment() { close(); }
void BackendAttachment::configure(TwitchConfiguration configuration)
{
    configuration.clientId = configuration.clientId.trimmed();
    configuration.channel = configuration.channel.trimmed().toLower();
    std::lock_guard lock(state_->mutex);
    if (state_->configuration != configuration) {
        state_->configuration = std::move(configuration);
        state_->configured = true;
    }
}
void BackendAttachment::connect() { std::lock_guard lock(state_->mutex); state_->connect = true; }
void BackendAttachment::setEventTestEnabled(bool enabled)
{
    std::lock_guard lock(state_->mutex);
    state_->eventTestEnabled = enabled;
    if (!enabled) state_->syntheticRequests.clear();
}
void BackendAttachment::injectSyntheticEvent(SyntheticEventKind kind, SyntheticEventValues values)
{
    std::lock_guard lock(state_->mutex);
    if (state_->active && state_->eventTestEnabled)
        state_->syntheticRequests.emplace_back(kind, std::move(values));
}
void BackendAttachment::close()
{
    Consumer retired;
    EventSubscription retiredSubscription;
    {
        std::lock_guard lock(state_->mutex);
        state_->active = false;
        retired = std::move(state_->consumer);
        retiredSubscription = std::move(state_->subscription);
    } // Captured owners can reenter attachment methods during destruction.
}
QString BackendAttachment::status() const { std::lock_guard lock(state_->mutex); return state_->status; }

struct PluginRuntime::Service final : QObject {
    std::shared_ptr<EventDispatcher> dispatcher;
    std::vector<std::weak_ptr<BackendAttachment::State>> attachments;
    TwitchConfiguration current;
    QSet<QByteArray> knownTokens;
    std::unique_ptr<TwitchClient> client;
    std::unique_ptr<SyntheticEventProducer> synthetic;
    QTimer timer;
    bool selected = false;
    bool started = false;
    bool tokensChanged = false;

    static QByteArray hash(const QString &token) { return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256); }
    Service(std::shared_ptr<EventDispatcher> bus, TwitchClient::Dependencies dependencies, TwitchClient::LogCallback log)
        : dispatcher(std::move(bus))
    {
        client = std::make_unique<TwitchClient>(*dispatcher, log, [this](TwitchTokens tokens) {
            if (!current.accessToken.isEmpty()) knownTokens.insert(hash(current.accessToken));
            current.accessToken = tokens.accessToken;
            current.refreshToken = tokens.refreshToken;
            knownTokens.insert(hash(current.accessToken));
            tokensChanged = true;
        }, std::move(dependencies));
        synthetic = std::make_unique<SyntheticEventProducer>(*dispatcher, std::move(log));
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, this, [this] { pump(); });
        timer.start();
    }
    ~Service() override { timer.stop(); synthetic.reset(); client->stop(); client.reset(); }
    bool matches(const TwitchConfiguration &config) const
    {
        if (client->isLocalTestMode() || client->hasConnectionConfigurationError())
            return true;
        const bool channelMatches = config.channel == current.channel ||
            (config.channel.isEmpty() && current.channel == client->authenticatedLogin()) ||
            (current.channel.isEmpty() && config.channel == client->authenticatedLogin());
        return config.clientId == current.clientId && channelMatches &&
            (config.accessToken.isEmpty() || config.accessToken == current.accessToken || knownTokens.contains(hash(config.accessToken)));
    }
    void select(const TwitchConfiguration &config)
    {
        for (auto &weak : attachments) if (auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            state->subscription.close();
            state->accepted = false;
        }
        current = config;
        selected = true;
        started = false;
        knownTokens.clear();
        if (!current.accessToken.isEmpty()) knownTokens.insert(hash(current.accessToken));
        client->configure(current);
    }
    void pump()
    {
        std::erase_if(attachments, [](const auto &weak) { auto state = weak.lock(); if (!state) return true;
            std::lock_guard lock(state->mutex); return !state->active; });
        if (attachments.empty()) {
            if (selected) client->stop();
            selected = started = false;
            current = {};
            knownTokens.clear();
            return;
        }
        bool eventTestEnabled = false;
        std::vector<std::pair<SyntheticEventKind, SyntheticEventValues>> syntheticRequests;
        for (const auto &weak : attachments) if (auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            eventTestEnabled |= state->eventTestEnabled;
            while (!state->syntheticRequests.empty()) {
                syntheticRequests.push_back(std::move(state->syntheticRequests.front()));
                state->syntheticRequests.pop_front();
            }
        }
        synthetic->setEnabled(eventTestEnabled);
        for (auto &[kind, values] : syntheticRequests) (void)synthetic->inject(kind, values);
        // Selection happens before delivery so source creation order cannot start
        // a second client. A conflicting source can take over only when nobody
        // still requests the current account/channel.
        bool hasCompatible = false;
        if (selected) for (auto &weak : attachments) if (auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            hasCompatible |= matches(state->configuration);
        }
        std::optional<TwitchConfiguration> nextConfiguration;
        for (auto &weak : attachments) if (auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            if ((!selected || (!hasCompatible && (state->connect || state->configured || client->isLocalTestMode() ||
                                                   client->hasConnectionConfigurationError()))) &&
                (!state->configuration.clientId.isEmpty() || client->isLocalTestMode() || client->hasConnectionConfigurationError())) {
                nextConfiguration = state->configuration;
                break;
            }
        }
        if (nextConfiguration) select(*nextConfiguration);
        size_t accepted = 0;
        const auto consumers = attachments;
        bool startRequested = false;
        bool connectRequested = false;
        for (const auto &weak : consumers) if (auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            if (state->active && selected && matches(state->configuration)) {
                startRequested = !started;
                connectRequested |= std::exchange(state->connect, false);
            }
        }
        // Network setup and the log sink must not run with an attachment locked.
        // A log callback can legitimately read source status or close a handle.
        if (startRequested) { started = true; client->startOrResume(); }
        if (connectRequested) client->beginDeviceFlow();
        for (auto &weak : consumers) if (auto state = weak.lock()) {
            BackendAttachment::Consumer consumer;
            BackendAttachment::Delivery delivery;
            {
                std::lock_guard lock(state->mutex);
                if (!state->active) continue;
                const bool compatible = selected && matches(state->configuration);
                if (!compatible) {
                    delivery.reset = state->accepted;
                    state->subscription.close(); state->accepted = false;
                    state->status = selected ? QStringLiteral("Shared backend uses another channel or account; saved settings are unchanged")
                                             : QStringLiteral("Set a Twitch client ID and channel");
                    state->connect = false;
                    state->configured = false;
                } else {
                    ++accepted;
                    if (!state->accepted) {
                        state->subscription = dispatcher->subscribe();
                        state->accepted = true;
                        delivery.reset = true;
                    }
                    state->status = client->status();
                    delivery.batch = state->subscription.takeBatch(64);
                    if (delivery.batch.state == ConsumerState::Overflowed) {
                        state->subscription = dispatcher->subscribe();
                        state->status = QStringLiteral("Native consumer fell behind; resuming with new events");
                    }
                    if (!current.accessToken.isEmpty() && (tokensChanged || state->configured) &&
                        (state->configuration.accessToken != current.accessToken || state->configuration.refreshToken != current.refreshToken)) {
                        delivery.tokenSettings = state->configuration;
                        delivery.tokens = TwitchTokens{current.accessToken, current.refreshToken};
                        state->configuration.accessToken = current.accessToken;
                        state->configuration.refreshToken = current.refreshToken;
                    }
                    state->configured = false;
                }
                delivery.status = state->status;
                consumer = state->consumer;
            }
            const QPointer<Service> guard(this);
            if (consumer) consumer(std::move(delivery));
            if (!guard) return;
        }
        tokensChanged = false;
        if (!accepted && started) { client->stop(); started = false; }
    }
};

PluginRuntime::PluginRuntime(TwitchClient::Dependencies dependencies, TwitchClient::LogCallback log)
    : dispatcher_(std::make_shared<EventDispatcher>())
{
    auto *app = QCoreApplication::instance();
    Q_ASSERT(app);
    ownerThread_ = app->thread();
    auto create = [this, dependencies = std::move(dependencies), log = std::move(log)]() mutable {
        service_ = new Service(dispatcher_, std::move(dependencies), std::move(log));
        // Register before returning to the application event loop.
        auto *app = QCoreApplication::instance();
        quitConnection_ = QObject::connect(app, &QCoreApplication::aboutToQuit, app, [this] { shutdown(); }, Qt::DirectConnection);
    };
    if (QThread::currentThread() == app->thread()) create();
    else QMetaObject::invokeMethod(app, std::move(create), Qt::BlockingQueuedConnection);
}
PluginRuntime::~PluginRuntime() { shutdown(); QObject::disconnect(quitConnection_); }
std::shared_ptr<BackendAttachment> PluginRuntime::attach(BackendAttachment::Consumer consumer)
{
    auto state = std::make_shared<BackendAttachment::State>();
    state->consumer = std::move(consumer);
    std::lock_guard lock(gate_);
    if (stopped_) { state->active = false; state->status = QStringLiteral("Runtime stopped"); }
    else QMetaObject::invokeMethod(service_, [service = service_, weak = std::weak_ptr(state)] { service->attachments.push_back(weak); }, Qt::QueuedConnection);
    return std::shared_ptr<BackendAttachment>(new BackendAttachment(std::move(state)));
}
EventSubscription PluginRuntime::subscribe(EventFilter filter, QueueLimits limits) { return dispatcher_->subscribe(std::move(filter), limits); }
void PluginRuntime::shutdown()
{
    std::unique_lock lock(gate_);
    if (shutdownComplete_) return;
    if (QThread::currentThread() != ownerThread_) {
        if (!stopped_) {
            stopped_ = true;
            // The service owns this queued command. If aboutToQuit destroys it
            // first, Qt cancels the command and the condition still wakes us.
            QMetaObject::invokeMethod(service_, [this] { shutdown(); }, Qt::QueuedConnection);
        }
        shutdownDone_.wait(lock, [this] { return shutdownComplete_; });
        return;
    }
    if (!service_) return; // Reentrant owner-thread shutdown during destruction.
    stopped_ = true;
    auto *service = std::exchange(service_, nullptr);
    lock.unlock();
    delete service;
    dispatcher_->shutdown();
    lock.lock();
    shutdownComplete_ = true;
    shutdownDone_.notify_all();
}

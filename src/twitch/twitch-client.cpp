#include "twitch/twitch-client.hpp"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QThread>
#include <QUrlQuery>
#include <algorithm>

namespace {
constexpr auto requestedScopes = "user:read:chat moderator:read:followers bits:read";
}

TwitchClient::TwitchClient(EventDispatcher &dispatcher, LogCallback log, TokenCallback tokens, Dependencies dependencies)
    : transport_(dependencies.network ? dependencies.network : &network_), dependencies_(std::move(dependencies)),
      log_(std::move(log)), tokens_(std::move(tokens)), pipeline_(dispatcher, log_, dependencies_.network)
{
    if (!dependencies_.socketFactory) dependencies_.socketFactory = makeEventSubSocket;
    if (!dependencies_.openBrowser) dependencies_.openBrowser = [](const QUrl &url) { QDesktopServices::openUrl(url); };
    QObject::connect(&devicePollTimer_, &QTimer::timeout, this, [this] { pollDeviceToken(); });
    reconnectTimer_.setSingleShot(true);
    QObject::connect(&reconnectTimer_, &QTimer::timeout, this, [this] { if (running_) connectEventSub(); });
    watchdog_.setSingleShot(true);
    QObject::connect(&watchdog_, &QTimer::timeout, this, [this] { scheduleReconnect(); });
    handoffTimer_.setSingleShot(true);
    QObject::connect(&handoffTimer_, &QTimer::timeout, this, [this] { scheduleReconnect(); });
    validationTimer_.setInterval(45 * 60 * 1000);
    QObject::connect(&validationTimer_, &QTimer::timeout, this, [this] {
        if (running_ && !authenticating_) { authenticating_ = true; refreshAttempted_ = false; validateToken(); }
    });
}

TwitchClient::~TwitchClient() { stop(); }

void TwitchClient::setStatus(QString status, bool logStatus)
{
    if (statusText_ == status) return;
    statusText_ = std::move(status);
    if (logStatus && log_) log_(statusText_);
}

void TwitchClient::failAuthentication(QString status)
{
    stop();
    setStatus(std::move(status));
}

void TwitchClient::configure(TwitchConfiguration configuration)
{
    Q_ASSERT(QThread::currentThread() == thread());
    configuration.channel = configuration.channel.trimmed().toLower();
    if (configuration_ == configuration) return;
    stop();
    configuration_ = std::move(configuration);
}

void TwitchClient::closeSockets()
{
    for (auto *socket : {socket_.get(), replacement_.get()}) {
        if (socket) {
            QObject::disconnect(socket, nullptr, this, nullptr);
            socket->close();
        }
    }
    // A socket may currently be emitting the signal that requested this close.
    if (socket_) socket_.release()->deleteLater();
    if (replacement_) replacement_.release()->deleteLater();
    sessionId_.clear();
    ++sessionGeneration_;
}

void TwitchClient::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    running_ = false;
    ++generation_;
    authenticating_ = deviceRequestPending_ = refreshing_ = refreshAttempted_ = false;
    devicePollTimer_.stop(); reconnectTimer_.stop(); watchdog_.stop();
    validationTimer_.stop(); handoffTimer_.stop();
    const auto replies = std::exchange(replies_, {});
    for (auto *reply : replies) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort(); reply->deleteLater();
    }
    closeSockets();
    pipeline_.stop();
    subscriptions_.clear(); scopes_.clear(); deviceCode_.clear();
    userId_.clear(); userLogin_.clear(); broadcasterId_.clear();
    setStatus(QStringLiteral("Disconnected"));
}

void TwitchClient::finish(QNetworkReply *reply, ReplyCallback callback)
{
    reply->setParent(this); // Cancel deferred reply/timer work with this owner.
    replies_.insert(reply);
    const auto generation = generation_;
    reply->setReadBufferSize(1024 * 1024);
    QTimer::singleShot(10000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, generation, callback = std::move(callback)] {
        replies_.remove(reply);
        const auto bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (generation != generation_ || !running_) return;
        const auto json = bytes.size() <= 1024 * 1024 ? QJsonDocument::fromJson(bytes).object() : QJsonObject{};
        callback(status, json);
    });
}

QNetworkRequest TwitchClient::apiRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + configuration_.accessToken.toUtf8());
    request.setRawHeader("Client-Id", configuration_.clientId.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return request;
}

QNetworkReply *TwitchClient::postForm(const QUrl &url, const QList<QPair<QString, QString>> &fields)
{
    QByteArray encoded;
    for (const auto &field : fields) {
        if (!encoded.isEmpty()) encoded += '&';
        encoded += QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    return transport_->post(request, encoded);
}

void TwitchClient::startOrResume()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (running_) return;
    if (configuration_.clientId.isEmpty()) { setStatus(QStringLiteral("Client ID is missing")); return; }
    if (configuration_.accessToken.isEmpty()) { setStatus(QStringLiteral("Not authenticated - click Connect to Twitch")); return; }
    running_ = authenticating_ = true;
    setStatus(QStringLiteral("Validating Twitch login"));
    validateToken();
}

void TwitchClient::beginDeviceFlow()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!deviceCode_.isEmpty() || deviceRequestPending_) return;
    if (configuration_.clientId.isEmpty()) { setStatus(QStringLiteral("Client ID is missing")); return; }
    stop(); running_ = true; deviceRequestPending_ = true;
    setStatus(QStringLiteral("Starting Twitch authorization"));
    finish(postForm(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/device")), {
        {QStringLiteral("client_id"), configuration_.clientId}, {QStringLiteral("scopes"), QString::fromLatin1(requestedScopes)}}),
        [this](int status, const QJsonObject &json) {
        deviceRequestPending_ = false;
        if (status < 200 || status >= 300) { setStatus(QStringLiteral("Twitch device authorization failed")); return; }
        deviceCode_ = json.value(QStringLiteral("device_code")).toString();
        const QUrl url(json.value(QStringLiteral("verification_uri")).toString());
        if (deviceCode_.isEmpty() || url.isEmpty() || !url.isValid()) {
            deviceCode_.clear(); setStatus(QStringLiteral("Invalid Twitch device authorization response")); return;
        }
        deviceDeadline_ = QDeadlineTimer(std::clamp(json.value(QStringLiteral("expires_in")).toInt(600), 1, 3600) * 1000);
        pollIntervalMs_ = std::clamp(json.value(QStringLiteral("interval")).toInt(5), 1, 60) * 1000;
        // The code is UI-only, never sent to the developer log sink.
        setStatus(QStringLiteral("Authorize Twitch - code %1").arg(json.value(QStringLiteral("user_code")).toString()), false);
        dependencies_.openBrowser(url);
        devicePollTimer_.start(pollIntervalMs_);
        pollDeviceToken();
    });
}

void TwitchClient::pollDeviceToken()
{
    if (deviceCode_.isEmpty() || deviceRequestPending_) return;
    if (deviceDeadline_.hasExpired()) { failAuthentication(QStringLiteral("Twitch authorization expired - reconnect")); return; }
    deviceRequestPending_ = true;
    finish(postForm(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")), {
        {QStringLiteral("client_id"), configuration_.clientId}, {QStringLiteral("scopes"), QString::fromLatin1(requestedScopes)},
        {QStringLiteral("device_code"), deviceCode_},
        {QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")}}),
        [this](int status, const QJsonObject &json) {
        deviceRequestPending_ = false;
        if (status >= 200 && status < 300) {
            devicePollTimer_.stop(); deviceCode_.clear(); finishAuth(json); return;
        }
        const auto error = json.value(QStringLiteral("message")).toString(json.value(QStringLiteral("error")).toString());
        if (error == QStringLiteral("authorization_pending")) return;
        if (error == QStringLiteral("slow_down")) {
            pollIntervalMs_ = std::min(pollIntervalMs_ + 5000, 60000);
            devicePollTimer_.setInterval(pollIntervalMs_); return;
        }
        devicePollTimer_.stop(); deviceCode_.clear();
        setStatus(QStringLiteral("Twitch device token request failed - reconnect"));
    });
}

void TwitchClient::finishAuth(const QJsonObject &json)
{
    const auto access = json.value(QStringLiteral("access_token")).toString();
    if (access.isEmpty()) { authenticating_ = false; setStatus(QStringLiteral("Twitch returned no access token")); return; }
    configuration_.accessToken = access;
    const auto refresh = json.value(QStringLiteral("refresh_token")).toString();
    if (!refresh.isEmpty()) configuration_.refreshToken = refresh;
    if (tokens_) tokens_({configuration_.accessToken, configuration_.refreshToken});
    authenticating_ = true;
    validateToken();
}

void TwitchClient::validateToken()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/validate")));
    request.setTransferTimeout(10000);
    request.setRawHeader("Authorization", QByteArray("OAuth ") + configuration_.accessToken.toUtf8());
    finish(transport_->get(request), [this](int status, const QJsonObject &json) {
        if (status == 401) { refreshAccessToken(); return; }
        authenticating_ = false;
        if (status != 200) { failAuthentication(QStringLiteral("Twitch token validation failed - reconnect")); return; }
        if (json.value(QStringLiteral("client_id")).toString() != configuration_.clientId) {
            failAuthentication(QStringLiteral("Twitch token belongs to another client ID")); return;
        }
        const auto id = json.value(QStringLiteral("user_id")).toString();
        if (id.isEmpty()) { failAuthentication(QStringLiteral("Twitch token has no user ID")); return; }
        refreshAttempted_ = false;
        userId_ = id; userLogin_ = json.value(QStringLiteral("login")).toString();
        scopes_.clear();
        for (const auto &scope : json.value(QStringLiteral("scopes")).toArray()) scopes_.insert(scope.toString());
        validationTimer_.start();
        if (configuration_.channel.isEmpty()) configuration_.channel = userLogin_;
        if (!socket_) resolveBroadcaster();
    });
}

void TwitchClient::refreshAccessToken()
{
    if (refreshing_) return;
    if (configuration_.refreshToken.isEmpty() || refreshAttempted_) {
        failAuthentication(QStringLiteral("Twitch login expired - reconnect")); return;
    }
    refreshing_ = refreshAttempted_ = true;
    finish(postForm(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")), {
        {QStringLiteral("client_id"), configuration_.clientId}, {QStringLiteral("refresh_token"), configuration_.refreshToken},
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")}}), [this](int status, const QJsonObject &json) {
        refreshing_ = false;
        if (status < 200 || status >= 300) {
            failAuthentication(QStringLiteral("Twitch token refresh failed - reconnect")); return;
        }
        finishAuth(json);
    });
}

void TwitchClient::resolveBroadcaster()
{
    QUrl url(QStringLiteral("https://api.twitch.tv/helix/users"));
    QUrlQuery query; query.addQueryItem(QStringLiteral("login"), configuration_.channel); url.setQuery(query);
    finish(transport_->get(apiRequest(url)), [this](int status, const QJsonObject &json) {
        const auto data = json.value(QStringLiteral("data")).toArray();
        if (status != 200 || data.isEmpty()) { setStatus(QStringLiteral("Twitch channel lookup failed")); return; }
        broadcasterId_ = data.first().toObject().value(QStringLiteral("id")).toString();
        if (broadcasterId_.isEmpty()) { setStatus(QStringLiteral("Twitch channel ID is missing")); return; }
        pipeline_.setChannel(broadcasterId_, generation_);
        connectEventSub();
    });
}

void TwitchClient::connectEventSub(const QUrl &url, bool handoff)
{
    if (!running_) return;
    if (!handoff) closeSockets();
    auto socket = dependencies_.socketFactory();
    auto *sender = socket.get();
    socket->setParent(this); // Parent owns any sockets awaiting deferred deletion.
    QObject::connect(sender, &EventSubSocket::messageReceived, this, [this, sender](const QString &payload) {
        handleEventSubMessage(sender, payload);
    });
    QObject::connect(sender, &EventSubSocket::connectionLost, this, [this, sender] {
        if (sender == socket_.get() && replacement_) return;
        scheduleReconnect();
    });
    if (handoff) { replacement_ = std::move(socket); handoffTimer_.start(30000); }
    else { socket_ = std::move(socket); watchdog_.start(10000); }
    setStatus(QStringLiteral("Connecting to EventSub"));
    sender->open(url);
}

void TwitchClient::scheduleReconnect()
{
    if (!running_ || reconnectTimer_.isActive()) return;
    watchdog_.stop(); handoffTimer_.stop(); closeSockets();
    setStatus(QStringLiteral("EventSub disconnected - retrying"));
    reconnectTimer_.start(retryMs_);
    retryMs_ = std::min(retryMs_ * 2, 30000);
}

void TwitchClient::handleEventSubMessage(EventSubSocket *sender, const QString &payload)
{
    if (!running_ || (sender != socket_.get() && sender != replacement_.get())) return;
    const auto bytes = payload.toUtf8();
    if (bytes.size() > 1024 * 1024) return;
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto metadata = root.value(QStringLiteral("metadata")).toObject();
    const auto type = metadata.value(QStringLiteral("message_type")).toString();
    const auto data = root.value(QStringLiteral("payload")).toObject();
    if (type == QStringLiteral("session_welcome")) {
        const auto session = data.value(QStringLiteral("session")).toObject();
        const auto id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) return;
        keepaliveMs_ = (std::clamp(session.value(QStringLiteral("keepalive_timeout_seconds")).toInt(10), 10, 600) + 1) * 1000;
        watchdog_.start(keepaliveMs_); retryMs_ = 1000;
        if (sender == replacement_.get()) {
            auto old = std::move(socket_);
            socket_ = std::move(replacement_);
            QObject::disconnect(old.get(), nullptr, this, nullptr);
            old->close(); old.release()->deleteLater();
            handoffTimer_.stop(); sessionId_ = id;
            updateConnectionStatus(); // Subscriptions migrate: do not POST again.
        } else if (sessionId_ != id) {
            sessionId_ = id; subscriptions_.clear(); subscribeEvents();
        }
        return;
    }
    if (sender != socket_.get()) return;
    if (type == QStringLiteral("session_keepalive") || type == QStringLiteral("notification")) watchdog_.start(keepaliveMs_);
    if (type == QStringLiteral("session_reconnect")) {
        if (replacement_) return;
        const QUrl url(data.value(QStringLiteral("session")).toObject().value(QStringLiteral("reconnect_url")).toString());
        if (url.isValid() && url.scheme() == QStringLiteral("wss")) connectEventSub(url, true);
    } else if (type == QStringLiteral("notification")) {
        const auto result = pipeline_.ingest(bytes);
        if (result == IngestResult::Invalid && log_) log_(QStringLiteral("Ignored an invalid EventSub notification"));
    } else if (type == QStringLiteral("revocation")) {
        const auto subType = data.value(QStringLiteral("subscription")).toObject().value(QStringLiteral("type")).toString();
        if (subscriptions_.contains(subType)) subscriptions_[subType] = TwitchSubscriptionState::Revoked;
        updateConnectionStatus();
    }
}

void TwitchClient::subscribeEvents()
{
    const QJsonObject chatCondition{{QStringLiteral("broadcaster_user_id"), broadcasterId_}, {QStringLiteral("user_id"), userId_}};
    for (const auto *type : {"channel.chat.message", "channel.chat.message_delete", "channel.chat.clear",
                             "channel.chat.clear_user_messages", "channel.chat.notification"}) {
        if (scopes_.contains(QStringLiteral("user:read:chat"))) subscribeOne(QString::fromLatin1(type), QStringLiteral("1"), chatCondition);
        else subscriptions_[QString::fromLatin1(type)] = TwitchSubscriptionState::Unavailable;
    }
    if (scopes_.contains(QStringLiteral("moderator:read:followers")))
        subscribeOne(QStringLiteral("channel.follow"), QStringLiteral("2"), {
            {QStringLiteral("broadcaster_user_id"), broadcasterId_}, {QStringLiteral("moderator_user_id"), userId_}});
    else subscriptions_[QStringLiteral("channel.follow")] = TwitchSubscriptionState::Unavailable;
    if (scopes_.contains(QStringLiteral("bits:read")) && userId_ == broadcasterId_)
        subscribeOne(QStringLiteral("channel.cheer"), QStringLiteral("1"), {{QStringLiteral("broadcaster_user_id"), broadcasterId_}});
    else subscriptions_[QStringLiteral("channel.cheer")] = TwitchSubscriptionState::Unavailable;
    subscribeOne(QStringLiteral("channel.raid"), QStringLiteral("1"), {{QStringLiteral("to_broadcaster_user_id"), broadcasterId_}});
    updateConnectionStatus();
}

void TwitchClient::subscribeOne(QString type, QString version, QJsonObject condition, int attempt)
{
    const QJsonObject body{{QStringLiteral("type"), type}, {QStringLiteral("version"), version},
        {QStringLiteral("condition"), condition}, {QStringLiteral("transport"), QJsonObject{
            {QStringLiteral("method"), QStringLiteral("websocket")}, {QStringLiteral("session_id"), sessionId_}}}};
    subscriptions_[type] = TwitchSubscriptionState::Pending;
    const auto sessionGeneration = sessionGeneration_;
    const auto generation = generation_;
    finish(transport_->post(apiRequest(QUrl(QStringLiteral("https://api.twitch.tv/helix/eventsub/subscriptions"))),
        QJsonDocument(body).toJson(QJsonDocument::Compact)),
        [this, type, version, condition, attempt, sessionGeneration, generation](int status, const QJsonObject &) {
        if (sessionGeneration != sessionGeneration_) return;
        if (status == 202) subscriptions_[type] = TwitchSubscriptionState::Enabled;
        else if ((status == 0 || status == 429 || status >= 500) && attempt < 3) {
            QTimer::singleShot(1000 * (1 << attempt), this, [this, type, version, condition, attempt, sessionGeneration, generation] {
                if (running_ && generation == generation_ && sessionGeneration == sessionGeneration_)
                    subscribeOne(type, version, condition, attempt + 1);
            });
        } else subscriptions_[type] = TwitchSubscriptionState::Failed;
        updateConnectionStatus();
    });
}

void TwitchClient::updateConnectionStatus()
{
    QStringList unavailable;
    int enabled = 0;
    for (auto it = subscriptions_.cbegin(); it != subscriptions_.cend(); ++it) {
        if (it.value() == TwitchSubscriptionState::Enabled) ++enabled;
        else if (it.value() != TwitchSubscriptionState::Pending) unavailable.push_back(it.key());
    }
    unavailable.sort();
    QString status = QStringLiteral("EventSub: %1 subscriptions active").arg(enabled);
    if (!unavailable.isEmpty()) status += QStringLiteral("; unavailable (check authorization): ") + unavailable.join(QStringLiteral(", "));
    setStatus(std::move(status));
}

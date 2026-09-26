#pragma once

#include "core/ordered-event-pipeline.hpp"
#include "twitch/eventsub-socket.hpp"
#include <QJsonObject>
#include <QNetworkReply>
#include <QSet>

struct TwitchConfiguration {
    QString clientId;
    QString channel;
    QString accessToken;
    QString refreshToken;
    bool operator==(const TwitchConfiguration &) const = default;
};
struct TwitchTokens { QString accessToken; QString refreshToken; };
enum class TwitchSubscriptionState { Pending, Enabled, Failed, Revoked, Unavailable };

class TwitchClient final : public QObject {
public:
    using LogCallback = std::function<void(QString)>;
    using TokenCallback = std::function<void(TwitchTokens)>;
    struct Dependencies {
        QNetworkAccessManager *network = nullptr;
        std::function<std::unique_ptr<EventSubSocket>()> socketFactory;
        std::function<void(QUrl)> openBrowser;
    };
    TwitchClient(EventDispatcher &dispatcher, LogCallback log, TokenCallback tokens, Dependencies dependencies);
    ~TwitchClient() override;
    void configure(TwitchConfiguration configuration);
    void startOrResume();
    void beginDeviceFlow();
    void stop();
    [[nodiscard]] QString status() const { return statusText_; }
    [[nodiscard]] QString authenticatedLogin() const { return userLogin_; }
    [[nodiscard]] QString broadcasterId() const { return broadcasterId_; }
    [[nodiscard]] const QHash<QString, TwitchSubscriptionState> &subscriptions() const { return subscriptions_; }
private:
    using ReplyCallback = std::function<void(int, QJsonObject)>;
    void setStatus(QString status, bool logStatus = true);
    void failAuthentication(QString status);
    void finish(QNetworkReply *reply, ReplyCallback callback);
    void validateToken();
    void refreshAccessToken();
    void finishAuth(const QJsonObject &json);
    void pollDeviceToken();
    void resolveBroadcaster();
    void connectEventSub(const QUrl &url = QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws")), bool handoff = false);
    void handleEventSubMessage(EventSubSocket *sender, const QString &payload);
    void subscribeEvents();
    void subscribeOne(QString type, QString version, QJsonObject condition, int attempt = 0);
    void scheduleReconnect();
    void closeSockets();
    void updateConnectionStatus();
    QNetworkRequest apiRequest(const QUrl &url) const;
    QNetworkReply *postForm(const QUrl &url, const QList<QPair<QString, QString>> &fields);

    QNetworkAccessManager network_;
    QNetworkAccessManager *transport_;
    Dependencies dependencies_;
    LogCallback log_;
    TokenCallback tokens_;
    OrderedEventPipeline pipeline_;
    std::unique_ptr<EventSubSocket> socket_, replacement_;
    QTimer devicePollTimer_, reconnectTimer_, watchdog_, validationTimer_, handoffTimer_;
    QSet<QNetworkReply *> replies_;
    TwitchConfiguration configuration_;
    QSet<QString> scopes_;
    QHash<QString, TwitchSubscriptionState> subscriptions_;
    QString userId_, userLogin_, broadcasterId_, deviceCode_, sessionId_;
    QString statusText_{QStringLiteral("Disconnected")};
    std::uint64_t generation_ = 0;
    std::uint64_t sessionGeneration_ = 0;
    bool running_ = false;
    bool authenticating_ = false;
    bool deviceRequestPending_ = false;
    bool refreshing_ = false;
    bool refreshAttempted_ = false;
    QDeadlineTimer deviceDeadline_;
    int pollIntervalMs_ = 5000;
    int retryMs_ = 1000;
    int keepaliveMs_ = 11000;
};

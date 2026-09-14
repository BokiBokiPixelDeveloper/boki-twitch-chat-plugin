#pragma once

#include "chat/chat-types.hpp"
#include "chat/emote-service.hpp"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>
#include <memory>

class TwitchClient {
public:
    using MessageCallback = std::function<void(ChatMessage)>;
    using GifCallback = std::function<void(DecodedGif)>;
    using StatusCallback = std::function<void(QString)>;
    using TokenCallback = std::function<void(QString accessToken, QString refreshToken)>;

    TwitchClient(MessageCallback onMessage, GifCallback onGif, StatusCallback onStatus, TokenCallback onTokens);
    ~TwitchClient();

    void configure(QString clientId, QString channel, QString accessToken, QString refreshToken);
    void startOrResume();
    void beginDeviceFlow();
    void disconnect();

    [[nodiscard]] QString status() const { return statusText_; }
    [[nodiscard]] QString authenticatedLogin() const { return userLogin_; }

private:
    static constexpr const char *kScope = "user:read:chat";

    void setStatus(const QString &status);
    void validateToken();
    void refreshAccessToken();
    void finishAuthFromTokenResponse(const QJsonObject &json);

    void pollDeviceToken();
    void resolveBroadcaster();
    void connectEventSub(const QUrl &url = QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws")));
    void handleEventSubMessage(const QString &payload);
    void subscribeChat(const QString &sessionId);
    void downloadGif(const QUrl &url);

    QNetworkRequest apiRequest(const QUrl &url) const;
    static QByteArray encodeForm(const QList<QPair<QString, QString>> &pairs);

    QNetworkAccessManager network_;
    QWebSocket socket_;
    QTimer devicePollTimer_;

    MessageCallback onMessage_;
    GifCallback onGif_;
    StatusCallback onStatus_;
    TokenCallback onTokens_;

    QString clientId_;
    QString channelLogin_;
    QString accessToken_;
    QString refreshToken_;
    QString userId_;
    QString userLogin_;
    QString broadcasterId_;
    QString deviceCode_;
    QString deviceScopes_;
    int devicePollIntervalMs_ = 5000;

    EmoteService emotes_;

    QString statusText_{QStringLiteral("Nicht verbunden")};
};

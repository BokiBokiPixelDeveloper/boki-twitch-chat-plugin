#include "twitch/twitch-client.hpp"

#include <QBuffer>
#include <QDesktopServices>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <obs-module.h>

namespace {
QJsonObject parseObject(const QByteArray &bytes)
{
    const auto doc = QJsonDocument::fromJson(bytes);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QString jsonErrorMessage(const QByteArray &bytes)
{
    const auto obj = parseObject(bytes);
    if (obj.contains(QStringLiteral("message")))
        return obj.value(QStringLiteral("message")).toString();
    if (obj.contains(QStringLiteral("error")))
        return obj.value(QStringLiteral("error")).toString();
    return QString::fromUtf8(bytes.left(300));
}
} // namespace

TwitchClient::TwitchClient(MessageCallback onMessage, GifCallback onGif, StatusCallback onStatus, TokenCallback onTokens)
    : onMessage_(std::move(onMessage)),
      onGif_(std::move(onGif)),
      onStatus_(std::move(onStatus)),
      onTokens_(std::move(onTokens))
{
    devicePollTimer_.setSingleShot(false);
    QObject::connect(&devicePollTimer_, &QTimer::timeout, [&]() { pollDeviceToken(); });

    QObject::connect(&socket_, &QWebSocket::textMessageReceived,
                     [&](const QString &msg) { handleEventSubMessage(msg); });
    QObject::connect(&socket_, &QWebSocket::connected, [&]() {
        setStatus(QStringLiteral("EventSub verbunden – warte auf Session"));
    });
    QObject::connect(&socket_, &QWebSocket::disconnected, [&]() {
        if (!accessToken_.isEmpty())
            setStatus(QStringLiteral("EventSub getrennt"));
    });
}

TwitchClient::~TwitchClient()
{
    disconnect();
}

void TwitchClient::configure(QString clientId, QString channel, QString accessToken, QString refreshToken)
{
    clientId_ = std::move(clientId);
    channelLogin_ = std::move(channel).trimmed();
    accessToken_ = std::move(accessToken);
    refreshToken_ = std::move(refreshToken);
}

void TwitchClient::setStatus(const QString &status)
{
    statusText_ = status;
    blog(LOG_INFO, "[bokis-twitch-chat-plugin] %s", status.toUtf8().constData());
    if (onStatus_)
        onStatus_(status);
}

QByteArray TwitchClient::encodeForm(const QList<QPair<QString, QString>> &pairs)
{
    QUrlQuery query;
    for (const auto &pair : pairs)
        query.addQueryItem(pair.first, pair.second);
    return query.query(QUrl::FullyEncoded).toUtf8();
}

QNetworkRequest TwitchClient::apiRequest(const QUrl &url) const
{
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken_.toUtf8());
    req.setRawHeader("Client-Id", clientId_.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return req;
}

void TwitchClient::startOrResume()
{
    if (clientId_.isEmpty()) {
        setStatus(QStringLiteral("Client ID fehlt"));
        return;
    }
    if (accessToken_.isEmpty()) {
        setStatus(QStringLiteral("Nicht authentifiziert – 'Mit Twitch verbinden' klicken"));
        return;
    }
    validateToken();
}

void TwitchClient::beginDeviceFlow()
{
    if (clientId_.isEmpty()) {
        setStatus(QStringLiteral("Client ID fehlt"));
        return;
    }

    devicePollTimer_.stop();
    deviceCode_.clear();
    deviceScopes_ = QString::fromLatin1(kScope);

    QNetworkRequest req(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/device")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    auto *reply = network_.post(req, encodeForm({
        {QStringLiteral("client_id"), clientId_},
        {QStringLiteral("scopes"), deviceScopes_},
    }));

    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status < 200 || status >= 300) {
            setStatus(QStringLiteral("Device-Login fehlgeschlagen: %1").arg(jsonErrorMessage(bytes)));
            return;
        }

        const auto json = parseObject(bytes);
        deviceCode_ = json.value(QStringLiteral("device_code")).toString();
        const QString userCode = json.value(QStringLiteral("user_code")).toString();
        const QUrl verifyUrl(json.value(QStringLiteral("verification_uri")).toString());
        devicePollIntervalMs_ = qMax(1000, json.value(QStringLiteral("interval")).toInt(5) * 1000);

        if (deviceCode_.isEmpty() || !verifyUrl.isValid()) {
            setStatus(QStringLiteral("Ungültige Device-Login-Antwort von Twitch"));
            return;
        }

        setStatus(QStringLiteral("Twitch freigeben – Code %1").arg(userCode));
        QDesktopServices::openUrl(verifyUrl);
        devicePollTimer_.start(devicePollIntervalMs_);
        pollDeviceToken();
    });
}

void TwitchClient::pollDeviceToken()
{
    if (deviceCode_.isEmpty())
        return;

    QNetworkRequest req(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    auto *reply = network_.post(req, encodeForm({
        {QStringLiteral("client_id"), clientId_},
        {QStringLiteral("scopes"), deviceScopes_},
        {QStringLiteral("device_code"), deviceCode_},
        {QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")},
    }));

    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status >= 200 && status < 300) {
            devicePollTimer_.stop();
            deviceCode_.clear();
            finishAuthFromTokenResponse(parseObject(bytes));
            return;
        }

        const QString message = jsonErrorMessage(bytes);
        if (message.contains(QStringLiteral("authorization_pending"), Qt::CaseInsensitive))
            return;
        if (message.contains(QStringLiteral("slow_down"), Qt::CaseInsensitive)) {
            devicePollIntervalMs_ += 2000;
            devicePollTimer_.setInterval(devicePollIntervalMs_);
            return;
        }

        devicePollTimer_.stop();
        setStatus(QStringLiteral("Device-Token fehlgeschlagen: %1").arg(message));
    });
}

void TwitchClient::finishAuthFromTokenResponse(const QJsonObject &json)
{
    const QString access = json.value(QStringLiteral("access_token")).toString();
    const QString refresh = json.value(QStringLiteral("refresh_token")).toString();
    if (access.isEmpty()) {
        setStatus(QStringLiteral("Twitch lieferte keinen Access Token"));
        return;
    }
    accessToken_ = access;
    if (!refresh.isEmpty())
        refreshToken_ = refresh;
    if (onTokens_)
        onTokens_(accessToken_, refreshToken_);
    validateToken();
}

void TwitchClient::validateToken()
{
    QNetworkRequest req(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/validate")));
    req.setRawHeader("Authorization", QByteArray("OAuth ") + accessToken_.toUtf8());
    auto *reply = network_.get(req);

    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (status == 401) {
            refreshAccessToken();
            return;
        }
        if (status < 200 || status >= 300) {
            setStatus(QStringLiteral("Token-Validierung fehlgeschlagen: %1").arg(jsonErrorMessage(bytes)));
            return;
        }

        const auto json = parseObject(bytes);
        userId_ = json.value(QStringLiteral("user_id")).toString();
        userLogin_ = json.value(QStringLiteral("login")).toString();
        if (userId_.isEmpty()) {
            setStatus(QStringLiteral("Token enthält keine Twitch User-ID"));
            return;
        }
        if (channelLogin_.isEmpty())
            channelLogin_ = userLogin_;
        resolveBroadcaster();
    });
}

void TwitchClient::refreshAccessToken()
{
    if (refreshToken_.isEmpty()) {
        accessToken_.clear();
        setStatus(QStringLiteral("Twitch-Login abgelaufen – neu verbinden"));
        return;
    }

    QNetworkRequest req(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    auto *reply = network_.post(req, encodeForm({
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
        {QStringLiteral("refresh_token"), refreshToken_},
        {QStringLiteral("client_id"), clientId_},
    }));

    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status < 200 || status >= 300) {
            accessToken_.clear();
            refreshToken_.clear();
            if (onTokens_)
                onTokens_({}, {});
            setStatus(QStringLiteral("Token-Refresh fehlgeschlagen – neu verbinden"));
            return;
        }
        finishAuthFromTokenResponse(parseObject(bytes));
    });
}

void TwitchClient::resolveBroadcaster()
{
    QUrl url(QStringLiteral("https://api.twitch.tv/helix/users"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("login"), channelLogin_);
    url.setQuery(q);
    auto *reply = network_.get(apiRequest(url));

    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status < 200 || status >= 300) {
            setStatus(QStringLiteral("Kanalauflösung fehlgeschlagen: %1").arg(jsonErrorMessage(bytes)));
            return;
        }

        const auto data = parseObject(bytes).value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) {
            setStatus(QStringLiteral("Twitch-Kanal nicht gefunden: %1").arg(channelLogin_));
            return;
        }
        broadcasterId_ = data.first().toObject().value(QStringLiteral("id")).toString();
        connectEventSub();
    });
}

void TwitchClient::connectEventSub(const QUrl &url)
{
    if (socket_.state() != QAbstractSocket::UnconnectedState)
        socket_.close();
    setStatus(QStringLiteral("Verbinde EventSub…"));
    socket_.open(url);
}

void TwitchClient::handleEventSubMessage(const QString &payload)
{
    const auto root = QJsonDocument::fromJson(payload.toUtf8()).object();
    const QString type = root.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("message_type")).toString();
    const auto data = root.value(QStringLiteral("payload")).toObject();

    if (type == QStringLiteral("session_welcome")) {
        const QString sessionId = data.value(QStringLiteral("session")).toObject().value(QStringLiteral("id")).toString();
        setStatus(QStringLiteral("EventSub aktiv – abonniere Chat"));
        subscribeChat(sessionId);
        return;
    }

    if (type == QStringLiteral("session_reconnect")) {
        const QUrl reconnectUrl(data.value(QStringLiteral("session")).toObject().value(QStringLiteral("reconnect_url")).toString());
        if (reconnectUrl.isValid())
            connectEventSub(reconnectUrl);
        return;
    }

    if (type != QStringLiteral("notification"))
        return;

    const QString subType = root.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("subscription_type")).toString();
    if (subType != QStringLiteral("channel.chat.message"))
        return;

    const auto event = data.value(QStringLiteral("event")).toObject();
    const auto message = event.value(QStringLiteral("message")).toObject();

    PendingChatMessage pending;
    pending.userName = event.value(QStringLiteral("chatter_user_name")).toString();
    pending.text = message.value(QStringLiteral("text")).toString();
    const QString color = event.value(QStringLiteral("color")).toString();
    if (QColor(color).isValid())
        pending.userColor = QColor(color);
    if (onMessage_)
        onMessage_(std::move(pending));

    const auto fragments = message.value(QStringLiteral("fragments")).toArray();
    for (const auto &value : fragments) {
        const auto fragment = value.toObject();
        if (fragment.value(QStringLiteral("type")).toString() != QStringLiteral("gif"))
            continue;
        const auto gif = fragment.value(QStringLiteral("gif")).toObject();
        const QUrl url(gif.value(QStringLiteral("url")).toString());
        if (url.isValid())
            downloadGif(url);
    }
}

void TwitchClient::subscribeChat(const QString &sessionId)
{
    QJsonObject body;
    body.insert(QStringLiteral("type"), QStringLiteral("channel.chat.message"));
    body.insert(QStringLiteral("version"), QStringLiteral("1"));
    body.insert(QStringLiteral("condition"), QJsonObject{
        {QStringLiteral("broadcaster_user_id"), broadcasterId_},
        {QStringLiteral("user_id"), userId_},
    });
    body.insert(QStringLiteral("transport"), QJsonObject{
        {QStringLiteral("method"), QStringLiteral("websocket")},
        {QStringLiteral("session_id"), sessionId},
    });

    auto *reply = network_.post(apiRequest(QUrl(QStringLiteral("https://api.twitch.tv/helix/eventsub/subscriptions"))),
                                QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status != 202) {
            setStatus(QStringLiteral("Chat-Abo fehlgeschlagen: %1").arg(jsonErrorMessage(bytes)));
            return;
        }
        setStatus(QStringLiteral("Verbunden als %1 → #%2").arg(userLogin_, channelLogin_));
    });
}

void TwitchClient::downloadGif(const QUrl &url)
{
    auto *reply = network_.get(QNetworkRequest(url));
    QObject::connect(reply, &QNetworkReply::finished, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (status < 200 || status >= 300 || bytes.isEmpty())
            return;
        DecodedGif gif = decodeGif(bytes);
        if (!gif.frames.empty() && onGif_)
            onGif_(std::move(gif));
    });
}

DecodedGif TwitchClient::decodeGif(const QByteArray &bytes)
{
    DecodedGif out;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);

    constexpr int kMaxFrames = 180;
    while (reader.canRead() && static_cast<int>(out.frames.size()) < kMaxFrames) {
        QImage frame = reader.read();
        if (frame.isNull())
            break;
        frame = frame.convertToFormat(QImage::Format_RGBA8888);
        out.frames.push_back(std::move(frame));
        out.delaysMs.push_back(qMax(16, reader.nextImageDelay()));
    }
    return out;
}

void TwitchClient::disconnect()
{
    devicePollTimer_.stop();
    if (socket_.state() != QAbstractSocket::UnconnectedState)
        socket_.close();
}

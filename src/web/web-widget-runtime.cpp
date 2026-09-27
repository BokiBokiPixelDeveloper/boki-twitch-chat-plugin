#include "web/web-widget-runtime.hpp"
#include "web/web-event-serializer.hpp"
#include "web/widget-resource-request.hpp"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>
#include <obs.h>
#include <obs-module.h>

namespace {
QByteArray randomToken()
{
    QByteArray bytes(32, Qt::Uninitialized);
    auto *words = reinterpret_cast<quint32 *>(bytes.data());
    QRandomGenerator::system()->fillRange(words, bytes.size() / sizeof(quint32));
    return bytes.toHex();
}
bool browserSourceRegistered()
{
    for (size_t i = 0;; ++i) {
        const char *id = nullptr;
        if (!obs_enum_source_types(i, &id)) return false;
        if (id && strcmp(id, "browser_source") == 0) return true;
    }
}
}

class WebWidgetRuntime::State {
public:
    State(obs_source_t *parentSource, std::shared_ptr<PluginRuntime> pluginRuntime,
          std::shared_ptr<std::atomic_bool> accepted, uint32_t w, uint32_t h)
        : parent(parentSource), runtime(std::move(pluginRuntime)), backendAccepted(std::move(accepted)), width(w), height(h),
          webSocketServer(QStringLiteral("Boki Web Widget Bridge"), QWebSocketServer::NonSecureMode)
    {
        if (!obs_get_module("obs-browser") || !browserSourceRegistered()) {
            status = QStringLiteral("Web Widget Runtime unavailable. The required OBS browser component could not be loaded.");
            blog(LOG_ERROR, "[WebWidget][Runtime] %s", status.toUtf8().constData());
            return;
        }
        capability = randomToken();
        httpServer.setMaxPendingConnections(4);
        webSocketServer.setMaxPendingConnections(2);
        if (!httpServer.listen(QHostAddress::LocalHost, 0) || !webSocketServer.listen(QHostAddress::LocalHost, 0)) {
            status = QStringLiteral("Web Widget Runtime unavailable. The local bridge could not be started.");
            blog(LOG_ERROR, "[WebWidget][Bridge] %s", status.toUtf8().constData());
            return;
        }
        QObject::connect(&httpServer, &QTcpServer::newConnection, [&] { acceptHttp(); });
        QObject::connect(&webSocketServer, &QWebSocketServer::newConnection, [&] { acceptWebSocket(); });
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [&] { pump(); });
        timer.start();
        loadTimer.start();
        createBrowser();
    }
    ~State()
    {
        timer.stop();
        subscription.close();
        if (socket) { socket->close(); socket->deleteLater(); socket = nullptr; }
        webSocketServer.close();
        httpServer.close();
        destroyBrowser();
    }

    void createBrowser()
    {
        obs_data_t *settings = obs_data_create();
        const QString url = QStringLiteral("http://127.0.0.1:%1/%2/index.html")
                                .arg(httpServer.serverPort()).arg(QString::fromLatin1(capability));
        obs_data_set_bool(settings, "is_local_file", false);
        obs_data_set_string(settings, "url", url.toUtf8().constData());
        obs_data_set_int(settings, "width", width);
        obs_data_set_int(settings, "height", height);
        obs_data_set_bool(settings, "shutdown", false);
        obs_data_set_bool(settings, "restart_when_active", false);
        obs_data_set_int(settings, "webpage_control_level", 0);
        obs_data_set_bool(settings, "reroute_audio", true);
        obs_data_set_bool(settings, "fps_custom", true);
        obs_data_set_int(settings, "fps", 30);
        obs_data_set_string(settings, "css", "html,body{margin:0;background:transparent;overflow:hidden}");
        browser = obs_source_create_private("browser_source", "Boki Web Widget Runtime", settings);
        obs_data_release(settings);
        if (!browser) {
            status = QStringLiteral("Web Widget Runtime unavailable. The OBS browser source could not be created.");
            blog(LOG_ERROR, "[WebWidget][Runtime] %s", status.toUtf8().constData());
            return;
        }
        obs_source_set_muted(browser, true);
        attached = obs_source_add_active_child(parent, browser);
        status = QStringLiteral("Web Widget loading");
        blog(LOG_INFO, "[WebWidget][Runtime] Created independent browser runtime");
    }
    void destroyBrowser()
    {
        if (!browser) return;
        if (attached) obs_source_remove_active_child(parent, browser);
        attached = false;
        obs_source_release(browser);
        browser = nullptr;
    }
    void acceptHttp()
    {
        while (auto *client = httpServer.nextPendingConnection()) {
            client->setParent(&httpServer);
            QTimer::singleShot(3000, client, [client] { if (client->state() != QAbstractSocket::UnconnectedState) client->disconnectFromHost(); });
            QObject::connect(client, &QTcpSocket::readyRead, client, [this, client] {
                QByteArray request = client->property("requestBytes").toByteArray() + client->readAll();
                if (request.size() > 8192) { client->disconnectFromHost(); return; }
                client->setProperty("requestBytes", request);
                if (!request.endsWith("\r\n\r\n")) return;
                const auto parsed = WidgetResourceRequest::resourceName(request, capability, httpServer.serverPort());
                if (!parsed) { respond(client, "text/plain", "Not found", "404 Not Found"); return; }
                const QByteArray &name = *parsed;
                if (name == "index.html") respondResource(client, ":/bokis-web-widget/index.html", "text/html; charset=utf-8");
                else if (name == "bridge.js") respondResource(client, ":/bokis-web-widget/bridge.js", "text/javascript; charset=utf-8");
                else if (name == "widget.js") respondResource(client, ":/bokis-web-widget/widget.js", "text/javascript; charset=utf-8");
                else if (name == "style.css") respondResource(client, ":/bokis-web-widget/style.css", "text/css; charset=utf-8");
                else if (name == "bootstrap.json") {
                    QJsonObject value{{"webSocketUrl", QStringLiteral("ws://127.0.0.1:%1").arg(webSocketServer.serverPort())},
                                      {"capability", QString::fromLatin1(capability)}};
                    respond(client, "application/json", QJsonDocument(value).toJson(QJsonDocument::Compact));
                } else respond(client, "text/plain", "Not found", "404 Not Found");
            });
        }
    }
    static void respondResource(QTcpSocket *client, const QString &path, const QByteArray &type)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { respond(client, "text/plain", "Unavailable", "500 Internal Server Error"); return; }
        respond(client, type, file.readAll());
    }
    static void respond(QTcpSocket *client, const QByteArray &type, const QByteArray &body, const QByteArray &code = "200 OK")
    {
        QByteArray headers = "HTTP/1.1 " + code + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
            "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n";
        if (type.startsWith("text/html")) headers += "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' https://static-cdn.jtvnw.net https://cdn.7tv.app https://cdn.betterttv.net https://cdn.frankerfacez.com; connect-src 'self' ws://127.0.0.1:*; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'\r\n";
        client->write(headers + "\r\n" + body);
        client->disconnectFromHost();
    }
    void acceptWebSocket()
    {
        while (auto *candidate = webSocketServer.nextPendingConnection()) {
            const QString expectedOrigin = QStringLiteral("http://127.0.0.1:%1").arg(httpServer.serverPort());
            if (candidate->origin() != expectedOrigin) {
                candidate->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Invalid origin"));
                candidate->deleteLater();
                continue;
            }
            if (socket) { candidate->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Runtime already connected")); candidate->deleteLater(); continue; }
            socket = candidate;
            socket->setParent(&webSocketServer);
            socket->setMaxAllowedIncomingFrameSize(8192);
            socket->setMaxAllowedIncomingMessageSize(8192);
            QObject::connect(socket, &QWebSocket::textMessageReceived, [this](const QString &text) {
                if (text.toUtf8().size() > 8192) { socket->close(QWebSocketProtocol::CloseCodeTooMuchData); return; }
                const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
                if (!authenticated) {
                    authenticated = object.value("type") == "hello" && object.value("capability").toString().toLatin1() == capability;
                    if (!authenticated) { socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Invalid capability")); return; }
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "welcome"}, {"schemaVersion", 1}, {"width", int(width)}, {"height", int(height)}}).toJson(QJsonDocument::Compact)));
                } else if (object.value("type") == "ready") { ready = true; status = QStringLiteral("Web Widget ready"); }
                else if (object.value("type") == "ack" && object.value("deliveryId").toString() == pendingDelivery) { pendingDelivery.clear(); pendingTimer.invalidate(); }
            });
            QObject::connect(socket, &QWebSocket::disconnected, [this, candidate] { if (socket == candidate) { socket = nullptr; ready = authenticated = false; pendingDelivery.clear(); status = QStringLiteral("Web Widget disconnected"); } candidate->deleteLater(); });
        }
    }
    void pump()
    {
        if (!ready && loadTimer.isValid() && loadTimer.elapsed() > 10000 && browser)
            status = QStringLiteral("Web Widget failed to become ready. Use Refresh Web Widget to retry.");
        if (!pendingDelivery.isEmpty() && pendingTimer.isValid() && pendingTimer.elapsed() > 10000 && socket) {
            status = QStringLiteral("Web Widget stopped acknowledging events. Reconnecting the bridge.");
            socket->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("Event acknowledgement timeout"));
            return;
        }
        const bool accepted = backendAccepted->load();
        if (!accepted) {
            if (wasAccepted) {
                subscription.close();
                pendingDelivery.clear();
                sendReset(QStringLiteral("backendUnavailable"));
            }
            wasAccepted = false;
            return;
        }
        if (!wasAccepted) subscription = runtime->subscribe({}, {256, 32 * 1024 * 1024});
        wasAccepted = true;
        if (!ready || !socket || !pendingDelivery.isEmpty()) return;
        auto batch = subscription.takeBatch(32);
        if (batch.state == ConsumerState::Overflowed) { subscription = runtime->subscribe({}, {256, 32 * 1024 * 1024}); sendReset(QStringLiteral("consumerOverflow")); return; }
        if (batch.state == ConsumerState::RuntimeStopped) { status = QStringLiteral("Web Widget runtime stopped"); return; }
        if (batch.events.empty()) return;
        QJsonArray events;
        for (const auto &event : batch.events) events.append(WebEventSerializer::serialize(*event));
        pendingDelivery = QString::number(++deliveryId);
        pendingTimer.start();
        const QJsonObject envelope{{"type", "events"}, {"deliveryId", pendingDelivery}, {"events", events}};
        const QByteArray json = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        if (json.size() > 1024 * 1024) { pendingDelivery.clear(); sendReset(QStringLiteral("oversizeBatch")); return; }
        socket->sendTextMessage(QString::fromUtf8(json));
    }
    void sendReset(const QString &reason)
    {
        if (socket) socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "reset"}, {"reason", reason}}).toJson(QJsonDocument::Compact)));
    }

    obs_source_t *parent = nullptr;
    obs_source_t *browser = nullptr;
    bool attached = false;
    std::shared_ptr<PluginRuntime> runtime;
    std::shared_ptr<std::atomic_bool> backendAccepted;
    EventSubscription subscription;
    uint32_t width;
    uint32_t height;
    QString status = QStringLiteral("Web Widget disabled");
    QByteArray capability;
    QTcpServer httpServer;
    QWebSocketServer webSocketServer;
    QWebSocket *socket = nullptr;
    QTimer timer;
    QElapsedTimer loadTimer;
    QElapsedTimer pendingTimer;
    bool authenticated = false;
    bool ready = false;
    bool wasAccepted = false;
    quint64 deliveryId = 0;
    QString pendingDelivery;
};

WebWidgetRuntime::WebWidgetRuntime(obs_source_t *parent, std::shared_ptr<PluginRuntime> runtime,
                                   std::shared_ptr<std::atomic_bool> backendAccepted, uint32_t width, uint32_t height)
    : state_(std::make_unique<State>(parent, std::move(runtime), std::move(backendAccepted), width, height)) {}
WebWidgetRuntime::~WebWidgetRuntime() = default;
bool WebWidgetRuntime::available() const { return state_->browser != nullptr; }
QString WebWidgetRuntime::status() const { return state_->status; }
obs_source_t *WebWidgetRuntime::child() const { return state_->browser; }
void WebWidgetRuntime::render() { if (auto *source = state_->browser) obs_source_video_render(source); }
void WebWidgetRuntime::resize(uint32_t width, uint32_t height)
{
    state_->width = width; state_->height = height;
    if (!state_->browser) return;
    obs_data_t *settings = obs_source_get_settings(state_->browser);
    obs_data_set_int(settings, "width", width); obs_data_set_int(settings, "height", height);
    obs_source_update(state_->browser, settings); obs_data_release(settings);
}

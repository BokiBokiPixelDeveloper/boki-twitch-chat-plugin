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
#include <QThread>
#include <QSemaphore>
#include <mutex>
#include <utility>
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

class WebWidgetRuntime::State : public QObject {
public:
    State(std::shared_ptr<PluginRuntime> pluginRuntime,
          std::shared_ptr<std::atomic_bool> accepted, uint32_t w, uint32_t h)
        : runtime(std::move(pluginRuntime)), backendAccepted(std::move(accepted)), width(w), height(h),
          webSocketServer(QStringLiteral("Boki Web Widget Bridge"), QWebSocketServer::NonSecureMode)
    {
        capability = randomToken();
        httpServer.setMaxPendingConnections(4);
        webSocketServer.setMaxPendingConnections(2);
        if (!httpServer.listen(QHostAddress::LocalHost, 0) || !webSocketServer.listen(QHostAddress::LocalHost, 0)) {
            setStatus(QStringLiteral("Web Widget Runtime unavailable. The local bridge could not be started."));
            blog(LOG_ERROR, "[WebWidget][Bridge] %s", getStatus().toUtf8().constData());
            return;
        }
        QObject::connect(&httpServer, &QTcpServer::newConnection, this, [this] { acceptHttp(); });
        QObject::connect(&webSocketServer, &QWebSocketServer::newConnection, this, [this] { acceptWebSocket(); });
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, this, [this] { pump(); });
        timer.start();
        loadTimer.start();
        setStatus(QStringLiteral("Web Widget loading"));
    }
    ~State() override { shutdown(); }

    void shutdown()
    {
        Q_ASSERT(QThread::currentThread() == thread());
        if (cleaned) return;
        cleaned = true;
        stopped = true;
        subscription.close();
        blog(LOG_INFO, "[WebWidget][Runtime] Event subscription released");
        timer.stop();
        QObject::disconnect(&timer, nullptr, this, nullptr);
        blog(LOG_INFO, "[WebWidget][Runtime] Timers stopped");
        // Disconnect callbacks before abort: socket shutdown can emit disconnected inline.
        QObject::disconnect(&webSocketServer, nullptr, this, nullptr);
        for (auto *client : webSocketServer.findChildren<QWebSocket *>(QString(), Qt::FindDirectChildrenOnly)) {
            QObject::disconnect(client, nullptr, this, nullptr);
            client->abort();
            delete client;
        }
        socket = nullptr;
        blog(LOG_INFO, "[WebWidget][Runtime] WebSocket clients closed");
        webSocketServer.close();
        blog(LOG_INFO, "[WebWidget][Runtime] WebSocket server stopped");
        QObject::disconnect(&httpServer, nullptr, this, nullptr);
        httpServer.close();
        for (auto *client : httpServer.findChildren<QTcpSocket *>(QString(), Qt::FindDirectChildrenOnly)) {
            QObject::disconnect(client, nullptr, nullptr, nullptr);
            client->abort();
            delete client;
        }
        blog(LOG_INFO, "[WebWidget][Runtime] HTTP server stopped");
    }

    void setStatus(QString value) { std::lock_guard lock(statusMutex); status = std::move(value); }
    QString getStatus() const { std::lock_guard lock(statusMutex); return status; }
    QString url() const
    {
        if (!httpServer.isListening() || !webSocketServer.isListening()) return {};
        return QStringLiteral("http://127.0.0.1:%1/%2/index.html")
            .arg(httpServer.serverPort()).arg(QString::fromLatin1(capability));
    }
    void acceptHttp()
    {
        if (stopped) return;
        while (auto *client = httpServer.nextPendingConnection()) {
            client->setParent(&httpServer);
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
            QTimer::singleShot(3000, client, [client] { if (client->state() != QAbstractSocket::UnconnectedState) client->disconnectFromHost(); });
            QObject::connect(client, &QTcpSocket::readyRead, this, [this, client] {
                if (stopped || !client->isOpen()) return;
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
        if (stopped) return;
        while (auto *candidate = webSocketServer.nextPendingConnection()) {
            candidate->setParent(&webSocketServer);
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
            QObject::connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                if (stopped || !socket) return;
                if (text.toUtf8().size() > 8192) { socket->close(QWebSocketProtocol::CloseCodeTooMuchData); return; }
                const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
                if (!authenticated) {
                    authenticated = object.value("type") == "hello" && object.value("capability").toString().toLatin1() == capability;
                    if (!authenticated) { socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Invalid capability")); return; }
                    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "welcome"}, {"schemaVersion", 1}, {"width", int(width)}, {"height", int(height)}}).toJson(QJsonDocument::Compact)));
                } else if (object.value("type") == "ready") { ready = true; setStatus(QStringLiteral("Web Widget ready")); }
                else if (object.value("type") == "ack" && object.value("deliveryId").toString() == pendingDelivery) { pendingDelivery.clear(); pendingTimer.invalidate(); }
            });
            QObject::connect(socket, &QWebSocket::disconnected, this, [this, candidate] { if (stopped) return; if (socket == candidate) { socket = nullptr; ready = authenticated = false; pendingDelivery.clear(); setStatus(QStringLiteral("Web Widget disconnected")); } candidate->deleteLater(); });
        }
    }
    void pump()
    {
        if (stopped) return;
        if (!ready && loadTimer.isValid() && loadTimer.elapsed() > 10000)
            setStatus(QStringLiteral("Web Widget failed to become ready. Use Refresh Web Widget to retry."));
        if (!pendingDelivery.isEmpty() && pendingTimer.isValid() && pendingTimer.elapsed() > 10000 && socket) {
            setStatus(QStringLiteral("Web Widget stopped acknowledging events. Reconnecting the bridge."));
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
        if (batch.state == ConsumerState::RuntimeStopped) { setStatus(QStringLiteral("Web Widget runtime stopped")); return; }
        if (batch.events.empty()) return;
        QJsonArray events;
        for (const auto &event : batch.events) events.append(WebEventSerializer::serialize(*event));
        pendingDelivery = QString::number(++deliveryId);
        pendingTimer.start();
        const QJsonObject envelope{{"type", "events"}, {"deliveryId", pendingDelivery}, {"events", events}};
        const QByteArray json = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        if (json.size() > 1024 * 1024) { pendingDelivery.clear(); sendReset(QStringLiteral("oversizeBatch")); return; }
        if (!stopped) socket->sendTextMessage(QString::fromUtf8(json));
    }
    void sendReset(const QString &reason)
    {
        if (!stopped && socket) socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "reset"}, {"reason", reason}}).toJson(QJsonDocument::Compact)));
    }

    std::atomic_bool stopped = false;
    bool cleaned = false;
    mutable std::mutex statusMutex;
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

// This event loop belongs to the bridge, not the OBS UI. It never calls OBS source
// operations or takes ChatSource's mutex, so joining it cannot wait on the UI/render path.
class WebWidgetRuntime::Worker final : public QThread {
public:
    std::shared_ptr<PluginRuntime> runtime;
    std::shared_ptr<std::atomic_bool> accepted;
    uint32_t width = 0, height = 0;
    QSemaphore started;
    State *state = nullptr;
    QString url;
    void run() override
    {
        State bridge(runtime, accepted, width, height);
        state = &bridge;
        url = bridge.url();
        started.release();
        exec();
        bridge.shutdown();
        // State and all Qt children are destroyed here, on their owning thread.
    }
};

WebWidgetRuntime::WebWidgetRuntime(obs_source_t *parent, std::shared_ptr<PluginRuntime> runtime,
                                   std::shared_ptr<std::atomic_bool> backendAccepted, uint32_t width, uint32_t height)
    : parent_(parent)
{
    if (!obs_get_module("obs-browser") || !browserSourceRegistered()) {
        status_ = QStringLiteral("Web Widget Runtime unavailable. The required OBS browser component could not be loaded.");
        blog(LOG_ERROR, "[WebWidget][Runtime] %s", status_.toUtf8().constData());
        return;
    }
    worker_ = std::make_unique<Worker>();
    // The control object has no event handlers; OBS may retire it on another thread.
    worker_->moveToThread(nullptr);
    worker_->runtime = std::move(runtime);
    worker_->accepted = std::move(backendAccepted);
    worker_->width = width;
    worker_->height = height;
    worker_->start();
    worker_->started.acquire();
    if (worker_->url.isEmpty()) {
        status_ = worker_->state->getStatus();
        shutdown();
        return;
    }
    obs_data_t *settings = obs_data_create();
    obs_data_set_bool(settings, "is_local_file", false);
    obs_data_set_string(settings, "url", worker_->url.toUtf8().constData());
    obs_data_set_int(settings, "width", width);
    obs_data_set_int(settings, "height", height);
    obs_data_set_bool(settings, "shutdown", false);
    obs_data_set_bool(settings, "restart_when_active", false);
    obs_data_set_int(settings, "webpage_control_level", 0);
    obs_data_set_bool(settings, "reroute_audio", true);
    obs_data_set_bool(settings, "fps_custom", true);
    obs_data_set_int(settings, "fps", 30);
    // The bundled external stylesheet owns presentation; CEF-injected CSS violates CSP.
    obs_data_set_string(settings, "css", "");
    browser_ = obs_source_create_private("browser_source", "Boki Web Widget Runtime", settings);
    obs_data_release(settings);
    if (!browser_) {
        status_ = QStringLiteral("Web Widget Runtime unavailable. The OBS browser source could not be created.");
        blog(LOG_ERROR, "[WebWidget][Runtime] %s", status_.toUtf8().constData());
        shutdown();
        return;
    }
    obs_source_set_muted(browser_, true);
    attached_ = obs_source_add_active_child(parent_, browser_);
    if (!attached_) {
        status_ = QStringLiteral("Web Widget Runtime unavailable. The browser child could not be attached.");
        blog(LOG_ERROR, "[WebWidget][Runtime] %s", status_.toUtf8().constData());
        shutdown();
        return;
    }
    blog(LOG_INFO, "[WebWidget][Runtime] Created independent browser runtime");
}
WebWidgetRuntime::~WebWidgetRuntime() { shutdown(); }
void WebWidgetRuntime::shutdown()
{
    if (stopped_) return;
    stopped_ = true;
    blog(LOG_INFO, "[WebWidget][Runtime] Shutdown requested");
    if (worker_) {
        worker_->state->stopped = true;
        worker_->quit();
        worker_->wait();
        worker_.reset();
    }
    if (attached_) {
        attached_ = false;
        obs_source_remove_active_child(parent_, browser_);
        blog(LOG_INFO, "[WebWidget][Runtime] Browser child detached");
    }
    if (auto *browser = std::exchange(browser_, nullptr)) {
        obs_source_release(browser);
        blog(LOG_INFO, "[WebWidget][Runtime] Browser source released");
    }
    blog(LOG_INFO, "[WebWidget][Runtime] Shutdown complete");
}
bool WebWidgetRuntime::available() const { return browser_ != nullptr; }
QString WebWidgetRuntime::status() const { return worker_ ? worker_->state->getStatus() : status_; }
obs_source_t *WebWidgetRuntime::child() const { return browser_; }
void WebWidgetRuntime::render() { if (browser_) obs_source_video_render(browser_); }
void WebWidgetRuntime::resize(uint32_t width, uint32_t height)
{
    if (worker_) QMetaObject::invokeMethod(worker_->state, [state = worker_->state, width, height] {
        if (state->stopped) return;
        state->width = width;
        state->height = height;
    }, Qt::QueuedConnection);
    if (!browser_) return;
    obs_data_t *settings = obs_source_get_settings(browser_);
    obs_data_set_int(settings, "width", width); obs_data_set_int(settings, "height", height);
    obs_source_update(browser_, settings); obs_data_release(settings);
}

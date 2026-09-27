#include "web/web-widget-runtime.hpp"
#include <obs-module.h>
#include <QTest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <thread>
#include <mutex>
#include <cstdarg>

namespace {
QString browserUrl;
int created = 0, detached = 0, released = 0;
bool attached = false, failCreate = false, failAttach = false, moduleAvailable = true;
std::mutex logMutex;
QStringList logs;
obs_source_t *fakeSource = reinterpret_cast<obs_source_t *>(quintptr(1));
}
extern "C" {
obs_module_t *__wrap_obs_get_module(const char *) { return moduleAvailable ? reinterpret_cast<obs_module_t *>(quintptr(1)) : nullptr; }
bool __wrap_obs_enum_source_types(size_t index, const char **id) { *id = "browser_source"; return index == 0; }
obs_source_t *__wrap_obs_source_create_private(const char *, const char *, obs_data_t *settings)
{
    browserUrl = QString::fromUtf8(obs_data_get_string(settings, "url"));
    Q_ASSERT(QString::fromUtf8(obs_data_get_string(settings, "css")).isEmpty());
    if (failCreate) return nullptr;
    ++created;
    return fakeSource;
}
void __wrap_obs_source_set_muted(obs_source_t *, bool) {}
bool __wrap_obs_source_add_active_child(obs_source_t *, obs_source_t *) { attached = !failAttach; return attached; }
void __wrap_obs_source_remove_active_child(obs_source_t *, obs_source_t *) { Q_ASSERT(attached); attached = false; ++detached; }
void __wrap_obs_source_release(obs_source_t *) { Q_ASSERT(!attached); ++released; }
void __wrap_obs_source_video_render(obs_source_t *) {}
obs_data_t *__wrap_obs_source_get_settings(const obs_source_t *) { return obs_data_create(); }
void __wrap_obs_source_update(obs_source_t *, obs_data_t *) {}
void __wrap_blog(int, const char *format, ...)
{
    char buffer[1024];
    va_list args; va_start(args, format); vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    std::lock_guard lock(logMutex);
    logs.append(QString::fromUtf8(buffer));
}
}

class WebRuntimeTests : public QObject {
    Q_OBJECT
    std::shared_ptr<PluginRuntime> plugin;
    std::shared_ptr<std::atomic_bool> accepted;
    std::unique_ptr<WebWidgetRuntime> create()
    { return std::make_unique<WebWidgetRuntime>(nullptr, plugin, accepted, 640, 480); }
    QJsonObject bootstrap()
    {
        const QUrl url(browserUrl);
        QTcpSocket http;
        http.connectToHost(url.host(), url.port());
        if (!http.waitForConnected(2000)) return {};
        QByteArray target = url.path().toUtf8();
        target.replace("index.html", "bootstrap.json");
        http.write("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(url.port()) + "\r\n\r\n");
        http.waitForBytesWritten(2000);
        QByteArray response;
        while (http.waitForReadyRead(2000)) response += http.readAll();
        return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object();
    }
private Q_SLOTS:
    void init()
    {
        created = detached = released = 0;
        attached = failCreate = failAttach = false; moduleAvailable = true;
        { std::lock_guard lock(logMutex); logs.clear(); }
        plugin = std::make_shared<PluginRuntime>();
        accepted = std::make_shared<std::atomic_bool>(true);
    }
    void cleanup() { plugin->shutdown(); plugin.reset(); }
    void repeatedShutdownAndRefresh()
    {
        for (int i = 0; i < 10; ++i) {
            auto runtime = create();
            QVERIFY(runtime->available());
            runtime->resize(800, 600);
            runtime->shutdown();
            runtime->shutdown();
            QVERIFY(!runtime->child());
        }
        QCOMPARE(created, 10); QCOMPARE(detached, 10); QCOMPARE(released, 10);
        std::lock_guard lock(logMutex);
        QCOMPARE(logs.count("[WebWidget][Runtime] HTTP server stopped"), 10);
        QCOMPARE(logs.count("[WebWidget][Runtime] WebSocket server stopped"), 10);
        QCOMPARE(logs.count("[WebWidget][Runtime] Shutdown complete"), 10);
    }
    void destructionWithoutApplicationEventLoop()
    {
        // The UI waits for an OBS destruction thread and processes no queued calls.
        std::thread sourceThread([&] { auto runtime = create(); runtime->resize(900, 700); });
        sourceThread.join();
        QCOMPARE(detached, 1); QCOMPARE(released, 1);
    }
    void connectedClientRefreshAndPartialHttp()
    {
        for (int i = 0; i < 3; ++i) {
            auto runtime = create();
            const auto config = bootstrap();
            QVERIFY(!config.isEmpty());
            const QUrl url(browserUrl);
            QWebSocket client(QStringLiteral("http://127.0.0.1:%1").arg(url.port()));
            QSignalSpy messages(&client, &QWebSocket::textMessageReceived);
            client.open(QUrl(config.value("webSocketUrl").toString()));
            QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
            client.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "hello"}, {"capability", config.value("capability")}}).toJson()));
            QTRY_COMPARE(messages.size(), 1);
            client.sendTextMessage(QStringLiteral("{\"type\":\"ready\"}"));
            QTRY_COMPARE(runtime->status(), QStringLiteral("Web Widget ready"));
            auto backend = plugin->attach([](BackendAttachment::Delivery) {});
            backend->setEventTestEnabled(true);
            QTest::qWait(80);
            backend->injectSyntheticEvent(SyntheticEventKind::ChatMessage);
            QTRY_VERIFY(messages.size() >= 2);
            auto otherConsumer = plugin->subscribe();
            // Queue publication while the bridge subscription is being retired.
            backend->injectSyntheticEvent(SyntheticEventKind::Follow);
            QTcpSocket partial;
            partial.connectToHost(url.host(), url.port());
            QVERIFY(partial.waitForConnected(2000));
            partial.write("GET /"); partial.waitForBytesWritten(2000);
            runtime.reset();
            QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
            QTest::qWait(50);
            QVERIFY(!otherConsumer.takeBatch().events.empty());
            backend->close();
            QTcpSocket probe; probe.connectToHost(url.host(), url.port());
            QVERIFY(!probe.waitForConnected(200));
        }
        QCOMPARE(detached, 3); QCOMPARE(released, 3);
    }
    void failuresAndRuntimeShutdown()
    {
        moduleAvailable = false;
        { auto runtime = create(); QVERIFY(!runtime->available()); }
        moduleAvailable = true; failCreate = true;
        { auto runtime = create(); QVERIFY(!runtime->available()); }
        failCreate = false; failAttach = true;
        { auto runtime = create(); plugin->shutdown(); }
        QCOMPARE(created, 1); QCOMPARE(detached, 0); QCOMPARE(released, 1);
    }
};
QTEST_GUILESS_MAIN(WebRuntimeTests)
#include "web-runtime-tests.moc"

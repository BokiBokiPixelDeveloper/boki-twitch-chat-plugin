#include "web/web-widget-runtime.hpp"
#include "web/widget-package-importer.hpp"
#include <obs-module.h>
#include <QTest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTemporaryDir>
#include <QProcess>
#include <QCryptographicHash>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <cstdarg>

namespace {
QString browserUrl;
int created = 0, detached = 0, released = 0;
bool failCreate = false, failAttach = false, moduleAvailable = true;
std::mutex logMutex;
QStringList logs;
std::unordered_set<obs_source_t *> attachedSources;
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
    return reinterpret_cast<obs_source_t *>(quintptr(created));
}
void __wrap_obs_source_set_muted(obs_source_t *, bool) {}
bool __wrap_obs_source_add_active_child(obs_source_t *, obs_source_t *source)
{
    if (failAttach) return false;
    return attachedSources.insert(source).second;
}
void __wrap_obs_source_remove_active_child(obs_source_t *, obs_source_t *source)
{
    Q_ASSERT(attachedSources.erase(source) == 1);
    ++detached;
}
void __wrap_obs_source_release(obs_source_t *source) { Q_ASSERT(!attachedSources.contains(source)); ++released; }
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
    std::unique_ptr<WebWidgetRuntime> create(WebWidgetRuntime::WidgetSelection selection)
    { return std::make_unique<WebWidgetRuntime>(nullptr, plugin, accepted, 640, 480, std::move(selection)); }
    QByteArray get(const QString &relative)
    {
        const QUrl url(browserUrl); QTcpSocket http; http.connectToHost(url.host(), url.port());
        if (!http.waitForConnected(2000)) return {};
        const QByteArray base = url.path().left(url.path().lastIndexOf('/') + 1).toUtf8();
        http.write("GET " + base + relative.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(url.port()) + "\r\n\r\n");
        http.waitForBytesWritten(2000); QByteArray response;
        while (http.waitForReadyRead(2000)) response += http.readAll();
        return response.mid(response.indexOf("\r\n\r\n") + 4);
    }
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
        failCreate = failAttach = false; moduleAvailable = true;
        attachedSources.clear();
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
    void startupTimeoutIsTerminalAndReloadable()
    {
        auto runtime = create();
        QTRY_VERIFY_WITH_TIMEOUT(runtime->status().contains("failed"), 12000);
        QVERIFY(runtime->status().contains("bridge startup timed out"));
        const auto config = bootstrap();
        const QUrl url(browserUrl);
        QWebSocket client(QStringLiteral("http://127.0.0.1:%1").arg(url.port()));
        QSignalSpy messages(&client, &QWebSocket::textMessageReceived);
        client.open(QUrl(config.value("webSocketUrl").toString()));
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "hello"}, {"capability", config.value("capability")}}).toJson()));
        QTRY_COMPARE(messages.size(), 1);
        client.sendTextMessage(QStringLiteral("{\"type\":\"ready\"}"));
        QTest::qWait(80);
        QVERIFY(runtime->status().contains("failed"));
        runtime.reset();
        runtime = create();
        QCOMPARE(runtime->status(), QStringLiteral("Web Widget loading"));
        runtime.reset();
        QCOMPARE(created, 2); QCOMPARE(detached, 2); QCOMPARE(released, 2);
    }
    void genericAndDevelopmentBrowserExecution()
    {
        if (!qEnvironmentVariableIsSet("RUN_WIDGET_BROWSER_TESTS")) QSKIP("Set RUN_WIDGET_BROWSER_TESTS=1 for Chromium execution");
        QTemporaryDir temporary;
        auto package = std::make_shared<WidgetPackage>();
        package->originalRoot = temporary.path();
        package->entrypoints.html = "index.html";
        const QByteArray html = "<div id=\"generic\"></div><script>window.genericInitialized=typeof BokiChat==='object';</script>";
        QFile file(temporary.filePath("index.html")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(html); file.close();
        package->files.push_back({"index.html", QCryptographicHash::hash(html, QCryptographicHash::Sha256), quint64(html.size())});
        for (const auto &kind : {QStringLiteral("generic"), QStringLiteral("development")}) {
            auto runtime = kind == "generic" ? create({package, WidgetCompatibility::GenericWebWidget, {}, "generic-probe"}) : create();
            auto backend = plugin->attach([](BackendAttachment::Delivery) {});
            backend->setEventTestEnabled(true);
            QProcess browser;
            browser.start("node", {QStringLiteral(BROWSER_PROBE), browserUrl, kind});
            QVERIFY(browser.waitForStarted());
            for (int i = 0; i < 200 && browser.state() != QProcess::NotRunning; ++i) {
                QTest::qWait(100);
                if (runtime->status().contains("ready")) backend->injectSyntheticEvent(SyntheticEventKind::ChatMessage);
            }
            const auto output = browser.readAllStandardOutput() + browser.readAllStandardError();
            qInfo().noquote() << output;
            QCOMPARE(browser.state(), QProcess::NotRunning);
            QCOMPARE(browser.exitStatus(), QProcess::NormalExit);
            QVERIFY(output.contains("\"rows\":"));
            QCOMPARE(browser.exitCode(), 0);
            QVERIFY(runtime->status().contains("ready") || runtime->status().contains("disconnected"));
            backend->close();
        }
    }
    void scrapbookBrowserExecution()
    {
        if (!qEnvironmentVariableIsSet("RUN_WIDGET_BROWSER_TESTS")) QSKIP("Set RUN_WIDGET_BROWSER_TESTS=1 for Chromium execution");
        QTemporaryDir temporary; WidgetPackageStore store(temporary.filePath("store"));
        WidgetPackageImporter importer(store); QString error;
        const auto package = importer.importArchive(QStringLiteral(SCRAPBOOK_FIXTURE), &error);
        QVERIFY2(package, qPrintable(error));
        auto runtime = create({package, WidgetCompatibility::StreamElements, QStringLiteral("channel"), QStringLiteral("browser-probe")});
        auto backend = plugin->attach([](BackendAttachment::Delivery) {});
        backend->setEventTestEnabled(true);
        QProcess blocked;
        blocked.start("node", {QStringLiteral(BROWSER_PROBE), browserUrl, "blocked-script"});
        QVERIFY(blocked.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(blocked.state(), QProcess::NotRunning, 20000);
        qInfo().noquote() << blocked.readAllStandardOutput() + blocked.readAllStandardError();
        QCOMPARE(blocked.exitCode(), 0);
        QTRY_VERIFY(runtime->status().contains("failed"));
        QVERIFY(runtime->status().contains("script"));
        runtime.reset();
        runtime = create({package, WidgetCompatibility::StreamElements, QStringLiteral("channel"), QStringLiteral("browser-reload")});
        QProcess browser;
        browser.start("node", {QStringLiteral(BROWSER_PROBE), browserUrl});
        QVERIFY(browser.waitForStarted());
        for (int i = 0; i < 200 && browser.state() != QProcess::NotRunning; ++i) {
            QTest::qWait(100);
            if (runtime->status().contains("ready")) backend->injectSyntheticEvent(SyntheticEventKind::ChatMessage);
        }
        const auto output = browser.readAllStandardOutput() + browser.readAllStandardError();
        qInfo().noquote() << output;
        QCOMPARE(browser.state(), QProcess::NotRunning);
        QCOMPARE(browser.exitStatus(), QProcess::NormalExit);
        QVERIFY(output.contains("\"rows\":"));
        QCOMPARE(browser.exitCode(), 0);
        backend->close();
    }
    void scrapbookPackageIsServedWithoutChangingOriginals()
    {
        if (!QFile::exists(QStringLiteral(SCRAPBOOK_FIXTURE))) QSKIP("Local Scrapbook fixture is unavailable");
        QFile archive(QStringLiteral(SCRAPBOOK_FIXTURE)); QVERIFY(archive.open(QIODevice::ReadOnly));
        const auto before = QCryptographicHash::hash(archive.readAll(), QCryptographicHash::Sha256); archive.close();
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); WidgetPackageStore store(temporary.filePath("store"));
        WidgetPackageImporter importer(store); QString error; const auto package = importer.importArchive(QStringLiteral(SCRAPBOOK_FIXTURE), &error);
        QVERIFY2(package, qPrintable(error));
        auto first = create({package, WidgetCompatibility::StreamElements, QStringLiteral("channel"), QStringLiteral("instance-one")});
        QVERIFY(first->available());
        const auto wrapper = get(QStringLiteral("index.html"));
        QVERIFY(wrapper.contains("streamelements-adapter.js")); QVERIFY(wrapper.contains("code.jquery.com/jquery-3.7.1.min.js"));
        QVERIFY(wrapper.indexOf("streamelements-adapter.js") < wrapper.indexOf("package/js.txt"));
        const QUrl firstUrl(browserUrl); const auto capability = firstUrl.path().split('/').value(1).toUtf8();
        QTcpSocket script; script.connectToHost(firstUrl.host(), firstUrl.port()); QVERIFY(script.waitForConnected(2000));
        script.write("GET /" + capability + "/package/js.txt HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(firstUrl.port()) + "\r\n\r\n");
        script.waitForBytesWritten(2000); QByteArray response; while (script.waitForReadyRead(2000)) response += script.readAll();
        const auto javascript = response.mid(response.indexOf("\r\n\r\n") + 4);
        QVERIFY(javascript.contains("just followed")); QVERIFY(!javascript.contains("{followerAlertMessage}")); QVERIFY(javascript.contains("\\p{C}"));
        QFile installedScript(QDir(package->originalRoot).filePath("js.txt")); QVERIFY(installedScript.open(QIODevice::ReadOnly));
        const auto originalScript = installedScript.readAll(); installedScript.close();
        QVERIFY(installedScript.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(installedScript.write("tampered"), qint64(8)); installedScript.close();
        QCOMPARE(get(QStringLiteral("package/js.txt")), QByteArray("Not found"));
        QVERIFY(installedScript.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(installedScript.write(originalScript), qint64(originalScript.size())); installedScript.close();
        auto second = create({package, WidgetCompatibility::StreamElements, QStringLiteral("channel"), QStringLiteral("instance-two")});
        QVERIFY(second->available()); QVERIFY(first->available());
        second.reset(); first.reset();
        QVERIFY(archive.open(QIODevice::ReadOnly)); QCOMPARE(QCryptographicHash::hash(archive.readAll(), QCryptographicHash::Sha256), before);
        QVERIFY(store.package(package->id));
    }
};
QTEST_GUILESS_MAIN(WebRuntimeTests)
#include "web-runtime-tests.moc"

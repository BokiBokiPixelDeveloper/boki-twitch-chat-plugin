#include "updater/update-checker.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QtTest>
#include <obs.h>
#include <cstring>
#include <QStandardPaths>
#include <QtEndian>
#include <vector>

namespace {
struct Task { obs_task_t callback; void *data; };
std::vector<Task> tasks;
void drainUi()
{
    auto pending = std::move(tasks);
    tasks.clear();
    for (const auto &task : pending)
        task.callback(task.data);
}
class Reply final : public QNetworkReply {
public:
    explicit Reply(QObject *parent) : QNetworkReply(parent) { open(ReadOnly); }
    void abort() override {}
    void complete(QByteArray bytes, bool fail = false)
    {
        payload = std::move(bytes);
        if (fail)
            setError(ConnectionRefusedError, QStringLiteral("Test network failure"));
        setFinished(true);
        Q_EMIT finished();
    }
    qint64 bytesAvailable() const override { return payload.size() + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 max) override
    {
        const auto count = std::min(max, static_cast<qint64>(payload.size()));
        if (!count) return -1;
        std::memcpy(data, payload.constData(), count);
        payload.remove(0, count);
        return count;
    }
private:
    QByteArray payload;
};
class Network final : public QNetworkAccessManager {
public:
    Reply *last = nullptr;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &, QIODevice *) override
    {
        ++requests;
        last = new Reply(this);
        return last;
    }
};
QByteArray testBinary()
{
#ifdef _WIN32
    QByteArray bytes(256, '\0');
    bytes.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(128, bytes.data() + 0x3c);
    bytes.replace(128, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(0x8664, bytes.data() + 132);
    qToLittleEndian<quint16>(0x20b, bytes.data() + 152);
    return bytes;
#else
    return QByteArray("\x7f" "ELFverified test plugin");
#endif
}
const QByteArray binary = testBinary();
QByteArray manifest(QString version = QStringLiteral("2.0.0"))
{
    return QJsonDocument(QJsonObject{
        {"version", version}, {"platforms", QJsonObject{{postexit::platformKey(), QJsonObject{
            {"url", "https://example.invalid/plugin.so"},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(binary, QCryptographicHash::Sha256).toHex())},
            {"size", binary.size()}}}}}}).toJson();
}
}

// Exercise the production OBS queue boundary without requiring an OBS process.
extern "C" void obs_queue_task(obs_task_type type, obs_task_t task, void *data, bool wait)
{
    QCOMPARE(type, OBS_TASK_UI);
    QVERIFY(!wait);
    tasks.push_back({task, data});
}
extern "C" uint32_t obs_get_version() { return (32u << 24) | (2u << 16) | 2u; }

class UpdateCheckerTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() { drainUi(); }
    void lifecycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        int notifications = 0;
        UpdateChecker checker("1.0.0", [&] { ++notifications; });
        auto network = std::make_unique<Network>();
        auto *net = network.get();
        checker.network_ = std::move(network);
        checker.pendingDirectory_ = dir.path() + "/pending";
        checker.targetPath_ = dir.path() + "/" + postexit::pluginFileName();
        checker.launcher_ = [](const QString &, const QString &, int fd, QString &) { return fd >= 0; };
        QCOMPARE(checker.state(), UpdateChecker::State::Idle);
        checker.installAvailableUpdate();
        QCOMPARE(net->requests, 0);
        checker.checkForUpdates();
        QCOMPARE(checker.state(), UpdateChecker::State::Checking);
        QVERIFY(checker.busy());
        QVERIFY(checker.status().startsWith("Checking"));
        QCOMPARE(notifications, 0);
        QVERIFY(tasks.empty()); // No synchronous or queued rebuild from the button.
        checker.checkForUpdates();
        QCOMPARE(net->requests, 1);
        net->last->complete(manifest());
        QCOMPARE(checker.state(), UpdateChecker::State::Available);
        QVERIFY(checker.hasAvailableUpdate());
        QCOMPARE(notifications, 0);
        drainUi();
        QCOMPARE(notifications, 1);
        checker.installAvailableUpdate();
        QCOMPARE(checker.state(), UpdateChecker::State::Downloading);
        QVERIFY(checker.status().startsWith("Downloading"));
        QVERIFY(tasks.empty());
        checker.installAvailableUpdate();
        checker.checkForUpdates();
        QCOMPARE(net->requests, 2);
        net->last->complete(binary);
        QCOMPARE(checker.state(), UpdateChecker::State::Ready);
        QCOMPARE(checker.status(), QStringLiteral("Update ready – close OBS completely. Installation will start automatically after it exits."));
        QVERIFY(!checker.busy());
        QVERIFY(!checker.hasAvailableUpdate());
        QCOMPARE(checker.currentVersion_, QStringLiteral("1.0.0"));
        QFile staged(checker.pendingDirectory_ + "/" + postexit::pluginFileName());
        QVERIFY(staged.open(QIODevice::ReadOnly));
        QCOMPARE(staged.readAll(), binary);
        checker.checkForUpdates();
        checker.installAvailableUpdate();
        QCOMPARE(net->requests, 2);
        drainUi();
        QCOMPARE(notifications, 2);
    }
    void failures_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("network") << 0;
        QTest::newRow("size") << 1;
        QTest::newRow("hash") << 2;
        QTest::newRow("staging") << 3;
    }
    void failures()
    {
        QFETCH(int, failure);
        QTemporaryDir dir;
        UpdateChecker checker("1.0.0", [] {});
        auto network = std::make_unique<Network>();
        auto *net = network.get();
        checker.network_ = std::move(network);
        checker.pendingDirectory_ = dir.path() + "/pending";
        checker.targetPath_ = dir.path() + "/" + postexit::pluginFileName();
        checker.launcher_ = [](const QString &, const QString &, int fd, QString &) { return fd >= 0; };
        checker.checkForUpdates();
        net->last->complete(manifest());
        drainUi();
        checker.installAvailableUpdate();
        if (failure == 3) {
            QFile blocker(checker.pendingDirectory_ + "/" + postexit::pluginFileName());
            QVERIFY(QDir().mkpath(blocker.fileName()));
        }
        net->last->complete(failure == 1 ? QByteArray("short") : failure == 2 ? QByteArray(binary.size(), 'x') : binary,
                            failure == 0);
        QCOMPARE(checker.state(), UpdateChecker::State::Error);
        QVERIFY(!checker.busy());
        QVERIFY(checker.hasAvailableUpdate());
        if (failure != 3) QVERIFY(!QFile::exists(checker.pendingDirectory_ + "/" + postexit::pluginFileName()));
        checker.installAvailableUpdate();
        QCOMPARE(checker.state(), UpdateChecker::State::Downloading);
        QCOMPARE(net->requests, 3);
    }
    void pairedDownload_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("both-verified") << -1;
        QTest::newRow("helper-network") << 0;
        QTest::newRow("helper-size") << 1;
        QTest::newRow("helper-hash") << 2;
        QTest::newRow("helper-not-elf") << 3;
        QTest::newRow("helper-staging") << 4;
    }
    void pairedDownload()
    {
        QFETCH(int, failure);
        QTemporaryDir dir;
        const QByteArray helper = failure == 3 ? QByteArray("invalid helper") : testBinary() + "helper";
        auto object = QJsonDocument::fromJson(manifest()).object();
        auto platforms = object["platforms"].toObject();
        auto linux = platforms[postexit::platformKey()].toObject();
        linux["helper"] = QJsonObject{{"url", "https://example.invalid/helper.bin"}, {"size", helper.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(helper, QCryptographicHash::Sha256).toHex())}};
        platforms[postexit::platformKey()] = linux; object["platforms"] = platforms;
        UpdateChecker checker("1.0.0", [] {});
        checker.pendingDirectory_ = dir.path() + "/pending";
        checker.targetPath_ = dir.path() + "/" + postexit::pluginFileName();
        checker.network_ = std::make_unique<Network>();
        int launches = 0;
        checker.launcher_ = [&](const QString &, const QString &, int fd, QString &) { ++launches; return fd >= 0; };
        auto *net = static_cast<Network *>(checker.network_.get());
        checker.checkForUpdates(); net->last->complete(QJsonDocument(object).toJson());
        checker.installAvailableUpdate(); net->last->complete(binary);
        QCOMPARE(checker.state(), UpdateChecker::State::Downloading);
        QCOMPARE(launches, 0);
        QCOMPARE(net->requests, 3);
        QVERIFY(!QFile::exists(checker.pendingDirectory_ + "/pending.json"));
        QCOMPARE(postexit::acquireLock(checker.pendingDirectory_ + "/update.lock"), -1);
        checker.installAvailableUpdate(); checker.checkForUpdates();
        QCOMPARE(net->requests, 3);
        if (failure == 4) QVERIFY(QDir().mkpath(checker.pendingDirectory_ + "/" + postexit::helperFileName()));
        net->last->complete(failure == 1 ? helper + "x" : failure == 2 ? QByteArray(helper.size(), 'x') : helper, failure == 0);
        if (failure == -1) {
            QCOMPARE(checker.state(), UpdateChecker::State::Ready);
            QCOMPARE(launches, 1);
            const auto pending = postexit::readPending(checker.pendingDirectory_);
            QVERIFY(pending.helper.has_value());
            QCOMPARE(pending.helper->size, helper.size());
            QFile file(pending.helper->binary); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), helper);
        } else {
            QCOMPARE(checker.state(), UpdateChecker::State::Error);
            QCOMPARE(launches, 0);
            QVERIFY(!QFile::exists(checker.pendingDirectory_ + "/pending.json"));
            QVERIFY(!QFile::exists(checker.pendingDirectory_ + "/" + postexit::pluginFileName()));
            checker.installAvailableUpdate();
            QCOMPARE(checker.state(), UpdateChecker::State::Downloading);
            QCOMPARE(net->requests, 4);
        }
    }
    void malformedHelper()
    {
        for (const auto &helper : {QJsonValue(QJsonValue::Null), QJsonValue(QJsonObject{}),
            QJsonValue(QJsonObject{{"url", "file:///tmp/helper"}, {"sha256", QString(64, 'a')}, {"size", 123}}),
            QJsonValue(QJsonObject{{"url", "https://example.invalid/helper"}, {"sha256", QString(64, 'z')}, {"size", 123}})}) {
            auto object = QJsonDocument::fromJson(manifest()).object();
            auto platforms = object["platforms"].toObject();
            auto linux = platforms[postexit::platformKey()].toObject();
            linux["helper"] = helper; platforms[postexit::platformKey()] = linux; object["platforms"] = platforms;
            UpdateChecker checker("1.0.0", [] {});
            checker.network_ = std::make_unique<Network>();
            checker.checkForUpdates();
            static_cast<Network *>(checker.network_.get())->last->complete(QJsonDocument(object).toJson());
            QCOMPARE(checker.state(), UpdateChecker::State::Error);
            QVERIFY(!checker.hasAvailableUpdate());
        }
    }
    void concurrentDownloadAndPending()
    {
        QTemporaryDir dir;
        UpdateChecker first("1.0.0", [] {}), second("1.0.0", [] {});
        int launches = 0;
        int inheritedLock = -1;
        for (auto *checker : {&first, &second}) {
            checker->pendingDirectory_ = dir.path() + "/pending";
            checker->targetPath_ = dir.path() + "/" + postexit::pluginFileName();
            checker->network_ = std::make_unique<Network>();
            checker->launcher_ = [&](const QString &, const QString &, int fd, QString &) {
                ++launches;
                inheritedLock = postexit::duplicateLock(fd);
                return inheritedLock >= 0;
            };
            checker->checkForUpdates();
            static_cast<Network *>(checker->network_.get())->last->complete(manifest());
        }
        first.installAvailableUpdate();
        second.installAvailableUpdate();
        QCOMPARE(static_cast<Network *>(second.network_.get())->requests, 1);
        static_cast<Network *>(first.network_.get())->last->complete(binary);
        second.installAvailableUpdate();
        QCOMPARE(second.state(), UpdateChecker::State::Ready);
        QCOMPARE(launches, 1);
        QVERIFY(QFile::exists(first.pendingDirectory_ + "/pending.json"));
        postexit::releaseLock(inheritedLock);
    }
    void helperFailurePreservesPending()
    {
        QTemporaryDir dir;
        UpdateChecker checker("1.0.0", [] {});
        checker.pendingDirectory_ = dir.path() + "/pending";
        checker.targetPath_ = dir.path() + "/" + postexit::pluginFileName();
        checker.network_ = std::make_unique<Network>();
        auto *net = static_cast<Network *>(checker.network_.get());
        checker.launcher_ = [](const QString &, const QString &, int, QString &error) {
            error = "missing executable"; return false;
        };
        checker.checkForUpdates(); net->last->complete(manifest());
        checker.installAvailableUpdate(); net->last->complete(binary);
        QCOMPARE(checker.state(), UpdateChecker::State::Error);
        QVERIFY(QFile::exists(checker.pendingDirectory_ + "/pending.json"));
        checker.checkForUpdates();
        QCOMPARE(net->requests, 2);
        QVERIFY(checker.status().contains("missing executable"));
    }
    void resultSurvivesAutomaticCheck()
    {
        const auto path = postexit::stateDirectory() + "/last-update-result.json";
        QVERIFY(QDir().mkpath(postexit::stateDirectory()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"success":true,"fromVersion":"1.0","toVersion":"2.0","timestamp":"2026-01-01T00:00:00Z"})");
        file.close();
        UpdateChecker checker("2.0", [] {});
        checker.network_ = std::make_unique<Network>();
        QVERIFY(checker.status().contains("Update to 2.0 installed successfully."));
        checker.checkForUpdates();
        QVERIFY(checker.status().contains("Update to 2.0 installed successfully."));
        QVERIFY(!QFile::exists(path));
    }
    void manifestResults_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<bool>("fail");
        QTest::addColumn<bool>("current");
        QTest::newRow("current") << manifest("1.0.0") << false << true;
        QTest::newRow("invalid") << QByteArray("invalid") << false << false;
        QTest::newRow("incomplete") << QByteArray("{}") << false << false;
        QTest::newRow("no-platform-package") << QByteArray(R"({"schema":1,"version":"2.0","platforms":{}})") << false << false;
        auto incompatible = QJsonDocument::fromJson(manifest()).object();
        auto platforms = incompatible["platforms"].toObject();
        auto platform = platforms[postexit::platformKey()].toObject();
        platform["obs"] = QJsonObject{{"minVersion", "33.0.0"}, {"maxMajorVersion", 33}};
        platforms[postexit::platformKey()] = platform; incompatible["platforms"] = platforms;
        QTest::newRow("incompatible-obs") << QJsonDocument(incompatible).toJson() << false << false;
#ifdef _WIN32
        QTest::newRow("old-linux-manifest") << QByteArray(
            R"({"schema":1,"version":"2.0","platforms":{"linux-x86_64":{"url":"https://example.invalid/plugin.so"}}})")
            << false << false;
#endif
        QTest::newRow("network") << QByteArray() << true << false;
    }
    void manifestResults()
    {
        QFETCH(QByteArray, payload);
        QFETCH(bool, fail);
        QFETCH(bool, current);
        int notifications = 0;
        auto checker = std::make_unique<UpdateChecker>("1.0.0", [&] { ++notifications; });
        auto network = std::make_unique<Network>();
        auto *net = network.get();
        checker->network_ = std::move(network);
        checker->checkForUpdates();
        net->last->complete(payload, fail);
        QCOMPARE(checker->state(), current ? UpdateChecker::State::Current : UpdateChecker::State::Error);
        QVERIFY(!checker->busy());
        QVERIFY(!checker->hasAvailableUpdate());
        QCOMPARE(notifications, 0);
        checker.reset();
        drainUi(); // Pending tasks must not dereference the destroyed source/checker.
        QCOMPARE(notifications, 0);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir isolated;
    qputenv("XDG_CACHE_HOME", isolated.path().toUtf8());
    qputenv("XDG_STATE_HOME", isolated.path().toUtf8());
    UpdateCheckerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "updater-tests.moc"

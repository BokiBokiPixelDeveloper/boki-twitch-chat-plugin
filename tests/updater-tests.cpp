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
const QByteArray binary("verified test plugin");
QByteArray manifest(QString version = QStringLiteral("2.0.0"))
{
    return QJsonDocument(QJsonObject{
        {"version", version}, {"platforms", QJsonObject{{"linux-x86_64", QJsonObject{
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
        QCOMPARE(checker.state(), UpdateChecker::State::Idle);
        checker.installAvailableUpdate();
        QCOMPARE(net->requests, 0);
        checker.checkForUpdates();
        QCOMPARE(checker.state(), UpdateChecker::State::Checking);
        QVERIFY(checker.busy());
        QVERIFY(checker.status().startsWith("Suche"));
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
        QVERIFY(checker.status().startsWith("Lade"));
        QVERIFY(tasks.empty());
        checker.installAvailableUpdate();
        checker.checkForUpdates();
        QCOMPARE(net->requests, 2);
        net->last->complete(binary);
        QCOMPARE(checker.state(), UpdateChecker::State::Ready);
        QCOMPARE(checker.status(), QStringLiteral("Update bereit – OBS neu starten"));
        QVERIFY(!checker.busy());
        QVERIFY(!checker.hasAvailableUpdate());
        QCOMPARE(checker.currentVersion_, QStringLiteral("1.0.0"));
        QFile staged(checker.pendingDirectory_ + "/bokis-twitch-chat-plugin.so");
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
        checker.checkForUpdates();
        net->last->complete(manifest());
        drainUi();
        checker.installAvailableUpdate();
        if (failure == 3) {
            QFile blocker(checker.pendingDirectory_);
            QVERIFY(blocker.open(QIODevice::WriteOnly));
        }
        net->last->complete(failure == 1 ? QByteArray("short") : failure == 2 ? QByteArray(binary.size(), 'x') : binary,
                            failure == 0);
        QCOMPARE(checker.state(), UpdateChecker::State::Error);
        QVERIFY(!checker.busy());
        QVERIFY(checker.hasAvailableUpdate());
        QVERIFY(!QFile::exists(checker.pendingDirectory_ + "/bokis-twitch-chat-plugin.so"));
        checker.installAvailableUpdate();
        QCOMPARE(checker.state(), UpdateChecker::State::Downloading);
        QCOMPARE(net->requests, 3);
    }
    void manifestResults_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<bool>("fail");
        QTest::addColumn<bool>("current");
        QTest::newRow("current") << manifest("1.0.0") << false << true;
        QTest::newRow("invalid") << QByteArray("invalid") << false << false;
        QTest::newRow("incomplete") << QByteArray("{}") << false << false;
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
QTEST_GUILESS_MAIN(UpdateCheckerTests)
#include "updater-tests.moc"

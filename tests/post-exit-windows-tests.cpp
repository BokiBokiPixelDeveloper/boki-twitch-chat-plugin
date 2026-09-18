#include "updater/windows-platform.hpp"
#include "updater/post-exit-internal.hpp"
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <shlobj.h>
#include <cstdio>

namespace {
QByteArray pe()
{
    QByteArray bytes(256, '\0');
    bytes.replace(0, 2, "MZ"); bytes[0x3c] = 64;
    bytes.replace(64, 4, QByteArray("PE\0\0", 4));
    bytes[68] = char(0x64); bytes[69] = char(0x86);
    bytes[88] = char(0x0b); bytes[89] = char(0x02);
    return bytes;
}
void put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
}
QByteArray get(const QString &path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
QString hash(const QByteArray &bytes)
{ return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
struct Fixture {
    QTemporaryDir temp{QDir::tempPath() + QString::fromUtf8("/Bokis ü 日本語 spaces-XXXXXX")};
    QString pending = temp.path() + "/pending", target = temp.path() + "/" + postexit::pluginFileName();
    QString backups = temp.path() + "/backups", result = temp.path() + "/last-update-result.json";
    QByteArray old = "old plugin", payload = pe() + "new plugin";
    postexit::Pending p{"1.0", "2.0", hash(payload), pending + "/" + postexit::pluginFileName(),
                       target, payload.size(), std::nullopt};
    Fixture() {
        if (!temp.isValid() || !QDir().mkpath(pending)) qFatal("Temporary directory unavailable");
        put(target, old); put(p.binary, payload); save();
    }
    void save() { QString error; if (!postexit::writePending(pending, p, error)) qFatal("%s", qPrintable(error)); }
    void addHelper(const QByteArray &bytes = pe() + "new helper") {
        p.helper = postexit::PendingFile{hash(bytes), pending + "/" + postexit::helperFileName(),
            temp.path() + "/" + postexit::helperFileName(), bytes.size()};
        put(p.helper->target, "old helper"); put(p.helper->binary, bytes); save();
    }
    bool install(QString &error, const postexit::RenameOperation &rename = {}) {
        return postexit::install(pending, backups, result, [] { return true; }, error, rename);
    }
};
bool ready(QProcess &child, const QStringList &arguments)
{
    child.start(QCoreApplication::applicationFilePath(), arguments);
    return child.waitForStarted() && child.waitForReadyRead() && child.readAllStandardOutput().trimmed() == "ready";
}
bool finish(QProcess &child) { child.write("exit\n"); return child.waitForFinished(); }
}

class WindowsPostExitTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void knownFolderResolution()
    {
        QStandardPaths::setTestModeEnabled(false);
        const auto restoreTestMode = qScopeGuard([] { QStandardPaths::setTestModeEnabled(true); });
        PWSTR path = nullptr;
        QVERIFY(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path)));
        const auto expected = QDir::fromNativeSeparators(QString::fromWCharArray(path)) + "/BokisTwitchChatPlugin";
        CoTaskMemFree(path);
        QCOMPARE(postexit::windows::localRoot(), expected); // Read-only: never creates real user state.
    }
    void pathsAndPending()
    {
        QCOMPARE(postexit::platformKey(), "windows-x86_64");
        QVERIFY(QDir::isAbsolutePath(postexit::windows::localRoot()));
        QCOMPARE(postexit::cacheDirectory(), postexit::windows::localRoot() + "/cache/pending");
        QCOMPARE(postexit::dataDirectory(), postexit::windows::localRoot() + "/backups");
        QCOMPARE(postexit::stateDirectory(), postexit::windows::localRoot() + "/state");
        Fixture f; f.addHelper();
        const auto p = postexit::readPending(f.pending);
        QCOMPARE(p.target, f.target); QCOMPARE(p.helper->target, f.p.helper->target);
        QVERIFY(p.target.contains(QString::fromUtf8("日本語")));
        QCOMPARE(postexit::windows::quoteArgument("a b\\"), QString("\"a b\\\\\""));
        QCOMPARE(postexit::windows::quoteArgument("a\"b"), QString("\"a\\\"b\""));
    }
    void binaryValidation()
    {
        QVERIFY(postexit::validBinary(pe()));
        QVERIFY(!postexit::validBinary("MZ"));
        auto bad = pe(); bad[68] = 0; QVERIFY(!postexit::validBinary(bad)); // wrong machine
        bad = pe(); bad[64] = 0; QVERIFY(!postexit::validBinary(bad)); // signature
        bad = pe(); bad[89] = 0; QVERIFY(!postexit::validBinary(bad)); // PE32, not PE32+
        bad = pe(); bad[0x3f] = char(0xff); QVERIFY(!postexit::validBinary(bad));
    }
    void successfulPair()
    {
        Fixture f; f.addHelper(); QString error;
        const auto helper = get(f.p.helper->binary);
        QVERIFY2(f.install(error), qPrintable(error));
        QCOMPARE(get(f.target), f.payload); QCOMPARE(get(f.p.helper->target), helper);
        const auto backups = QDir(f.backups).entryList(QDir::Files);
        QCOMPARE(backups.size(), 2);
        for (const auto &name : backups)
            QCOMPARE(get(f.backups + "/" + name), name.endsWith(".dll") ? f.old : QByteArray("old helper"));
        QVERIFY(!QFile::exists(f.pending + "/pending.json"));
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        QVERIFY(!QFile::exists(f.p.binary)); QVERIFY(!QFile::exists(f.p.helper->binary));
        QVERIFY(QDir(f.temp.path()).entryList({"*.update-*", "*.rollback-*"}, QDir::Files).isEmpty());
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY(result["success"].toBool()); QVERIFY(result["helperUpdated"].toBool());
        QCOMPARE(result["toVersion"].toString(), "2.0");
        QVERIFY(!result["timestamp"].toString().isEmpty());
    }
    void rejectedPayloads_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("plugin-hash") << 0; QTest::newRow("plugin-size") << 1;
        QTest::newRow("invalid-pe") << 2; QTest::newRow("missing-plugin") << 3;
        QTest::newRow("helper-hash") << 4; QTest::newRow("helper-size") << 5;
        QTest::newRow("invalid-helper-pe") << 6; QTest::newRow("missing-helper") << 7;
        QTest::newRow("invalid-metadata") << 8; QTest::newRow("backup-failure") << 9;
    }
    void rejectedPayloads()
    {
        QFETCH(int, kind); Fixture f; f.addHelper(); QString error;
        if (kind == 0) f.p.sha256 = QString(64, '0');
        if (kind == 1) ++f.p.size;
        if (kind == 2) { auto bad = QByteArray(f.payload.size(), 'x'); put(f.p.binary, bad); f.p.sha256 = hash(bad); }
        if (kind == 3) QFile::remove(f.p.binary);
        if (kind == 4) f.p.helper->sha256 = QString(64, '0');
        if (kind == 5) ++f.p.helper->size;
        if (kind == 6) {
            auto bad = QByteArray(f.p.helper->size, 'x'); put(f.p.helper->binary, bad); f.p.helper->sha256 = hash(bad);
        }
        if (kind == 7) QFile::remove(f.p.helper->binary);
        if (kind == 8) f.p.target = f.temp.path() + "/other.dll";
        if (kind == 9) put(f.backups, "not a directory");
        f.save();
        QVERIFY(!f.install(error)); QVERIFY(!error.isEmpty());
        QCOMPARE(get(f.target), f.old); QCOMPARE(get(f.p.helper->target), QByteArray("old helper"));
        QVERIFY(!QJsonDocument::fromJson(get(f.result)).object()["success"].toBool(true));
        QVERIFY(QFile::exists(f.pending + "/pending.json"));
    }
    void locks()
    {
        Fixture f;
        int lock = postexit::acquireLock(f.pending + "/update.lock"); QVERIFY(lock >= 0);
        QCOMPARE(postexit::acquireLock(f.pending + "/update.lock"), -1);
        int inherited = postexit::duplicateLock(lock); QVERIFY(inherited >= 0);
        postexit::releaseLock(lock); QCOMPARE(postexit::acquireLock(f.pending + "/update.lock"), -1);
        postexit::releaseLock(inherited);
        lock = postexit::acquireLock(f.pending + "/update.lock"); QVERIFY(lock >= 0); postexit::releaseLock(lock);
        lock = postexit::acquireLock(f.target + ".use.lock", true); QVERIFY(lock >= 0);
        int second = postexit::acquireLock(f.target + ".use.lock", true); QVERIFY(second >= 0);
        QString error; QVERIFY(!f.install(error)); QCOMPARE(get(f.target), f.old);
        postexit::releaseLock(second); postexit::releaseLock(lock);
    }
    void exactProcessWait()
    {
        QProcess child; QVERIFY(ready(child, {"--wait-child"}));
        const auto pid = DWORD(child.processId());
        postexit::windows::Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        QVERIFY(process.valid()); const auto created = postexit::windows::creationTime(process.value);
        QVERIFY(!created.isEmpty());
        QVERIFY(!postexit::windows::waitForOrigin(process.value, pid, created, 0));
        QVERIFY(!postexit::windows::waitForOrigin(process.value, pid + 1, created, 0));
        QVERIFY(!postexit::windows::waitForOrigin(process.value, pid, "wrong-creation", 0));
        QVERIFY(finish(child));
        QVERIFY(postexit::windows::waitForOrigin(process.value, pid, created, 1000));
        QVERIFY(!postexit::windows::waitForOrigin(process.value, pid + 1, created, 0));
    }
    void inaccessibleProcessClassification()
    {
        QString error;
        QVERIFY(postexit::windows::mappingDecision(false, true, false, false, error));
        QVERIFY(error.isEmpty());
        QVERIFY(!postexit::windows::mappingDecision(true, true, false, false, error));
        QVERIFY(postexit::windows::mappingDecision(true, false, false, false, error));
        QVERIFY(!postexit::windows::mappingDecision(false, true, true, true, error));
    }
    void mappedDllBlocks()
    {
        Fixture f; put(f.target, get(TEST_IMAGE_PATH));
        const auto alias = f.temp.path() + "/alias.dll";
        QCOMPARE(postexit::detail::linkFile(f.target, alias), 0);
        QProcess mapper; QVERIFY(ready(mapper, {"--map-child", alias}));
        QString error; QVERIFY(!f.install(error)); QCOMPARE(get(f.target), get(TEST_IMAGE_PATH));
        QVERIFY(finish(mapper));
        QVERIFY2(f.install(error), qPrintable(error)); QCOMPARE(get(f.target), f.payload);
    }
    void rollback_data()
    {
        QTest::addColumn<int>("failAt");
        QTest::newRow("first-replacement") << 1; QTest::newRow("second-replacement") << 2;
    }
    void rollback()
    {
        QFETCH(int, failAt); Fixture f; f.addHelper(); QString error; int calls = 0;
        bool helperSwapped = false;
        QVERIFY(!f.install(error, [&](const QString &from, const QString &to) {
            ++calls;
            if (calls == 2) helperSwapped = get(f.p.helper->target) != "old helper";
            return calls == failAt ? -1 : postexit::detail::renameFile(from, to);
        }));
        if (failAt == 2) QVERIFY(helperSwapped);
        QCOMPARE(get(f.target), f.old); QCOMPARE(get(f.p.helper->target), QByteArray("old helper"));
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        QVERIFY(!QJsonDocument::fromJson(get(f.result)).object()["rollbackFailed"].toBool(true));
    }
    void journalRecovery()
    {
        Fixture f; f.addHelper();
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-child", f.pending, f.backups, f.result});
        QVERIFY(child.waitForFinished()); QCOMPARE(child.exitCode(), 99);
        QVERIFY(QFile::exists(f.pending + "/transaction.json"));
        QCOMPARE(get(f.target), f.old); QCOMPARE(get(f.p.helper->target), get(f.p.helper->binary));
        QString error; QVERIFY2(f.install(error), qPrintable(error));
        QCOMPARE(get(f.target), f.payload);
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        const auto backups = QDir(f.backups).entryList(QDir::Files);
        QCOMPARE(backups.size(), 4); // Both attempts backed up the restored originals.
        for (const auto &name : backups)
            QCOMPARE(get(f.backups + "/" + name), name.endsWith(".dll") ? f.old : QByteArray("old helper"));
    }
    void committedCleanupRecovery()
    {
        Fixture f; f.addHelper(); QString error;
        const auto result = f.result;
        put(f.temp.path() + "/blocked", "not a directory"); f.result = f.temp.path() + "/blocked/result.json";
        QVERIFY(!f.install(error)); QCOMPARE(get(f.target), f.payload);
        QVERIFY(QFile::exists(f.pending + "/transaction.json"));
        f.result = result; QVERIFY2(f.install(error), qPrintable(error));
        QCOMPARE(QDir(f.backups).entryList(QDir::Files).size(), 2);
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
    }
    void temporaryRunnerLifecycle_data()
    {
        QTest::addColumn<bool>("selfUpdate");
        QTest::newRow("plugin-only") << false; QTest::newRow("helper-self-update") << true;
    }
    void temporaryRunnerLifecycle()
    {
        QFETCH(bool, selfUpdate); Fixture f;
        const auto installedHelper = f.temp.path() + "/" + postexit::helperFileName();
        const auto original = get(HELPER_PATH);
        QVERIFY(postexit::validBinary(original.first(4096)));
        if (selfUpdate) f.addHelper(original + "new helper marker");
        put(installedHelper, original);
        QProcess origin;
        QVERIFY2(ready(origin, {"--launch-child", f.pending, installedHelper, f.backups, f.result}),
                 qPrintable(QString::fromUtf8(origin.readAllStandardError())));
        QCOMPARE(get(f.target), f.old); QCOMPARE(get(installedHelper), original);
        QCOMPARE(postexit::acquireLock(f.pending + "/update.lock"), -1);
        QVERIFY(!QFile::exists(f.result));
        QProcess duplicate;
        duplicate.start(QCoreApplication::applicationFilePath(),
            {"--launch-child", f.pending, installedHelper, f.backups, f.result});
        QVERIFY(duplicate.waitForFinished());
        QCOMPARE(duplicate.exitCode(), 9);
        QCOMPARE(get(f.target), f.old);
        // The running EXE is the copy: the installed helper is writable while it waits.
        postexit::windows::Handle probe(CreateFileW(installedHelper.toStdWString().c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        QVERIFY(probe.valid()); CloseHandle(probe.value); probe.value = INVALID_HANDLE_VALUE;
        const auto runners = QDir(f.temp.path() + "/runners").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(runners.size(), 1);
        QVERIFY(finish(origin));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.result), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(f.pending + "/pending.json"), 5000);
        QCOMPARE(get(f.target), f.payload);
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY2(result["success"].toBool(), qPrintable(result["error"].toString()));
        QCOMPARE(result["helperUpdated"].toBool(), selfUpdate);
        QCOMPARE(get(installedHelper), selfUpdate ? original + "new helper marker" : original);
        // Wait for the detached worker to release its lock before fixture cleanup.
        int lock = -1;
        QTRY_VERIFY_WITH_TIMEOUT(lock >= 0 || (lock = postexit::acquireLock(f.pending + "/update.lock")) >= 0, 5000);
        postexit::releaseLock(lock);
        const auto runnerDir = f.temp.path() + "/runners/" + runners.first();
        QTRY_VERIFY_WITH_TIMEOUT(QDir(runnerDir).removeRecursively(), 5000);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); QStandardPaths::setTestModeEnabled(true);
    const auto args = app.arguments();
    if (args.size() > 1 && args[1].endsWith("-child")) {
        HMODULE mapped = nullptr;
        if (args[1] == "--map-child") {
            mapped = LoadLibraryW(args[2].toStdWString().c_str()); if (!mapped) return 8;
        } else if (args[1] == "--launch-child") {
            // The runner must work without Qt, OBS, or developer DLLs on PATH.
            qputenv("PATH", qgetenv("SystemRoot") + "\\System32");
            const int lock = postexit::acquireLock(args[2] + "/update.lock"); QString error;
            if (lock < 0 || !postexit::windows::launchRunner(args[2], args[3], lock, error, args[4], args[5])) {
                std::fprintf(stderr, "%s\n", qPrintable(error)); return 9;
            }
            postexit::releaseLock(lock);
        } else if (args[1] == "--crash-child") {
            QString error;
            postexit::install(args[2], args[3], args[4], [] { return true; }, error,
                [](const QString &from, const QString &to) -> int {
                    if (postexit::detail::renameFile(from, to) != 0) return -1;
                    ExitProcess(99); // Crash after helper replacement with a prepared journal.
                });
            return 10;
        }
        std::printf("ready\n"); std::fflush(stdout); std::getchar();
        if (mapped) FreeLibrary(mapped);
        return 0;
    }
    WindowsPostExitTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "post-exit-windows-tests.moc"

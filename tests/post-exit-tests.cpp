#include "updater/post-exit.hpp"
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <QJsonArray>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>

namespace {
const QByteArray oldBytes("old working plugin"), newBytes("\x7f" "ELFnew plugin");
void put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("fixture write failed");
}
QByteArray get(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
struct Fixture {
    QTemporaryDir temp;
    QString pending = temp.path() + "/pending", target = temp.path() + "/bokis-twitch-chat-plugin.so";
    QString backups = temp.path() + "/backups", result = temp.path() + "/result.json";
    postexit::Pending p{"1.0", "2.0", QString::fromLatin1(QCryptographicHash::hash(newBytes, QCryptographicHash::Sha256).toHex()),
                        pending + "/bokis-twitch-chat-plugin.so", target, newBytes.size(), std::nullopt};
    Fixture() { QDir().mkpath(pending); put(target, oldBytes); put(p.binary, newBytes); save(); }
    void addHelper(const QByteArray &payload = QByteArray("\x7f" "ELFnew helper")) {
        p.helper = postexit::PendingFile{QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()),
            pending + "/bokis-twitch-chat-updater", temp.path() + "/bokis-twitch-chat-updater", payload.size()};
        put(p.helper->target, "old helper"); put(p.helper->binary, payload); save();
    }
    void save() { QString error; if (!postexit::writePending(pending, p, error)) qFatal("metadata write failed"); }
};
}
class PostExitTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void validInstall()
    {
        Fixture f; QString error; bool waited = false;
        QVERIFY(postexit::install(f.pending, f.backups, f.result, [&] {
            waited = true;
            return get(f.target) == oldBytes && !QFile::exists(f.backups);
        }, error));
        QVERIFY(waited);
        QCOMPARE(get(f.target), newBytes);
        const auto backups = QDir(f.backups).entryList({"*.so"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        QCOMPARE(get(f.backups + "/" + backups.first()), oldBytes);
        QVERIFY(!QFile::exists(f.p.binary));
        QVERIFY(!QFile::exists(f.pending + "/pending.json"));
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY(result["success"].toBool());
        QCOMPARE(result["fromVersion"].toString(), "1.0");
        QCOMPARE(result["toVersion"].toString(), "2.0");
        QVERIFY(!result["timestamp"].toString().isEmpty());
        struct stat st{}; QVERIFY(stat(QFile::encodeName(f.target).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, 0755u);
    }
    void failures_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("alive") << 0;
        QTest::newRow("sha256") << 1;
        QTest::newRow("size") << 2;
        QTest::newRow("missing-binary") << 3;
        QTest::newRow("backup-fails-before-rename") << 4;
        QTest::newRow("missing-metadata") << 5;
        QTest::newRow("not-elf") << 6;
        QTest::newRow("another-instance") << 7;
    }
    void failures()
    {
        QFETCH(int, kind);
        Fixture f; QString error; int useLock = -1;
        if (kind == 1) { f.p.sha256 = QString(64, '0'); f.save(); }
        if (kind == 2) { ++f.p.size; f.save(); }
        if (kind == 3) QFile::remove(f.p.binary);
        if (kind == 4) put(f.backups, "block directory creation");
        if (kind == 5) QFile::remove(f.pending + "/pending.json");
        if (kind == 6) put(f.p.binary, QByteArray(newBytes.size(), 'x'));
        if (kind == 7) useLock = postexit::acquireLock(f.target + ".use.lock", true);
        QVERIFY(!postexit::install(f.pending, f.backups, f.result, [kind] { return kind != 0; }, error));
        if (useLock >= 0) close(useLock);
        QVERIFY(!error.isEmpty());
        QCOMPARE(get(f.target), oldBytes);
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY(!result["success"].toBool(true));
        QVERIFY(!result["error"].toString().isEmpty());
        if (kind != 5) QVERIFY(QFile::exists(f.pending + "/pending.json"));
    }
    void duplicateLock()
    {
        Fixture f;
        const int lock = postexit::acquireLock(f.pending + "/update.lock");
        QVERIFY(lock >= 0);
        QCOMPARE(postexit::acquireLock(f.pending + "/update.lock"), -1);
        close(lock);
        const int again = postexit::acquireLock(f.pending + "/update.lock");
        QVERIFY(again >= 0); close(again);
    }
    void fallbackIdentity()
    {
        QVERIFY(!postexit::processStart(getpid()).isEmpty());
        QVERIFY(postexit::waitForProcess(getpid(), "different-start-time", -1));
        QVERIFY(!postexit::waitForProcess(getpid(), {}, -1));
    }
    void detachedLifecycle_data()
    {
        QTest::addColumn<bool>("selfUpdate");
        QTest::newRow("plugin-only") << false;
        QTest::newRow("running-helper-replaces-itself") << true;
    }
    void detachedLifecycle()
    {
        QFETCH(bool, selfUpdate);
        Fixture f;
        QByteArray helperBytes;
        if (selfUpdate) {
            helperBytes = get(HELPER_PATH) + "new helper image marker";
            f.addHelper(helperBytes);
            put(f.p.helper->target, get(HELPER_PATH));
            QVERIFY(QFile::setPermissions(f.p.helper->target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("XDG_STATE_HOME", f.temp.path() + "/state");
        env.insert("XDG_DATA_HOME", f.temp.path() + "/data");
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(), {"--launch-child", f.pending});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForReadyRead());
        QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
        QCOMPARE(get(f.target), oldBytes);
        QCOMPARE(postexit::acquireLock(f.pending + "/update.lock"), -1);
        // A second helper for the same pending state exits without installing.
        QProcess duplicate;
        duplicate.start(HELPER_PATH, {f.pending, QString::number(child.processId()), postexit::processStart(child.processId()), "-1", "-1"});
        QVERIFY(duplicate.waitForFinished());
        QCOMPARE(duplicate.exitCode(), 3);
        QCOMPARE(get(f.target), oldBytes);
        child.write("exit\n");
        QVERIFY(child.waitForFinished());
        QTRY_COMPARE_WITH_TIMEOUT(get(f.target), newBytes, 5000);
        const auto result = f.temp.path() + "/state/bokis-twitch-chat-plugin/last-update-result.json";
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(result), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(f.pending + "/pending.json"), 5000);
        QVERIFY(QJsonDocument::fromJson(get(result)).object()["success"].toBool());
        if (selfUpdate) {
            QCOMPARE(get(f.p.helper->target), helperBytes);
            QVERIFY(!QFile::exists(f.p.helper->binary));
            QVERIFY(QJsonDocument::fromJson(get(result)).object()["helperUpdated"].toBool());
        }
    }
    void detachedOriginIdentity()
    {
        Fixture f;
        QProcess mapper;
        mapper.start(QCoreApplication::applicationFilePath(), {"--map-child", f.target});
        QVERIFY(mapper.waitForReadyRead());
        QCOMPARE(mapper.readAllStandardOutput(), QByteArray("mapped\n"));
        QProcess origin;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("XDG_STATE_HOME", f.temp.path() + "/state");
        env.insert("XDG_DATA_HOME", f.temp.path() + "/data");
        origin.setProcessEnvironment(env);
        origin.start(QCoreApplication::applicationFilePath(), {"--launch-child", f.pending});
        QVERIFY(origin.waitForReadyRead());
        QCOMPARE(origin.readAllStandardOutput(), QByteArray("ready\n"));
        origin.write("exit\n");
        QVERIFY(origin.waitForFinished());
        const auto result = f.temp.path() + "/state/bokis-twitch-chat-plugin/last-update-result.json";
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(result), 5000);
        mapper.write("exit\n");
        QVERIFY(mapper.waitForFinished());
        const auto outcome = QJsonDocument::fromJson(get(result)).object();
        QVERIFY(!outcome["success"].toBool(true));
        QVERIFY2(outcome["error"].toString().contains("gemappt"), qPrintable(outcome["error"].toString()));
        QCOMPARE(get(f.target), oldBytes);
    }
    void helperTransaction()
    {
        Fixture f; f.addHelper(); QString error;
        const auto stagedHelper = get(f.p.helper->binary);
        const auto metadata = postexit::readPending(f.pending);
        QVERIFY(metadata.helper.has_value());
        QCOMPARE(QJsonDocument::fromJson(get(f.pending + "/pending.json")).object()["schema"].toInt(), 2);
        QVERIFY2(postexit::install(f.pending, f.backups, f.result, [] { return true; }, error), qPrintable(error));
        QCOMPARE(get(f.target), newBytes);
        QCOMPARE(get(f.p.helper->target), stagedHelper);
        const auto backupNames = QDir(f.backups).entryList(QDir::Files);
        QCOMPARE(backupNames.size(), 2);
        for (const auto &name : backupNames)
            QCOMPARE(get(f.backups + "/" + name), name.endsWith(".so") ? oldBytes : QByteArray("old helper"));
        QVERIFY(!QFile::exists(f.p.helper->binary));
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        QVERIFY(QJsonDocument::fromJson(get(f.result)).object()["helperUpdated"].toBool());
    }
    void helperFailures_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("helper-hash") << 0;
        QTest::newRow("helper-size") << 1;
        QTest::newRow("helper-missing") << 2;
        QTest::newRow("helper-not-elf") << 3;
        QTest::newRow("first-rename") << 4;
        QTest::newRow("second-rename-rolls-helper-back") << 5;
    }
    void helperFailures()
    {
        QFETCH(int, kind);
        Fixture f; f.addHelper(); QString error;
        if (kind == 0) f.p.helper->sha256 = QString(64, '0');
        if (kind == 1) ++f.p.helper->size;
        if (kind == 2) QFile::remove(f.p.helper->binary);
        if (kind == 3) put(f.p.helper->binary, QByteArray(f.p.helper->size, 'x'));
        f.save();
        int renames = 0;
        bool helperWasSwapped = false;
        auto rename = [&](const QString &from, const QString &to) {
            ++renames;
            if (renames == 2) helperWasSwapped = get(f.p.helper->target) != "old helper";
            if ((kind == 4 && renames == 1) || (kind == 5 && renames == 2)) { errno = EIO; return -1; }
            return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData());
        };
        QVERIFY(!postexit::install(f.pending, f.backups, f.result, [] { return true; }, error, rename));
        QCOMPARE(get(f.target), oldBytes);
        QCOMPARE(get(f.p.helper->target), QByteArray("old helper"));
        if (kind == 5) QVERIFY(helperWasSwapped);
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY(!result["success"].toBool(true));
        QVERIFY(!result["helperUpdated"].toBool(true));
        QVERIFY(!result["rollbackFailed"].toBool(true));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(f.pending + "/pending.json"));
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        QVERIFY(QDir(f.temp.path()).entryList({"*.rollback-*", "*.update-*"}, QDir::Files).isEmpty());
    }
    void malformedJournalRequiresRecovery()
    {
        Fixture f; f.addHelper(); QString error;
        put(f.pending + "/transaction.json", "invalid interrupted state");
        QVERIFY(!postexit::install(f.pending, f.backups, f.result, [] { return true; }, error));
        QVERIFY(QJsonDocument::fromJson(get(f.result)).object()["rollbackFailed"].toBool());
        QCOMPARE(get(f.pending + "/transaction.json"), QByteArray("invalid interrupted state"));
        QCOMPARE(get(f.target), oldBytes);
        QCOMPARE(get(f.p.helper->target), QByteArray("old helper"));
    }
    void rollbackFailureIsExplicit()
    {
        Fixture f; f.addHelper(); QString error;
        int renames = 0;
        auto rename = [&](const QString &from, const QString &to) {
            if (++renames == 2) {
                // Simulate loss of the rollback inode, independently of the durable XDG backup.
                const auto links = QDir(f.temp.path()).entryList({"bokis-twitch-chat-updater.rollback-*"}, QDir::Files);
                if (links.size() != 1 || !QFile::remove(f.temp.path() + "/" + links.first())) qFatal("rollback fixture failed");
                errno = EIO; return -1;
            }
            return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData());
        };
        QVERIFY(!postexit::install(f.pending, f.backups, f.result, [] { return true; }, error, rename));
        const auto result = QJsonDocument::fromJson(get(f.result)).object();
        QVERIFY(!result["success"].toBool(true));
        QVERIFY(result["rollbackFailed"].toBool());
        QVERIFY(error.contains("Rollback unvollständig"));
        QVERIFY(QFile::exists(f.pending + "/transaction.json"));
        QCOMPARE(QDir(f.backups).entryList(QDir::Files).size(), 2);
        QCOMPARE(get(f.target), oldBytes);
    }
    void mappedByAnotherProcess()
    {
        Fixture f; f.addHelper(); QString error;
        const auto alias = f.temp.path() + "/alias with spaces";
        QVERIFY(::link(QFile::encodeName(f.target).constData(), QFile::encodeName(alias).constData()) == 0);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--map-child", alias});
        QVERIFY(child.waitForReadyRead());
        QCOMPARE(child.readAllStandardOutput(), QByteArray("mapped\n"));
        const bool installed = postexit::install(f.pending, f.backups, f.result, [] { return true; }, error);
        child.write("exit\n");
        QVERIFY(child.waitForFinished());
        QVERIFY(!installed);
        QVERIFY2(error.contains("gemappt"), qPrintable(error));
        QCOMPARE(get(f.target), oldBytes);
        QCOMPARE(get(f.p.helper->target), QByteArray("old helper"));
    }
    void mappingCandidates_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("same-user-non-obs-unreadable-maps") << QString("other") << true;
        QTest::newRow("obs-mapped-plugin") << QString("mapped") << false;
        QTest::newRow("obs-unreadable-maps") << QString("unreadable") << false;
        QTest::newRow("obs-readable-unmapped") << QString("clear") << true;
        QTest::newRow("exit-after-classification") << QString("exit") << true;
        QTest::newRow("pid-reused-after-classification") << QString("reuse") << true;
        QTest::newRow("zombie-after-classification") << QString("zombie") << true;
        QTest::newRow("exit-before-exe-access") << QString("no-exe") << true;
        QTest::newRow("obs-missing-maps-but-live") << QString("missing-maps") << false;
        QTest::newRow("same-inode-different-exe-path") << QString("alias") << false;
    }
    void mappingCandidates()
    {
        QFETCH(QString, kind); QFETCH(bool, allowed);
        Fixture f; QString error;
        const auto proc = f.temp.path() + "/proc";
        const auto process = proc + "/1340";
        QVERIFY(QDir().mkpath(process));
        QVERIFY(QDir().mkpath(proc + "/self"));
        put(proc + "/self/maps", "");
        const auto executable = f.temp.path() + "/obs";
        const auto other = f.temp.path() + "/other/obs"; // Same name is not identity.
        QVERIFY(QDir().mkpath(QFileInfo(other).absolutePath()));
        put(executable, "original executable"); put(other, "different executable");
        const auto statBytes = [](const QByteArray &start, const QByteArray &state = "S") {
            return "1340 (obs with spaces) " + state + ' ' + QByteArray("0 ").repeated(18) + start + " 0\n";
        };
        put(process + "/stat", statBytes("100"));
        if (kind != "no-exe") {
            if (kind == "alias")
                QVERIFY(::link(QFile::encodeName(executable).constData(), QFile::encodeName(process + "/exe").constData()) == 0);
            else
                QVERIFY(QFile::link(kind == "other" ? other : executable, process + "/exe"));
        }
        if (kind == "other" || kind == "unreadable" || kind == "exit" || kind == "reuse" || kind == "zombie") {
            // A directory gives a deterministic read failure even under root.
            QVERIFY(QDir().mkpath(process + "/maps"));
            QVERIFY(QFile::setPermissions(process + "/maps", QFileDevice::Permissions{}));
        } else if (kind != "missing-maps") {
            struct stat st{};
            QVERIFY(stat(QFile::encodeName(f.target).constData(), &st) == 0);
            put(process + "/maps", kind == "clear" ? QByteArray() :
                QStringLiteral("1000-2000 r--p 00000000 %1:%2 %3 /alias with spaces\n")
                    .arg(major(st.st_dev), 0, 16).arg(minor(st.st_dev), 0, 16).arg(st.st_ino).toUtf8());
        }
        int classified = 0;
        postexit::MappingScan scan{executable, proc, [&](const QString &path) {
            ++classified;
            if (kind == "exit") QFile::remove(path + "/stat");
            if (kind == "reuse") put(path + "/stat", statBytes(QByteArray::number(100 + classified)));
            if (kind == "zombie") put(path + "/stat", statBytes("100", "Z"));
        }};
        QCOMPARE(postexit::ensureNotMapped(f.target, error, scan), allowed);
        QCOMPARE(classified, kind == "other" || kind == "no-exe" ? 0 : 1);
        if (!allowed) QVERIFY2(!error.isEmpty(), "OBS candidate must fail closed");
        // Exercise the same scan through installation, including its primary lock.
        QCOMPARE(postexit::install(f.pending, f.backups, f.result, [] { return true; }, error, {}, scan), allowed);
        QCOMPARE(get(f.target), allowed ? newBytes : oldBytes);
        QFile::setPermissions(process + "/maps", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    void interruptedTransaction_data()
    {
        QTest::addColumn<bool>("committed");
        QTest::newRow("recover-half-swap") << false;
        QTest::newRow("finish-committed-cleanup") << true;
    }
    void interruptedTransaction()
    {
        QFETCH(bool, committed);
        Fixture f; f.addHelper(); QString error;
        const QString id(32, 'a');
        QJsonArray entries;
        for (const auto &file : {*f.p.helper, postexit::PendingFile{f.p.sha256, f.p.binary, f.target, f.p.size}}) {
            QVERIFY(::link(QFile::encodeName(file.target).constData(), QFile::encodeName(file.target + ".rollback-" + id).constData()) == 0);
            entries.append(QJsonObject{{"target", file.target}, {"sha256", file.sha256}});
            if (committed || file.target == f.p.helper->target) {
                put(file.target + ".update-" + id, get(file.binary));
                QVERIFY(::rename(QFile::encodeName(file.target + ".update-" + id).constData(), QFile::encodeName(file.target).constData()) == 0);
            }
        }
        put(f.pending + "/transaction.json", QJsonDocument(QJsonObject{{"schema", 1}, {"id", id}, {"version", f.p.version},
            {"phase", committed ? "committed" : "prepared"}, {"files", entries}}).toJson());
        if (committed) QFile::remove(f.p.helper->binary); // Interrupted pending cleanup.
        QVERIFY2(postexit::install(f.pending, f.backups, f.result, [] { return true; }, error), qPrintable(error));
        QCOMPARE(get(f.target), newBytes);
        QVERIFY(!QFile::exists(f.pending + "/transaction.json"));
        if (!committed) {
            const auto helpers = QDir(f.backups).entryList({"*updater"}, QDir::Files);
            QCOMPARE(helpers.size(), 1);
            QCOMPARE(get(f.backups + "/" + helpers.first()), QByteArray("old helper"));
        }
    }
    void execFailure()
    {
        Fixture f; QString error;
        const int lock = postexit::acquireLock(f.pending + "/update.lock");
        QVERIFY(!postexit::launch(f.pending, f.temp.path() + "/missing-helper", lock, error));
        close(lock);
        QCOMPARE(get(f.target), oldBytes);
        QVERIFY(!error.isEmpty());
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--map-child") {
        const int fd = open(argv[2], O_RDONLY);
        if (fd < 0) return 1;
        void *mapping = mmap(nullptr, 1, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) return 1;
        std::puts("mapped"); std::fflush(stdout);
        const auto ignored = std::getchar(); (void)ignored;
        munmap(mapping, 1); close(fd); return 0;
    }
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--launch-child") {
        const QString dir = QString::fromLocal8Bit(argv[2]);
        const int lock = postexit::acquireLock(dir + "/update.lock");
        QString error;
        const auto installedHelper = QFileInfo(dir).absolutePath() + "/bokis-twitch-chat-updater";
        if (!postexit::launch(dir, QFile::exists(installedHelper) ? installedHelper : QString(HELPER_PATH), lock, error)) return 1;
        close(lock);
        std::puts("ready"); std::fflush(stdout);
        return std::getchar() == EOF ? 1 : 0;
    }
    PostExitTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "post-exit-tests.moc"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "updater/post-exit.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <sys/sysmacros.h>
#include <vector>
#include <QSaveFile>
#include <QUuid>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdexcept>

namespace postexit {
namespace {
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
QString xdg(const char *variable, const QString &fallback)
{
    const QString path = qEnvironmentVariable(variable);
    return (QDir::isAbsolutePath(path) ? path : QDir::homePath() + fallback) + "/bokis-twitch-chat-plugin";
}
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void syncDirectory(const QString &path)
{
    Fd fd(open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(fd.value >= 0 && fsync(fd.value) == 0, "Verzeichnis konnte nicht synchronisiert werden");
}
void saveJson(const QString &path, const QJsonObject &object)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Verzeichnis konnte nicht erstellt werden");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson();
    require(file.open(QIODevice::WriteOnly), "Metadaten konnten nicht geöffnet werden");
    require(file.write(bytes) == bytes.size() && file.flush() && fsync(file.handle()) == 0,
            "Metadaten konnten nicht synchronisiert werden");
    require(file.commit(), "Metadaten konnten nicht atomar gespeichert werden");
    syncDirectory(QFileInfo(path).absolutePath());
}
void copySynced(QFile &input, const QString &path, mode_t mode)
{
    Fd output(open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    require(output.value >= 0, "Temporäre Datei/Backup konnte nicht erstellt werden");
    require(input.seek(0), "Quelldatei konnte nicht gelesen werden");
    while (!input.atEnd()) {
        const auto bytes = input.read(64 * 1024);
        require(!bytes.isEmpty(), "Lesefehler beim Kopieren");
        qint64 offset = 0;
        while (offset < bytes.size()) {
            const auto written = write(output.value, bytes.constData() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            require(written > 0, "Schreibfehler beim Kopieren");
            offset += written;
        }
    }
    require(fchmod(output.value, mode) == 0 && fsync(output.value) == 0, "Datei konnte nicht synchronisiert werden");
}
void verify(QFile &file, const PendingFile &p)
{
    require(file.size() == p.size, "Dateigröße stimmt nicht");
    require(file.seek(0) && file.read(4) == QByteArray("\x7f" "ELF", 4), "Keine ELF-Datei");
    require(file.seek(0), "Pending-Datei nicht lesbar");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file) && QString::fromLatin1(hash.result().toHex()) == p.sha256.toLower(), "SHA-256 stimmt nicht");
}
}
QString cacheDirectory() { return xdg("XDG_CACHE_HOME", "/.cache") + "/pending"; }
QString dataDirectory() { return xdg("XDG_DATA_HOME", "/.local/share") + "/backups"; }
QString stateDirectory() { return xdg("XDG_STATE_HOME", "/.local/state"); }
QString modulePath()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&modulePath), &info) || !info.dli_fname) return {};
    return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).canonicalFilePath();
}
int acquireLock(const QString &path, bool shared)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return -1;
    int fd = open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0 && flock(fd, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;
}
void holdProcessUseLock()
{
    // Deliberately retained until kernel process teardown, including after dlclose.
    // CLOEXEC prevents inheritance into the helper or other executed programs.
    static const int lock = acquireLock(modulePath() + ".use.lock", true);
    (void)lock;
}
namespace {
QString startAt(const QString &path)
{
    QFile file(path + "/stat");
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    // comm can contain spaces and parentheses. Field 22 follows the last ')'.
    const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 19 ? QString::fromLatin1(fields[19]) : QString();
}
}
QString processStart(pid_t pid) { return startAt(QStringLiteral("/proc/%1").arg(pid)); }
bool waitForProcess(pid_t pid, const QString &start, int pidfd)
{
    if (pid <= 1 || start.isEmpty()) return false;
    if (pidfd >= 0) {
        pollfd p{pidfd, POLLIN, 0};
        int status;
        do { status = poll(&p, 1, -1); } while (status < 0 && errno == EINTR);
        return status == 1 && (p.revents & POLLIN);
    }
    for (;;) {
        const auto current = processStart(pid);
        if (!current.isEmpty() && current != start) return true;
        if (current.isEmpty()) {
            if (kill(pid, 0) < 0 && errno == ESRCH) return true;
            return false; // Unreadable procfs is not proof of exit.
        }
        poll(nullptr, 0, 200);
    }
}
bool ensureNotMapped(const QString &target, QString &error, const MappingScan &scan)
{
    struct stat targetStat{};
    if (stat(QFile::encodeName(target).constData(), &targetStat) != 0) {
        error = "Plugin-Ziel konnte für die Prozessprüfung nicht gelesen werden";
        return false;
    }
    struct stat executableStat{};
    if (stat(QFile::encodeName(scan.executable).constData(), &executableStat) != 0) {
        error = "OBS-Executable konnte für die Prozessprüfung nicht gelesen werden";
        return false;
    }
    const auto executablePath = QFileInfo(scan.executable).canonicalFilePath();
    const QDir proc(scan.procRoot);
    if (!QFile::exists(scan.procRoot + "/self/maps")) { error = "procfs-Prozessprüfung nicht verfügbar"; return false; }
    for (const auto &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        pid.toLongLong(&numeric);
        if (!numeric) continue;
        const auto path = scan.procRoot + "/" + pid;
        // Pin this proc directory: subsequent accesses cannot follow a reused PID.
        Fd process(open(QFile::encodeName(path).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (process.value < 0) continue;
        const auto pinned = QStringLiteral("/proc/self/fd/%1").arg(process.value);
        const auto start = startAt(pinned);
        struct stat candidate{};
        const auto candidateExe = pinned + "/exe";
        if (stat(QFile::encodeName(candidateExe).constData(), &candidate) != 0) continue;
        const bool sameInode = candidate.st_dev == executableStat.st_dev && candidate.st_ino == executableStat.st_ino;
        if (!sameInode && (executablePath.isEmpty() || QFileInfo(candidateExe).canonicalFilePath() != executablePath)) continue;
        // Unknown/non-OBS executables never cause a maps-permission failure.
        // Cooperating instances are covered independently by the exclusive .use.lock.
        if (scan.beforeMaps) scan.beforeMaps(path);
        const auto exited = [&] {
            const auto current = startAt(pinned);
            if (!start.isEmpty() && !current.isEmpty() && current != start) return true;
            struct stat st{};
            if (stat(QFile::encodeName(pinned + "/stat").constData(), &st) != 0 &&
                (errno == ENOENT || errno == ESRCH)) return true;
            // A zombie has no executable or mappings, even while its proc directory exists.
            QFile state(pinned + "/stat");
            if (state.open(QIODevice::ReadOnly)) {
                const auto bytes = state.readAll();
                const auto status = bytes.mid(bytes.lastIndexOf(')') + 2, 1);
                return status == "Z" || status == "X";
            }
            return false;
        };
        QFile maps(pinned + "/maps");
        const bool opened = maps.open(QIODevice::ReadOnly);
        const auto contents = opened ? maps.readAll() : QByteArray(); // procfs reports size zero.
        if (exited()) continue;
        if (!opened || maps.error() != QFileDevice::NoError || start.isEmpty()) {
            error = QStringLiteral("Prozessprüfung für OBS-PID %1 verweigert; Installation abgebrochen").arg(pid);
            return false;
        }
        for (const auto &line : contents.split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 5) continue;
            const auto device = fields[3].split(':');
            if (device.size() != 2) continue;
            bool inodeOk = false, majorOk = false, minorOk = false;
            const auto inode = fields[4].toULongLong(&inodeOk);
            const auto devMajor = device[0].toUInt(&majorOk, 16), devMinor = device[1].toUInt(&minorOk, 16);
            if (inodeOk && majorOk && minorOk && inode == targetStat.st_ino &&
                devMajor == major(targetStat.st_dev) && devMinor == minor(targetStat.st_dev)) {
                error = QStringLiteral("Plugin ist noch in PID %1 gemappt – alle OBS-Instanzen vollständig schließen").arg(pid);
                return false;
            }
        }
    }
    return true;
}

bool writePending(const QString &directory, const Pending &p, QString &error)
{
    try {
        QJsonObject object{{"schema", p.helper ? 2 : 1}, {"fromVersion", p.fromVersion}, {"version", p.version},
            {"sha256", p.sha256}, {"size", p.size}, {"binary", p.binary}, {"target", p.target}};
        if (p.helper) object["helper"] = QJsonObject{{"sha256", p.helper->sha256}, {"size", p.helper->size},
            {"binary", p.helper->binary}, {"target", p.helper->target}};
        saveJson(directory + "/pending.json", object);
        return true;
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
}
Pending readPending(const QString &directory)
{
    QFile file(directory + "/pending.json");
    require(file.open(QIODevice::ReadOnly), "Pending-Metadaten fehlen");
    const auto p = QJsonDocument::fromJson(file.readAll()).object();
    Pending result{p["fromVersion"].toString(), p["version"].toString(), p["sha256"].toString(),
                   p["binary"].toString(), p["target"].toString(), p["size"].toInteger(-1), std::nullopt};
    const int schema = p["schema"].toInt();
    require((schema == 1 || schema == 2) && !result.version.isEmpty() && result.sha256.size() == 64 && result.size > 0 &&
            QDir::isAbsolutePath(result.target) && result.binary == directory + "/bokis-twitch-chat-plugin.so" &&
            QFileInfo(result.target).fileName() == "bokis-twitch-chat-plugin.so", "Ungültige Pending-Metadaten");
    require((schema == 2) == p.contains("helper"), "Ungültiges Pending-Schema für Helper-Update");
    if (schema == 2) {
        const auto h = p["helper"].toObject();
        result.helper = PendingFile{h["sha256"].toString(), h["binary"].toString(), h["target"].toString(), h["size"].toInteger(-1)};
        require(QRegularExpression("^[0-9a-fA-F]{64}$").match(result.helper->sha256).hasMatch() && result.helper->size > 0 &&
                result.helper->binary == directory + "/bokis-twitch-chat-updater" &&
                result.helper->target == QFileInfo(result.target).absolutePath() + "/bokis-twitch-chat-updater",
                "Ungültige Pending-Metadaten für Helper");
    }
    return result;
}
bool launch(const QString &directory, const QString &helper, int lock, QString &error)
{
    // Capture the process identity while the calling OBS is definitely alive.
    const auto pid = getpid();
    const auto start = processStart(pid).toUtf8();
    Fd pidfd(static_cast<int>(syscall(SYS_pidfd_open, pid, 0)));
    Fd executable(open("/proc/self/exe", O_PATH | O_CLOEXEC));
    int pipefd[2];
    if (lock < 0 || executable.value < 0 || start.isEmpty() || pipe2(pipefd, O_CLOEXEC) != 0) { error = "Helper-Start konnte nicht vorbereitet werden"; return false; }
    Fd reader(pipefd[0]), writer(pipefd[1]);
    const auto exe = QFile::encodeName(helper), dir = QFile::encodeName(directory);
    const auto pidArg = QByteArray::number(pid), fdArg = QByteArray::number(pidfd.value), lockArg = QByteArray::number(lock);
    const auto executableArg = QByteArray::number(executable.value);
    char *args[] = {const_cast<char *>(exe.constData()), const_cast<char *>(dir.constData()),
        const_cast<char *>(pidArg.constData()), const_cast<char *>(start.constData()),
        const_cast<char *>(fdArg.constData()), const_cast<char *>(lockArg.constData()), const_cast<char *>(executableArg.constData()), nullptr};
    const long maxFd = sysconf(_SC_OPEN_MAX);
    const pid_t child = fork();
    if (child == 0) {
        close(reader.value);
        if (setsid() >= 0) {
            const auto detached = fork();
            if (detached > 0) _exit(0);
            if (detached == 0) {
                // Only async-signal-safe operations between fork and exec.
                if (syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) != 0) {
                    for (long fd = 3; fd < maxFd; ++fd) fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC);
                }
                const int ignoredChdir = chdir("/");
                (void)ignoredChdir;
                fcntl(lock, F_SETFD, 0);
                fcntl(executable.value, F_SETFD, 0);
                if (pidfd.value >= 0) fcntl(pidfd.value, F_SETFD, 0);
                int nullfd = open("/dev/null", O_RDWR);
                if (nullfd >= 0) { for (int i = 0; i < 3; ++i) dup2(nullfd, i); }
                execv(exe.constData(), args);
            }
        }
        int failure = errno;
        const auto ignored = write(writer.value, &failure, sizeof(failure));
        (void)ignored;
        _exit(127);
    }
    close(writer.value); writer.value = -1;
    if (child < 0) { error = "fork fehlgeschlagen"; return false; }
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    int failure = 0;
    ssize_t count;
    do { count = read(reader.value, &failure, sizeof(failure)); } while (count < 0 && errno == EINTR);
    if (count != 0) { error = QStringLiteral("Helper konnte nicht gestartet werden: %1").arg(QString::fromLocal8Bit(strerror(failure))); return false; }
    return true;
}
namespace {
int renameFile(const QString &from, const QString &to)
{
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData());
}
void checkMappings(const QString &path, const MappingScan &scan)
{
    QString error;
    if (!ensureNotMapped(path, error, scan)) throw std::runtime_error(error.toStdString());
}
struct InstallFile {
    PendingFile file;
    QString candidate, rollback;
    bool swapped = false;
};
void restore(const InstallFile &item, const MappingScan &scan)
{
    // Keep the original rollback inode available until all restores are durable.
    struct stat original{}, current{};
    require(stat(QFile::encodeName(item.rollback).constData(), &original) == 0, "Rücksicherungsdatei fehlt");
    if (stat(QFile::encodeName(item.file.target).constData(), &current) == 0 &&
        original.st_dev == current.st_dev && original.st_ino == current.st_ino) return;
    if (QFileInfo(item.file.target).fileName() == "bokis-twitch-chat-plugin.so") checkMappings(item.file.target, scan);
    const auto restoring = item.rollback + ".restore";
    QFile::remove(restoring);
    require(::link(QFile::encodeName(item.rollback).constData(), QFile::encodeName(restoring).constData()) == 0,
            "Rollback-Link konnte nicht erstellt werden");
    require(renameFile(restoring, item.file.target) == 0, "Rollback-Rename fehlgeschlagen; manuelle Wiederherstellung erforderlich");
    syncDirectory(QFileInfo(item.file.target).absolutePath());
}
void cleanupTransaction(const std::vector<InstallFile> &files, const QString &journal)
{
    // Remove the journal durably first: otherwise recovery could reference deleted originals.
    if (QFile::exists(journal)) {
        require(QFile::remove(journal), "Transaktionsjournal konnte nicht entfernt werden");
        syncDirectory(QFileInfo(journal).absolutePath());
    }
    for (const auto &item : files) {
        QFile::remove(item.candidate);
        QFile::remove(item.rollback);
        QFile::remove(item.rollback + ".restore");
    }
}
}
bool install(const QString &directory, const QString &backups, const QString &result,
             const std::function<bool()> &waiter, QString &error, const RenameOperation &renameOperation, const MappingScan &scan)
{
    Pending p;
    const QString journalPath = directory + "/transaction.json";
    std::vector<InstallFile> files;
    bool committed = false, journalPrepared = false, rollbackFailed = false;
    Fd useLock; // Retain the target lock through rollback, result persistence and cleanup.
    try {
        p = readPending(directory);
        require(waiter(), "Prozessende konnte nicht sicher festgestellt werden");
        p = readPending(directory);
        useLock.value = acquireLock(p.target + ".use.lock");
        require(useLock.value >= 0, "Eine weitere OBS-Instanz verwendet das Plugin");
        checkMappings(p.target, scan);
        if (p.helper) files.push_back({*p.helper, {}, {}, false}); // Helper first; plugin is the final swap.
        files.push_back({{p.sha256, p.binary, p.target, p.size}, {}, {}, false});

        if (QFile::exists(journalPath)) {
            QFile journalFile(journalPath);
            require(journalFile.open(QIODevice::ReadOnly), "Transaktionsjournal nicht lesbar");
            const auto journal = QJsonDocument::fromJson(journalFile.readAll()).object();
            const auto entries = journal["files"].toArray();
            const auto id = journal["id"].toString();
            require(journal["schema"].toInt() == 1 && journal["version"].toString() == p.version &&
                    QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch() &&
                    entries.size() == static_cast<qsizetype>(files.size()), "Ungültiges Transaktionsjournal");
            for (size_t i = 0; i < files.size(); ++i) {
                auto &item = files[i];
                const auto entry = entries[static_cast<qsizetype>(i)].toObject();
                require(entry["target"].toString() == item.file.target && entry["sha256"].toString() == item.file.sha256,
                        "Transaktionsjournal gehört zu einem anderen Update");
                item.candidate = item.file.target + ".update-" + id;
                item.rollback = item.file.target + ".rollback-" + id;
            }
            const auto phase = journal["phase"].toString();
            require(phase == "prepared" || phase == "committed", "Ungültiger Transaktionsstatus");
            journalPrepared = true;
            if (phase == "committed") {
                for (const auto &item : files) {
                    QFile installed(item.file.target);
                    require(installed.open(QIODevice::ReadOnly), "Installierte Transaktion nicht lesbar");
                    verify(installed, item.file);
                }
                committed = true; // Finish an interrupted result/state cleanup without installing twice.
            } else {
                for (auto it = files.rbegin(); it != files.rend(); ++it) {
                    if (it->file.target == p.target) checkMappings(p.target, scan);
                    restore(*it, scan);
                }
                cleanupTransaction(files, journalPath);
                journalPrepared = false;
            }
        }
        if (!committed) {
            // Verify EVERY payload before preparing or exchanging either installed file.
            for (const auto &item : files) {
                QFile input(item.file.binary);
                require(!QFileInfo(item.file.binary).isSymLink() && input.open(QIODevice::ReadOnly), "Pending-Datei fehlt oder ist nicht lesbar");
                verify(input, item.file);
                require(!QFileInfo(item.file.target).isSymLink(), "Installationsziel darf kein Symlink sein");
            }
            require(QDir().mkpath(backups), "Backup-Verzeichnis konnte nicht erstellt werden");
            const auto id = QUuid::createUuid().toString(QUuid::Id128);
            const auto stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz") + "-" + id;
            QJsonArray entries;
            for (auto &item : files) {
                item.candidate = item.file.target + ".update-" + id;
                item.rollback = item.file.target + ".rollback-" + id;
                QFile input(item.file.binary), old(item.file.target);
                require(input.open(QIODevice::ReadOnly) && old.open(QIODevice::ReadOnly), "Update/Original konnte nicht gelesen werden");
                struct stat st{};
                require(fstat(old.handle(), &st) == 0 && S_ISREG(st.st_mode), "Installiertes Ziel ist keine reguläre Datei");
                copySynced(old, backups + "/" + stamp + "-" + QFileInfo(item.file.target).fileName(), 0755);
                copySynced(input, item.candidate, 0755);
                QFile copied(item.candidate);
                require(copied.open(QIODevice::ReadOnly), "Temporäre Datei nicht lesbar");
                verify(copied, item.file);
                require(::link(QFile::encodeName(item.file.target).constData(), QFile::encodeName(item.rollback).constData()) == 0,
                        "Lokale Rücksicherung konnte nicht erstellt werden");
                syncDirectory(QFileInfo(item.file.target).absolutePath());
                entries.append(QJsonObject{{"target", item.file.target}, {"sha256", item.file.sha256}});
            }
            syncDirectory(backups);
            QJsonObject journal{{"schema", 1}, {"id", id}, {"version", p.version}, {"phase", "prepared"}, {"files", entries}};
            // Set before saving: even an fsync failure can leave a visible journal.
            journalPrepared = true;
            saveJson(journalPath, journal);
            for (auto &item : files) {
                checkMappings(p.target, scan); // Also catch instances that appeared while copying/backing up.
                const auto renamed = renameOperation ? renameOperation(item.candidate, item.file.target) : renameFile(item.candidate, item.file.target);
                require(renamed == 0, "Atomarer Austausch fehlgeschlagen");
                item.swapped = true;
                syncDirectory(QFileInfo(item.file.target).absolutePath());
            }
            journal["phase"] = "committed";
            saveJson(journalPath, journal);
            committed = true;
        }
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        if (!committed && journalPrepared) {
            try {
                for (auto it = files.rbegin(); it != files.rend(); ++it) {
                    if (it->file.target == p.target && it->swapped) checkMappings(p.target, scan);
                    restore(*it, scan);
                }
                cleanupTransaction(files, journalPath);
                journalPrepared = false;
            } catch (const std::exception &rollbackError) {
                rollbackFailed = true;
                error += QStringLiteral("; Rollback unvollständig: ") + QString::fromUtf8(rollbackError.what());
            }
        }
    }
    // An unvalidated/interrupted journal can describe a partially installed pair.
    // Never tell the UI that originals were retained while recovery is unresolved.
    if (!committed && QFile::exists(journalPath)) rollbackFailed = true;
    if (!committed && !journalPrepared && !QFile::exists(journalPath)) {
        for (const auto &item : files) {
            if (!item.candidate.isEmpty()) QFile::remove(item.candidate);
            if (!item.rollback.isEmpty()) QFile::remove(item.rollback);
        }
    }
    try {
        saveJson(result, {{"success", committed}, {"fromVersion", p.fromVersion}, {"toVersion", p.version},
            {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"error", error},
            {"rollbackFailed", rollbackFailed}, {"helperUpdated", committed && p.helper.has_value()}});
        if (committed) {
            // A committed journal makes partially completed cleanup safe to resume.
            for (const auto &item : files) {
                if (QFile::exists(item.file.binary)) require(QFile::remove(item.file.binary), "Pending-Datei konnte nicht entfernt werden");
            }
            require(QFile::remove(directory + "/pending.json"), "Pending-Metadaten konnten nicht entfernt werden");
            syncDirectory(directory);
            cleanupTransaction(files, journalPath);
        }
    } catch (const std::exception &e) { error += QStringLiteral("; ") + QString::fromUtf8(e.what()); return false; }
    return committed;
}
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "updater/post-exit-internal.hpp"
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
namespace detail {
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
void syncDirectory(const QString &path)
{
    Fd fd(open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    require(fd.value >= 0 && fsync(fd.value) == 0, "Directory could not be synchronized");
}
void saveJson(const QString &path, const QJsonObject &object)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Directory could not be created");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson();
    require(file.open(QIODevice::WriteOnly), "Metadata could not be opened");
    require(file.write(bytes) == bytes.size() && file.flush() && fsync(file.handle()) == 0,
            "Metadata could not be synchronized");
    require(file.commit(), "Metadata could not be saved atomically");
    syncDirectory(QFileInfo(path).absolutePath());
}
void copySynced(QFile &input, const QString &path, int mode)
{
    Fd output(open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    require(output.value >= 0, "Temporary file or backup could not be created");
    require(input.seek(0), "Source file could not be read");
    while (!input.atEnd()) {
        const auto bytes = input.read(64 * 1024);
        require(!bytes.isEmpty(), "Read error while copying");
        qint64 offset = 0;
        while (offset < bytes.size()) {
            const auto written = write(output.value, bytes.constData() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            require(written > 0, "Write error while copying");
            offset += written;
        }
    }
    require(fchmod(output.value, mode) == 0 && fsync(output.value) == 0, "File could not be synchronized");
}

int renameFile(const QString &from, const QString &to)
{ return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()); }
int linkFile(const QString &from, const QString &to)
{ return ::link(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()); }
bool sameFile(const QString &a, const QString &b)
{
    struct stat x{}, y{};
    return stat(QFile::encodeName(a).constData(), &x) == 0 && stat(QFile::encodeName(b).constData(), &y) == 0 &&
           x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
bool regularFile(QFile &file)
{ struct stat st{}; return fstat(file.handle(), &st) == 0 && S_ISREG(st.st_mode); }
}
using namespace detail;
void releaseLock(int fd) { if (fd >= 0) close(fd); }
int duplicateLock(int fd) { return dup(fd); }
bool flushFile(int fd) { return fsync(fd) == 0; }
QString platformKey() { return "linux-x86_64"; }
QString pluginFileName() { return "bokis-twitch-chat-plugin.so"; }
QString helperFileName() { return "bokis-twitch-chat-updater"; }
bool validBinary(const QByteArray &bytes) { return bytes.startsWith(QByteArray("\x7f" "ELF", 4)); }
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
        error = "Plugin target could not be read for the process check";
        return false;
    }
    struct stat executableStat{};
    if (stat(QFile::encodeName(scan.executable).constData(), &executableStat) != 0) {
        error = "OBS executable could not be read for the process check";
        return false;
    }
    const auto executablePath = QFileInfo(scan.executable).canonicalFilePath();
    const QDir proc(scan.procRoot);
    if (!QFile::exists(scan.procRoot + "/self/maps")) { error = "procfs process check unavailable"; return false; }
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
            error = QStringLiteral("Process check denied for OBS PID %1; installation aborted").arg(pid);
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
                error = QStringLiteral("Plugin is still mapped in PID %1 – close all OBS instances completely").arg(pid);
                return false;
            }
        }
    }
    return true;
}

bool launch(const QString &directory, const QString &helper, int lock, QString &error)
{
    // Capture the process identity while the calling OBS is definitely alive.
    const auto pid = getpid();
    const auto start = processStart(pid).toUtf8();
    Fd pidfd(static_cast<int>(syscall(SYS_pidfd_open, pid, 0)));
    Fd executable(open("/proc/self/exe", O_PATH | O_CLOEXEC));
    int pipefd[2];
    if (lock < 0 || executable.value < 0 || start.isEmpty() || pipe2(pipefd, O_CLOEXEC) != 0) { error = "Helper launch could not be prepared"; return false; }
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
    if (child < 0) { error = "fork failed"; return false; }
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    int failure = 0;
    ssize_t count;
    do { count = read(reader.value, &failure, sizeof(failure)); } while (count < 0 && errno == EINTR);
    if (count != 0) { error = QStringLiteral("Helper could not be started: %1").arg(QString::fromLocal8Bit(strerror(failure))); return false; }
    return true;
}

}

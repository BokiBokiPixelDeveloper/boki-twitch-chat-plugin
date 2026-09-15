#pragma once
#include <QString>
#include <functional>
#include <optional>
#include <sys/types.h>

namespace postexit {
struct PendingFile {
    QString sha256, binary, target;
    qint64 size = -1;
};
struct Pending {
    QString fromVersion, version, sha256, binary, target;
    qint64 size = -1;
    std::optional<PendingFile> helper;
};
QString cacheDirectory();
QString dataDirectory();
QString stateDirectory();
QString modulePath();
void holdProcessUseLock();
QString processStart(pid_t pid);
// A persistent lock inode: never unlink a lock file.
int acquireLock(const QString &path, bool shared = false);
bool writePending(const QString &directory, const Pending &pending, QString &error);
Pending readPending(const QString &directory);
bool launch(const QString &directory, const QString &helper, int lock, QString &error);
bool waitForProcess(pid_t pid, const QString &start, int pidfd);
// Optional rename operation permits deterministic I/O failure tests; rollback uses POSIX rename.
using RenameOperation = std::function<int(const QString &, const QString &)>;
struct MappingScan {
    // An inherited /proc/self/fd/N keeps the original executable identity alive after exit.
    QString executable = "/proc/self/exe";
    QString procRoot = "/proc";
    // Deterministic process-exit/PID-reuse tests, called after candidate classification.
    std::function<void(const QString &)> beforeMaps;
};
bool ensureNotMapped(const QString &target, QString &error, const MappingScan &scan = {});
bool install(const QString &directory, const QString &backups, const QString &result,
             const std::function<bool()> &waiter, QString &error, const RenameOperation &renameOperation = {}, const MappingScan &scan = {});
}

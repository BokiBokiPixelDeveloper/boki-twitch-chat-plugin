#include "updater/post-exit.hpp"
#include <QCoreApplication>
#include <QFile>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 6 && argc != 7) { std::fprintf(stderr, "Usage: %s pending-directory pid start-ticks pidfd lockfd [executablefd]\n", argv[0]); return 2; }
    bool ok = false;
    const int pid = QString::fromLocal8Bit(argv[2]).toInt(&ok);
    if (!ok || pid <= 1) return 2;
    const int pidfd = QString::fromLocal8Bit(argv[4]).toInt(&ok);
    if (!ok) return 2;
    int lock = QString::fromLocal8Bit(argv[5]).toInt(&ok);
    if (!ok) return 2;
    const QString directory = QString::fromLocal8Bit(argv[1]);
    if (lock < 0) lock = postexit::acquireLock(directory + "/update.lock");
    if (lock < 0) return 3;
    // An inherited lock must refer to our persistent lock inode.
    struct stat held{}, expected{};
    if (fstat(lock, &held) || stat(QFile::encodeName(directory + "/update.lock").constData(), &expected) ||
        held.st_dev != expected.st_dev || held.st_ino != expected.st_ino || flock(lock, LOCK_EX | LOCK_NB) != 0) return 3;
    // New launchers pass an open executable; legacy callers must still have a live origin.
    const auto start = QString::fromLocal8Bit(argv[3]);
    if (argc == 6 && postexit::processStart(pid) != start) return 2;
    const int executable = argc == 7 ? QString::fromLocal8Bit(argv[6]).toInt(&ok) :
        open(QFile::encodeName(QStringLiteral("/proc/%1/exe").arg(pid)).constData(), O_PATH | O_CLOEXEC);
    if ((argc == 7 && !ok) || executable < 0) return 2;
    if (argc == 6 && postexit::processStart(pid) != start) { close(executable); return 2; }
    const postexit::MappingScan scan{QStringLiteral("/proc/self/fd/%1").arg(executable), "/proc", {}};
    QString error;
    const bool success = postexit::install(directory, postexit::dataDirectory(),
        postexit::stateDirectory() + "/last-update-result.json",
        [&] { return postexit::waitForProcess(pid, QString::fromLocal8Bit(argv[3]), pidfd); }, error, {}, scan);
    if (!error.isEmpty()) std::fprintf(stderr, "%s\n", qPrintable(error));
    close(executable);
    close(lock);
    if (pidfd >= 0) close(pidfd);
    return success ? 0 : 1;
}

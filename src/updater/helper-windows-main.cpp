#include "updater/windows-platform.hpp"
#include <QCoreApplication>
#include <QFile>
#include <io.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments(); // Qt obtains the original Unicode Windows command line.
    if (args.size() != 11 || args[1] != "--run") return 2;
    bool ok = false;
    const DWORD pid = args[3].toULong(&ok); if (!ok || !pid) return 2;
    const auto processValue = args[5].toULongLong(&ok); if (!ok) return 2;
    postexit::windows::Handle origin(reinterpret_cast<HANDLE>(quintptr(processValue)));
    const auto lockValue = args[6].toULongLong(&ok); if (!ok) return 2;
    const auto eventValue = args[7].toULongLong(&ok); if (!ok) return 2;
    postexit::windows::Handle ready(reinterpret_cast<HANDLE>(quintptr(eventValue)));
    HANDLE inheritedLock = reinterpret_cast<HANDLE>(quintptr(lockValue));
    if (!postexit::windows::validateLockHandle(inheritedLock, args[2] + "/update.lock") ||
        GetProcessId(origin.value) != pid || postexit::windows::creationTime(origin.value) != args[4]) return 3;
    const int lock = _open_osfhandle(reinterpret_cast<intptr_t>(inheritedLock), _O_BINARY | _O_RDONLY | _O_NOINHERIT);
    if (lock < 0) return 3;
    if (!SetEvent(ready.value)) { postexit::releaseLock(lock); return 3; }
    QString error;
    postexit::MappingScan scan; scan.executable = args[8];
    const bool success = postexit::install(args[2], args[9], args[10],
        [&] { return postexit::windows::waitForOrigin(origin.value, pid, args[4]); }, error, {}, scan);
    postexit::releaseLock(lock);
    return success ? 0 : 1;
}

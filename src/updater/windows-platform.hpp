#pragma once
#include "updater/post-exit.hpp"
#include <windows.h>
#include <utility>

namespace postexit::windows {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
QString localRoot();
QString executablePath(HANDLE process);
QString creationTime(HANDLE process);
bool waitForOrigin(HANDLE process, DWORD pid, const QString &creation, DWORD timeout = INFINITE);
QString quoteArgument(const QString &argument);
// Tests may supply temporary result/backup directories without changing Known Folders.
bool launchRunner(const QString &directory, const QString &helper, int lock, QString &error,
                  const QString &backups, const QString &result);
bool validateLockHandle(HANDLE handle, const QString &path);
// Classification is separate from enumeration so access-denied cases are testable.
bool mappingDecision(bool candidate, bool alive, bool modulesReadable, bool targetMapped, QString &error);
}

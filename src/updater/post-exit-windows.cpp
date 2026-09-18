#include "updater/post-exit-internal.hpp"
#include "updater/windows-platform.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <shlobj.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <io.h>
#include <fcntl.h>
#include <vector>

namespace postexit {
using windows::Handle;
namespace {
std::wstring wide(const QString &s) { return QDir::toNativeSeparators(s).toStdWString(); }
QString normalized(QString s)
{
    s = QDir::fromNativeSeparators(s);
    if (s.startsWith("//?/UNC/")) s = "//" + s.mid(8);
    else if (s.startsWith("//?/")) s = s.mid(4);
    return QDir::cleanPath(s).toCaseFolded();
}
QString handlePath(HANDLE file)
{
    std::vector<wchar_t> path(32768);
    const DWORD n = GetFinalPathNameByHandleW(file, path.data(), DWORD(path.size()), FILE_NAME_NORMALIZED);
    return n && n < path.size() ? QString::fromWCharArray(path.data(), int(n)) : QString{};
}
}
namespace detail {
bool sameFile(const QString &a, const QString &b)
{
    Handle x(CreateFileW(wide(a).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    Handle y(CreateFileW(wide(b).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION i{}, j{};
    return x.valid() && y.valid() && GetFileInformationByHandle(x.value, &i) && GetFileInformationByHandle(y.value, &j) &&
        i.dwVolumeSerialNumber == j.dwVolumeSerialNumber && i.nFileIndexHigh == j.nFileIndexHigh && i.nFileIndexLow == j.nFileIndexLow;
}
bool regularFile(QFile &file)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
int renameFile(const QString &from, const QString &to)
{
    // Candidates and rollback files are on the target volume. Unlike ReplaceFile,
    // MoveFileEx supports WRITE_THROUGH; never use COPY_ALLOWED across volumes.
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}
int linkFile(const QString &from, const QString &to)
{ return CreateHardLinkW(wide(to).c_str(), wide(from).c_str(), nullptr) ? 0 : -1; }
void syncDirectory(const QString &)
{
    // Win32 has no portable directory-fsync equivalent. Each candidate/backup is
    // FlushFileBuffers'd; journal/result publication and swaps use WRITE_THROUGH.
}
void copySynced(QFile &input, const QString &path, int)
{
    Handle output(CreateFileW(wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    require(output.valid() && input.seek(0), "Candidate or backup could not be created");
    while (!input.atEnd()) {
        const auto bytes = input.read(64 * 1024);
        require(!bytes.isEmpty(), "Read error while copying");
        DWORD written = 0;
        require(WriteFile(output.value, bytes.constData(), DWORD(bytes.size()), &written, nullptr) &&
                written == bytes.size(), "Write error while copying");
    }
    require(FlushFileBuffers(output.value), "File could not be flushed");
}
void saveJson(const QString &path, const QJsonObject &object)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Metadata directory could not be created");
    const auto temporary = path + "." + QUuid::createUuid().toString(QUuid::Id128);
    try {
        {
            Handle file(CreateFileW(wide(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            const auto bytes = QJsonDocument(object).toJson();
            DWORD written = 0;
            require(file.valid() && WriteFile(file.value, bytes.constData(), DWORD(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(file.value), "Metadata could not be flushed");
        }
        require(renameFile(temporary, path) == 0, "Metadata could not be published atomically");
    } catch (...) { QFile::remove(temporary); throw; }
}
}
QString platformKey() { return "windows-x86_64"; }
QString pluginFileName() { return "bokis-twitch-chat-plugin.dll"; }
QString helperFileName() { return "bokis-twitch-chat-updater.exe"; }
bool validBinary(const QByteArray &bytes)
{
    if (bytes.size() < 64 || bytes.first(2) != "MZ") return false;
    const auto offset = qFromLittleEndian<quint32>(bytes.constData() + 0x3c);
    if (offset < 64 || offset > 4096 - 26 || qsizetype(offset) + 26 > bytes.size()) return false;
    return bytes.mid(offset, 4) == QByteArray("PE\0\0", 4) &&
        qFromLittleEndian<quint16>(bytes.constData() + offset + 4) == 0x8664 &&
        qFromLittleEndian<quint16>(bytes.constData() + offset + 24) == 0x20b;
}
bool flushFile(int fd) { return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(fd))); }
void releaseLock(int fd) { if (fd >= 0) _close(fd); }
int duplicateLock(int fd) { return _dup(fd); }
QString cacheDirectory() { return windows::localRoot() + "/cache/pending"; }
QString dataDirectory() { return windows::localRoot() + "/backups"; }
QString stateDirectory() { return windows::localRoot() + "/state"; }
QString modulePath()
{
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&modulePath), &module)) return {};
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    return n && n < path.size() ? QFileInfo(QString::fromWCharArray(path.data(), int(n))).canonicalFilePath() : QString{};
}
int acquireLock(const QString &path, bool shared)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || QFileInfo(path).isSymLink()) return -1;
    const auto name = wide(path);
    // Shared readers keep the persistent file's inode alive; exclusive opens
    // conflict with readers/writers in every session. Duplicated handles retain
    // the sharing exclusion after the originating process exits.
    HANDLE handle = CreateFileW(name.c_str(), shared ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                                shared ? FILE_SHARE_READ : 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle); return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDONLY | _O_NOINHERIT);
    if (fd < 0) CloseHandle(handle);
    return fd;
}
void holdProcessUseLock()
{
    static const int held = acquireLock(modulePath() + ".use.lock", true);
    (void)held; // Intentionally retained until process teardown, including dlclose.
}
namespace windows {
QString localRoot()
{
    if (QStandardPaths::isTestModeEnabled()) {
        static QTemporaryDir isolated;
        if (!isolated.isValid()) throw std::runtime_error("Temporary test storage unavailable");
        return isolated.path() + "/BokisTwitchChatPlugin";
    }
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path)))
        throw std::runtime_error("LocalAppData Known Folder is unavailable");
    const auto root = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    CoTaskMemFree(path);
    return root + "/BokisTwitchChatPlugin";
}
QString executablePath(HANDLE process)
{
    std::vector<wchar_t> path(32768); DWORD size = DWORD(path.size());
    return QueryFullProcessImageNameW(process, 0, path.data(), &size) ?
        QDir::fromNativeSeparators(QString::fromWCharArray(path.data(), int(size))) : QString{};
}
QString creationTime(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
    return QString::number((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}
bool waitForOrigin(HANDLE process, DWORD pid, const QString &creation, DWORD timeout)
{
    if (!process || process == INVALID_HANDLE_VALUE || creation.isEmpty() ||
        GetProcessId(process) != pid || creationTime(process) != creation) return false;
    return WaitForSingleObject(process, timeout) == WAIT_OBJECT_0;
}
QString quoteArgument(const QString &argument)
{
    QString result = "\""; int slashes = 0;
    for (const QChar c : argument) {
        if (c == '\\') { ++slashes; continue; }
        result += QString(slashes * (c == '"' ? 2 : 1), '\\');
        slashes = 0;
        if (c == '"') result += '\\';
        result += c;
    }
    return result + QString(slashes * 2, '\\') + '"';
}
bool validateLockHandle(HANDLE handle, const QString &path)
{ return normalized(handlePath(handle)) == normalized(QFileInfo(path).absoluteFilePath()); }
bool mappingDecision(bool candidate, bool alive, bool modulesReadable, bool targetMapped, QString &error)
{
    if (!alive) return true;
    if (targetMapped) { error = "Another process still has the plugin DLL loaded"; return false; }
    if (candidate && !modulesReadable) { error = "Could not inspect a relevant OBS process; close all OBS instances"; return false; }
    return true;
}
}
bool ensureNotMapped(const QString &target, QString &error, const MappingScan &scan)
{
    const QString origin = scan.executable.isEmpty() ? windows::executablePath(GetCurrentProcess()) : scan.executable;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) { error = "Process enumeration failed"; return false; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) { error = "Process enumeration failed"; return false; }
    do {
        if (!entry.th32ProcessID || entry.th32ProcessID == GetCurrentProcessId()) continue;
        Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!query.valid()) continue; // An inaccessible, unclassified process is not evidence of an OBS instance.
        const QString image = windows::executablePath(query.value);
        const bool candidate = !image.isEmpty() && (normalized(image) == normalized(origin) || detail::sameFile(image, origin));
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        bool readable = false, mapped = false;
        if (process.valid() && windows::creationTime(process.value) == windows::creationTime(query.value)) {
            std::vector<HMODULE> modules(256); DWORD needed{};
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (!EnumProcessModulesEx(process.value, modules.data(), DWORD(modules.size() * sizeof(HMODULE)),
                                          &needed, LIST_MODULES_ALL)) break;
                if (needed > modules.size() * sizeof(HMODULE)) { modules.resize(needed / sizeof(HMODULE) + 64); continue; }
                readable = true;
                for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
                    std::vector<wchar_t> path(32768);
                    const DWORD n = GetModuleFileNameExW(process.value, modules[i], path.data(), DWORD(path.size()));
                    if (!n || n >= path.size()) { readable = false; continue; }
                    const QString module = QString::fromWCharArray(path.data(), int(n));
                    if (normalized(module) == normalized(target) || detail::sameFile(module, target)) { mapped = true; break; }
                }
                break;
            }
        }
        if (!windows::mappingDecision(candidate, WaitForSingleObject(query.value, 0) != WAIT_OBJECT_0, readable, mapped, error))
            return false;
    } while (Process32NextW(snapshot.value, &entry));
    // Also let the kernel reject write access to an image mapping that was hidden
    // by a process ACL, belonged to another OBS executable, or appeared during enumeration.
    Handle writable(CreateFileW(wide(target).c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!writable.valid()) { error = "Plugin DLL is loaded, locked, or not writable"; return false; }
    return true;
}
bool windows::launchRunner(const QString &directory, const QString &helper, int lock, QString &error,
                           const QString &backups, const QString &result)
{
    using detail::require;
    try {
        Handle origin(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()));
        const auto created = creationTime(origin.value);
        require(origin.valid() && !created.isEmpty() && lock >= 0, "Origin process or update lock unavailable");
        const auto runners = QFileInfo(directory).absolutePath() + "/runners";
        const auto runnerDir = runners + "/" + QUuid::createUuid().toString(QUuid::Id128);
        require(QDir().mkpath(runnerDir), "Temporary runner directory could not be created");
        const auto runner = runnerDir + "/" + helperFileName();
        QFile source(helper);
        require(source.open(QIODevice::ReadOnly) && validBinary(source.peek(4096)), "Installed helper is missing or invalid");
        detail::copySynced(source, runner, 0755);
        QFile copy(runner);
        require(copy.open(QIODevice::ReadOnly), "Temporary runner cannot be verified");
        source.seek(0);
        QCryptographicHash a(QCryptographicHash::Sha256), b(QCryptographicHash::Sha256);
        require(a.addData(&source) && b.addData(&copy) && a.result() == b.result(), "Temporary runner checksum mismatch");
        source.close(); copy.close();
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::vector<Handle> inherited;
        for (HANDLE handle : {origin.value, reinterpret_cast<HANDLE>(_get_osfhandle(lock)), ready.value}) {
            HANDLE duplicate{};
            require(DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, TRUE,
                                    DUPLICATE_SAME_ACCESS), "Handle inheritance could not be prepared");
            inherited.emplace_back(duplicate);
        }
        HANDLE handles[]{inherited[0].value, inherited[1].value, inherited[2].value};
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "Process attributes unavailable");
        struct Attributes { PPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr),
                "Handle whitelist could not be configured");
        QStringList args{runner, "--run", directory, QString::number(GetCurrentProcessId()), created};
        for (HANDLE handle : handles) args << QString::number(reinterpret_cast<quintptr>(handle));
        args << executablePath(origin.value) << backups << result;
        QStringList quoted; for (const auto &arg : args) quoted << quoteArgument(arg);
        auto command = quoted.join(' ').toStdWString();
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        require(CreateProcessW(wide(runner).c_str(), command.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, wide(runnerDir).c_str(),
                               &startup.StartupInfo, &process), "Temporary updater runner could not be started");
        Handle child(process.hProcess), thread(process.hThread);
        HANDLE wait[]{ready.value, child.value};
        require(WaitForMultipleObjects(2, wait, FALSE, 10000) == WAIT_OBJECT_0, "Updater did not acknowledge startup; pending files retained");
        return true;
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
}
bool launch(const QString &directory, const QString &helper, int lock, QString &error)
{ return windows::launchRunner(directory, helper, lock, error, dataDirectory(), stateDirectory() + "/last-update-result.json"); }
}

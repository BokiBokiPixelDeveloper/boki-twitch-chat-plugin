// Embed the Qt worker and runtime in the one self-updatable EXE. The installed
// EXE is copied to a unique runner directory before any execution.
#include <windows.h>
#include <string>
#include <vector>
#include "helper-resources.hpp"

namespace {
std::wstring quote(const std::wstring &argument)
{
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
bool extract(const std::wstring &directory)
{
    for (const auto &resource : helperResources) {
        HRSRC entry = FindResourceW(nullptr, MAKEINTRESOURCEW(resource.id), MAKEINTRESOURCEW(10));
        if (!entry) return false;
        const DWORD size = SizeofResource(nullptr, entry);
        const void *data = LockResource(LoadResource(nullptr, entry));
        const auto path = directory + L"\\" + resource.name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (!data || file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok) return false;
    }
    return true;
}
}
int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 11 || std::wstring(argv[1]) != L"--run") return 2;
    std::vector<wchar_t> module(32768);
    DWORD size = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
    if (!size || size >= module.size()) return 2;
    const std::wstring path(module.data(), size);
    const auto directory = path.substr(0, path.find_last_of(L"\\/"));
    if (!extract(directory)) return 3;
    const auto worker = directory + L"\\bokis-updater-worker.exe";
    std::wstring command = quote(worker);
    for (int i = 1; i < argc; ++i) command += L" " + quote(argv[i]);
    HANDLE handles[3]{};
    try {
        for (int i = 0; i < 3; ++i) handles[i] = reinterpret_cast<HANDLE>(std::stoull(argv[5 + i]));
    } catch (...) { return 2; }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return 3;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes); return 3;
    }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!started) return 3;
    for (HANDLE handle : handles) CloseHandle(handle);
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    return int(result);
}

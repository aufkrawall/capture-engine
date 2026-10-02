// Logging, string/path/file helpers shared by setup and uninstaller.

#include "setup.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

namespace ce::setup {
namespace {

CRITICAL_SECTION g_logLock;
bool g_logLockReady = false;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_logPath;

}  // namespace

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void LogOpen(const wchar_t* name) {
    if (!g_logLockReady) {
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;
    }
    wchar_t temp[MAX_PATH + 2] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH)
        return;
    g_logPath = std::wstring(temp, length) + name;
    // Shared for reading so the file can be opened while setup is still running.
    g_logFile = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE)
        g_logPath.clear();
}

void LogClose() {
    if (!g_logLockReady)
        return;
    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
        CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_logLock);
}

const std::wstring& LogFilePath() {
    return g_logPath;
}

void Log(const char* format, ...) {
    char message[1536];
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0)
        return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1700];
    const int total = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d %s\r\n", now.wHour, now.wMinute, now.wSecond,
                               now.wMilliseconds, message);
    if (total <= 0)
        return;
    const DWORD bytes = static_cast<DWORD>(total < static_cast<int>(sizeof(line)) ? total : sizeof(line) - 1);
    OutputDebugStringA(line);
    if (!g_logLockReady || g_logFile == INVALID_HANDLE_VALUE)
        return;
    EnterCriticalSection(&g_logLock);
    DWORD ignored = 0;
    WriteFile(g_logFile, line, bytes, &ignored, nullptr);
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), length);
    return result;
}

std::string Narrow(std::wstring_view wide) {
    if (wide.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
    }
    if (buffer)
        LocalFree(buffer);
    wchar_t code[32];
    swprintf(code, 32, L" (error %lu)", static_cast<unsigned long>(error));
    return (text.empty() ? std::wstring(L"Windows error") : text) + code;
}

// ---------------------------------------------------------------------------
// Paths and files
// ---------------------------------------------------------------------------

std::wstring ModulePath() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= 32768)
            return {};
        path.resize(path.size() * 2);
    }
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring JoinPath(const std::wstring& parent, std::wstring_view child) {
    std::wstring result = parent;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/')
        result.push_back(L'\\');
    for (wchar_t character : child)
        result.push_back(character == L'/' ? L'\\' : character);
    return result;
}

std::wstring KnownFolder(const GUID& id) {
    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder)) && folder)
        result = folder;
    if (folder)
        CoTaskMemFree(folder);
    return result;
}

std::wstring ProgramFilesDirectory() {
    return KnownFolder(FOLDERID_ProgramFiles);
}

std::wstring WindowsDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring NearestExistingDirectory(std::wstring path) {
    while (!path.empty() && !DirectoryExists(path)) {
        const std::wstring parent = DirectoryOf(path);
        if (parent == path)
            break;
        path = parent;
    }
    return path;
}

bool CreateDirectoryTree(const std::wstring& path, DWORD* error) {
    if (DirectoryExists(path))
        return true;
    const std::wstring parent = DirectoryOf(path);
    if (!parent.empty() && parent != path && !DirectoryExists(parent) && !CreateDirectoryTree(parent, error))
        return false;
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        return true;
    if (error)
        *error = GetLastError();
    return false;
}

bool ReadWholeFile(const std::wstring& path, std::string* contents, uint64_t limit) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid())
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit)
        return false;
    contents->assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t done = 0;
    while (done < contents->size()) {
        DWORD read = 0;
        if (!::ReadFile(file.Get(), contents->data() + done, static_cast<DWORD>(contents->size() - done), &read,
                        nullptr) ||
            read == 0)
            return false;
        done += read;
    }
    return true;
}

bool WriteFileAtomically(const std::wstring& path, const void* data, size_t size, bool replaceExisting,
                         DWORD* error) {
    const std::wstring staging = path + L".cenew";
    DeleteFileW(staging.c_str());
    Handle file(CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        if (error)
            *error = GetLastError();
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t done = 0;
    bool ok = true;
    while (ok && done < size) {
        DWORD written = 0;
        ok = ::WriteFile(file.Get(), bytes + done, static_cast<DWORD>(size - done), &written, nullptr) && written != 0;
        done += written;
    }
    ok = ok && FlushFileBuffers(file.Get());
    if (!ok) {
        if (error)
            *error = GetLastError();
        file.Reset();
        DeleteFileW(staging.c_str());
        return false;
    }
    file.Reset();
    if (!MoveFileExW(staging.c_str(), path.c_str(),
                     (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH)) {
        if (error)
            *error = GetLastError();
        DeleteFileW(staging.c_str());
        return false;
    }
    return true;
}

std::wstring ProcessImagePath(DWORD processId) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process.Valid())
        return {};
    std::wstring path(1024, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.Get(), 0, path.data(), &length))
        return {};
    path.resize(length);
    return path;
}

bool IsProcessElevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated != 0;
}

bool IsProcessAlive(DWORD processId) {
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, processId));
    return process.Valid() && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT;
}

}  // namespace ce::setup

#include "wer_dump_adoption.h"

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <system_error>

#include "crash_dump_policy.h"
#include "logging.h"

namespace ce::wer_dump_adoption {
namespace {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring converted(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, converted.data(), required);
    return converted;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string converted(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, converted.data(), required, nullptr, nullptr);
    return converted;
}

// LocalDumps stores DumpFolder as REG_EXPAND_SZ, so a configured value can be
// %LOCALAPPDATA%-relative just like the default store.
std::wstring ExpandEnvironment(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    wchar_t expanded[MAX_PATH * 2] = {};
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    if (written == 0 || written > std::size(expanded)) {
        return value;
    }
    return expanded;
}

std::wstring ReadLocalDumpsFolderValue(HKEY root, const std::wstring& subKeyPath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKeyPath.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD sizeBytes = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, L"DumpFolder", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &sizeBytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_EXPAND_SZ && type != REG_SZ)) {
        return {};
    }
    return ExpandEnvironment(buffer);
}

std::wstring DefaultLocalDumpStoreDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (written == 0 || written >= MAX_PATH) {
        return {};
    }
    std::filesystem::path store(localAppData);
    store /= Utf8ToWide(ce::crash_dump_policy::kWerLocalDumpsDefaultRelativeDir);
    return store.wstring();
}

void AppendUniqueDirectory(std::vector<std::wstring>& directories, std::wstring candidate) {
    if (candidate.empty()) {
        return;
    }
    for (const auto& existing : directories) {
        if (_wcsicmp(existing.c_str(), candidate.c_str()) == 0) {
            return;
        }
    }
    directories.push_back(std::move(candidate));
}

// "WerFault has finished" must be an observation, never a timing assumption:
// an exclusive open succeeds only once WerFault closed its own handle. The
// caller retries on its next poll tick, so a dump still being written is simply
// reported as absent rather than waited for.
bool IsCompletedNonEmptyDumpFile(const std::filesystem::path& path, uint64_t* outSizeBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        return false;
    }
    // An exclusive open is what proves WerFault closed its handle. While it is
    // still writing, the file is open without FILE_SHARE_READ for other writers
    // and this fails, so no second sample and no elapsed-time guess is needed.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0 /*no sharing*/, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    if (outSizeBytes) {
        *outSizeBytes = static_cast<uint64_t>(size);
    }
    return true;
}

}  // namespace

std::vector<std::wstring> ResolveLocalDumpSearchDirectories(const char* imageFileName) {
    std::vector<std::wstring> directories;
    const std::wstring localDumpsKey = ce::crash_dump_policy::kWerLocalDumpsKeyPath;

    const std::wstring imageName = Utf8ToWide(imageFileName ? imageFileName : "");
    if (!imageName.empty()) {
        AppendUniqueDirectory(directories,
                              ReadLocalDumpsFolderValue(HKEY_LOCAL_MACHINE, localDumpsKey + L"\\" + imageName));
    }
    AppendUniqueDirectory(directories, ReadLocalDumpsFolderValue(HKEY_LOCAL_MACHINE, localDumpsKey));
    AppendUniqueDirectory(directories, DefaultLocalDumpStoreDirectory());
    return directories;
}

bool SessionDirectoryHasDumpForProcess(const std::string& sessionDumpDirectory, DWORD processId) {
    if (sessionDumpDirectory.empty()) {
        return false;
    }
    char pidToken[32] = {};
    snprintf(pidToken, sizeof(pidToken), "pid%lu", processId);

    std::error_code ec;
    std::filesystem::directory_iterator it(sessionDumpDirectory, ec);
    if (ec) {
        return false;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string fileName = entry.path().filename().string();
        if (!ce::crash_dump_policy::EndsWithAsciiInsensitive(fileName.c_str(), ".dmp")) {
            continue;
        }
        if (ce::crash_dump_policy::ContainsAsciiInsensitive(fileName.c_str(), pidToken)) {
            return true;
        }
    }
    return false;
}

bool TryAdoptLocalDump(const char* imageFileName, DWORD processId, const std::string& sessionDumpDirectory,
                       std::string* outAdoptedPath, std::string* outSourcePath, uint64_t* outSizeBytes) {
    if (sessionDumpDirectory.empty()) {
        return false;
    }
    const std::string werFileName = ce::crash_dump_policy::BuildWerLocalDumpFileName(imageFileName, processId);
    if (werFileName.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(sessionDumpDirectory, ec);
    if (ec) {
        return false;
    }

    const std::string adoptedFileName =
        ce::crash_dump_policy::BuildAdoptedWerCrashDumpFileName(imageFileName, processId);
    const std::filesystem::path destination = std::filesystem::path(sessionDumpDirectory) / adoptedFileName;

    for (const std::wstring& directory : ResolveLocalDumpSearchDirectories(imageFileName)) {
        const std::filesystem::path candidate = std::filesystem::path(directory) / Utf8ToWide(werFileName);
        uint64_t sizeBytes = 0;
        if (!IsCompletedNonEmptyDumpFile(candidate, &sizeBytes)) {
            continue;
        }

        // Move, so the WER store does not keep a second multi-hundred-megabyte
        // copy of the same dump. A cross-volume move falls back to copy+remove.
        ec.clear();
        std::filesystem::rename(candidate, destination, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(candidate, destination, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                LogWarn("[CrashDump] Found WER dump %s but could not move it into the session (%s)",
                        WideToUtf8(candidate.wstring()).c_str(), ec.message().c_str());
                continue;
            }
            std::error_code removeEc;
            std::filesystem::remove(candidate, removeEc);
        }

        if (outAdoptedPath) {
            *outAdoptedPath = destination.string();
        }
        if (outSourcePath) {
            *outSourcePath = WideToUtf8(candidate.wstring());
        }
        if (outSizeBytes) {
            *outSizeBytes = sizeBytes;
        }
        return true;
    }
    return false;
}

size_t PurgeInertCaptureEngineLocalDumpsRegistration(const std::string& captureEngineLogsRoot) {
    if (captureEngineLogsRoot.empty()) {
        return 0;
    }
    const std::wstring localDumpsKey = ce::crash_dump_policy::kWerLocalDumpsKeyPath;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, localDumpsKey.c_str(), 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) {
        return 0;
    }

    std::vector<std::wstring> doomedSubKeys;
    for (DWORD index = 0;; ++index) {
        wchar_t subKeyName[256] = {};
        DWORD subKeyLength = static_cast<DWORD>(std::size(subKeyName));
        const LSTATUS status = RegEnumKeyExW(key, index, subKeyName, &subKeyLength, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS) {
            break;
        }
        const std::wstring dumpFolder = ReadLocalDumpsFolderValue(key, subKeyName);
        if (ce::crash_dump_policy::IsCaptureEngineWrittenLocalDumpsSubkey(WideToUtf8(dumpFolder).c_str(),
                                                                         captureEngineLogsRoot.c_str())) {
            doomedSubKeys.emplace_back(subKeyName);
        }
    }

    size_t removed = 0;
    for (const std::wstring& subKeyName : doomedSubKeys) {
        if (RegDeleteTreeW(key, subKeyName.c_str()) == ERROR_SUCCESS) {
            ++removed;
        }
    }

    // The root itself also carried CE's values in older builds. Only the three
    // values CE wrote are removed, and only when DumpFolder still names a CE
    // session directory; the key stays so another product's subkeys survive.
    const std::wstring rootDumpFolder = ReadLocalDumpsFolderValue(HKEY_CURRENT_USER, localDumpsKey);
    if (ce::crash_dump_policy::IsCaptureEngineWrittenLocalDumpsSubkey(WideToUtf8(rootDumpFolder).c_str(),
                                                                     captureEngineLogsRoot.c_str())) {
        RegDeleteValueW(key, L"DumpFolder");
        RegDeleteValueW(key, L"DumpType");
        RegDeleteValueW(key, L"DumpCount");
        ++removed;
    }

    RegCloseKey(key);
    if (removed > 0) {
        LogInfo(
            "[CrashDump] Removed %zu inert HKCU WER LocalDumps entries left by earlier builds (WER reads LocalDumps "
            "from HKLM only, so they never produced a dump)",
            removed);
    }
    return removed;
}

}  // namespace ce::wer_dump_adoption

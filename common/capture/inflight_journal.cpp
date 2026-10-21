#include "inflight_journal.h"

#include "common/logging/log_privacy.h"
#include "common/logging/logging.h"

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace ce::capture_output {
namespace {

#if defined(CE_UNIT_TESTS)
constexpr wchar_t kProductFolder[] = L"CaptureEngine_UT";
#else
constexpr wchar_t kProductFolder[] = L"CaptureEngine";
#endif
constexpr wchar_t kEntryExtension[] = L".path";

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::wstring FromUtf8(const std::string& utf8) {
    if (utf8.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), chars);
    return wide;
}

// FNV-1a over the case-folded UTF-16 path: one entry per staging file, whatever the caller's spelling of its case.
uint64_t PathHash(const std::filesystem::path& path) {
    std::wstring folded = path.lexically_normal().wstring();
    if (!folded.empty())
        CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t c : folded) {
        hash ^= static_cast<uint16_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

void LogWriteFailureOnce(const std::filesystem::path& directory, DWORD error) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        LogWarn("[RecordingRecovery] cannot record an in-flight recording in %s (error=%lu); an interrupted recording "
                "will not be repaired automatically",
                ce::privacy::CollapsePathForLog(directory.string()).c_str(), static_cast<unsigned long>(error));
}

}  // namespace

InFlightJournal::InFlightJournal(std::filesystem::path directory) : directory_(std::move(directory)) {}

InFlightJournal InFlightJournal::ForThisUser() {
    PWSTR localAppData = nullptr;
    std::filesystem::path directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) &&
        localAppData) {
        directory = std::filesystem::path(localAppData) / kProductFolder / L"inflight";
    }
    if (localAppData)
        CoTaskMemFree(localAppData);
    return InFlightJournal(std::move(directory));
}

std::filesystem::path InFlightJournal::EntryPath(const std::filesystem::path& staging) const {
    wchar_t name[32];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"%016llx%ls", static_cast<unsigned long long>(PathHash(staging)),
             kEntryExtension);
    return directory_ / name;
}

bool InFlightJournal::Add(const std::filesystem::path& staging) const {
    if (directory_.empty() || staging.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    const std::filesystem::path entry = EntryPath(staging);
    const std::string content = ToUtf8(std::filesystem::absolute(staging, ec).wstring());
    HANDLE file = CreateFileW(entry.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LogWriteFailureOnce(directory_, GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size();
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(entry.c_str());
        LogWriteFailureOnce(directory_, error);
    }
    return ok;
}

void InFlightJournal::Remove(const std::filesystem::path& staging) const {
    if (directory_.empty() || staging.empty())
        return;
    DeleteFileW(EntryPath(staging).c_str());
}

std::vector<std::filesystem::path> InFlightJournal::Entries() const {
    std::vector<std::filesystem::path> entries;
    std::error_code ec;
    if (directory_.empty() || !std::filesystem::is_directory(directory_, ec))
        return entries;
    for (const auto& item : std::filesystem::directory_iterator(directory_, ec)) {
        if (!item.is_regular_file(ec) || item.path().extension() != kEntryExtension)
            continue;
        std::ifstream in(item.path(), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::wstring path = FromUtf8(content);
        if (path.empty())
            continue;
        entries.emplace_back(path);
    }
    return entries;
}

}  // namespace ce::capture_output

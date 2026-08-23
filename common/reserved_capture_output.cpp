#include "reserved_capture_output.h"

#include "log_privacy.h"
#include "logging.h"
#include "path_utils.h"

#include <atomic>
#include <array>
#include <cwchar>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace ce::capture_output {
namespace {

std::atomic<uint64_t> g_outputSequence{1};

OutputNameSeed CurrentSeed() {
    FILETIME fileTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;

    OutputNameSeed seed;
    seed.utcMilliseconds = ticks.QuadPart / 10000ull;
    seed.processId = GetCurrentProcessId();
    seed.sequence = g_outputSequence.fetch_add(1, std::memory_order_relaxed);
    return seed;
}

bool IsSafeFilenameComponent(const std::wstring& value, size_t maximumLength, bool allowEmpty) {
    if ((!allowEmpty && value.empty()) || value.size() > maximumLength)
        return false;
    for (wchar_t character : value) {
        const bool alphaNumeric = (character >= L'a' && character <= L'z') ||
                                  (character >= L'A' && character <= L'Z') || (character >= L'0' && character <= L'9');
        if (!alphaNumeric && character != L'_' && character != L'-')
            return false;
    }
    return true;
}

std::optional<std::wstring> BuildFilename(const std::wstring& prefix, const std::wstring& extension,
                                          const OutputNameSeed& seed, uint32_t collisionAttempt) {
    std::wstring normalizedExtension = extension;
    if (!normalizedExtension.empty() && normalizedExtension.front() == L'.')
        normalizedExtension.erase(normalizedExtension.begin());
    if (!IsSafeFilenameComponent(prefix, 64, false) || !IsSafeFilenameComponent(normalizedExtension, 16, true) ||
        seed.utcMilliseconds > std::numeric_limits<uint64_t>::max() / 10000ull) {
        return std::nullopt;
    }

    ULARGE_INTEGER ticks{};
    ticks.QuadPart = seed.utcMilliseconds * 10000ull;
    FILETIME fileTime{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&fileTime, &utc))
        return std::nullopt;
    const unsigned milliseconds = static_cast<unsigned>(seed.utcMilliseconds % 1000ull);

    std::array<wchar_t, 192> buffer{};
    const int written =
        _snwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, L"%ls_%04u%02u%02uT%02u%02u%02u%03uZ_p%lu_s%llu%ls%ls%ls",
                     prefix.c_str(), utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, milliseconds,
                     static_cast<unsigned long>(seed.processId), static_cast<unsigned long long>(seed.sequence),
                     collisionAttempt == 0 ? L"" : (L"_c" + std::to_wstring(collisionAttempt)).c_str(),
                     normalizedExtension.empty() ? L"" : L".", normalizedExtension.c_str());
    if (written <= 0 || static_cast<size_t>(written) >= buffer.size())
        return std::nullopt;
    return buffer.data();
}

}  // namespace

std::filesystem::path GetExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ResolveCaptureDirectory(const std::string& configuredDirectory,
                                              const std::filesystem::path& executableDirectory) {
    std::filesystem::path directory =
        configuredDirectory.empty() ? executableDirectory / L"captures" : std::filesystem::path(configuredDirectory);
    if (!configuredDirectory.empty() && directory.is_relative()) {
        directory = executableDirectory / directory;
    }
    directory = ce::path::ResolveMappedDrivePath(directory).path;

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error) {
        return directory;
    }

    // A user with an invalid/unwritable [Output] directory must never get
    // recordings in an unexpected location without a diagnostic. Rate-limit
    // the warning so a persistently failing mapped drive cannot spam the log.
    static std::atomic<uint32_t> s_fallbackLogCount{0};
    const uint32_t fallbackIndex = s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed);
    if (fallbackIndex < 4) {
        LogWarn(
            "[Output] Configured capture directory could not be created; using fallback '%ls' "
            "(configured='%s' error=%u). Recordings will be written below the fallback path.",
            (executableDirectory / L"captures").wstring().c_str(),
            ce::privacy::CollapsePathForLog(configuredDirectory).c_str(), static_cast<unsigned>(error.value()));
    } else if (fallbackIndex == 4) {
        LogWarn("[Output] Further capture-directory fallback diagnostics are suppressed for this process");
    }
    const std::filesystem::path fallback = executableDirectory / L"captures";
    error.clear();
    std::filesystem::create_directories(fallback, error);
    return fallback;
}

ReservedCaptureOutput::~ReservedCaptureOutput() {
    Reset();
}

ReservedCaptureOutput::ReservedCaptureOutput(ReservedCaptureOutput&& other) noexcept {
    *this = std::move(other);
}

ReservedCaptureOutput& ReservedCaptureOutput::operator=(ReservedCaptureOutput&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Reset();
    path_ = std::move(other.path_);
    reservationHandle_ = std::exchange(other.reservationHandle_, INVALID_HANDLE_VALUE);
    identity_ = other.identity_;
    published_ = other.published_;
    other.identity_ = {};
    other.published_ = true;
    return *this;
}

ReservedCaptureOutput ReservedCaptureOutput::Reserve(const std::filesystem::path& directory, const std::wstring& prefix,
                                                     const std::wstring& extension) {
    return ReserveWithSeed(directory, prefix, extension, CurrentSeed());
}

ReservedCaptureOutput ReservedCaptureOutput::ReserveForTesting(const std::filesystem::path& directory,
                                                               const std::wstring& prefix,
                                                               const std::wstring& extension,
                                                               const OutputNameSeed& seed) {
    return ReserveWithSeed(directory, prefix, extension, seed);
}

ReservedCaptureOutput ReservedCaptureOutput::ReserveWithSeed(const std::filesystem::path& directory,
                                                             const std::wstring& prefix, const std::wstring& extension,
                                                             const OutputNameSeed& seed) {
    ReservedCaptureOutput output;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return output;
    }

    constexpr uint32_t kMaximumCollisionAttempts = 1024;
    for (uint32_t attempt = 0; attempt < kMaximumCollisionAttempts; ++attempt) {
        const std::optional<std::wstring> filename = BuildFilename(prefix, extension, seed, attempt);
        if (!filename)
            return output;
        const std::filesystem::path candidate = directory / *filename;
        // A metadata-only (desiredAccess == 0) handle does not participate in
        // Windows delete-share enforcement. Keep read access so the live
        // reservation prevents DeleteFile/ReplaceFile while still allowing the
        // muxer to reopen and write the placeholder through FILE_SHARE_WRITE.
        HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            output.path_ = candidate;
            output.reservationHandle_ = handle;
            if (!QueryIdentity(handle, output.identity_)) {
                output.Reset();
                return {};
            }
            return output;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return output;
        }
    }
    return output;
}

bool ReservedCaptureOutput::QueryIdentity(HANDLE handle, FileIdentity& identity) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        identity = {};
        return false;
    }
    identity.volumeSerial = info.dwVolumeSerialNumber;
    identity.fileIndex = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    identity.valid = true;
    return true;
}

std::string ReservedCaptureOutput::Utf8Path() const {
    const std::wstring wide = path_.wstring();
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return path_.string();
    }
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    return utf8;
}

bool ReservedCaptureOutput::ReleaseToWriter() {
    if (path_.empty() || published_) {
        return false;
    }
    return reservationHandle_ != INVALID_HANDLE_VALUE && CurrentPathMatchesReservation();
}

void ReservedCaptureOutput::Publish() {
    if (reservationHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(reservationHandle_);
        reservationHandle_ = INVALID_HANDLE_VALUE;
    }
    published_ = true;
}

bool ReservedCaptureOutput::CurrentPathMatchesReservation() const {
    if (path_.empty() || !identity_.valid) {
        return false;
    }
    HANDLE handle =
        CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FileIdentity current{};
    const bool queried = QueryIdentity(handle, current);
    CloseHandle(handle);
    return queried && current.valid && current.volumeSerial == identity_.volumeSerial &&
           current.fileIndex == identity_.fileIndex;
}

bool ReservedCaptureOutput::CommitStagingFile(ReservedCaptureOutput& staging) {
    if (&staging == this || published_ || staging.published_ || path_.empty() || staging.path_.empty() ||
        reservationHandle_ == INVALID_HANDLE_VALUE || staging.reservationHandle_ == INVALID_HANDLE_VALUE ||
        !CurrentPathMatchesReservation() || !staging.CurrentPathMatchesReservation()) {
        return false;
    }
    if (!CloseHandle(staging.reservationHandle_)) {
        return false;
    }
    staging.reservationHandle_ = INVALID_HANDLE_VALUE;
    if (!CloseHandle(reservationHandle_)) {
        return false;
    }
    reservationHandle_ = INVALID_HANDLE_VALUE;
    if (!ReplaceFileW(path_.c_str(), staging.path_.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return false;
    }
    published_ = true;
    staging.published_ = true;
    return true;
}

bool ReservedCaptureOutput::PublishToNewPath(const std::filesystem::path& directory, const std::wstring& prefix,
                                             const std::wstring& extension) {
    return PublishToNewPathWithSeed(directory, prefix, extension, CurrentSeed());
}

bool ReservedCaptureOutput::PublishToNewPathForTesting(const std::filesystem::path& directory,
                                                       const std::wstring& prefix, const std::wstring& extension,
                                                       const OutputNameSeed& seed) {
    return PublishToNewPathWithSeed(directory, prefix, extension, seed);
}

bool ReservedCaptureOutput::PublishToNewPathWithSeed(const std::filesystem::path& directory,
                                                     const std::wstring& prefix, const std::wstring& extension,
                                                     const OutputNameSeed& seed) {
    if (published_ || path_.empty() || reservationHandle_ == INVALID_HANDLE_VALUE ||
        path_.parent_path() != directory || !CurrentPathMatchesReservation()) {
        SetLastError(ERROR_INVALID_STATE);
        return false;
    }
    if (!CloseHandle(reservationHandle_)) {
        reservationHandle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    reservationHandle_ = INVALID_HANDLE_VALUE;

    constexpr uint32_t kMaximumCollisionAttempts = 1024;
    for (uint32_t attempt = 0; attempt < kMaximumCollisionAttempts; ++attempt) {
        const std::optional<std::wstring> filename = BuildFilename(prefix, extension, seed, attempt);
        if (!filename) {
            SetLastError(ERROR_INVALID_NAME);
            return false;
        }
        const std::filesystem::path candidate = directory / *filename;
        if (MoveFileExW(path_.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH)) {
            path_ = candidate;
            published_ = true;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            SetLastError(error);
            return false;
        }
    }
    SetLastError(ERROR_FILE_EXISTS);
    return false;
}

bool ReservedCaptureOutput::CleanupOwnedFile() {
    if (published_ || path_.empty() || !identity_.valid) {
        return false;
    }
    if (reservationHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(reservationHandle_);
        reservationHandle_ = INVALID_HANDLE_VALUE;
    }

    HANDLE handle = CreateFileW(path_.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FileIdentity current{};
    const bool matches = QueryIdentity(handle, current) && current.valid &&
                         current.volumeSerial == identity_.volumeSerial && current.fileIndex == identity_.fileIndex;
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool removed =
        matches && SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    CloseHandle(handle);
    if (removed) {
        path_.clear();
        identity_ = {};
    }
    return removed;
}

void ReservedCaptureOutput::Reset() noexcept {
    if (!published_) {
        CleanupOwnedFile();
    } else if (reservationHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(reservationHandle_);
    }
    reservationHandle_ = INVALID_HANDLE_VALUE;
    path_.clear();
    identity_ = {};
    published_ = false;
}

}  // namespace ce::capture_output

#include "service_internal.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <filesystem>
#include <iterator>

#include "common/logging/logging.h"
#include "common/setup/elevation_runtime_policy.h"

// The display flight recorder for an unelevated CaptureEngine: a circular raw ETW recording of the display pipeline
// (display_flight_recorder.h) that CE's display timing seals when a scanout stall episode is over. The service writes
// only into <installation>\DisplayTraces, a folder it creates under the administrator-only installation folder, so no
// client can redirect a SYSTEM write through a link. A sealed file is handed over by granting the installing user read
// and delete on it; the client moves it into its own session folder.
namespace ce::elevation {
namespace {

constexpr wchar_t kFlightSessionName[] = L"CE_DisplayFlight_Elevation";
constexpr wchar_t kWorkFileName[] = L"display_trace.recording";
// A client asks again for every new episode; the limits only stop a looping client from churning the disk.
constexpr uint64_t kMinimumSealIntervalMs = 2'000;
constexpr uint32_t kMaximumSealsPerRecording = 32;
// A sealed file the client never collected (it crashed) is removed when a recording starts.
constexpr uint64_t kUncollectedLifetimeMs = 10ull * 60 * 1000;

std::filesystem::path TraceFolder() {
    const std::filesystem::path installation = ce::startup::ApplicationFolderForServiceExecutable(ExecutablePath());
    return installation.empty() ? std::filesystem::path{} : installation / kSealedDisplayTraceFolder;
}

// The folder exists as a plain directory: never a link someone planted before the service created it.
DWORD PrepareTraceFolder(const std::filesystem::path& folder) {
    if (folder.empty())
        return ERROR_BAD_PATHNAME;
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError();
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return ERROR_CANT_ACCESS_FILE;
    return ERROR_SUCCESS;
}

void RemoveUncollectedTraces(const std::filesystem::path& folder) {
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = (folder / (std::wstring(kSealedDisplayTracePrefix) + L"*.etl")).wstring();
    HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    do {
        const uint64_t ageMs = (ticks(now) - ticks(entry.ftLastWriteTime)) / 10'000;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && ageMs > kUncollectedLifetimeMs)
            DeleteFileW((folder / entry.cFileName).c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

// Adds read and delete for the installing user to the sealed file's inherited DACL.
DWORD GrantOwnerCollection(const std::filesystem::path& file, const std::wstring& owner) {
    PSID ownerSid = nullptr;
    if (!ConvertStringSidToSidW(owner.c_str(), &ownerSid))
        return GetLastError();
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &current, nullptr, &descriptor);
    PACL updated = nullptr;
    if (status == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | DELETE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(ownerSid);
        status = SetEntriesInAclW(1, &access, current, &updated);
    }
    if (status == ERROR_SUCCESS) {
        std::wstring path = file.wstring();
        status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                       updated, nullptr);
    }
    if (updated)
        LocalFree(updated);
    if (descriptor)
        LocalFree(descriptor);
    LocalFree(ownerSid);
    return status;
}

void LogProviders(const display_flight_recorder::ProviderStatus& providers) {
    LogInfo("[ElevationService] Display flight recorder on (circular %luMB, providers dxgi=%lu dxgkrnl=%lu dwm=%lu "
            "tokens=%lu nvidia=%lu frameType=%lu process=%lu)",
            display_flight_recorder::kMaximumFileMegabytes, providers.runtime, providers.graphicsKernel,
            providers.compositor, providers.tokens, providers.nvidiaSchedule, providers.frameType, providers.process);
}

}  // namespace

DWORD TraceManager::AcquireFlight() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (flightUsers_ == 0) {
        const std::filesystem::path folder = TraceFolder();
        DWORD status = PrepareTraceFolder(folder);
        if (status == ERROR_SUCCESS) {
            RemoveUncollectedTraces(folder);
            status = flight_.Start(kFlightSessionName, (folder / kWorkFileName).wstring());
        }
        if (status != ERROR_SUCCESS) {
            LogWarn("[ElevationService] Display flight recorder could not start: %lu", status);
            return status;
        }
        flightSeals_ = 0;
        LogProviders(flight_.Providers());
    }
    ++flightUsers_;
    return ERROR_SUCCESS;
}

void TraceManager::ReleaseFlight() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (flightUsers_ == 0)
        return;
    if (--flightUsers_ == 0)
        StopFlightLocked();
}

void TraceManager::StopFlightLocked() {
    if (flight_.Running())
        LogInfo("[ElevationService] Display flight recorder off after %u seal(s)", flightSeals_);
    flight_.Stop();
    flightUsers_ = 0;
}

SealedDisplayTrace TraceManager::SealFlight() {
    SealedDisplayTrace sealed;
    std::lock_guard<std::mutex> guard(mutex_);
    const uint64_t nowMs = GetTickCount64();
    if (!flight_.Running()) {
        sealed.status = ERROR_INVALID_STATE;
        return sealed;
    }
    const bool tooSoon = lastSealTickMs_ != 0 && nowMs - lastSealTickMs_ < kMinimumSealIntervalMs;
    if (flightSeals_ >= kMaximumSealsPerRecording || tooSoon) {
        sealed.status = ERROR_RETRY;
        return sealed;
    }
    lastSealTickMs_ = nowMs;
    ++flightSeals_;
    wchar_t name[64] = {};
    swprintf(name, std::size(name), L"%ls%llu.etl", kSealedDisplayTracePrefix, static_cast<unsigned long long>(nowMs));
    const std::filesystem::path file = TraceFolder() / name;
    const display_flight_recorder::SealResult result = flight_.Seal(file.wstring());
    sealed.status = result.status;
    sealed.restartStatus = result.restartStatus;
    sealed.eventsLost = result.eventsLost;
    sealed.buffersLost = result.buffersLost;
    sealed.bytes = result.bytes;
    if (sealed.status == ERROR_SUCCESS)
        sealed.status = GrantOwnerCollection(file, owner_);
    if (sealed.status == ERROR_SUCCESS)
        wcsncpy_s(sealed.path.data(), sealed.path.size(), file.c_str(), _TRUNCATE);
    else
        DeleteFileW(file.c_str());
    LogInfo("[ElevationService] Display flight recording sealed (status=%u restart=%u bytes=%llu eventsLost=%u "
            "buffersLost=%u seal=%u)",
            sealed.status, sealed.restartStatus, static_cast<unsigned long long>(sealed.bytes), sealed.eventsLost,
            sealed.buffersLost, flightSeals_);
    return sealed;
}

}  // namespace ce::elevation

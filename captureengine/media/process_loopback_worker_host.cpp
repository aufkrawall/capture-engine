#include "process_loopback_worker_host.h"

#include <windows.h>
#include <shellapi.h>

#include "common/platform/build_identity_check.h"
#include "common/platform/package_module.h"
#include "common/platform/raii_helpers.h"
#include "common/platform/secure_dll_loading.h"
#include "mediaengine/audio/process_loopback_protocol.h"
#include "mediaengine/audio/process_loopback_selector_transport.h"
#include "mediaengine/audio/process_loopback_selector_validation.h"

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <limits>

namespace {

constexpr wchar_t kProcessLoopbackWorkerCommand[] = L"--process-loopback-worker";

using WorkerEntry = int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, const wchar_t*, int, int, uint32_t);
using WorkerEntryV2 = int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, const wchar_t*, int, int, uint32_t,
                              uint32_t, const wchar_t* const*);
using WorkerEntryV3 = int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, const wchar_t*, int, int, uint32_t,
                              const void*, uint64_t);

bool ParseUnsigned(const wchar_t* value, uint64_t minimum, uint64_t maximum, uint64_t& result) {
    if (!value || !*value || *value < L'0' || *value > L'9') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(value, &end, 10);
    if (errno == ERANGE || !end || *end != L'\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    result = static_cast<uint64_t>(parsed);
    return true;
}

bool IsInheritedHandle(uint64_t value) {
    if (value == 0 || value > std::numeric_limits<uintptr_t>::max()) {
        return false;
    }
    DWORD flags = 0;
    return GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)), &flags) != FALSE &&
           (flags & HANDLE_FLAG_INHERIT) != 0;
}

bool IsUnsignaledEvent(HANDLE handle) {
    if (WaitForSingleObject(handle, 0) != WAIT_TIMEOUT || !SetEvent(handle)) {
        return false;
    }
    const bool signaled = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    const bool remainsSignaled = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    const bool reset = ResetEvent(handle) != FALSE;
    const bool unsignaledAgain = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    return signaled && remainsSignaled && reset && unsignaledAgain;
}

bool ValidateInheritedTransport(uint64_t mappingValue, uint64_t packetEventValue, uint64_t stopEventValue,
                                uint64_t generation, uint32_t sampleRate, uint32_t channels) {
    const HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(mappingValue));
    const HANDLE packetEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packetEventValue));
    const HANDLE stopEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(stopEventValue));
    const uint64_t mappingBytes = ce::process_loopback::MappingBytes(sampleRate, channels, 32);
    if (mappingBytes == 0 || mappingBytes > std::numeric_limits<SIZE_T>::max() ||
        !IsUnsignaledEvent(packetEvent) || !IsUnsignaledEvent(stopEvent)) {
        return false;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(mappingBytes));
    const bool valid = view && ce::process_loopback::Validate(view, generation);
    if (view)
        UnmapViewOfFile(view);
    return valid;
}

int RunProcessLoopbackWorkerHost(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 11) {
        return ERROR_BAD_ARGUMENTS;
    }
    uint64_t mappingHandle = 0;
    uint64_t packetEvent = 0;
    uint64_t stopEvent = 0;
    uint64_t generation = 0;
    uint64_t targetPid = 0;
    uint64_t sampleRate = 0;
    uint64_t channels = 0;
    uint64_t channelMask = 0;
    if (!ParseUnsigned(arguments[2], 1, std::numeric_limits<uintptr_t>::max(), mappingHandle) ||
        !ParseUnsigned(arguments[3], 1, std::numeric_limits<uintptr_t>::max(), packetEvent) ||
        !ParseUnsigned(arguments[4], 1, std::numeric_limits<uintptr_t>::max(), stopEvent) ||
        !ParseUnsigned(arguments[5], 1, std::numeric_limits<uint64_t>::max(), generation) ||
        !ParseUnsigned(arguments[6], 0, std::numeric_limits<uint32_t>::max(), targetPid) ||
        !ParseUnsigned(arguments[7], 8000, 384000, sampleRate) ||
        !ParseUnsigned(arguments[8], 1, 8, channels) ||
        !ParseUnsigned(arguments[9], 0, std::numeric_limits<uint32_t>::max(), channelMask) ||
        (targetPid == 0) == (arguments[10] && *arguments[10] == L'\0') ||
        (targetPid == 0 && (!arguments[10] ||
                            !ce::process_loopback::IsValidWorkerProcessSelector(arguments[10]))) ||
        mappingHandle == packetEvent ||
        mappingHandle == stopEvent || packetEvent == stopEvent || !IsInheritedHandle(mappingHandle) ||
        !IsInheritedHandle(packetEvent) || !IsInheritedHandle(stopEvent) ||
        (channelMask != 0 && static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(channelMask))) != channels) ||
        !ValidateInheritedTransport(mappingHandle, packetEvent, stopEvent, generation,
                                    static_cast<uint32_t>(sampleRate), static_cast<uint32_t>(channels))) {
        return ERROR_INVALID_PARAMETER;
    }
    const bool mappedPolicy = argumentCount == 14 && arguments[11] &&
                              wcscmp(arguments[11], L"--selector-policy") == 0;
    if (targetPid != 0 && argumentCount != 11)
        return ERROR_INVALID_PARAMETER;
    uint64_t policyHandleValue = 0;
    uint64_t policyBytes = 0;
    ce::HandleGuard policyHandle;
    ce::MappingGuard policyView;
    if (mappedPolicy) {
        if (!ParseUnsigned(arguments[12], 1, std::numeric_limits<uintptr_t>::max(), policyHandleValue) ||
            !ParseUnsigned(arguments[13], 8, std::numeric_limits<SIZE_T>::max(), policyBytes) ||
            policyHandleValue == mappingHandle || policyHandleValue == packetEvent || policyHandleValue == stopEvent ||
            !IsInheritedHandle(policyHandleValue))
            return ERROR_INVALID_PARAMETER;
        policyHandle.reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(policyHandleValue)));
        if (!SetHandleInformation(policyHandle.get(), HANDLE_FLAG_INHERIT, 0))
            return ERROR_INVALID_HANDLE;
        policyView.reset(MapViewOfFile(policyHandle.get(), FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(policyBytes)));
        std::vector<WhitelistEntry> parsed;
        if (!policyView || !ce::process_loopback::DecodeSelectorPolicy(
                               std::span(static_cast<const uint8_t*>(policyView.get()), static_cast<size_t>(policyBytes)),
                               parsed))
            return ERROR_INVALID_DATA;
    }
    for (int index = 11; !mappedPolicy && index < argumentCount; ++index) {
        std::wstring_view selector;
        bool exact = false;
        if (!arguments[index] || !ce::process_loopback::DecodeWorkerExclusion(arguments[index], selector, exact))
            return ERROR_INVALID_PARAMETER;
    }
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(mappingHandle)), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packetEvent)), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(stopEvent)), HANDLE_FLAG_INHERIT, 0)) {
        return ERROR_INVALID_HANDLE;
    }
    // The package's mediaengine and FFmpeg, also when this worker is a client's own executable (11, part 6).
    const std::filesystem::path executableDir = ce::package::PackageDirectory();
    if (executableDir.empty()) {
        return static_cast<int>(GetLastError());
    }
    DWORD loadError = ERROR_SUCCESS;
    if (!ce::security::EnsureSecureDllSearchDirectory(executableDir / L"ffmpeg", &loadError)) {
        return static_cast<int>(loadError);
    }
    HMODULE mediaEngine = ce::security::LoadLibraryFromSecurePath(executableDir / L"mediaengine.dll", &loadError);
    if (!mediaEngine) {
        return static_cast<int>(loadError);
    }
    // Same package-private rule as MediaEngine_Load: a DLL from another build is never called into.
    namespace bid = ce::build_identity;
    if (bid::CheckExport(reinterpret_cast<bid::ExportFn>(GetProcAddress(mediaEngine, bid::kMediaEngineExportName)),
                         bid::Current()) != bid::Verdict::Match) {
        FreeLibrary(mediaEngine);
        return ERROR_REVISION_MISMATCH;
    }
    auto entry = reinterpret_cast<WorkerEntry>(GetProcAddress(mediaEngine, "MediaEngine_RunProcessLoopbackWorker"));
    if (!entry) {
        const int error = static_cast<int>(GetLastError());
        FreeLibrary(mediaEngine);
        return error;
    }
    int result = ERROR_PROC_NOT_FOUND;
    if (argumentCount == 11) {
        result = entry(mappingHandle, packetEvent, stopEvent, generation, static_cast<uint32_t>(targetPid),
                       arguments[10], static_cast<int>(sampleRate), static_cast<int>(channels),
                       static_cast<uint32_t>(channelMask));
    } else if (mappedPolicy) {
        auto entryV3 = reinterpret_cast<WorkerEntryV3>(
            GetProcAddress(mediaEngine, "MediaEngine_RunProcessLoopbackWorkerV3"));
        if (entryV3)
            result = entryV3(mappingHandle, packetEvent, stopEvent, generation, static_cast<uint32_t>(targetPid),
                             arguments[10], static_cast<int>(sampleRate), static_cast<int>(channels),
                             static_cast<uint32_t>(channelMask), policyView.get(), policyBytes);
    } else {
        auto entryV2 = reinterpret_cast<WorkerEntryV2>(
            GetProcAddress(mediaEngine, "MediaEngine_RunProcessLoopbackWorkerV2"));
        if (entryV2)
            result = entryV2(mappingHandle, packetEvent, stopEvent, generation, static_cast<uint32_t>(targetPid),
                             arguments[10], static_cast<int>(sampleRate), static_cast<int>(channels),
                             static_cast<uint32_t>(channelMask), static_cast<uint32_t>(argumentCount - 11),
                             arguments + 11);
    }
    FreeLibrary(mediaEngine);
    return result;
}

}  // namespace

std::optional<int> TryRunProcessLoopbackWorkerHost() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        return ERROR_BAD_ARGUMENTS;
    }
    if (argumentCount < 2 || wcscmp(arguments[1], kProcessLoopbackWorkerCommand) != 0) {
        LocalFree(reinterpret_cast<HLOCAL>(arguments));
        return std::nullopt;
    }
    const int result = RunProcessLoopbackWorkerHost(argumentCount, arguments);
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return result;
}

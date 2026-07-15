#include <windows.h>

#include "../mediaengine/process_loopback_protocol.h"
#include "../common/secure_dll_loading.h"

#include <bit>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <limits>

namespace {

using WorkerEntry = int (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint32_t, const wchar_t*, int, int, uint32_t);

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

bool IsValidProcessName(const wchar_t* name) {
    if (!name || !*name) {
        return false;
    }
    const size_t length = wcslen(name);
    if (length >= MAX_PATH || wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0 ||
        name[length - 1] == L'.' || name[length - 1] == L' ' ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, nullptr, 0, nullptr, nullptr) <= 1) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const wchar_t character = name[index];
        if (character < 0x20 || wcschr(L"\\/:*?\"<>|", character)) {
            return false;
        }
    }
    return true;
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 10) {
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
    if (!ParseUnsigned(argv[1], 1, std::numeric_limits<uintptr_t>::max(), mappingHandle) ||
        !ParseUnsigned(argv[2], 1, std::numeric_limits<uintptr_t>::max(), packetEvent) ||
        !ParseUnsigned(argv[3], 1, std::numeric_limits<uintptr_t>::max(), stopEvent) ||
        !ParseUnsigned(argv[4], 1, std::numeric_limits<uint64_t>::max(), generation) ||
        !ParseUnsigned(argv[5], 0, std::numeric_limits<uint32_t>::max(), targetPid) ||
        !ParseUnsigned(argv[6], 8000, 384000, sampleRate) || !ParseUnsigned(argv[7], 1, 8, channels) ||
        !ParseUnsigned(argv[8], 0, std::numeric_limits<uint32_t>::max(), channelMask) ||
        (targetPid == 0) == (argv[9] && *argv[9] == L'\0') || (targetPid == 0 && !IsValidProcessName(argv[9])) ||
        mappingHandle == packetEvent ||
        mappingHandle == stopEvent || packetEvent == stopEvent || !IsInheritedHandle(mappingHandle) ||
        !IsInheritedHandle(packetEvent) || !IsInheritedHandle(stopEvent) ||
        (channelMask != 0 && std::popcount(static_cast<uint32_t>(channelMask)) != channels) ||
        !ValidateInheritedTransport(mappingHandle, packetEvent, stopEvent, generation,
                                    static_cast<uint32_t>(sampleRate), static_cast<uint32_t>(channels))) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(mappingHandle)), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packetEvent)), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(stopEvent)), HANDLE_FLAG_INHERIT, 0)) {
        return ERROR_INVALID_HANDLE;
    }
    wchar_t executablePath[MAX_PATH]{};
    const DWORD chars = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (chars == 0 || chars >= MAX_PATH) {
        return static_cast<int>(GetLastError());
    }
    const std::filesystem::path executableDir = std::filesystem::path(executablePath).parent_path();
    DWORD loadError = ERROR_SUCCESS;
    if (!ce::security::EnsureSecureDllSearchDirectory(executableDir / L"ffmpeg", &loadError)) {
        return static_cast<int>(loadError);
    }
    HMODULE mediaEngine =
        ce::security::LoadLibraryFromSecurePath(executableDir / L"mediaengine.dll", &loadError);
    if (!mediaEngine) {
        return static_cast<int>(loadError);
    }
    auto entry = reinterpret_cast<WorkerEntry>(GetProcAddress(mediaEngine, "MediaEngine_RunProcessLoopbackWorker"));
    if (!entry) {
        const int error = static_cast<int>(GetLastError());
        FreeLibrary(mediaEngine);
        return error;
    }
    const int result = entry(mappingHandle, packetEvent, stopEvent, generation, static_cast<uint32_t>(targetPid),
                             argv[9], static_cast<int>(sampleRate), static_cast<int>(channels),
                             static_cast<uint32_t>(channelMask));
    FreeLibrary(mediaEngine);
    return result;
}

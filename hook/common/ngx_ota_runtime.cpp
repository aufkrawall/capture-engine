#include "ngx_ota_runtime.h"

#include <windows.h>

#include <atomic>

#include "hook_common.h"
#include "ngx_ota_policy.h"

#include "../../common/shared_defs.h"

namespace ce::ngx_ota {

namespace {

// kNgxOtaModeDefault until a profile resolves. Read from the CreateProcess hook
// on whatever thread the NGX core happens to launch its updater from, so it has
// to be atomic rather than merely written once.
std::atomic<uint8_t> g_Mode{kNgxOtaModeDefault};
std::atomic<uint8_t> g_LogLevel{kNgxLogLevelDefault};
std::atomic<bool> g_PolicyApplied{false};
std::atomic<uint32_t> g_RefusedLaunches{0};
// Set once the shared-memory fallback below has answered, so the mapping is
// opened at most once per process even when the answer is "no host".
std::atomic<bool> g_EarlyModeResolved{false};

// Reads the resolved profile's ngx_ota mode straight out of the injector's
// shared memory.
//
// This exists because the hook thread is too late. The CreateProcess hook goes
// in during DllMain, but the policy it consults was not published until the hook
// thread had loaded config - about 1.1 s later in session 20260918_223542 - and
// NGX launches its updater inside that window. Nine nvngx_update.exe processes,
// all parented to the game, were created at 22:35:48 while CE published the
// policy at 22:35:49.072 and only began refusing at 22:35:49.170. Answering
// "default" until told is what let them through.
//
// The injector had already published the resolved value at 22:35:42.842, six
// seconds before the game even started, so the answer was available the whole
// time. Only OpenFileMapping/MapViewOfFile are used here: neither takes the
// loader lock, so this is safe on the DllMain-time path the CreateProcess hook
// can be entered from.
uint8_t ReadModeFromSharedMemory() {
    uint8_t mode = kNgxOtaModeDefault;

    HANDLE discovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!discovery) {
        return mode;
    }
    auto* info = static_cast<DiscoveryInfo*>(MapViewOfFile(discovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    uint32_t injectPid = 0;
    if (ValidateDiscoveryInfo(info)) {
        injectPid = info->GetInjectPid();
    }
    if (info) {
        UnmapViewOfFile(info);
    }
    CloseHandle(discovery);
    if (injectPid == 0) {
        return mode;
    }

    wchar_t sharedMemName[64] = {};
    GenerateSharedMemName(sharedMemName, 64, injectPid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!mapping) {
        return mode;
    }
    auto* shared =
        static_cast<SharedMemoryLayout*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout)));
    if (shared) {
        // The magic is published last, after every field is constructed, so it
        // is the only safe gate on reading the payload.
        if (shared->GetMagic() == SHARED_MEMORY_MAGIC && shared->GetVersion() == SHARED_MEMORY_VERSION &&
            shared->abiSignature.load(std::memory_order_acquire) == SHARED_MEMORY_ABI_SIGNATURE) {
            const uint8_t published = shared->graphicsConfig.ngxOtaMode;
            if (IsNgxOtaMode(published)) {
                mode = published;
            }
        }
        UnmapViewOfFile(shared);
    }
    CloseHandle(mapping);
    return mode;
}

// SetEnvironmentVariableA with a null value REMOVES the variable, which is what
// clearing an inherited suppression has to do: `_nvngx.dll` reads the whole
// environment block, and an empty-but-present variable is not reliably the same
// as an absent one.
void WriteEnvironmentVariable(const char* name, const char* value) {
    if (!name || !name[0]) {
        return;
    }
    if (value && value[0]) {
        SetEnvironmentVariableA(name, value);
    } else {
        SetEnvironmentVariableA(name, nullptr);
    }
}

void ApplyUpdaterEnvironment(uint8_t mode) {
    const char* value = DisableUpdaterEnvironmentValue(mode);
    if (!value) {
        return;  // `default`: the environment stays exactly as inherited.
    }
    WriteEnvironmentVariable(kDisableUpdaterVariable, value);
    HookLogImportant("NGX OTA: ngx_ota=%s - %s %s", ModeName(mode),
                     value[0] ? "published" : "cleared", kDisableUpdaterVariable);
}

void ApplyLogEnvironment(uint8_t level, const char* sessionLogDirectory) {
    const char* levelValue = NgxLogLevelEnvironmentValue(level);
    if (!levelValue) {
        return;  // `default`: NGX keeps whatever the driver and game decided.
    }
    WriteEnvironmentVariable(kLogLevelVariable, levelValue);
    if (sessionLogDirectory && sessionLogDirectory[0]) {
        WriteEnvironmentVariable(kLogPathOverrideVariable, sessionLogDirectory);
        WriteEnvironmentVariable(kEnableLogPathOverrideVariable, "1");
        HookLogImportant("NGX OTA: ngx_log level %s, NGX log routed to %s", levelValue, sessionLogDirectory);
    } else {
        HookLogImportant("NGX OTA: ngx_log level %s, NGX keeps its own log location", levelValue);
    }
}

}  // namespace

void PublishPolicy(uint8_t otaMode, uint8_t logLevel, const char* sessionLogDirectory) {
    const uint8_t resolvedOta = IsNgxOtaMode(otaMode) ? otaMode : kNgxOtaModeDefault;
    const uint8_t resolvedLog = IsNgxLogLevel(logLevel) ? logLevel : kNgxLogLevelDefault;

    const uint8_t previousOta = g_Mode.exchange(resolvedOta, std::memory_order_acq_rel);
    const uint8_t previousLog = g_LogLevel.exchange(resolvedLog, std::memory_order_acq_rel);
    const bool firstApplication = !g_PolicyApplied.exchange(true, std::memory_order_acq_rel);

    // Rewriting the environment on every config republication would fight a
    // game that sets these itself, so only an actual policy change acts.
    if (firstApplication || previousOta != resolvedOta) {
        ApplyUpdaterEnvironment(resolvedOta);
    }
    if (firstApplication || previousLog != resolvedLog) {
        ApplyLogEnvironment(resolvedLog, sessionLogDirectory);
    }
}

uint8_t CurrentMode() {
    if (g_PolicyApplied.load(std::memory_order_acquire)) {
        return g_Mode.load(std::memory_order_acquire);
    }
    // No policy yet: fall back to what the injector published, once. A process
    // with no CE host, or one whose host predates this field, resolves to
    // default and stops asking.
    if (!g_EarlyModeResolved.exchange(true, std::memory_order_acq_rel)) {
        const uint8_t early = ReadModeFromSharedMemory();
        if (early != kNgxOtaModeDefault) {
            uint8_t expected = kNgxOtaModeDefault;
            if (g_Mode.compare_exchange_strong(expected, early, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                HookLogImportant(
                    "NGX OTA: resolved ngx_ota=%s from the injector's published config before the hook thread's "
                    "own config load, so an updater launched this early is still answered",
                    ModeName(early));
            }
        }
    }
    return g_Mode.load(std::memory_order_acquire);
}

bool ShouldRefuseProcessLaunch(const char* imagePath) {
    if (!imagePath || !imagePath[0]) {
        return false;
    }
    return ShouldRefuseUpdaterLaunch(CurrentMode(), IsNgxUpdaterImage(imagePath));
}

void NoteUpdaterLaunchRefused(const char* imagePath) {
    const uint32_t index = g_RefusedLaunches.fetch_add(1, std::memory_order_relaxed);
    // The NGX core retries per feature, so this can repeat. Log the first few
    // and then only every thousandth, the same rate-limiting shape the loader
    // redirect refusals use.
    if (index < 8 || (index % 1000) == 0) {
        HookLogImportant(
            "NGX OTA: refused the NGX updater launch (%s) because ngx_ota=off; NVIDIA's runtime falls back to the "
            "files already in its cache (refusal #%u)",
            imagePath ? imagePath : "unnamed", index + 1);
    }
}

}  // namespace ce::ngx_ota

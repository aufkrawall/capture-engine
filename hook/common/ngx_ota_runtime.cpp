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
// Set once a CE host has actually answered the shared-memory read below, so the
// steady state costs nothing. Deliberately NOT set when no host answered: there
// is then nothing to cache, the retry is one failed OpenFileMappingW on a path
// that runs per process creation, and a host that starts later still resolves.
std::atomic<bool> g_EarlyModeResolved{false};
// The last value CE wrote to `__NGX_DISABLE_UPDATER`, or -1 for "never wrote
// it". Both the DllMain-time early application and the hook thread's
// PublishPolicy go through the same writer, and neither may restate a value the
// environment already carries - that would duplicate the log line and fight a
// game that set the variable itself between the two.
std::atomic<int32_t> g_EnvironmentOtaMode{-1};
// What `__NGX_DISABLE_UPDATER` held before CE first touched it, so a later
// authoritative `default` can restore exactly what the process received. The
// two writers - DllMain and the hook thread's config load - are ordered by
// construction (the hook thread does not exist yet during DllMain), so the
// capture needs no more than a one-shot.
std::atomic<bool> g_InheritedCaptured{false};
bool g_InheritedPresent = false;
char g_InheritedValue[64] = {};

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
//
// `answered` distinguishes "a CE host published kNgxOtaModeDefault" from "no
// host could be reached", which the returned mode alone cannot express. Only
// the former is worth caching.
uint8_t ReadModeFromSharedMemory(bool& answered) {
    uint8_t mode = kNgxOtaModeDefault;
    answered = false;

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
                answered = true;
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

// Records the inherited value, once, before CE's first write can replace it.
void CaptureInheritedDisableUpdaterOnce() {
    if (g_InheritedCaptured.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const DWORD length =
        GetEnvironmentVariableA(kDisableUpdaterVariable, g_InheritedValue, sizeof(g_InheritedValue));
    // Zero means absent (or unreadable); a length at or beyond the buffer means
    // a value CE cannot reproduce, and inventing a truncation would be worse
    // than admitting it, so both are recorded as "nothing to restore".
    g_InheritedPresent = length > 0 && length < sizeof(g_InheritedValue);
    if (!g_InheritedPresent) {
        g_InheritedValue[0] = '\0';
    }
}

void ApplyUpdaterEnvironment(uint8_t mode, const char* phase) {
    CaptureInheritedDisableUpdaterOnce();

    const char* value = DisableUpdaterEnvironmentValue(mode);
    const int32_t previous =
        g_EnvironmentOtaMode.exchange(static_cast<int32_t>(mode), std::memory_order_acq_rel);
    if (previous == static_cast<int32_t>(mode)) {
        return;  // The environment already states this; restating it says nothing.
    }

    if (!value) {
        // `default` is inert by contract - no environment write. That is only
        // true if CE also undoes a write it made earlier, which it can now
        // have: DllMain applies the injector's resolved mode, and the hook
        // thread's own config can still resolve to `default` (base config vs
        // an active profile). Leaving the suppression behind would make
        // `default` quietly mean `off`.
        if (previous < 0) {
            return;  // CE never wrote it; the environment is already as inherited.
        }
        WriteEnvironmentVariable(kDisableUpdaterVariable, g_InheritedPresent ? g_InheritedValue : nullptr);
        HookLogImportant("NGX OTA: ngx_ota=default - restored %s to what the process inherited (%s) (%s)",
                         kDisableUpdaterVariable, g_InheritedPresent ? g_InheritedValue : "absent",
                         phase ? phase : "unspecified");
        return;
    }

    WriteEnvironmentVariable(kDisableUpdaterVariable, value);
    HookLogImportant("NGX OTA: ngx_ota=%s - %s %s (%s)", ModeName(mode),
                     value[0] ? "published" : "cleared", kDisableUpdaterVariable,
                     phase ? phase : "unspecified");
}

// Resolves the injector's published mode into `g_Mode` if nothing better is
// known yet. Returns the mode in force afterwards.
//
// A caller that finds the read already in flight on another thread repeats it
// rather than settling for "default": duplicating one shared-memory read is
// cheap, and answering "default" to a CreateProcess call that is the NGX
// updater is the exact failure this whole path exists to prevent. The previous
// one-shot exchange had that hole.
uint8_t ResolveEarlyModeFromPublishedConfig() {
    if (g_EarlyModeResolved.load(std::memory_order_acquire)) {
        return g_Mode.load(std::memory_order_acquire);
    }

    bool answered = false;
    const uint8_t early = ReadModeFromSharedMemory(answered);
    if (!answered) {
        return g_Mode.load(std::memory_order_acquire);
    }
    g_EarlyModeResolved.store(true, std::memory_order_release);

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
    return g_Mode.load(std::memory_order_acquire);
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

void ApplyEarlyPolicyFromPublishedConfig() {
    if (g_PolicyApplied.load(std::memory_order_acquire)) {
        return;  // The hook thread's own config already decided; it wins.
    }
    ApplyUpdaterEnvironment(ResolveEarlyModeFromPublishedConfig(), "DllMain");
}

void PublishPolicy(uint8_t otaMode, uint8_t logLevel, const char* sessionLogDirectory) {
    const uint8_t resolvedOta = IsNgxOtaMode(otaMode) ? otaMode : kNgxOtaModeDefault;
    const uint8_t resolvedLog = IsNgxLogLevel(logLevel) ? logLevel : kNgxLogLevelDefault;

    const uint8_t previousOta = g_Mode.exchange(resolvedOta, std::memory_order_acq_rel);
    const uint8_t previousLog = g_LogLevel.exchange(resolvedLog, std::memory_order_acq_rel);
    const bool firstApplication = !g_PolicyApplied.exchange(true, std::memory_order_acq_rel);

    // Rewriting the environment on every config republication would fight a
    // game that sets these itself, so only an actual policy change acts.
    if (firstApplication || previousOta != resolvedOta) {
        ApplyUpdaterEnvironment(resolvedOta, "hook thread config load");
    }
    if (firstApplication || previousLog != resolvedLog) {
        ApplyLogEnvironment(resolvedLog, sessionLogDirectory);
    }
}

uint8_t CurrentMode() {
    if (g_PolicyApplied.load(std::memory_order_acquire)) {
        return g_Mode.load(std::memory_order_acquire);
    }
    // No policy yet: fall back to what the injector published. DllMain normally
    // does this already (ApplyEarlyPolicyFromPublishedConfig), so this is the
    // path for a process that never reached it.
    return ResolveEarlyModeFromPublishedConfig();
}

bool ShouldRefuseProcessLaunch(const char* imagePath) {
    if (!imagePath || !imagePath[0]) {
        return false;
    }
    return ShouldRefuseUpdaterLaunch(CurrentMode(), IsNgxUpdaterImage(imagePath));
}

bool ShouldRefuseProcessLaunch(const wchar_t* imagePath) {
    if (!imagePath || !imagePath[0]) {
        return false;
    }
    return ShouldRefuseUpdaterLaunch(CurrentMode(), IsNgxUpdaterImage(imagePath));
}

namespace {

// The NGX core retries per feature, so a refusal repeats. Log the first few and
// then only every thousandth, the same rate-limiting shape the loader redirect
// refusals use.
// Measured: NGX's startup burst is exactly nine processes in session
// 20260919_194456 - one `dlss`, five `dlssg`, three `dlssd`, all launched
// inside 19:45:13.383-19:45:14. The previous threshold of 8 clipped the last
// one and made "did CE see all of them?" unanswerable from the log, which is
// the one question this diagnostic exists to settle. 32 clears the observed
// burst with room for the extra per-feature retries a refusal can provoke,
// and is still bounded.
constexpr uint32_t kFullyLoggedLaunches = 32;

bool ShouldLogRefusal(uint32_t& index) {
    index = g_RefusedLaunches.fetch_add(1, std::memory_order_relaxed);
    return index < kFullyLoggedLaunches || (index % 1000) == 0;
}

std::atomic<uint32_t> g_AllowedLaunches{0};

bool ShouldLogAllowedLaunch(uint32_t& index) {
    index = g_AllowedLaunches.fetch_add(1, std::memory_order_relaxed);
    return index < kFullyLoggedLaunches || (index % 1000) == 0;
}

}  // namespace

void NoteUpdaterLaunchRefused(const char* imagePath) {
    uint32_t index = 0;
    if (ShouldLogRefusal(index)) {
        HookLogImportant(
            "NGX OTA: refused the NGX updater launch (%s) because ngx_ota=off; NVIDIA's runtime falls back to "
            "the files already in its cache (refusal #%u)",
            imagePath ? imagePath : "unnamed", index + 1);
    }
}

void NoteUpdaterLaunchRefused(const wchar_t* imagePath) {
    uint32_t index = 0;
    if (ShouldLogRefusal(index)) {
        HookLogImportant(
            "NGX OTA: refused the NGX updater launch (%ls) because ngx_ota=off; NVIDIA's runtime falls back to "
            "the files already in its cache (refusal #%u)",
            imagePath ? imagePath : L"unnamed", index + 1);
    }
}

// Reported even though CE did nothing about it, because "I saw updaters
// spawning" is otherwise unanswerable from the log. Three situations look
// identical from outside the process - CE never saw the launch (the calling
// module's import slots were not CE's, or the launch predates injection), CE
// saw it and the mode does not refuse, or CE refused it - and only the third
// used to leave a trace. Session 20260919_193954 is the case: nine updaters at
// 19:40:43 with ngx_ota=on, and nothing in hook_debug.log either way.
void NoteUpdaterLaunchAllowed(const char* imagePath) {
    if (!imagePath || !imagePath[0] || !IsNgxUpdaterImage(imagePath)) {
        return;
    }
    uint32_t index = 0;
    if (ShouldLogAllowedLaunch(index)) {
        HookLogImportant(
            "NGX OTA: saw an NGX updater launch (%s) and let it through - ngx_ota=%s, only `off` refuses "
            "(launch #%u)",
            imagePath, ModeName(CurrentMode()), index + 1);
    }
}

void NoteUpdaterLaunchAllowed(const wchar_t* imagePath) {
    if (!imagePath || !imagePath[0] || !IsNgxUpdaterImage(imagePath)) {
        return;
    }
    uint32_t index = 0;
    if (ShouldLogAllowedLaunch(index)) {
        HookLogImportant(
            "NGX OTA: saw an NGX updater launch (%ls) and let it through - ngx_ota=%s, only `off` refuses "
            "(launch #%u)",
            imagePath, ModeName(CurrentMode()), index + 1);
    }
}

}  // namespace ce::ngx_ota

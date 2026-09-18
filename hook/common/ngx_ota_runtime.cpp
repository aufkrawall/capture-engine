#include "ngx_ota_runtime.h"

#include <windows.h>

#include <atomic>

#include "hook_common.h"
#include "ngx_ota_policy.h"

namespace ce::ngx_ota {

namespace {

// kNgxOtaModeDefault until a profile resolves. Read from the CreateProcess hook
// on whatever thread the NGX core happens to launch its updater from, so it has
// to be atomic rather than merely written once.
std::atomic<uint8_t> g_Mode{kNgxOtaModeDefault};
std::atomic<uint8_t> g_LogLevel{kNgxLogLevelDefault};
std::atomic<bool> g_PolicyApplied{false};
std::atomic<uint32_t> g_RefusedLaunches{0};

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

#include "ngx_drs_override.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "hook_common.h"
#include "overlay_compat.h"

namespace ce::ngx_drs {

namespace {

// Guarded rather than atomic: the struct is five fields and every reader is off
// the hot path (a DRS read happens a handful of times per feature creation).
std::mutex g_OverridesMutex;
DlssDrsOverrides g_ConfiguredOverrides;
std::atomic<bool> g_AnyOverrideConfigured{false};
std::atomic<PfnNvApiDrsGetSetting> g_OriginalGetSetting{nullptr};

DlssDrsOverrides LoadConfiguredOverrides() {
    if (!g_AnyOverrideConfigured.load(std::memory_order_acquire))
        return DlssDrsOverrides{};
    std::lock_guard<std::mutex> lock(g_OverridesMutex);
    return g_ConfiguredOverrides;
}

bool ContainsInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) {
        return false;
    }
    const size_t needleLength = strlen(needle);
    for (const char* cursor = haystack; *cursor; ++cursor) {
        if (_strnicmp(cursor, needle, needleLength) == 0) {
            return true;
        }
    }
    return false;
}

// A Streamline plugin exports exactly one entry point besides DllMain, and it
// does so under whatever file name the driver's OTA store gave it. Checking the
// export is what recognizes `160_E658703.dll` as sl.common.
bool ExportsStreamlinePluginEntry(HMODULE module) {
    return module != nullptr && GetProcAddress(module, "slGetPluginFunction") != nullptr;
}

HMODULE ModuleFromCodeAddress(const void* address) {
    if (!address)
        return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

int32_t __cdecl Detour_NvApiDrsGetSetting(void* session, void* profile, uint32_t settingId, NvDrsSetting* setting) {
    const PfnNvApiDrsGetSetting original = g_OriginalGetSetting.load(std::memory_order_acquire);
    if (!original) {
        // Defensive: MaybeWrapQueryInterface only hands out this detour once the
        // driver entry point is stored, so there is always something to forward to.
        return kNvApiError;
    }
    if (HookIsShuttingDown())
        return original(session, profile, settingId, setting);

    const uint32_t callerVersion = setting ? setting->version : 0;
    const int32_t status = original(session, profile, settingId, setting);

    const DlssDrsOverrides overrides = LoadConfiguredOverrides();
    uint32_t driverValue = 0;
    if (!setting || callerVersion != kNvDrsSettingVer1 ||
        !ResolveSubstitutedValue(overrides, settingId, driverValue)) {
        return status;
    }

    FillSubstitutedSetting(*setting, settingId, driverValue);

    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 16 || (logCount % 256) == 0) {
        HookLogImportant(
            "NGX DRS: answered NvAPI_DRS_GetSetting(0x%08X, %s) with %u (driver status %d, call %u)", settingId,
            DrsSettingIdName(settingId), driverValue, status, logCount + 1);
    }
    return kNvApiOk;
}

void LogConfiguredOverrides(const DlssDrsOverrides& overrides) {
    char targetText[32];
    if (overrides.dynamicTargetFps == kDlssFGTargetFpsMaxRefresh)
        snprintf(targetText, sizeof(targetText), "max refresh");
    else if (overrides.dynamicTargetFps == kDlssFGTargetFpsDefault)
        snprintf(targetText, sizeof(targetText), "default");
    else
        snprintf(targetText, sizeof(targetText), "%u fps", static_cast<unsigned>(overrides.dynamicTargetFps));

    const char* vsyncText = overrides.vsyncMode == kDrsVSyncModeForceOn    ? "force on"
                            : overrides.vsyncMode == kDrsVSyncModeForceOff ? "force off"
                                                                           : "untouched";
    HookLogImportant(
        "NGX DRS: configured DLSS driver-settings answers: preset='%c' mode=%s fixed=%ux dynamicMax=%ux "
        "targetRate=%s driverVSync=%s",
        PresetIdToLetter(overrides.renderPreset), DlssFGModeName(overrides.frameGenerationMode),
        static_cast<unsigned>(overrides.fixedCountMultiplier), static_cast<unsigned>(overrides.dynamicMaxMultiplier),
        targetText, vsyncText);
}

}  // namespace

bool IsDlssDrsConsumerModuleName(const char* modulePath) {
    // nvngx_dlssg is the NGX frame generation snippet (render preset).
    // sl.common performs the actual NvAPI call for Streamline; sl.dlss_g asks
    // it to, and sl.interposer is listed because a statically bound
    // interposer build can host the same DRS context.
    return ContainsInsensitive(modulePath, "nvngx_dlssg") || ContainsInsensitive(modulePath, "sl.common") ||
           ContainsInsensitive(modulePath, "sl.dlss_g") || ContainsInsensitive(modulePath, "sl.interposer");
}

bool IsFrameGenerationSnippetModulePath(const char* modulePath) {
    return ContainsInsensitive(modulePath, "nvngx_dlssg");
}

void SetConfiguredOverrides(const DlssDrsOverrides& rawOverrides) {
    const DlssDrsOverrides normalized = Normalize(rawOverrides);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_OverridesMutex);
        changed = g_ConfiguredOverrides != normalized;
        g_ConfiguredOverrides = normalized;
    }
    g_AnyOverrideConfigured.store(HasAnyOverride(normalized), std::memory_order_release);
    if (changed) {
        LogConfiguredOverrides(normalized);
    }
}

DlssDrsOverrides GetConfiguredOverrides() {
    return LoadConfiguredOverrides();
}

uint32_t GetConfiguredPreset() {
    return LoadConfiguredOverrides().renderPreset;
}

bool IsArmed() {
    return g_AnyOverrideConfigured.load(std::memory_order_acquire);
}

bool IsDlssDrsConsumerModuleLoaded(const char* modulePath, void* module) {
    return IsDlssDrsConsumerModule(modulePath, ExportsStreamlinePluginEntry(static_cast<HMODULE>(module)));
}

void* MaybeWrapQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress) {
    if (HookIsShuttingDown())
        return nullptr;

    const DlssDrsOverrides overrides = LoadConfiguredOverrides();
    if (!HasAnyOverride(overrides) || functionId != kNvApiIdDrsGetSetting || !resolved) {
        return nullptr;
    }

    char callerPath[MAX_PATH] = {};
    if (!ce::overlay_compat::TryGetModulePathFromCodeAddress(callerAddress, callerPath, sizeof(callerPath))) {
        return nullptr;
    }
    const bool streamlinePlugin = ExportsStreamlinePluginEntry(ModuleFromCodeAddress(callerAddress));
    if (!ShouldWrapQueryInterface(overrides, functionId, callerPath, streamlinePlugin)) {
        return nullptr;
    }

    // Never chain onto ourselves: an earlier resolution may already have been
    // answered with the detour and handed back through a caller-owned cache.
    if (resolved != reinterpret_cast<void*>(&Detour_NvApiDrsGetSetting)) {
        g_OriginalGetSetting.store(reinterpret_cast<PfnNvApiDrsGetSetting>(resolved), std::memory_order_release);
    }
    if (!g_OriginalGetSetting.load(std::memory_order_acquire)) {
        return nullptr;
    }

    static std::atomic<uint32_t> s_wrapLogs{0};
    const uint32_t logIndex = s_wrapLogs.fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 4) {
        HookLogImportant(
            "NGX DRS: wrapping NvAPI_DRS_GetSetting for %s (streamlinePlugin=%d driver entry %p) - the configured "
            "DLSS frame generation driver settings will be answered in-process",
            callerPath, streamlinePlugin ? 1 : 0, resolved);
    }
    return reinterpret_cast<void*>(&Detour_NvApiDrsGetSetting);
}

}  // namespace ce::ngx_drs

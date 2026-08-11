#include "ngx_fg_preset_override.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "hook_common.h"
#include "overlay_compat.h"

namespace ce::ngx_fg_preset {

namespace {

std::atomic<uint32_t> g_ConfiguredPreset{0};
std::atomic<PfnNvApiDrsGetSetting> g_OriginalGetSetting{nullptr};

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

    const uint32_t preset = g_ConfiguredPreset.load(std::memory_order_acquire);
    if (!setting || !ShouldSubstituteSetting(preset, settingId, callerVersion)) {
        return status;
    }

    FillSubstitutedSetting(*setting, settingId, preset);

    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 8 || (logCount % 256) == 0) {
        HookLogImportant(
            "NGX FG preset: answered NvAPI_DRS_GetSetting(0x%08X) with preset '%c' (value %u, driver status %d, "
            "call %u)",
            settingId, PresetIdToLetter(preset), preset, status, logCount + 1);
    }
    return kNvApiOk;
}

}  // namespace

uint32_t NormalizePreset(uint32_t preset) {
    return (preset >= 1 && preset <= 26) ? preset : 0u;
}

char PresetIdToLetter(uint32_t preset) {
    return NormalizePreset(preset) ? static_cast<char>('A' + static_cast<int>(preset) - 1) : '?';
}

bool IsFrameGenerationSnippetModulePath(const char* modulePath) {
    // The DLSS-G snippet is the only module that reads this setting. Matching it
    // by name keeps every other NvAPI consumer - the game, Streamline, Reflex -
    // on the untouched driver pointer.
    return ContainsInsensitive(modulePath, "nvngx_dlssg");
}

bool ShouldWrapQueryInterface(uint32_t configuredPreset, uint32_t functionId, const char* callerModulePath) {
    return NormalizePreset(configuredPreset) != 0 && functionId == kNvApiIdDrsGetSetting &&
           IsFrameGenerationSnippetModulePath(callerModulePath);
}

bool ShouldSubstituteSetting(uint32_t configuredPreset, uint32_t settingId, uint32_t callerStructVersion) {
    return NormalizePreset(configuredPreset) != 0 && settingId == kRenderPresetDrsSettingId &&
           callerStructVersion == kNvDrsSettingVer1;
}

void FillSubstitutedSetting(NvDrsSetting& setting, uint32_t settingId, uint32_t preset) {
    setting.settingId = settingId;
    setting.settingType = kNvDrsDwordType;
    // The snippet only accepts a value it believes the current profile set
    // explicitly, so report current-profile location and a non-predefined value.
    setting.settingLocation = kNvDrsCurrentProfileLocation;
    setting.isCurrentPredefined = 0;
    setting.isPredefinedValid = 0;
    setting.currentValue.u32Value = NormalizePreset(preset);
}

void SetConfiguredPreset(uint32_t preset) {
    const uint32_t normalized = NormalizePreset(preset);
    const uint32_t previous = g_ConfiguredPreset.exchange(normalized, std::memory_order_acq_rel);
    if (previous != normalized) {
        HookLogImportant("NGX FG preset: configured preset is now '%c' (value %u, was %u)", PresetIdToLetter(normalized),
                         normalized, previous);
    }
}

uint32_t GetConfiguredPreset() {
    return g_ConfiguredPreset.load(std::memory_order_acquire);
}

bool IsArmed() {
    return GetConfiguredPreset() != 0;
}

void* MaybeWrapQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress) {
    if (HookIsShuttingDown())
        return nullptr;

    const uint32_t preset = g_ConfiguredPreset.load(std::memory_order_acquire);
    if (NormalizePreset(preset) == 0 || functionId != kNvApiIdDrsGetSetting || !resolved) {
        return nullptr;
    }

    char callerPath[MAX_PATH] = {};
    if (!ce::overlay_compat::TryGetModulePathFromCodeAddress(callerAddress, callerPath, sizeof(callerPath))) {
        return nullptr;
    }
    if (!ShouldWrapQueryInterface(preset, functionId, callerPath)) {
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

    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "NGX FG preset: wrapping NvAPI_DRS_GetSetting for %s (preset '%c', driver entry %p) - DRS setting "
            "0x%08X will report the configured frame generation preset",
            callerPath, PresetIdToLetter(preset), resolved, kRenderPresetDrsSettingId);
    }
    return reinterpret_cast<void*>(&Detour_NvApiDrsGetSetting);
}

}  // namespace ce::ngx_fg_preset

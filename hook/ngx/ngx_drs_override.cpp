#include "ngx_drs_override.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/platform/module_enumeration.h"
#include "hook/runtime/hook_common.h"
#include "hook/overlay/overlay_compat.h"

namespace ce::ngx_drs {

namespace {

// Guarded rather than atomic: the struct is five fields and every reader is off
// the hot path (a DRS read happens a handful of times per feature creation).
std::mutex g_OverridesMutex;
DlssDrsOverrides g_ConfiguredOverrides;
std::atomic<bool> g_AnyOverrideConfigured{false};
// Mirrors g_ConfiguredOverrides.frameGenerationMode so the per-GetState read
// never takes the lock.
std::atomic<uint8_t> g_ConfiguredFrameGenerationMode{kDlssFGModeDefault};
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
    g_ConfiguredFrameGenerationMode.store(normalized.frameGenerationMode, std::memory_order_release);
    g_AnyOverrideConfigured.store(HasAnyOverride(normalized), std::memory_order_release);
    if (changed) {
        LogConfiguredOverrides(normalized);
    }
}

DlssDrsOverrides GetConfiguredOverrides() {
    return LoadConfiguredOverrides();
}

uint8_t GetConfiguredFrameGenerationMode() {
    return g_ConfiguredFrameGenerationMode.load(std::memory_order_acquire);
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

uint32_t ForEachLoadedDlssDrsConsumer(DlssDrsConsumerVisitor visitor, void* context) {
    if (!visitor)
        return 0;
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules))
        return 0;

    uint32_t visited = 0;
    for (HMODULE module : modules) {
        // Pinned for the visit: the export probe reads the image, and a title tearing the
        // Streamline stack down on another thread must not leave it reading freed pages.
        HMODULE retained = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module),
                                &retained)) {
            continue;
        }
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(retained, path, MAX_PATH) && IsDlssDrsConsumerModuleLoaded(path, retained)) {
            visitor(retained, path, context);
            ++visited;
        }
        FreeLibrary(retained);
    }
    return visited;
}

namespace {

struct RetargetedSlot {
    void** slot;
    void* from;
    void* to;
};
std::mutex g_RetargetedMutex;
std::vector<RetargetedSlot> g_RetargetedSlots;

bool IsWritableCommittedRegion(const MEMORY_BASIC_INFORMATION& region) {
    return region.State == MEM_COMMIT && (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
           (region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
}

using PfnNvApiQueryInterface = void*(__cdecl*)(uint32_t);

}  // namespace

uint32_t RetargetCachedPointers(void* module, const void* from, void* to) {
    if (!module || !from || !to || from == to)
        return 0;

    auto* const base = static_cast<uint8_t*>(module);
    MEMORY_BASIC_INFORMATION region = {};
    if (VirtualQuery(base, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return 0;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    uint32_t replaced = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        const DWORD flags = section->Characteristics;
        if ((flags & IMAGE_SCN_MEM_WRITE) == 0 || (flags & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE)) != 0)
            continue;
        const size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        uint8_t* cursor = base + section->VirtualAddress;
        uint8_t* const end = cursor + size;
        while (cursor < end) {
            if (VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region))
                break;
            uint8_t* const regionEnd =
                (std::min)(end, static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize);
            if (IsWritableCommittedRegion(region)) {
                const uintptr_t aligned = (reinterpret_cast<uintptr_t>(cursor) + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
                for (void** slot = reinterpret_cast<void**>(aligned);
                     reinterpret_cast<uint8_t*>(slot + 1) <= regionEnd; ++slot) {
                    if (*slot != from)
                        continue;
                    if (InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), to,
                                                          const_cast<void*>(from)) == from) {
                        std::lock_guard<std::mutex> lock(g_RetargetedMutex);
                        g_RetargetedSlots.push_back({slot, const_cast<void*>(from), to});
                        ++replaced;
                    }
                }
            }
            cursor = regionEnd;
        }
    }
    return replaced;
}

CachedNvApiRetarget RetargetCachedNvApiPointers(void* module, void* queryInterfaceDetour) {
    CachedNvApiRetarget result;
    if (!module || !queryInterfaceDetour || HookIsShuttingDown())
        return result;

    const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi)
        return result;
    const auto driverQueryInterface =
        reinterpret_cast<PfnNvApiQueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!driverQueryInterface || reinterpret_cast<void*>(driverQueryInterface) == queryInterfaceDetour)
        return result;

    result.queryInterface =
        RetargetCachedPointers(module, reinterpret_cast<void*>(driverQueryInterface), queryInterfaceDetour);

    // The getter itself, for a core that has already read a driver setting. Only while
    // something is configured: unconfigured, the wrapper would answer nothing.
    if (IsArmed()) {
        void* const driverGetter = driverQueryInterface(kNvApiIdDrsGetSetting);
        if (driverGetter && driverGetter != reinterpret_cast<void*>(&Detour_NvApiDrsGetSetting)) {
            // Published before any slot can point at the detour, which forwards to it.
            PfnNvApiDrsGetSetting expected = nullptr;
            g_OriginalGetSetting.compare_exchange_strong(expected,
                                                         reinterpret_cast<PfnNvApiDrsGetSetting>(driverGetter),
                                                         std::memory_order_acq_rel);
            result.drsGetSetting = RetargetCachedPointers(module, driverGetter,
                                                          reinterpret_cast<void*>(&Detour_NvApiDrsGetSetting));
        }
    }
    return result;
}

void RestoreRetargetedPointers() {
    std::lock_guard<std::mutex> lock(g_RetargetedMutex);
    for (const RetargetedSlot& entry : g_RetargetedSlots) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(entry.slot), &region, sizeof(region)) != sizeof(region) ||
            !IsWritableCommittedRegion(region)) {
            continue;
        }
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry.slot), entry.from, entry.to);
    }
    g_RetargetedSlots.clear();
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

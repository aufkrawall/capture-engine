#include "ffx_hook.h"
#include <psapi.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "../common/ffx_api_parsing.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/iat_hook.h"

// ============================================================================
// FFX API Type Definitions (from FFX SDK)
// We don't include the SDK headers directly to avoid dependency issues
// ============================================================================

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader {
    ffxStructType_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks {
    void* pUserData;
    ffxAlloc alloc;
    ffxDealloc dealloc;
} ffxAllocationCallbacks;

// FFX Effect IDs - match values from ffx_api.h
constexpr uint32_t FFX_API_EFFECT_MASK = 0x00ff0000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATION = 0x00020000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN = 0x00030000u;

// Return codes
constexpr ffxReturnCode_t FFX_API_RETURN_OK = 0;

// Function signatures
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc,
                                               const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* context, const ffxConfigureDescHeader* desc);

// ============================================================================
// Hook State
// ============================================================================

namespace {

std::mutex g_InitMutex;
std::atomic<bool> g_Initialized{false};
std::atomic<int> g_ActiveFGContextCount{0};
std::atomic<bool> g_NoModulesLogged{false};

// CRITICAL FIX: Track context types to know if destroyed context is FG
std::mutex g_ContextMapMutex;
std::unordered_map<ffxContext, uint32_t> g_ContextTypeMap;

// Original function pointers
PfnFfxCreateContext g_Original_ffxCreateContext = nullptr;
PfnFfxDestroyContext g_Original_ffxDestroyContext = nullptr;
PfnFfxConfigure g_Original_ffxConfigure = nullptr;

std::atomic<bool> g_ffxCreateContextInlineHooked{false};
std::atomic<bool> g_ffxDestroyContextInlineHooked{false};
std::atomic<bool> g_ffxConfigureInlineHooked{false};
std::atomic<void*> g_ffxCreateContextTarget{nullptr};
std::atomic<void*> g_ffxDestroyContextTarget{nullptr};
std::atomic<void*> g_ffxConfigureTarget{nullptr};

template <typename T>
bool InstallInlineHookOnce(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
                           std::atomic<void*>& targetSlot, const char* hookName) {
    if (!target) {
        return false;
    }

    if (target == detour) {
        original = nullptr;
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    if (installedFlag.load(std::memory_order_acquire) && installedTarget == target) {
        return false;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(target, detour, &trampoline)) {
        HookLogImportant("FFX Hook: Failed to inline hook %s at %p", hookName, target);
        return false;
    }

    original = reinterpret_cast<T>(trampoline);
    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("FFX Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target, trampoline);
    return true;
}

// Track which module we hooked (for cleanup)
HMODULE g_HookedModule = nullptr;

// Extract effect ID from structure type
inline uint32_t GetEffectId(ffxStructType_t type) {
    return static_cast<uint32_t>(type) & FFX_API_EFFECT_MASK;
}

// ============================================================================
// Hooked Functions
// ============================================================================

ffxReturnCode_t Hooked_ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                        const ffxAllocationCallbacks* memCb) {
    if (!g_Original_ffxCreateContext) {
        HookLog("FFX Hook: ffxCreateContext called but original not set!");
        return 1;  // Error
    }

    // Call original first
    ffxReturnCode_t result = g_Original_ffxCreateContext(context, desc, memCb);

    if (result == FFX_API_RETURN_OK && desc) {
        uint32_t effectId = GetEffectId(desc->type);

        // CRITICAL FIX: Track context type for proper cleanup
        {
            std::lock_guard<std::mutex> lock(g_ContextMapMutex);
            g_ContextTypeMap[context] = effectId;
        }

        // Check if this is a Frame Generation context
        if (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION || effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN) {
            int prevCount = g_ActiveFGContextCount.fetch_add(1, std::memory_order_acq_rel);
            HookLog(
                "FFX Hook: Frame Generation context CREATED (type=0x%llx, "
                "effectId=0x%x, activeContexts=%d)",
                (unsigned long long)desc->type, effectId, prevCount + 1);

            // Signal FG activation to the detection system
            // Only on first context (0 -> 1 transition)
            if (prevCount == 0) {
                HookLog("FFX Hook: FSR Frame Generation ACTIVATED (first context created)");
                g_FGCompat.SetFSRFGActive(true);
            }
        }
    }

    return result;
}

ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!g_Original_ffxDestroyContext) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1;  // Error
    }

    // CRITICAL FIX: Check if this is an FG context before decrementing
    bool isFGContext = false;
    {
        std::lock_guard<std::mutex> lock(g_ContextMapMutex);
        auto it = g_ContextTypeMap.find(context);
        if (it != g_ContextTypeMap.end()) {
            uint32_t effectId = it->second;
            isFGContext = (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION ||
                           effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
            g_ContextTypeMap.erase(it);
        }
    }

    // Call original
    ffxReturnCode_t result = g_Original_ffxDestroyContext(context, memCb);

    // Only decrement if this was actually an FG context
    if (result == FFX_API_RETURN_OK && isFGContext) {
        int prevCount = g_ActiveFGContextCount.load(std::memory_order_acquire);
        int newCount = g_ActiveFGContextCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount < 0) {
            g_ActiveFGContextCount.store(0, std::memory_order_release);
            newCount = 0;
        }

        HookLog("FFX Hook: FG Context destroyed (activeContexts=%d)", newCount);

        // Signal FG deactivation when all contexts are destroyed
        if (newCount == 0) {
            HookLog(
                "FFX Hook: FSR Frame Generation DEACTIVATED (all contexts "
                "destroyed)");
            g_FGCompat.SetFSRFGActive(false);
        }
    } else if (result == FFX_API_RETURN_OK && !isFGContext) {
        HookLog("FFX Hook: Non-FG Context destroyed");
    }

    return result;
}

ffxReturnCode_t Hooked_ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!g_Original_ffxConfigure) {
        HookLog("FFX Hook: ffxConfigure called but original not set!");
        return 1;  // Error
    }

    const ffxReturnCode_t result = g_Original_ffxConfigure(context, desc);
    if (result != FFX_API_RETURN_OK || !desc) {
        return result;
    }

    const auto parsed =
        ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
    if (!parsed.recognized) {
        return result;
    }

    HookLog("FFX Hook: Frame Generation configure %s (context=%p, frameID=%llu, type=0x%llx)",
            parsed.enabled ? "ENABLED" : "DISABLED", context, (unsigned long long)parsed.frameId,
            (unsigned long long)desc->type);

    // Native FSR can keep its context alive while toggling FG on/off via
    // ffxConfigure. Trust that runtime signal over context lifetime.
    g_FGCompat.SetFSRFGActive(parsed.enabled);
    return result;
}

// ============================================================================
// Hook Installation via GetProcAddress Detour
// ============================================================================

bool InstallHooksForModule(HMODULE hModule, const char* moduleName) {
    if (!hModule)
        return false;

    HookLog("FFX Hook: Installing hooks for module %s (%p)", moduleName, hModule);
    g_FGCompat.SetFSRFGSupportPresent(true);

    // Get the original functions
    PfnFfxCreateContext createCtx = (PfnFfxCreateContext)GetProcAddress(hModule, "ffxCreateContext");
    PfnFfxDestroyContext destroyCtx = (PfnFfxDestroyContext)GetProcAddress(hModule, "ffxDestroyContext");
    PfnFfxConfigure configureCtx = (PfnFfxConfigure)GetProcAddress(hModule, "ffxConfigure");

    if (!createCtx && !destroyCtx && !configureCtx) {
        HookLog("FFX Hook: No supported FFX exports found in %s - skipping", moduleName);
        return false;
    }

    // Store originals
    g_Original_ffxCreateContext = createCtx;
    g_Original_ffxDestroyContext = destroyCtx;
    g_Original_ffxConfigure = configureCtx;
    g_HookedModule = hModule;

    // Install IAT hooks in all loaded modules to intercept calls to FFX functions
    // This patches the import tables of the game exe and all loaded DLLs

    void* dummy = nullptr;
    if (createCtx) {
        InstallInlineHookOnce(reinterpret_cast<void*>(createCtx), reinterpret_cast<void*>(Hooked_ffxCreateContext),
                              g_Original_ffxCreateContext, g_ffxCreateContextInlineHooked, g_ffxCreateContextTarget,
                              "ffxCreateContext");
        HookLog("FFX Hook: ffxCreateContext found at %p, hooking via IAT", createCtx);
        // Patch IAT for all modules that import from the FFX DLL
        IATHook::PatchIATAllModules(moduleName, "ffxCreateContext", (void*)Hooked_ffxCreateContext, &dummy);
        // Also register for dynamic hook (GetProcAddress interception)
        IATHook::RegisterDynamicHook("ffxCreateContext", (void*)Hooked_ffxCreateContext,
                                     (void**)&g_Original_ffxCreateContext);
    }

    if (destroyCtx) {
        InstallInlineHookOnce(reinterpret_cast<void*>(destroyCtx), reinterpret_cast<void*>(Hooked_ffxDestroyContext),
                              g_Original_ffxDestroyContext, g_ffxDestroyContextInlineHooked,
                              g_ffxDestroyContextTarget, "ffxDestroyContext");
        HookLog("FFX Hook: ffxDestroyContext found at %p, hooking via IAT", destroyCtx);
        IATHook::PatchIATAllModules(moduleName, "ffxDestroyContext", (void*)Hooked_ffxDestroyContext, &dummy);
        IATHook::RegisterDynamicHook("ffxDestroyContext", (void*)Hooked_ffxDestroyContext,
                                     (void**)&g_Original_ffxDestroyContext);
    }

    if (configureCtx) {
        InstallInlineHookOnce(reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure),
                              g_Original_ffxConfigure, g_ffxConfigureInlineHooked, g_ffxConfigureTarget,
                              "ffxConfigure");
        HookLog("FFX Hook: ffxConfigure found at %p, hooking via IAT", configureCtx);
        IATHook::PatchIATAllModules(moduleName, "ffxConfigure", (void*)Hooked_ffxConfigure, &dummy);
        IATHook::RegisterDynamicHook("ffxConfigure", (void*)Hooked_ffxConfigure, (void**)&g_Original_ffxConfigure);
    }

    HookLog("FFX Hook: Hooks installed successfully for %s", moduleName);
    return true;
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace FFXHook {

void Init() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    if (g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    static int s_initCallCount = 0;
    ++s_initCallCount;

    if (!g_NoModulesLogged.load(std::memory_order_acquire)) {
        HookLog("FFX Hook: Initializing...");
    }

    // Try to find FFX modules
    // These are the common DLL names for FSR Frame Generation
    // Also includes dlssg-to-fsr3 mod DLLs that redirect DLSS FG to FSR FG
    const wchar_t* ffxModules[] = {// FSR 4 / FSR 3.1 DLLs (UE5 native integration) - CHECK FIRST
                                   L"amd_fidelityfx_framegeneration_dx12.dll", L"amd_fidelityfx_framegeneration_vk.dll",
                                   // Standard AMD FSR FG DLLs
                                   L"amd_fidelityfx_fg.dll", L"ffx_frameinterpolation_x64.dll",
                                   L"amd_fidelityfx_framegeneration.dll", L"ffx_framegeneration.dll",
                                   // dlssg-to-fsr3 mod - uses nvngx_dlssg.dll as a proxy that calls FFX API
                                   L"nvngx_dlssg.dll",
                                   // FSR3 FG Mod common names
                                   L"fsr3fg.dll", L"fsr3mod.dll"};

    const char* ffxModuleNames[] = {"amd_fidelityfx_framegeneration_dx12.dll",
                                    "amd_fidelityfx_framegeneration_vk.dll",
                                    "amd_fidelityfx_fg.dll",
                                    "ffx_frameinterpolation_x64.dll",
                                    "amd_fidelityfx_framegeneration.dll",
                                    "ffx_framegeneration.dll",
                                    "nvngx_dlssg.dll",
                                    "fsr3fg.dll",
                                    "fsr3mod.dll"};

    for (size_t i = 0; i < _countof(ffxModules); ++i) {
        HMODULE hMod = GetModuleHandleW(ffxModules[i]);
        if (hMod) {
            HookLog("FFX Hook: Found module %s at %p", ffxModuleNames[i], hMod);
            g_FGCompat.SetFSRFGSupportPresent(true);
            if (InstallHooksForModule(hMod, ffxModuleNames[i])) {
                g_Initialized.store(true, std::memory_order_release);
                g_NoModulesLogged.store(false, std::memory_order_release);
                return;
            }
            // Module exists but has no FFX exports (e.g. real nvngx_dlssg.dll)
            HookLog("FFX Hook: Module %s has no FFX exports, continuing search", ffxModuleNames[i]);
        }
    }

    if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLog("FFX Hook: No FFX modules found, hooks not installed");
    }

    // One-time diagnostic at 30th retry: enumerate loaded modules for debugging
    if (s_initCallCount == 30) {
        HookLogImportant("FFX Hook: Module enumeration diagnostic (call #%d):", s_initCallCount);
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
            int found = 0;
            for (DWORD i = 0; i < cbNeeded / sizeof(HMODULE); i++) {
                wchar_t modName[MAX_PATH];
                if (GetModuleFileNameW(hMods[i], modName, MAX_PATH)) {
                    std::wstring lower(modName);
                    for (auto& c : lower)
                        c = towlower(c);
                    if (lower.find(L"fidelity") != std::wstring::npos || lower.find(L"ffx") != std::wstring::npos ||
                        lower.find(L"framegen") != std::wstring::npos || lower.find(L"fsr") != std::wstring::npos ||
                        lower.find(L"amd_") != std::wstring::npos) {
                        char narrowName[MAX_PATH];
                        WideCharToMultiByte(CP_UTF8, 0, modName, -1, narrowName, MAX_PATH, NULL, NULL);
                        HookLogImportant("FFX Hook:   Loaded: %s", narrowName);
                        found++;
                    }
                }
            }
            if (found == 0) {
                HookLogImportant("FFX Hook:   No AMD/FFX/FSR modules among %d loaded",
                                 (int)(cbNeeded / sizeof(HMODULE)));
            }
        }
    }
}

bool IsInitialized() {
    return g_Initialized.load(std::memory_order_acquire);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    if (!g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    HookLog("FFX Hook: Shutting down...");

    // Remove IAT hooks
    if (g_HookedModule) {
        // Note: IATHook::RemoveHook would need to be implemented
        // For now, we just clear the state - hooks will naturally be cleaned up
        // when the DLL unloads
    }

    g_Original_ffxCreateContext = nullptr;
    g_Original_ffxDestroyContext = nullptr;
    g_Original_ffxConfigure = nullptr;
    g_HookedModule = nullptr;
    g_ActiveFGContextCount.store(0, std::memory_order_release);
    g_FGCompat.SetFSRFGActive(false);
    g_FGCompat.SetFSRFGSupportPresent(false);
    g_NoModulesLogged.store(false, std::memory_order_release);
    g_Initialized.store(false, std::memory_order_release);

    HookLog("FFX Hook: Shutdown complete");
}

}  // namespace FFXHook

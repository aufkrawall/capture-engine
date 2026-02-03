#include "ffx_hook.h"
#include "../common/hook_common.h"
#include "../common/fg_detection.h"
#include "../wrappers/iat_hook.h"
#include <mutex>
#include <atomic>

// ============================================================================
// FFX API Type Definitions (from FFX SDK)
// We don't include the SDK headers directly to avoid dependency issues
// ============================================================================

typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader
{
    ffxStructType_t      type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);
typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks
{
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
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);

// ============================================================================
// Hook State
// ============================================================================

namespace {

std::mutex g_InitMutex;
std::atomic<bool> g_Initialized{false};
std::atomic<int> g_ActiveFGContextCount{0};

// Original function pointers
PfnFfxCreateContext g_Original_ffxCreateContext = nullptr;
PfnFfxDestroyContext g_Original_ffxDestroyContext = nullptr;

// Track which module we hooked (for cleanup)
HMODULE g_HookedModule = nullptr;

// Extract effect ID from structure type
inline uint32_t GetEffectId(ffxStructType_t type)
{
    return static_cast<uint32_t>(type) & FFX_API_EFFECT_MASK;
}

// ============================================================================
// Hooked Functions
// ============================================================================

ffxReturnCode_t Hooked_ffxCreateContext(
    ffxContext* context,
    ffxCreateContextDescHeader* desc,
    const ffxAllocationCallbacks* memCb)
{
    if (!g_Original_ffxCreateContext) {
        HookLog("FFX Hook: ffxCreateContext called but original not set!");
        return 1; // Error
    }

    // Call original first
    ffxReturnCode_t result = g_Original_ffxCreateContext(context, desc, memCb);

    if (result == FFX_API_RETURN_OK && desc) {
        uint32_t effectId = GetEffectId(desc->type);

        // Check if this is a Frame Generation context
        if (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION ||
            effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN) {
            
            int prevCount = g_ActiveFGContextCount.fetch_add(1, std::memory_order_acq_rel);
            HookLog("FFX Hook: Frame Generation context CREATED (type=0x%llx, effectId=0x%x, activeContexts=%d)",
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

ffxReturnCode_t Hooked_ffxDestroyContext(
    ffxContext* context,
    const ffxAllocationCallbacks* memCb)
{
    if (!g_Original_ffxDestroyContext) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1; // Error
    }

    // Track if this might be an FG context (conservative - we decrement and check)
    // Note: We don't have easy access to the context type here, so we're conservative
    // If the count goes to 0, we signal deactivation
    int prevCount = g_ActiveFGContextCount.load(std::memory_order_acquire);
    
    // Call original
    ffxReturnCode_t result = g_Original_ffxDestroyContext(context, memCb);

    if (result == FFX_API_RETURN_OK && prevCount > 0) {
        int newCount = g_ActiveFGContextCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount < 0) {
            // Shouldn't happen, but clamp to 0
            g_ActiveFGContextCount.store(0, std::memory_order_release);
            newCount = 0;
        }

        HookLog("FFX Hook: Context destroyed (activeContexts=%d)", newCount);

        // Signal FG deactivation when all contexts are destroyed
        if (newCount == 0) {
            HookLog("FFX Hook: FSR Frame Generation DEACTIVATED (all contexts destroyed)");
            g_FGCompat.SetFSRFGActive(false);
        }
    }

    return result;
}

// ============================================================================
// Hook Installation via GetProcAddress Detour
// ============================================================================

void InstallHooksForModule(HMODULE hModule, const char* moduleName)
{
    if (!hModule) return;

    HookLog("FFX Hook: Installing hooks for module %s (%p)", moduleName, hModule);

    // Get the original functions
    PfnFfxCreateContext createCtx = (PfnFfxCreateContext)GetProcAddress(hModule, "ffxCreateContext");
    PfnFfxDestroyContext destroyCtx = (PfnFfxDestroyContext)GetProcAddress(hModule, "ffxDestroyContext");

    if (!createCtx && !destroyCtx) {
        HookLog("FFX Hook: Neither ffxCreateContext nor ffxDestroyContext found in %s", moduleName);
        return;
    }

    // Store originals
    g_Original_ffxCreateContext = createCtx;
    g_Original_ffxDestroyContext = destroyCtx;
    g_HookedModule = hModule;

    // Install IAT hooks in all loaded modules to intercept calls to FFX functions
    // This patches the import tables of the game exe and all loaded DLLs
    
    void* dummy = nullptr;
    if (createCtx) {
        HookLog("FFX Hook: ffxCreateContext found at %p, hooking via IAT", createCtx);
        // Patch IAT for all modules that import from the FFX DLL
        IATHook::PatchIATAllModules(moduleName, "ffxCreateContext", 
                                    (void*)Hooked_ffxCreateContext, &dummy);
        // Also register for dynamic hook (GetProcAddress interception)
        IATHook::RegisterDynamicHook("ffxCreateContext", (void*)Hooked_ffxCreateContext, 
                                     (void**)&g_Original_ffxCreateContext);
    }

    if (destroyCtx) {
        HookLog("FFX Hook: ffxDestroyContext found at %p, hooking via IAT", destroyCtx);
        IATHook::PatchIATAllModules(moduleName, "ffxDestroyContext", 
                                    (void*)Hooked_ffxDestroyContext, &dummy);
        IATHook::RegisterDynamicHook("ffxDestroyContext", (void*)Hooked_ffxDestroyContext, 
                                     (void**)&g_Original_ffxDestroyContext);
    }

    HookLog("FFX Hook: Hooks installed successfully for %s", moduleName);
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace FFXHook {

void Init()
{
    std::lock_guard<std::mutex> lock(g_InitMutex);

    if (g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    HookLog("FFX Hook: Initializing...");

    // Try to find FFX modules
    // These are the common DLL names for FSR Frame Generation
    // Also includes dlssg-to-fsr3 mod DLLs that redirect DLSS FG to FSR FG
    const wchar_t* ffxModules[] = {
        // FSR 4 / FSR 3.1 DLLs (UE5 native integration) - CHECK FIRST
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration_vk.dll",
        // Standard AMD FSR FG DLLs
        L"amd_fidelityfx_fg.dll",
        L"ffx_frameinterpolation_x64.dll",
        L"amd_fidelityfx_framegeneration.dll",
        L"ffx_framegeneration.dll",
        // dlssg-to-fsr3 mod - uses nvngx_dlssg.dll as a proxy that calls FFX API
        L"nvngx_dlssg.dll",
        // FSR3 FG Mod common names
        L"fsr3fg.dll",
        L"fsr3mod.dll"
    };

    const char* ffxModuleNames[] = {
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_framegeneration_vk.dll",
        "amd_fidelityfx_fg.dll",
        "ffx_frameinterpolation_x64.dll",
        "amd_fidelityfx_framegeneration.dll",
        "ffx_framegeneration.dll",
        "nvngx_dlssg.dll",
        "fsr3fg.dll",
        "fsr3mod.dll"
    };

    for (size_t i = 0; i < _countof(ffxModules); ++i) {
        HMODULE hMod = GetModuleHandleW(ffxModules[i]);
        if (hMod) {
            HookLog("FFX Hook: Found module %s at %p", ffxModuleNames[i], hMod);
            InstallHooksForModule(hMod, ffxModuleNames[i]);
            g_Initialized.store(true, std::memory_order_release);
            return;
        }
    }

    HookLog("FFX Hook: No FFX modules found, hooks not installed");
}

bool IsInitialized()
{
    return g_Initialized.load(std::memory_order_acquire);
}

void Shutdown()
{
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
    g_HookedModule = nullptr;
    g_ActiveFGContextCount.store(0, std::memory_order_release);
    g_Initialized.store(false, std::memory_order_release);

    HookLog("FFX Hook: Shutdown complete");
}

}  // namespace FFXHook

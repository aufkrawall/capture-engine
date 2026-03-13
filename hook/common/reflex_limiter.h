#pragma once
// Reflex/Low-Latency FPS Limiter
//
// Resolves NvAPI_D3D_SetSleepMode and NvAPI_D3D_Sleep from nvapi64.dll and
// proactively pushes minimumIntervalUs to enforce an FPS cap through the
// driver's low-latency pipeline. This is the most compatible approach for
// games using Reflex, as the driver handles frame pacing internally.
//
// Flow:
//   1. Init() resolves NvAPI function pointers via nvapi_QueryInterface
//   2. Installs GetProcAddress dynamic hook to intercept game's NvAPI calls
//   3. SetDevice() provides the D3D device from our Present hooks
//   4. SetTargetFps() + PushFpsLimit() pushes minimumIntervalUs to the driver
//   5. The driver's NvAPI_D3D_Sleep() enforces the frame interval natively
//
// Game Activation Detection:
//   When the game calls nvapi_QueryInterface(NVAPI_ID_D3D_SetSleepMode),
//   our dynamic hook intercepts and returns a wrapper. The wrapper detects
//   when the game sets bLowLatencyMode=true, marking gameActivated_=true.
//   This enables auto mode to only use Reflex when the game has activated it.

// clang-format off
#include <windows.h>
// clang-format on
#include <atomic>
#include "hook_common.h"
#include "reflex_defs.h"

// IAT hook infrastructure for game activation detection.
// Only available in the Hook DLL build, not in the Vulkan Layer.
#if defined(BUILDING_CAPTURE_HOOK) && !defined(VK_LAYER_CE_OVERLAY)
#include "../wrappers/inline_hook.h"
#define REFLEX_IAT_HOOK_AVAILABLE 1
#else
#define REFLEX_IAT_HOOK_AVAILABLE 0
#endif

class ReflexLimiter {
public:
    // Resolve NvAPI function pointers from nvapi64.dll.
    // Returns true if nvapi64.dll is loaded and functions were resolved.
    // Safe to call multiple times — will re-check if nvapi64.dll loads later.
    bool Init() {
        // If already successfully initialized, return cached result
        if (available_.load(std::memory_order_acquire))
            return true;

        // If we already attempted init this session and nvapi wasn't there,
        // still allow re-checking (game may load nvapi64.dll later when user
        // enables Reflex in settings). Reset inited_ to permit re-entry.
        if (inited_.load(std::memory_order_acquire)) {
            // Re-check: has nvapi64.dll been loaded since our last attempt?
            HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
            if (!hNvApi)
                return false;
            // DLL is now loaded — reset so we can initialize
            inited_.store(false, std::memory_order_release);
        }

        inited_.store(true, std::memory_order_release);

        HMODULE hNvApi = GetModuleHandleW(L"nvapi64.dll");
        if (!hNvApi) {
            return false;
        }

        origQueryInterface_ =
            reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(hNvApi, "nvapi_QueryInterface"));
        if (!origQueryInterface_) {
            HookLogImportant("ReflexLimiter: nvapi64.dll loaded but nvapi_QueryInterface not found in exports");
            return false;
        }

        // Hook nvapi_QueryInterface to intercept game's Reflex calls
        HookNvAPIQueryInterface();

        // Resolve original function pointers
        origSetSleepMode_ =
            reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
        origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(origQueryInterface_(NVAPI_ID_D3D_Sleep));

        if (!origSetSleepMode_ || !origSleep_) {
            HookLogImportant("ReflexLimiter: nvapi_QueryInterface resolved but SetSleepMode=%p Sleep=%p (incomplete)",
                    (void*)origSetSleepMode_, (void*)origSleep_);
            return false;
        }

        available_.store(true, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Ready (SetSleepMode=%p, Sleep=%p)", (void*)origSetSleepMode_, (void*)origSleep_);
        return true;
    }

    // Provide the D3D device from our Present hooks. Must be called before
    // PushFpsLimit() can work. Safe to call repeatedly (stores latest).
    void SetDevice(IUnknown* device) {
        lastDevice_ = device;
    }

    // Set the target FPS for Reflex-based limiting. 0 = disable override.
    void SetTargetFps(int fps) {
        if (fps <= 0) {
            targetIntervalUs_.store(0, std::memory_order_release);
        } else {
            targetIntervalUs_.store(1000000 / fps, std::memory_order_release);
        }
    }

    // Returns true if we've successfully pushed an FPS limit via Reflex.
    // This means: functions resolved + device set + last push succeeded.
    bool IsActive() const {
        return pushSucceeded_.load(std::memory_order_acquire);
    }

    // Returns true if NvAPI functions are resolved (nvapi64.dll present).
    // Does NOT guarantee PushFpsLimit will succeed (need device too).
    bool IsAvailable() const {
        return available_.load(std::memory_order_acquire);
    }

    // Directly push our FPS limit to the driver's Reflex pipeline.
    // Returns true if the SetSleepMode call succeeded.
    bool PushFpsLimit() {
        if (!origSetSleepMode_ || !lastDevice_) {
            pushSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);

        NV_SET_SLEEP_MODE_PARAMS params{};
        params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = 1;
        params.minimumIntervalUs = intervalUs;

        NvAPI_Status status = origSetSleepMode_(lastDevice_, &params);
        if (status == NVAPI_OK) {
            if (!pushSucceeded_.load(std::memory_order_acquire)) {
                HookLog("ReflexLimiter: Pushed FPS limit (intervalUs=%u)", intervalUs);
            }
            pushSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        if (pushSucceeded_.load(std::memory_order_acquire)) {
            HookLog("ReflexLimiter: PushFpsLimit failed (status=%d)", status);
        }
        pushSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    // Intercept a SetSleepMode call (for IAT hook integration).
    // When installed as a detour, this detects when the game activates Reflex.
    NvAPI_Status InterceptSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams) {
        if (!origSetSleepMode_ || !pParams)
            return NVAPI_ERROR;

        // Detect game activation: game called with low-latency mode enabled
        if (pParams->bLowLatencyMode && !gameActivated_.load(std::memory_order_acquire)) {
            gameActivated_.store(true, std::memory_order_release);
            HookLogImportant("ReflexLimiter: Game activated Reflex (minimumIntervalUs=%u)", pParams->minimumIntervalUs);
        } else if (!pParams->bLowLatencyMode && gameActivated_.load(std::memory_order_acquire)) {
            gameActivated_.store(false, std::memory_order_release);
            HookLogImportant("ReflexLimiter: Game deactivated Reflex");
        }

        // Store device for PushFpsLimit
        lastDevice_ = pDev;

        // Override minimumIntervalUs if we have a target
        uint32_t ourInterval = targetIntervalUs_.load(std::memory_order_acquire);
        if (ourInterval > 0) {
            pParams->minimumIntervalUs = ourInterval;
        }

        return origSetSleepMode_(pDev, pParams);
    }

    // Returns true if the game has activated Reflex (called SetSleepMode with bLowLatencyMode=true)
    bool IsGameActivated() const {
        return gameActivated_.load(std::memory_order_acquire);
    }

    // Set game activation state (used by Streamline hook to detect Reflex activation)
    void SetGameActivated(bool activated) {
        bool wasActive = gameActivated_.exchange(activated, std::memory_order_acq_rel);
        if (activated && !wasActive) {
            HookLogImportant("ReflexLimiter: Game ACTIVATED Reflex (via Streamline)");
        } else if (!activated && wasActive) {
            HookLogImportant("ReflexLimiter: Game DEACTIVATED Reflex (via Streamline)");
        }
    }

    void Shutdown() {
        targetIntervalUs_.store(0, std::memory_order_release);
        pushSucceeded_.store(false, std::memory_order_release);
        gameActivated_.store(false, std::memory_order_release);
        available_.store(false, std::memory_order_release);
        inited_.store(false, std::memory_order_release);
        lastDevice_ = nullptr;
        origSetSleepMode_ = nullptr;
        origSleep_ = nullptr;
        origQueryInterface_ = nullptr;
    }

    // Get original function pointers (for pass-through when not overriding)
    PFN_NvAPI_D3D_SetSleepMode GetOrigSetSleepMode() const {
        return origSetSleepMode_;
    }
    PFN_NvAPI_D3D_Sleep GetOrigSleep() const {
        return origSleep_;
    }

    // Hook the game's nvapi_QueryInterface to detect game Reflex activation.
    // Uses InlineHook to directly hook the function address, intercepting ALL calls
    // regardless of how the game obtained the function pointer (cached, GetProcAddress, etc).
    // When the game queries NVAPI_ID_D3D_SetSleepMode, we return a wrapper that
    // detects bLowLatencyMode=true and marks gameActivated_.
    // Only available in the Hook DLL build (not Vulkan Layer).
    void HookNvAPIQueryInterface() {
#if !REFLEX_IAT_HOOK_AVAILABLE
        HookLog("ReflexLimiter: Game activation detection not available in this build context");
        (void)realSetSleepModeForHook_;
        (void)detourOrigQueryInterface_;
#else
        if (!origQueryInterface_)
            return;

        // Verify we can resolve SetSleepMode before hooking
        auto pRealSetSleepMode = reinterpret_cast<void*>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
        if (!pRealSetSleepMode) {
            HookLog("ReflexLimiter: Cannot resolve SetSleepMode, skipping game activation hook");
            return;
        }

        // Store the real SetSleepMode for our intercept wrapper to forward to
        realSetSleepModeForHook_ = reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(pRealSetSleepMode);

        // Install inline hook on nvapi_QueryInterface itself.
        // This intercepts ALL calls to nvapi_QueryInterface, regardless of how the
        // game obtained the function pointer (direct call, cached from GetProcAddress, etc).
        void* queryInterfaceAddr = reinterpret_cast<void*>(origQueryInterface_);
        void* trampoline = nullptr;
        if (InlineHook::Install(queryInterfaceAddr, reinterpret_cast<void*>(&ReflexDetour_QueryInterface),
                                &trampoline)) {
            // Store the trampoline as the original function for forwarding non-intercepted calls
            detourOrigQueryInterface_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(trampoline);
            HookLogImportant(
                "ReflexLimiter: Inline hook installed on nvapi_QueryInterface (target=%p, detour=%p, "
                "trampoline=%p)",
                queryInterfaceAddr, (void*)&ReflexDetour_QueryInterface, trampoline);
        } else {
            HookLogImportant("ReflexLimiter: Failed to install inline hook on nvapi_QueryInterface");
        }
#endif
    }

    // Detour for nvapi_QueryInterface — intercepts game's NvAPI calls.
    // Only available in the Hook DLL build.
#if REFLEX_IAT_HOOK_AVAILABLE
    static void* __cdecl ReflexDetour_QueryInterface(uint32_t id);

    // Detour for NvAPI_D3D_SetSleepMode — detects game activation.
    static NvAPI_Status __cdecl ReflexDetour_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);
#endif

private:
    std::atomic<bool> inited_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> pushSucceeded_{false};
    std::atomic<bool> gameActivated_{false};  // True when game calls SetSleepMode with bLowLatencyMode=true
    IUnknown* lastDevice_ = nullptr;
    PFN_NvAPI_QueryInterface origQueryInterface_ = nullptr;
    PFN_NvAPI_D3D_SetSleepMode origSetSleepMode_ = nullptr;
    PFN_NvAPI_D3D_Sleep origSleep_ = nullptr;
    std::atomic<uint32_t> targetIntervalUs_{0};
    PFN_NvAPI_D3D_SetSleepMode realSetSleepModeForHook_ = nullptr;  // Real SetSleepMode for detour forwarding
    PFN_NvAPI_QueryInterface detourOrigQueryInterface_ = nullptr;   // Original QueryInterface from dynamic hook
};

// Global instance (forward-declared for static detour methods)
inline ReflexLimiter g_ReflexLimiter;

// ============================================================================
// Static detour method definitions (after g_ReflexLimiter declaration)
// Only compiled in the Hook DLL build context.
// ============================================================================

#if REFLEX_IAT_HOOK_AVAILABLE

inline void* __cdecl ReflexLimiter::ReflexDetour_QueryInterface(uint32_t id) {
    auto& limiter = g_ReflexLimiter;

    if (id == NVAPI_ID_D3D_SetSleepMode) {
        // Return our intercept wrapper instead of the real function
        return reinterpret_cast<void*>(&ReflexDetour_SetSleepMode);
    }

    // Forward other IDs to the real nvapi_QueryInterface
    if (limiter.detourOrigQueryInterface_) {
        return limiter.detourOrigQueryInterface_(id);
    }
    // Fallback: use the original resolved pointer
    if (limiter.origQueryInterface_) {
        return limiter.origQueryInterface_(id);
    }
    return nullptr;
}

inline NvAPI_Status __cdecl ReflexLimiter::ReflexDetour_SetSleepMode(IUnknown* pDev,
                                                                     NV_SET_SLEEP_MODE_PARAMS* pParams) {
    auto& limiter = g_ReflexLimiter;

    if (!pParams) {
        return NVAPI_ERROR;
    }

    // Detect game activation: game called with low-latency mode enabled
    if (pParams->bLowLatencyMode && !limiter.gameActivated_.load(std::memory_order_acquire)) {
        limiter.gameActivated_.store(true, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Game ACTIVATED Reflex via SetSleepMode (minimumIntervalUs=%u)",
                         pParams->minimumIntervalUs);
    } else if (!pParams->bLowLatencyMode && limiter.gameActivated_.load(std::memory_order_acquire)) {
        limiter.gameActivated_.store(false, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Game DEACTIVATED Reflex via SetSleepMode");
    }

    // Store device reference for PushFpsLimit
    limiter.lastDevice_ = pDev;

    // Forward to the real SetSleepMode function
    if (limiter.realSetSleepModeForHook_) {
        return limiter.realSetSleepModeForHook_(pDev, pParams);
    }
    if (limiter.origSetSleepMode_) {
        return limiter.origSetSleepMode_(pDev, pParams);
    }
    return NVAPI_ERROR;
}

#endif  // REFLEX_IAT_HOOK_AVAILABLE

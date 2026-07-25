#pragma once
// Reflex/Low-Latency FPS Limiter
//
// Resolves NvAPI_D3D_SetSleepMode and NvAPI_D3D_Sleep from nvapi64.dll and
// proactively pushes minimumIntervalUs to enforce an FPS cap through the
// driver's low-latency pipeline. The normal CE-owned limiter path calls the
// original NvAPI entrypoints directly and intentionally avoids patching
// nvapi64.dll code bytes, because some DLSS FG integrations validate those
// prologues during Reflex setup.
//
// Flow:
//   1. Init() resolves NvAPI function pointers via nvapi_QueryInterface
//   2. SetDevice() provides the D3D device from our Present hooks
//   3. SetTargetFps() + PushFpsLimit() pushes minimumIntervalUs to the driver
//   4. Sleep() can ask the driver's NvAPI_D3D_Sleep() to pace a CE-owned frame
//   5. Optional inline detours exist only for explicit native-call diagnostics
//
// Game Activation Detection:
//   Modern Streamline Reflex activation and sleep calls are observed in
//   streamline_hook.cpp. Direct NvAPI SetSleepMode/Sleep detours remain
//   available but are not installed by default, because returning wrapper
//   pointers or patching NvAPI prologues can trip integrity checks.

// clang-format off
#include <windows.h>
// clang-format on
#include <intrin.h>
#include <atomic>
#include <cstring>
#include "../apis/dx12_hook.h"
#include "fg_detection.h"
#include "fps_limiter_policy.h"
#include "hook_common.h"
#include "overlay_compat.h"
#include "reflex_defs.h"
#include "streamline_runtime_policy.h"

// IAT hook infrastructure for game activation detection.
// Only available in the Hook DLL build, not in the Vulkan Layer.
#if defined(BUILDING_CAPTURE_HOOK) && !defined(VK_LAYER_CE_OVERLAY)
#include "../wrappers/iat_hook.h"
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
    bool Init();

    // Provide the D3D device from our Present hooks. Must be called before
    // PushFpsLimit() can work. Safe to call repeatedly (stores latest).
    void SetDevice(IUnknown* device) {
        if (device && lastDevice_ != device) {
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
            manualRearmBeforeNextPush_.store(true, std::memory_order_release);
            if (targetIntervalUs_.load(std::memory_order_acquire) != 0) {
                HookLogImportant("ReflexLimiter: Device supplied for native pacing (device=%p)", device);
            }
        }
        lastDevice_ = device;
    }

    bool HasDevice() const {
        return lastDevice_ != nullptr;
    }

    // Set the target FPS for Reflex-based limiting. 0 = disable override and
    // actively clear the previously pushed driver interval when possible.
    void SetTargetFps(int fps);

    uint32_t GetTargetIntervalUs() const {
        return targetIntervalUs_.load(std::memory_order_acquire);
    }

    void MarkNativePacingSignal() {
        lastNativePacingSignalTick_.store(GetTickCount64(), std::memory_order_release);
    }

    bool HasRecentNativePacingSignal(uint32_t maxAgeMs) const {
        const ULONGLONG lastTick = lastNativePacingSignalTick_.load(std::memory_order_acquire);
        if (lastTick == 0) {
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        return now >= lastTick && (now - lastTick) <= maxAgeMs;
    }

    void MarkGameSleep(const char* sourceName = nullptr) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG previous = lastGameSleepTick_.exchange(now, std::memory_order_acq_rel);
        gameSleepObserved_.store(true, std::memory_order_release);
        gameSleepCount_.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0) {
            HookLogImportant("ReflexLimiter: Game called Reflex Sleep via %s",
                             sourceName && sourceName[0] ? sourceName : "unknown");
        }
    }

    bool HasRecentGameSleep(uint32_t maxAgeMs) const {
        const ULONGLONG lastTick = lastGameSleepTick_.load(std::memory_order_acquire);
        if (lastTick == 0) {
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        return now >= lastTick && (now - lastTick) <= maxAgeMs;
    }

    bool HasObservedGameSleep() const {
        return gameSleepObserved_.load(std::memory_order_acquire);
    }

    uint32_t GetGameSleepCount() const {
        return gameSleepCount_.load(std::memory_order_acquire);
    }

    void ConfigureHybridPacing(int64_t qpcFreq, int fps);

    void DisableHybridPacing() {
        hybridIntervalTicks_.store(0, std::memory_order_release);
        hybridTargetTick_.store(0, std::memory_order_release);
        hybridQpcFrequency_.store(0, std::memory_order_release);
    }

    void ApplyHybridPacingBeforeNativeSleep();

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

    bool AreInlineHooksInstalled() const {
        return directSetSleepModeHooked_ || directSleepHooked_ || directQueryInterfaceHooked_;
    }

    void SetManualLimiterConfiguredOrActive(bool configured);
    bool IsManualLimiterConfiguredOrActive() const;
    void EnsureGameOwnedReflexHooks();

#ifdef CE_UNIT_TESTS
    void TestInstallSetSleepModeForUnitTest(PFN_NvAPI_D3D_SetSleepMode setSleepMode, IUnknown* device) {
        Shutdown();
        origSetSleepMode_ = setSleepMode;
        realSetSleepModeForHook_ = setSleepMode;
        lastDevice_ = device;
        available_.store(setSleepMode != nullptr, std::memory_order_release);
        inited_.store(setSleepMode != nullptr, std::memory_order_release);
    }
#endif

    bool ClearFpsLimit();

    // Directly push our FPS limit to the driver's Reflex pipeline.
    // Returns true if the SetSleepMode call succeeded.
    bool PushFpsLimit();

    bool Sleep();

    // Intercept a SetSleepMode call (for IAT hook integration).
    // When installed as a detour, this detects when the game activates Reflex.
    NvAPI_Status InterceptSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);

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
            lastNativePacingSignalTick_.store(0, std::memory_order_release);
            lastGameSleepTick_.store(0, std::memory_order_release);
            gameSleepObserved_.store(false, std::memory_order_release);
            gameSleepCount_.store(0, std::memory_order_release);
        }
    }

    void Shutdown();

    // Get original function pointers (for pass-through when not overriding)
    PFN_NvAPI_D3D_SetSleepMode GetOrigSetSleepMode() const {
        return origSetSleepMode_;
    }
    PFN_NvAPI_D3D_Sleep GetOrigSleep() const {
        return origSleep_;
    }

    void EnsureNvAPIHooksInstalled();

    // Detour for NvAPI_D3D_SetSleepMode — detects game activation.
    // Only available in the Hook DLL build.
#if REFLEX_IAT_HOOK_AVAILABLE
    static void* __cdecl ReflexDetour_QueryInterface(uint32_t functionId);

    static NvAPI_Status __cdecl ReflexDetour_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);

    // Detour for NvAPI_D3D_Sleep — detects whether the game actually uses native pacing.
    static NvAPI_Status __cdecl ReflexDetour_Sleep(IUnknown* pDev);
#endif

private:
    static bool IsSystemModulePath(const char* path);
    static bool IsCaptureHookModulePath(const char* path);

#if REFLEX_IAT_HOOK_AVAILABLE
    void RegisterQueryInterfaceHook();
    bool ShouldReturnWrapperToCaller(const void* callerAddress, char* callerPath, size_t callerPathLen,
                                     const char** reasonOut) const;
#endif

    NV_SET_SLEEP_MODE_PARAMS BuildSleepModeParams(uint32_t intervalUs, bool forceLowLatency) const;

    PFN_NvAPI_D3D_SetSleepMode GetForwardSetSleepMode() const {
        return realSetSleepModeForHook_ ? realSetSleepModeForHook_ : origSetSleepMode_;
    }

    PFN_NvAPI_D3D_Sleep GetForwardSleep() const {
        return realSleepForHook_ ? realSleepForHook_ : origSleep_;
    }

    void ForceLowLatencyResetBeforeManualPush(PFN_NvAPI_D3D_SetSleepMode forwardSetSleepMode, uint32_t nextIntervalUs);

    std::atomic<bool> inited_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> pushSucceeded_{false};
    std::atomic<bool> sleepSucceeded_{false};
    std::atomic<bool> gameActivated_{false};  // True when game calls SetSleepMode with bLowLatencyMode=true
    IUnknown* lastDevice_ = nullptr;
    PFN_NvAPI_QueryInterface origQueryInterface_ = nullptr;
    PFN_NvAPI_D3D_SetSleepMode origSetSleepMode_ = nullptr;
    PFN_NvAPI_D3D_Sleep origSleep_ = nullptr;
    std::atomic<bool> manualLimiterConfiguredOrActive_{false};
    std::atomic<bool> manualRearmBeforeNextPush_{false};
    std::atomic<uint32_t> targetIntervalUs_{0};
    std::atomic<ULONGLONG> lastNativePacingSignalTick_{0};
    std::atomic<ULONGLONG> lastGameSleepTick_{0};
    std::atomic<bool> gameSleepObserved_{false};
    std::atomic<uint32_t> gameSleepCount_{0};
    std::atomic<uint32_t> lastPushedIntervalUs_{UINT32_MAX};
    PFN_NvAPI_D3D_SetSleepMode realSetSleepModeForHook_ = nullptr;  // Real SetSleepMode for detour forwarding
    PFN_NvAPI_D3D_Sleep realSleepForHook_ = nullptr;                // Real Sleep for detour forwarding
    bool directSetSleepModeHooked_ = false;
    bool directSleepHooked_ = false;
    bool directQueryInterfaceHooked_ = false;  // Inline hook on nvapi_QueryInterface (IAT fallback)
    PFN_NvAPI_D3D_SetSleepMode directSetSleepModeTrampoline_ = nullptr;
    PFN_NvAPI_D3D_Sleep directSleepTrampoline_ = nullptr;
    PFN_NvAPI_QueryInterface directQueryInterfaceTrampoline_ = nullptr;  // Trampoline for QueryInterface inline hook
    bool loggedMissingDevice_ = false;
    bool loggedMissingSleepDevice_ = false;
    std::atomic<bool> ceOwnedSleepLogged_{false};
    std::atomic<bool> hasLastSleepModeParams_{false};
    NV_SET_SLEEP_MODE_PARAMS lastSleepModeParams_{};
    std::atomic<int64_t> hybridQpcFrequency_{0};
    std::atomic<int64_t> hybridIntervalTicks_{0};
    std::atomic<int64_t> hybridTargetTick_{0};
};

// Global instance (forward-declared for static detour methods)
inline ReflexLimiter g_ReflexLimiter;


#include "reflex_limiter_detail/nvapi_hooks.h"
#include "reflex_limiter_detail/pacing.h"
#include "reflex_limiter_query_hook.inl"

// ============================================================================
// Static detour method definitions (after g_ReflexLimiter declaration)
// Only compiled in the Hook DLL build context.
// ============================================================================

#if REFLEX_IAT_HOOK_AVAILABLE

inline NvAPI_Status __cdecl ReflexLimiter::ReflexDetour_SetSleepMode(IUnknown* pDev,
                                                                     NV_SET_SLEEP_MODE_PARAMS* pParams) {
    auto& limiter = g_ReflexLimiter;

    auto forwardSetSleepMode = limiter.GetForwardSetSleepMode();
    if (!forwardSetSleepMode) {
        return NVAPI_ERROR;
    }

    static thread_local bool s_InsideReflexSetSleepMode = false;
    if (s_InsideReflexSetSleepMode) {
        return forwardSetSleepMode(pDev, pParams);
    }

    s_InsideReflexSetSleepMode = true;
    NvAPI_Status status = limiter.InterceptSetSleepMode(pDev, pParams);
    s_InsideReflexSetSleepMode = false;

    // Reuse the shared interception path so the inline NvAPI detour also applies
    // our minimumIntervalUs override instead of only observing activation.
    return status;
}

inline NvAPI_Status __cdecl ReflexLimiter::ReflexDetour_Sleep(IUnknown* pDev) {
    auto& limiter = g_ReflexLimiter;

    auto forwardSleep = limiter.GetForwardSleep();
    if (!forwardSleep) {
        return NVAPI_ERROR;
    }

    if (pDev && limiter.lastDevice_ != pDev) {
        limiter.lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    }
    limiter.lastDevice_ = pDev;

    static std::atomic<int> s_sleepInterceptLog{0};
    const int interceptLogCount = s_sleepInterceptLog.fetch_add(1, std::memory_order_relaxed);
    if (interceptLogCount < 10) {
        HookLogImportant(
            "ReflexLimiter: Sleep intercepted via inline hook (device=%p, forward=%p, directHooked=%d, "
            "sleepCount=%u, gameActivated=%d)",
            pDev, forwardSleep, limiter.directSleepHooked_ ? 1 : 0, limiter.GetGameSleepCount(),
            limiter.IsGameActivated() ? 1 : 0);
    }

    limiter.ApplyHybridPacingBeforeNativeSleep();

    NvAPI_Status status = forwardSleep(pDev);
    if (status == NVAPI_OK) {
        limiter.MarkGameSleep("NvAPI");
        limiter.MarkNativePacingSignal();
    } else {
        static std::atomic<int> s_sleepFailLogCount{0};
        const int failCount = s_sleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("ReflexLimiter: NvAPI Sleep forward failed (status=%d)", status);
        }
    }
    return status;
}

#endif  // REFLEX_IAT_HOOK_AVAILABLE

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
#include "ngx_fg_preset_override.h"
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

    // Arms only the filtered nvapi_QueryInterface resolution path (dynamic
    // GetProcAddress entry plus IAT patch) without enabling any Reflex-specific
    // behavior. Consumers that merely need to observe or wrap an NvAPI pointer -
    // currently the DLSS FG render-preset override - use this so a single hook
    // owns that export.
    void EnsureNvApiQueryInterfaceInterception();

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
inline bool ReflexLimiter::IsSystemModulePath(const char* path) {
    return ce::overlay_compat::detail::ContainsInsensitive(path, "\\system32\\") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "\\syswow64\\");
}

inline bool ReflexLimiter::IsCaptureHookModulePath(const char* path) {
    return ce::overlay_compat::detail::ContainsInsensitive(path, "capture_hook") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "d3d12_wrappers") ||
           ce::overlay_compat::detail::ContainsInsensitive(path, "vk_layer_ce_overlay");
}

inline void ReflexLimiter::SetManualLimiterConfiguredOrActive(bool configured) {
    const bool wasConfigured = manualLimiterConfiguredOrActive_.exchange(configured, std::memory_order_acq_rel);
    if (configured && !wasConfigured) {
        EnsureGameOwnedReflexHooks();
    }
}

inline bool ReflexLimiter::IsManualLimiterConfiguredOrActive() const {
    if (manualLimiterConfiguredOrActive_.load(std::memory_order_acquire)) {
        return true;
    }

#ifndef VK_LAYER_CE_OVERLAY
    auto sharedMemoryWantsManualReflex = [](SharedMemoryLayout* shm) {
        if (!shm) {
            return false;
        }
        constexpr uint32_t kNativeMode = static_cast<uint32_t>(LimiterMode::kNative);
        return ce::fps_limiter_policy::IsManualReflexLimiterConfigured(
            shm->fpsLimiter.GetGeneralEnabled(), shm->fpsLimiter.GetGeneralFps(),
            shm->fpsLimiter.GetGeneralLimiterMode(), shm->fpsLimiter.GetCaptureSyncEnabled(),
            shm->fpsLimiter.GetCaptureSyncLimiterMode(), kNativeMode);
    };

    if (g_IPC && sharedMemoryWantsManualReflex(g_IPC->GetSharedMem())) {
        return true;
    }
    if (sharedMemoryWantsManualReflex(g_pSharedMem)) {
        return true;
    }
    if (g_pLocalConfig) {
        const auto& fps = g_pLocalConfig->fpsLimiter;
        return ce::fps_limiter_policy::IsManualReflexLimiterConfigured(
            fps.generalEnabled, fps.generalFps, static_cast<uint32_t>(fps.generalLimiterMode), fps.captureSyncEnabled,
            static_cast<uint32_t>(fps.captureSyncLimiterMode), static_cast<uint32_t>(LimiterMode::kNative));
    }
#endif

    return false;
}

inline void ReflexLimiter::EnsureNvApiQueryInterfaceInterception() {
#if REFLEX_IAT_HOOK_AVAILABLE
    RegisterQueryInterfaceHook();
#endif
}

inline void ReflexLimiter::EnsureGameOwnedReflexHooks() {
#if REFLEX_IAT_HOOK_AVAILABLE
    if (IsManualLimiterConfiguredOrActive()) {
        RegisterQueryInterfaceHook();
        if (origQueryInterface_ && !directQueryInterfaceHooked_) {
            void* trampoline = nullptr;
            if (InlineHook::Install(reinterpret_cast<void*>(origQueryInterface_),
                                    reinterpret_cast<void*>(&ReflexDetour_QueryInterface), &trampoline)) {
                directQueryInterfaceHooked_ = true;
                directQueryInterfaceTrampoline_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(trampoline);
                origQueryInterface_ = directQueryInterfaceTrampoline_;
                HookLogImportant(
                    "ReflexLimiter: Inline hook installed on NvAPI_QueryInterface as IAT fallback "
                    "(trampoline=%p detour=%p orig=%p)",
                    trampoline, &ReflexDetour_QueryInterface, reinterpret_cast<void*>(origQueryInterface_));
            } else {
                static std::atomic<int> s_hookFailLog{0};
                if (s_hookFailLog.fetch_add(1, std::memory_order_relaxed) < 3) {
                    HookLogImportant("ReflexLimiter: Failed to install inline hook on NvAPI_QueryInterface");
                }
            }
        }
        EnsureNvAPIHooksInstalled();
    }
#endif
}

#if REFLEX_IAT_HOOK_AVAILABLE

inline void ReflexLimiter::RegisterQueryInterfaceHook() {
    static std::atomic<bool> s_dynamicHookRegistered{false};
    if (!s_dynamicHookRegistered.exchange(true, std::memory_order_acq_rel)) {
        IATHook::RegisterDynamicHook("nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface),
                                     reinterpret_cast<void**>(&origQueryInterface_));
        IATHook::InitializeGetProcAddressHook();
        HookLogImportant("ReflexLimiter: Registered filtered nvapi_QueryInterface GetProcAddress hook");
    }

    HMODULE hNvApi64 = GetModuleHandleW(L"nvapi64.dll");
    if (hNvApi64 && !origQueryInterface_) {
        origQueryInterface_ =
            reinterpret_cast<PFN_NvAPI_QueryInterface>(GetProcAddress(hNvApi64, "nvapi_QueryInterface"));
    }

    static std::atomic<bool> s_iatPatchAttempted{false};
    const bool anyNvApiLoaded = hNvApi64 || GetModuleHandleW(L"nvapi.dll");
    if (anyNvApiLoaded && !s_iatPatchAttempted.exchange(true, std::memory_order_acq_rel)) {
        void* original = nullptr;
        const bool patched64 = IATHook::PatchIATAllModules(
            "nvapi64.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface), &original);
        if (original && !origQueryInterface_) {
            origQueryInterface_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(original);
        }
        original = nullptr;
        const bool patched32 = IATHook::PatchIATAllModules(
            "nvapi.dll", "nvapi_QueryInterface", reinterpret_cast<void*>(&ReflexDetour_QueryInterface), &original);
        if (original && !origQueryInterface_) {
            origQueryInterface_ = reinterpret_cast<PFN_NvAPI_QueryInterface>(original);
        }
        HookLogImportant("ReflexLimiter: Filtered nvapi_QueryInterface IAT hook attempted (nvapi64=%d nvapi=%d)",
                         patched64 ? 1 : 0, patched32 ? 1 : 0);
    }
}

inline bool ReflexLimiter::ShouldReturnWrapperToCaller(const void* callerAddress, char* callerPath,
                                                       size_t callerPathLen, const char** reasonOut) const {
    if (callerPath && callerPathLen > 0) {
        callerPath[0] = '\0';
    }
    if (reasonOut) {
        *reasonOut = "unknown";
    }

    const bool haveCallerPath =
        ce::overlay_compat::TryGetModulePathFromCodeAddress(callerAddress, callerPath, callerPathLen);
    const bool callerIsStreamlineRuntime =
        haveCallerPath && (ce::overlay_compat::IsStreamlineFrameGenerationModulePath(callerPath) ||
                           ce::overlay_compat::IsFFXFrameGenerationModulePath(callerPath));
    const bool callerIsThirdPartyOverlay =
        haveCallerPath && ce::overlay_compat::IsThirdPartyOverlayModulePath(callerPath);
    const bool callerIsSystemModule = !haveCallerPath || IsSystemModulePath(callerPath);
    const bool callerIsCaptureHookModule = haveCallerPath && IsCaptureHookModulePath(callerPath);
    const bool manualReflex = IsManualLimiterConfiguredOrActive();
    const bool shouldReturnWrapper = ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(
        manualReflex, callerIsStreamlineRuntime, callerIsThirdPartyOverlay, callerIsSystemModule,
        callerIsCaptureHookModule);

    if (reasonOut) {
        if (!manualReflex) {
            *reasonOut = "manual_reflex_off";
        } else if (callerIsStreamlineRuntime) {
            *reasonOut = "streamline_or_fg_runtime";
        } else if (callerIsThirdPartyOverlay) {
            *reasonOut = "third_party_overlay";
        } else if (callerIsSystemModule) {
            *reasonOut = haveCallerPath ? "system_module" : "unknown_caller";
        } else if (callerIsCaptureHookModule) {
            *reasonOut = "capture_hook_module";
        } else {
            *reasonOut = "game_caller";
        }
    }

    return shouldReturnWrapper;
}

inline void* __cdecl ReflexLimiter::ReflexDetour_QueryInterface(uint32_t functionId) {
    auto& limiter = g_ReflexLimiter;
    auto queryInterface = limiter.origQueryInterface_;
    if (!queryInterface || queryInterface == &ReflexDetour_QueryInterface) {
        return nullptr;
    }

    static thread_local bool s_insideQueryInterface = false;
    if (s_insideQueryInterface) {
        return queryInterface(functionId);
    }

    s_insideQueryInterface = true;
    void* result = queryInterface(functionId);
    s_insideQueryInterface = false;

    // This detour is the process-wide nvapi_QueryInterface resolution point, so
    // the DLSS FG render-preset override plugs in here rather than installing a
    // second hook on the same export. It only claims the DRS getter resolved by
    // nvngx_dlssg and only while `dlss_fg_preset` is configured.
    if (void* fgPresetWrapper = ce::ngx_fg_preset::MaybeWrapQueryInterface(functionId, result,
                                                                           __builtin_return_address(0))) {
        return fgPresetWrapper;
    }

    if (functionId != NVAPI_ID_D3D_SetSleepMode && functionId != NVAPI_ID_D3D_Sleep) {
        return result;
    }

    if (functionId == NVAPI_ID_D3D_SetSleepMode && result && !limiter.origSetSleepMode_) {
        limiter.origSetSleepMode_ = reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(result);
        limiter.realSetSleepModeForHook_ = limiter.origSetSleepMode_;
    } else if (functionId == NVAPI_ID_D3D_Sleep && result && !limiter.origSleep_) {
        limiter.origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(result);
        limiter.realSleepForHook_ = limiter.origSleep_;
    }
    if (limiter.origSetSleepMode_ && limiter.origSleep_ && !limiter.available_.load(std::memory_order_acquire)) {
        limiter.available_.store(true, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Ready through filtered nvapi_QueryInterface hook (SetSleepMode=%p, Sleep=%p)",
                         (void*)limiter.origSetSleepMode_, (void*)limiter.origSleep_);
    }
    if (!result) {
        return nullptr;
    }

    char callerPath[MAX_PATH] = {};
    const char* reason = nullptr;
    const void* callerAddress = __builtin_return_address(0);
    const bool returnWrapper =
        limiter.ShouldReturnWrapperToCaller(callerAddress, callerPath, sizeof(callerPath), &reason);

    if (returnWrapper) {
        static std::atomic<int> s_wrapperLogCount{0};
        const int logCount = s_wrapperLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 12) {
            HookLogImportant("ReflexLimiter: Returning %s wrapper from nvapi_QueryInterface to %s",
                             functionId == NVAPI_ID_D3D_SetSleepMode ? "SetSleepMode" : "Sleep",
                             callerPath[0] ? callerPath : "unknown");
        }
        return functionId == NVAPI_ID_D3D_SetSleepMode ? reinterpret_cast<void*>(&ReflexDetour_SetSleepMode)
                                                       : reinterpret_cast<void*>(&ReflexDetour_Sleep);
    }

    if (limiter.IsManualLimiterConfiguredOrActive()) {
        static std::atomic<int> s_passthroughLogCount{0};
        const int logCount = s_passthroughLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 12) {
            HookLogImportant("ReflexLimiter: Passing through %s nvapi_QueryInterface caller (%s, %s)",
                             functionId == NVAPI_ID_D3D_SetSleepMode ? "SetSleepMode" : "Sleep",
                             reason ? reason : "unknown", callerPath[0] ? callerPath : "unknown");
        }
    }

    return result;
}

#endif  // REFLEX_IAT_HOOK_AVAILABLE


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

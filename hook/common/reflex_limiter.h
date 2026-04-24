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
#include <intrin.h>
#include <atomic>
#include "../apis/dx12_hook.h"
#include "fg_detection.h"
#include "hook_common.h"
#include "reflex_defs.h"
#include "streamline_runtime_policy.h"

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

        // Resolve original function pointers
        origSetSleepMode_ =
            reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(origQueryInterface_(NVAPI_ID_D3D_SetSleepMode));
        origSleep_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(origQueryInterface_(NVAPI_ID_D3D_Sleep));

        if (!origSetSleepMode_ || !origSleep_) {
            HookLogImportant("ReflexLimiter: nvapi_QueryInterface resolved but SetSleepMode=%p Sleep=%p (incomplete)",
                             (void*)origSetSleepMode_, (void*)origSleep_);
            return false;
        }

        // Cache the real SetSleepMode/Sleep entrypoints for direct inline hook forwarding.
        realSetSleepModeForHook_ = origSetSleepMode_;
        realSleepForHook_ = origSleep_;

        // NOTE: We intentionally do NOT hook nvapi_QueryInterface.
        // In some titles (e.g. GTA V Enhanced) the inline hook patch on
        // nvapi_QueryInterface is detected by Streamline/DLSS FG or anti-tamper
        // validation, causing Reflex init to abort and DLSS FG to fail with the
        // pink-tint diagnostic.
        //
        // We also defer the direct inline hooks on SetSleepMode/Sleep until
        // the FPS limiter is actually configured (targetIntervalUs_ > 0).
        // This keeps nvapi64.dll unmodified when the user is not using the
        // FPS limiter, avoiding detection by integrity checks during DLSS FG
        // initialization.  Activation detection for auto-mode still works via
        // the Streamline slReflexSetOptions / slReflexSetConstants hooks;
        // direct-nvapi activation detection is only needed when we are
        // actively limiting.
        EnsureNvAPIHooksInstalled();

        available_.store(true, std::memory_order_release);
        HookLogImportant("ReflexLimiter: Ready (SetSleepMode=%p, Sleep=%p)", (void*)origSetSleepMode_,
                         (void*)origSleep_);
        return true;
    }

    // Provide the D3D device from our Present hooks. Must be called before
    // PushFpsLimit() can work. Safe to call repeatedly (stores latest).
    void SetDevice(IUnknown* device) {
        if (device && lastDevice_ != device) {
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        }
        lastDevice_ = device;
    }

    // Set the target FPS for Reflex-based limiting. 0 = disable override.
    void SetTargetFps(int fps) {
        if (fps <= 0) {
            targetIntervalUs_.store(0, std::memory_order_release);
        } else {
            targetIntervalUs_.store(1000000 / fps, std::memory_order_release);
        }
        // If the user has just configured an FPS cap, ensure the inline hooks
        // on SetSleepMode/Sleep are installed so we can intercept and override.
        EnsureNvAPIHooksInstalled();
    }

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

    void MarkGameSleep() {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG previous = lastGameSleepTick_.exchange(now, std::memory_order_acq_rel);
        gameSleepObserved_.store(true, std::memory_order_release);
        gameSleepCount_.fetch_add(1, std::memory_order_acq_rel);
        if (previous == 0) {
            HookLogImportant("ReflexLimiter: Game called Reflex Sleep");
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

    void ConfigureHybridPacing(int64_t qpcFreq, int fps) {
        if (qpcFreq <= 0 || fps <= 0) {
            hybridIntervalTicks_.store(0, std::memory_order_release);
            hybridTargetTick_.store(0, std::memory_order_release);
            return;
        }

        const int64_t newIntervalTicks = qpcFreq / fps;
        const int64_t oldIntervalTicks = hybridIntervalTicks_.load(std::memory_order_acquire);
        hybridQpcFrequency_.store(qpcFreq, std::memory_order_release);
        hybridIntervalTicks_.store(newIntervalTicks, std::memory_order_release);
        if (newIntervalTicks != oldIntervalTicks) {
            hybridTargetTick_.store(0, std::memory_order_release);
        }
    }

    void DisableHybridPacing() {
        hybridIntervalTicks_.store(0, std::memory_order_release);
        hybridTargetTick_.store(0, std::memory_order_release);
        hybridQpcFrequency_.store(0, std::memory_order_release);
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
        // Ensure hooks are installed if the user has configured a cap but
        // Init() ran before the cap was set.
        EnsureNvAPIHooksInstalled();

        auto forwardSetSleepMode = GetForwardSetSleepMode();
        if (!forwardSetSleepMode || !lastDevice_) {
            if (!loggedMissingDevice_) {
                HookLog("ReflexLimiter: PushFpsLimit skipped (SetSleepMode=%p, device=%p)", (void*)forwardSetSleepMode,
                        lastDevice_);
                loggedMissingDevice_ = true;
            }
            pushSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        loggedMissingDevice_ = false;

        uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);
        if (pushSucceeded_.load(std::memory_order_acquire) &&
            lastPushedIntervalUs_.load(std::memory_order_acquire) == intervalUs) {
            return true;
        }

        NV_SET_SLEEP_MODE_PARAMS params{};
        if (hasLastSleepModeParams_.load(std::memory_order_acquire)) {
            params = lastSleepModeParams_;
        } else {
            params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            params.bLowLatencyMode = 1;
        }
        params.minimumIntervalUs = intervalUs;

        NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
        if (status == NVAPI_OK) {
            if (!pushSucceeded_.load(std::memory_order_acquire)) {
                HookLog("ReflexLimiter: Pushed FPS limit (intervalUs=%u boost=%u markers=%u)", intervalUs,
                        params.bLowLatencyBoost, params.bUseMarkersToOptimize);
            }
            lastPushedIntervalUs_.store(intervalUs, std::memory_order_release);
            pushSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        if (pushSucceeded_.load(std::memory_order_acquire)) {
            HookLog("ReflexLimiter: PushFpsLimit failed (status=%d)", status);
        }
        pushSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    bool Sleep() {
        auto forwardSleep = GetForwardSleep();
        if (!forwardSleep || !lastDevice_) {
            if (!loggedMissingSleepDevice_) {
                HookLog("ReflexLimiter: Sleep skipped (Sleep=%p, device=%p)", (void*)forwardSleep, lastDevice_);
                loggedMissingSleepDevice_ = true;
            }
            sleepSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        loggedMissingSleepDevice_ = false;

        bool pushOk = PushFpsLimit();
        if (!pushOk && !gameActivated_.load(std::memory_order_acquire)) {
            sleepSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        NvAPI_Status status = forwardSleep(lastDevice_);
        if (status == NVAPI_OK) {
            if (!pushOk && gameActivated_.load(std::memory_order_acquire)) {
                HookLog("ReflexLimiter: Sleep succeeded using game-managed SetSleepMode state");
            }
            sleepSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        if (sleepSucceeded_.load(std::memory_order_acquire)) {
            HookLog("ReflexLimiter: Sleep failed (status=%d)", status);
        }
        sleepSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    // Intercept a SetSleepMode call (for IAT hook integration).
    // When installed as a detour, this detects when the game activates Reflex.
    NvAPI_Status InterceptSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams) {
        auto forwardSetSleepMode = GetForwardSetSleepMode();
        if (!forwardSetSleepMode || !pParams)
            return NVAPI_ERROR;

        // Diagnostic: detect struct version mismatches early
        if (pParams->version != NV_SET_SLEEP_MODE_PARAMS_VER) {
            static std::atomic<int> s_versionMismatchLogCount{0};
            int logCount = s_versionMismatchLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant(
                    "ReflexLimiter: SetSleepMode version mismatch (got=0x%08X expected=0x%08X size=%zu)",
                    pParams->version, NV_SET_SLEEP_MODE_PARAMS_VER, sizeof(NV_SET_SLEEP_MODE_PARAMS));
            }
        }

        const uint32_t ourInterval = targetIntervalUs_.load(std::memory_order_acquire);

        // Version-aware copy: the game may pass a smaller struct than ours
        // (e.g. version 0x1002C = 44 bytes).  Copy only what the caller
        // provided and zero the rest to avoid reading adjacent stack memory.
        uint32_t callerSize = pParams->version & 0xFFFF;
        uint32_t ourSize = sizeof(NV_SET_SLEEP_MODE_PARAMS);
        if (callerSize != ourSize) {
            static std::atomic<int> s_sizeMismatchLogCount{0};
            int logCount = s_sizeMismatchLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant(
                    "ReflexLimiter: SetSleepMode struct size mismatch (caller=%u our=%u version=0x%08X)",
                    callerSize, ourSize, pParams->version);
            }
        }
        uint32_t copySize = (callerSize < ourSize) ? callerSize : ourSize;
        CopyMemory(&lastSleepModeParams_, pParams, copySize);
        if (copySize < ourSize) {
            ZeroMemory(reinterpret_cast<uint8_t*>(&lastSleepModeParams_) + copySize, ourSize - copySize);
        }
        hasLastSleepModeParams_.store(true, std::memory_order_release);

        // Detect game activation: game called with low-latency mode enabled
        if (pParams->bLowLatencyMode && !gameActivated_.load(std::memory_order_acquire)) {
            gameActivated_.store(true, std::memory_order_release);
            HookLogImportant(
                "ReflexLimiter: Game activated Reflex (minimumIntervalUs=%u, override=%u, boost=%u, markers=%u)",
                pParams->minimumIntervalUs, ourInterval, pParams->bLowLatencyBoost, pParams->bUseMarkersToOptimize);
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
            const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
            if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
                    true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
                HookLogImportant(
                    "ReflexLimiter: Reflex activation requesting Streamline enable preparation "
                    "(runtime=%s apiFSR=%d fgOwned=%d)",
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
                    runtimeOwnsSwapchain ? 1 : 0);
                DX12_PrepareForStreamlineEnableTransition();
            }
        } else if (!pParams->bLowLatencyMode && gameActivated_.load(std::memory_order_acquire)) {
            gameActivated_.store(false, std::memory_order_release);
            HookLogImportant("ReflexLimiter: Game deactivated Reflex");
        }

        // Store device for PushFpsLimit / Sleep
        if (pDev && lastDevice_ != pDev) {
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        }
        lastDevice_ = pDev;

        MarkNativePacingSignal();

        // Override minimumIntervalUs if we have a target
        if (ourInterval > 0) {
            pParams->minimumIntervalUs = ourInterval;
        }

        if (!forwardSetSleepMode) {
            HookLogImportant("ReflexLimiter: SetSleepMode forward missing — no real or original pointer available");
            return NVAPI_ERROR;
        }

        NvAPI_Status status = forwardSetSleepMode(pDev, pParams);
        if (status == NVAPI_OK) {
            static std::atomic<bool> s_loggedSuccess{false};
            if (!s_loggedSuccess.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "ReflexLimiter: SetSleepMode forward succeeded (version=0x%08X intervalUs=%u boost=%u markers=%u)",
                    pParams->version, pParams->minimumIntervalUs, pParams->bLowLatencyBoost,
                    pParams->bUseMarkersToOptimize);
            }
        } else {
            static std::atomic<int> s_failLogCount{0};
            int failCount = s_failLogCount.fetch_add(1, std::memory_order_relaxed);
            if (failCount < 5) {
                HookLogImportant(
                    "ReflexLimiter: SetSleepMode forward failed (status=%d version=0x%08X intervalUs=%u boost=%u "
                    "markers=%u)",
                    status, pParams->version, pParams->minimumIntervalUs, pParams->bLowLatencyBoost,
                    pParams->bUseMarkersToOptimize);
            }
        }
        return status;
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
            lastNativePacingSignalTick_.store(0, std::memory_order_release);
            lastGameSleepTick_.store(0, std::memory_order_release);
            gameSleepObserved_.store(false, std::memory_order_release);
            gameSleepCount_.store(0, std::memory_order_release);
        }
    }

    void Shutdown() {
        targetIntervalUs_.store(0, std::memory_order_release);
        pushSucceeded_.store(false, std::memory_order_release);
        sleepSucceeded_.store(false, std::memory_order_release);
        loggedMissingDevice_ = false;
        loggedMissingSleepDevice_ = false;
        DisableHybridPacing();
        hasLastSleepModeParams_.store(false, std::memory_order_release);
        ZeroMemory(&lastSleepModeParams_, sizeof(lastSleepModeParams_));
        gameActivated_.store(false, std::memory_order_release);
        available_.store(false, std::memory_order_release);
        inited_.store(false, std::memory_order_release);
        lastDevice_ = nullptr;
        origSetSleepMode_ = nullptr;
        origSleep_ = nullptr;
        origQueryInterface_ = nullptr;
        lastNativePacingSignalTick_.store(0, std::memory_order_release);
        lastGameSleepTick_.store(0, std::memory_order_release);
        gameSleepObserved_.store(false, std::memory_order_release);
        gameSleepCount_.store(0, std::memory_order_release);
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        directSetSleepModeHooked_ = false;
        directSleepHooked_ = false;
        directSetSleepModeTrampoline_ = nullptr;
        directSleepTrampoline_ = nullptr;
        realSetSleepModeForHook_ = nullptr;
        realSleepForHook_ = nullptr;
    }

    // Get original function pointers (for pass-through when not overriding)
    PFN_NvAPI_D3D_SetSleepMode GetOrigSetSleepMode() const {
        return origSetSleepMode_;
    }
    PFN_NvAPI_D3D_Sleep GetOrigSleep() const {
        return origSleep_;
    }



    void EnsureNvAPIHooksInstalled() {
#if !REFLEX_IAT_HOOK_AVAILABLE
        (void)realSetSleepModeForHook_;
        (void)realSleepForHook_;
        return;
#else
        // Defer hook installation until the FPS limiter is actually configured.
        // When targetIntervalUs_ == 0 the user is not using Reflex-based limiting,
        // so keeping nvapi64.dll unmodified avoids detection by integrity checks
        // during DLSS FG initialization in titles like GTA V Enhanced.
        if (targetIntervalUs_.load(std::memory_order_acquire) == 0) {
            static std::atomic<bool> s_loggedSkip{false};
            if (!s_loggedSkip.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "ReflexLimiter: Deferring SetSleepMode/Sleep inline hooks — no FPS cap configured");
            }
            return;
        }

        if (origSetSleepMode_ && !directSetSleepModeHooked_) {
            void* trampoline = nullptr;
            if (InlineHook::Install(reinterpret_cast<void*>(origSetSleepMode_),
                                    reinterpret_cast<void*>(&ReflexDetour_SetSleepMode), &trampoline)) {
                directSetSleepModeHooked_ = true;
                directSetSleepModeTrampoline_ = reinterpret_cast<PFN_NvAPI_D3D_SetSleepMode>(trampoline);
                realSetSleepModeForHook_ = directSetSleepModeTrampoline_;
                HookLogImportant(
                    "ReflexLimiter: Inline hook installed on NvAPI_D3D_SetSleepMode (target=%p, detour=%p, "
                    "trampoline=%p)",
                    (void*)origSetSleepMode_, (void*)&ReflexDetour_SetSleepMode, trampoline);
            } else {
                HookLogImportant("ReflexLimiter: Failed to install inline hook on NvAPI_D3D_SetSleepMode");
            }
        }

        if (origSleep_ && !directSleepHooked_) {
            void* trampoline = nullptr;
            if (InlineHook::Install(reinterpret_cast<void*>(origSleep_), reinterpret_cast<void*>(&ReflexDetour_Sleep),
                                    &trampoline)) {
                directSleepHooked_ = true;
                directSleepTrampoline_ = reinterpret_cast<PFN_NvAPI_D3D_Sleep>(trampoline);
                realSleepForHook_ = directSleepTrampoline_;
                HookLogImportant(
                    "ReflexLimiter: Inline hook installed on NvAPI_D3D_Sleep (target=%p, detour=%p, trampoline=%p)",
                    (void*)origSleep_, (void*)&ReflexDetour_Sleep, trampoline);
            } else {
                HookLogImportant("ReflexLimiter: Failed to install inline hook on NvAPI_D3D_Sleep");
            }
        }
#endif
    }

    // Detour for NvAPI_D3D_SetSleepMode — detects game activation.
    // Only available in the Hook DLL build.
#if REFLEX_IAT_HOOK_AVAILABLE
    static NvAPI_Status __cdecl ReflexDetour_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);

    // Detour for NvAPI_D3D_Sleep — detects whether the game actually uses native pacing.
    static NvAPI_Status __cdecl ReflexDetour_Sleep(IUnknown* pDev);
#endif

private:
    PFN_NvAPI_D3D_SetSleepMode GetForwardSetSleepMode() const {
        return realSetSleepModeForHook_ ? realSetSleepModeForHook_ : origSetSleepMode_;
    }

    PFN_NvAPI_D3D_Sleep GetForwardSleep() const {
        return realSleepForHook_ ? realSleepForHook_ : origSleep_;
    }

    std::atomic<bool> inited_{false};
    std::atomic<bool> available_{false};
    std::atomic<bool> pushSucceeded_{false};
    std::atomic<bool> sleepSucceeded_{false};
    std::atomic<bool> gameActivated_{false};  // True when game calls SetSleepMode with bLowLatencyMode=true
    IUnknown* lastDevice_ = nullptr;
    PFN_NvAPI_QueryInterface origQueryInterface_ = nullptr;
    PFN_NvAPI_D3D_SetSleepMode origSetSleepMode_ = nullptr;
    PFN_NvAPI_D3D_Sleep origSleep_ = nullptr;
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
    PFN_NvAPI_D3D_SetSleepMode directSetSleepModeTrampoline_ = nullptr;
    PFN_NvAPI_D3D_Sleep directSleepTrampoline_ = nullptr;
    bool loggedMissingDevice_ = false;
    bool loggedMissingSleepDevice_ = false;
    std::atomic<bool> hasLastSleepModeParams_{false};
    NV_SET_SLEEP_MODE_PARAMS lastSleepModeParams_{};
    std::atomic<int64_t> hybridQpcFrequency_{0};
    std::atomic<int64_t> hybridIntervalTicks_{0};
    std::atomic<int64_t> hybridTargetTick_{0};
};

// Global instance (forward-declared for static detour methods)
inline ReflexLimiter g_ReflexLimiter;

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
    limiter.MarkGameSleep();

    const int64_t intervalTicks = limiter.hybridIntervalTicks_.load(std::memory_order_acquire);
    const int64_t qpcFrequency = limiter.hybridQpcFrequency_.load(std::memory_order_acquire);
    if (intervalTicks > 0 && qpcFrequency > 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        int64_t targetTick = limiter.hybridTargetTick_.load(std::memory_order_acquire);
        if (targetTick == 0) {
            targetTick = now.QuadPart + intervalTicks / 2;
        }

        while (targetTick > now.QuadPart) {
            const int64_t diffTicks = targetTick - now.QuadPart;
            const int64_t diffUs = (diffTicks * 1000000) / qpcFrequency;
            if (diffUs > 2000) {
                ::Sleep(0);
            } else if (diffUs > 500) {
                SwitchToThread();
            } else {
                _mm_pause();
            }
            QueryPerformanceCounter(&now);
        }

        limiter.hybridTargetTick_.store(targetTick + intervalTicks, std::memory_order_release);
    }

    return forwardSleep(pDev);
}

#endif  // REFLEX_IAT_HOOK_AVAILABLE

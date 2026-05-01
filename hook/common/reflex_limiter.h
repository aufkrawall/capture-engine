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

        // NOTE: We intentionally do NOT inline-hook nvapi_QueryInterface.
        // In some titles (e.g. GTA V Enhanced) a code-byte patch on
        // nvapi_QueryInterface is detected by Streamline/DLSS FG or anti-tamper
        // validation, causing Reflex init to abort and DLSS FG to fail with the
        // pink-tint diagnostic. Explicit/manual Reflex limiting can still use a
        // caller-filtered IAT/GetProcAddress hook so game-owned Sleep calls see
        // wrappers while Streamline/DLSS FG modules keep original driver pointers.
        //
        // We also do not install the direct SetSleepMode/Sleep inline hooks
        // merely because CE's limiter is configured. The explicit limiter can
        // push and sleep through the original function pointers, while
        // activation/sleep observation for Streamline Reflex happens through
        // Streamline feature hooks. Keeping nvapi64.dll unmodified is the safer
        // default for DLSS FG startup validation.

        available_.store(true, std::memory_order_release);
        EnsureGameOwnedReflexHooks();
        HookLogImportant("ReflexLimiter: Ready (SetSleepMode=%p, Sleep=%p, inlineHooks=%d)", (void*)origSetSleepMode_,
                         (void*)origSleep_, AreInlineHooksInstalled() ? 1 : 0);
        return true;
    }

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
    void SetTargetFps(int fps) {
        const uint32_t oldInterval = targetIntervalUs_.load(std::memory_order_acquire);
        uint32_t newInterval = 0;
        if (fps <= 0) {
            newInterval = 0;
        } else {
            newInterval = 1000000 / fps;
        }

        if (oldInterval == newInterval) {
            return;
        }

        targetIntervalUs_.store(newInterval, std::memory_order_release);
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        ceOwnedSleepLogged_.store(false, std::memory_order_release);
        manualRearmBeforeNextPush_.store(newInterval != 0, std::memory_order_release);

        if (newInterval == 0) {
            if (oldInterval != 0) {
                ClearFpsLimit();
            }
            return;
        }

        HookLogImportant("ReflexLimiter: Target FPS set to %d (intervalUs=%u, inlineHooks=%d)", fps, newInterval,
                         AreInlineHooksInstalled() ? 1 : 0);
        EnsureGameOwnedReflexHooks();
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

    void ApplyHybridPacingBeforeNativeSleep() {
        const int64_t intervalTicks = hybridIntervalTicks_.load(std::memory_order_acquire);
        const int64_t qpcFrequency = hybridQpcFrequency_.load(std::memory_order_acquire);
        if (intervalTicks <= 0 || qpcFrequency <= 0) {
            return;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        int64_t targetTick = hybridTargetTick_.load(std::memory_order_acquire);
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

        hybridTargetTick_.store(targetTick + intervalTicks, std::memory_order_release);
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

    bool ClearFpsLimit() {
        auto forwardSetSleepMode = GetForwardSetSleepMode();
        if (!forwardSetSleepMode || !lastDevice_) {
            if (pushSucceeded_.exchange(false, std::memory_order_acq_rel)) {
                HookLogImportant("ReflexLimiter: Could not clear FPS limit (SetSleepMode=%p, device=%p)",
                                 (void*)forwardSetSleepMode, lastDevice_);
            }
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
            return false;
        }

        NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(0, false);
        NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
        if (status == NVAPI_OK) {
            if (pushSucceeded_.exchange(false, std::memory_order_acq_rel)) {
                HookLogImportant("ReflexLimiter: Cleared FPS limit (boost=%u markers=%u lowLatency=%u)",
                                 params.bLowLatencyBoost, params.bUseMarkersToOptimize, params.bLowLatencyMode);
            }
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
            return true;
        }

        HookLogImportant("ReflexLimiter: Clear FPS limit failed (status=%d boost=%u markers=%u lowLatency=%u)", status,
                         params.bLowLatencyBoost, params.bUseMarkersToOptimize, params.bLowLatencyMode);
        pushSucceeded_.store(false, std::memory_order_release);
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        return false;
    }

    // Directly push our FPS limit to the driver's Reflex pipeline.
    // Returns true if the SetSleepMode call succeeded.
    bool PushFpsLimit() {
        auto forwardSetSleepMode = GetForwardSetSleepMode();
        if (!forwardSetSleepMode || !lastDevice_) {
            if (!loggedMissingDevice_) {
                HookLogImportant(
                    "ReflexLimiter: PushFpsLimit skipped (SetSleepMode=%p, device=%p, intervalUs=%u, available=%d)",
                    (void*)forwardSetSleepMode, lastDevice_, targetIntervalUs_.load(std::memory_order_acquire),
                    available_.load(std::memory_order_acquire) ? 1 : 0);
                loggedMissingDevice_ = true;
            }
            pushSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        loggedMissingDevice_ = false;

        uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);
        if (intervalUs == 0) {
            ClearFpsLimit();
            return false;
        }

        if (manualLimiterConfiguredOrActive_.load(std::memory_order_acquire) &&
            manualRearmBeforeNextPush_.exchange(false, std::memory_order_acq_rel)) {
            ForceLowLatencyResetBeforeManualPush(forwardSetSleepMode, intervalUs);
            pushSucceeded_.store(false, std::memory_order_release);
            lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        }

        if (pushSucceeded_.load(std::memory_order_acquire) &&
            lastPushedIntervalUs_.load(std::memory_order_acquire) == intervalUs) {
            return true;
        }

        NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(intervalUs, true);

        NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
        if (status == NVAPI_OK) {
            if (!pushSucceeded_.load(std::memory_order_acquire)) {
                HookLogImportant(
                    "ReflexLimiter: Pushed FPS limit (device=%p intervalUs=%u boost=%u markers=%u lowLatency=%u "
                    "version=0x%08X)",
                    lastDevice_, intervalUs, params.bLowLatencyBoost, params.bUseMarkersToOptimize,
                    params.bLowLatencyMode, params.version);
            }
            lastPushedIntervalUs_.store(intervalUs, std::memory_order_release);
            pushSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        static std::atomic<int> s_pushFailLogCount{0};
        const int failCount = s_pushFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5 || (failCount % 300) == 0) {
            HookLogImportant(
                "ReflexLimiter: PushFpsLimit failed (status=%d device=%p version=0x%08X intervalUs=%u boost=%u "
                "markers=%u lowLatency=%u inlineHooks=%d gameActive=%d)",
                status, lastDevice_, params.version, intervalUs, params.bLowLatencyBoost, params.bUseMarkersToOptimize,
                params.bLowLatencyMode, AreInlineHooksInstalled() ? 1 : 0,
                gameActivated_.load(std::memory_order_acquire) ? 1 : 0);
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
            static std::atomic<int> s_sleepSkippedAfterPushFailLogCount{0};
            const int skipCount = s_sleepSkippedAfterPushFailLogCount.fetch_add(1, std::memory_order_relaxed);
            if (skipCount < 5 || (skipCount % 300) == 0) {
                HookLogImportant(
                    "ReflexLimiter: Sleep skipped after failed FPS-limit push (device=%p intervalUs=%u gameActive=0)",
                    lastDevice_, targetIntervalUs_.load(std::memory_order_acquire));
            }
            sleepSucceeded_.store(false, std::memory_order_release);
            return false;
        }

        NvAPI_Status status = forwardSleep(lastDevice_);
        if (status == NVAPI_OK) {
            if (!pushOk && gameActivated_.load(std::memory_order_acquire)) {
                HookLog("ReflexLimiter: Sleep succeeded using game-managed SetSleepMode state");
            } else if (!ceOwnedSleepLogged_.exchange(true, std::memory_order_acq_rel)) {
                HookLogImportant("ReflexLimiter: CE-owned NvAPI Sleep succeeded (pushOk=%d intervalUs=%u)",
                                 pushOk ? 1 : 0, targetIntervalUs_.load(std::memory_order_acquire));
            }
            sleepSucceeded_.store(true, std::memory_order_release);
            return true;
        }
        static std::atomic<int> s_sleepFailLogCount{0};
        const int failCount = s_sleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5 || (failCount % 300) == 0) {
            HookLogImportant("ReflexLimiter: Sleep failed (status=%d device=%p pushOk=%d gameActive=%d intervalUs=%u)",
                             status, lastDevice_, pushOk ? 1 : 0,
                             gameActivated_.load(std::memory_order_acquire) ? 1 : 0,
                             targetIntervalUs_.load(std::memory_order_acquire));
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
                HookLogImportant("ReflexLimiter: SetSleepMode version mismatch (got=0x%08X expected=0x%08X size=%zu)",
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
                HookLogImportant("ReflexLimiter: SetSleepMode struct size mismatch (caller=%u our=%u version=0x%08X)",
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
        manualLimiterConfiguredOrActive_.store(false, std::memory_order_release);
        manualRearmBeforeNextPush_.store(false, std::memory_order_release);
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
        ceOwnedSleepLogged_.store(false, std::memory_order_release);
        directQueryInterfaceHooked_ = false;
        directQueryInterfaceTrampoline_ = nullptr;
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
        // This method is intentionally not called by the default CE-owned Reflex
        // limiter path. It remains available for direct NvAPI native-call
        // diagnostics, but installing it patches nvapi64.dll code bytes and can
        // be visible to DLSS FG / anti-tamper validation.
        if (targetIntervalUs_.load(std::memory_order_acquire) == 0) {
            static std::atomic<bool> s_loggedSkip{false};
            if (!s_loggedSkip.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant("ReflexLimiter: Deferring SetSleepMode/Sleep inline hooks — no FPS cap configured");
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

    NV_SET_SLEEP_MODE_PARAMS BuildSleepModeParams(uint32_t intervalUs, bool forceLowLatency) const {
        NV_SET_SLEEP_MODE_PARAMS params{};
        if (hasLastSleepModeParams_.load(std::memory_order_acquire)) {
            params = lastSleepModeParams_;
        } else {
            params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            params.bLowLatencyMode = gameActivated_.load(std::memory_order_acquire) ? 1 : 0;
        }

        if (params.version == 0) {
            params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        }

        if (forceLowLatency) {
            params.bLowLatencyMode = 1;
        }
        params.minimumIntervalUs = intervalUs;
        return params;
    }

    PFN_NvAPI_D3D_SetSleepMode GetForwardSetSleepMode() const {
        return realSetSleepModeForHook_ ? realSetSleepModeForHook_ : origSetSleepMode_;
    }

    PFN_NvAPI_D3D_Sleep GetForwardSleep() const {
        return realSleepForHook_ ? realSleepForHook_ : origSleep_;
    }

    void ForceLowLatencyResetBeforeManualPush(PFN_NvAPI_D3D_SetSleepMode forwardSetSleepMode, uint32_t nextIntervalUs) {
        if (!forwardSetSleepMode || !lastDevice_) {
            return;
        }

        NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(0, false);
        params.bLowLatencyMode = 0;
        params.bLowLatencyBoost = 0;
        params.bUseMarkersToOptimize = 0;
        params.minimumIntervalUs = 0;

        const NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
        if (status == NVAPI_OK) {
            HookLogImportant(
                "ReflexLimiter: Re-armed manual FPS limit with low-latency reset before push "
                "(device=%p nextIntervalUs=%u version=0x%08X)",
                lastDevice_, nextIntervalUs, params.version);
        } else {
            HookLogImportant(
                "ReflexLimiter: Manual FPS-limit low-latency reset failed before push "
                "(status=%d device=%p nextIntervalUs=%u version=0x%08X)",
                status, lastDevice_, nextIntervalUs, params.version);
        }
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

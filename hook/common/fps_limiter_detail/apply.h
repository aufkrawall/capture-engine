#pragma once

// Out-of-line FpsLimiter member definitions. Included by fps_limiter.h
// after the class body; kept inline so the hot path is unchanged.

#include "../fps_limiter.h"

inline void FpsLimiter::Apply(bool allowPostPresentReflexCadence, bool gateEveryPresent) {
    SharedMemoryLayout* shm = nullptr;
    if (dbgShm) {
        shm = dbgShm;
    } else if (ipc) {
        shm = ipc->GetSharedMem();
    }

    if (!shm) {
        g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
        applyTraceCount_++;
        if (applyTraceCount_ <= 3)
            TraceLog("Apply: no shm ipc=%p", (void*)ipc);
        return;
    }

    if (qpcFrequency == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;
    }
    LARGE_INTEGER nowQpc;
    QueryPerformanceCounter(&nowQpc);

    bool captureRequested = shm->runtimeState.captureRequested.load(std::memory_order_acquire);
    bool captureSyncEnabled = shm->fpsLimiter.GetCaptureSyncEnabled();
    int captureSyncMultiplier = shm->fpsLimiter.GetCaptureSyncMultiplier();
    bool generalEnabled = shm->fpsLimiter.GetGeneralEnabled();
    int generalFps = shm->fpsLimiter.GetGeneralFps();
    int captureFps = shm->fpsLimiter.GetCaptureFps();
    bool useVFR = shm->fpsLimiter.GetUseVFR();
    uint32_t captureSyncMode = shm->fpsLimiter.GetCaptureSyncLimiterMode();
    uint32_t generalMode = shm->fpsLimiter.GetGeneralLimiterMode();

    // Publish session ID once — use QPC ticks for better entropy
    if (!sessionIdPublished) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        uint32_t sid = GetCurrentProcessId() ^ GetTickCount() ^ static_cast<uint32_t>(qpc.QuadPart) ^
                       static_cast<uint32_t>(qpc.QuadPart >> 32);
        shm->fpsLimiter.hookSessionId.store(sid, std::memory_order_release);
        sessionIdPublished = true;
        HookLog("FPS Limiter: Published Session ID: %u", sid);
    }

    // Check if limiter should be active
    bool limiterActive = false;
    int targetFps = 0;
    bool usingCaptureSync = false;
    uint32_t configuredMode = LimiterModeValues::kAuto;

    if (captureRequested && captureSyncEnabled) {
        if (captureFps > 0 && captureSyncMultiplier >= 1 && captureSyncMultiplier <= 8) {
            limiterActive = true;
            targetFps = captureFps * captureSyncMultiplier;
            usingCaptureSync = true;
            configuredMode = captureSyncMode;
        }
    } else if (generalEnabled && generalFps > 0) {
        limiterActive = true;
        targetFps = generalFps;
        configuredMode = generalMode;
    }

    // VFR only makes capture-grid synchronization meaningless. A separately
    // configured general cap must remain active.
    if (useVFR && usingCaptureSync) {
        usingCaptureSync = false;
        if (generalEnabled && generalFps > 0) {
            limiterActive = true;
            targetFps = generalFps;
            configuredMode = generalMode;
        } else {
            limiterActive = false;
        }
    }

    if (!limiterActive) {
        // The cadence lock serializes this cleanup against a concurrent owner
        // that may still be inside its final wait; the branch is rare (config
        // transitions) and every reset in it is idempotent.
        std::lock_guard<std::mutex> cadenceLock(cadenceMutex_);
        isActivelyLimiting_.store(false, std::memory_order_relaxed);
        g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
        lastActualWaitUs_ = 0;
        loggedNativeFallback_ = false;
        ResetReflexNativePacingState();
        // Release timer resolution if we had it set
        {
            std::lock_guard<std::mutex> lock(timerStateMutex_);
            if (timerResolutionSet) {
                if (s_TimerResolutionRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    timeEndPeriod(1);
                }
                timerResolutionSet = false;
            }
        }
        // Reset local limiter state when inactive. The output-group ordinal
        // resets too so re-activation cannot inherit a partial group.
        {
            std::lock_guard<std::mutex> admissionLock(admissionMutex_);
            if (groupAdmission_.Reset()) {
                groupAdmissionResetCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (localTargetTime_ != 0) {
            localTargetTime_ = 0;
            localIntervalFps_ = 0;
            localIntervalRemainder_ = 0;
            localFrameCount_ = 0;
            localStatsIntervalStart_ = 0;
            localStatsFrameCount_ = 0;
            localStatsWaitedFrames_ = 0;
            localStatsLateFrames_ = 0;
            localStatsResetFrames_ = 0;
            localStatsSkippedGridSlots_ = 0;
            localStatsLateUsSum_ = 0;
            localStatsMaxLateUs_ = 0;
        }
        if (!loggedInactive_) {
            TraceLog("Apply: INACTIVE capReq=%d capSync=%d genEn=%d genFps=%d capFps=%d vfr=%d",
                     captureRequested ? 1 : 0, captureSyncEnabled ? 1 : 0, generalEnabled ? 1 : 0, generalFps,
                     captureFps, useVFR ? 1 : 0);
            HookLog(
                "FPS Limiter: Inactive (general_enabled=%d, generalFps=%d, "
                "captureSync=%d, captureRequested=%d, useVFR=%d)",
                generalEnabled ? 1 : 0, generalFps, captureSyncEnabled ? 1 : 0, captureRequested ? 1 : 0,
                useVFR ? 1 : 0);
            loggedInactive_ = true;
        }
        loggedActive_ = false;
        return;
    }
    loggedInactive_ = false;  // Reset so transitions back to inactive are logged

    // =====================================================================
    // FG-aware target adjustment and the output-group cadence grid.
    // When FG is active, the output frame rate is fgMultiplier x base rate.
    // Real final-boundary call sites pace whole output groups on an exact
    // rational grid: interval = QPC_frequency * cadenceScale / configured
    // output target. A 130 fps cap with 3x FG therefore paces 43.333... groups
    // per second without drift instead of flooring to 43 (129 output fps).
    // Non-boundary call sites keep the floored base-frame target and legacy
    // dedup semantics.
    // =====================================================================
    // One detection snapshot drives both the multiplier math and the driver
    // interval semantics below, so they cannot disagree about the FG runtime.
    const ce::fg_runtime::RuntimeMode fgRuntimeMode = g_FGCompat.GetRuntimeMode();
    bool fgActive = ce::fg_runtime::IsRuntimeFGActive(fgRuntimeMode);
    int fgMultiplier = fgActive ? g_FGCompat.GetFGMultiplier() : 1;
    // FG implies at least 2x output (1 real + 1 interpolated per base frame).
    // Pattern detection may determine higher (3x/4x for multi-frame gen),
    // but 2x is the safe minimum when FG is API-confirmed.
    if (fgActive && fgMultiplier < 2)
        fgMultiplier = 2;
    if (fgMultiplier > 4)
        fgMultiplier = 4;

    const bool scaleForFrameGeneration = ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(
        usingCaptureSync, shm->runtimeState.IsInjectVideoCaptureRequested());
    int effectiveTargetFps = ce::fps_limiter_policy::ResolveFrameGenerationBaseTarget(
        targetFps, fgActive, fgMultiplier, scaleForFrameGeneration);
    const int cadenceScale = ce::fps_limiter_policy::ResolveCadenceScaleMultiplier(
        fgActive, fgMultiplier, scaleForFrameGeneration && gateEveryPresent);
    const int cadenceTargetFps = (cadenceScale > 1) ? targetFps : effectiveTargetFps;
    // A driver-owned low-latency interval that already accounts for NVIDIA's
    // generated frames takes the OUTPUT rate, never the FG-divided base target
    // CE's own cadence paces on - see ResolveNativeDriverPacingTargetFps().
    const int nativeDriverTargetFps = ce::fps_limiter_policy::ResolveNativeDriverPacingTargetFps(
        targetFps, effectiveTargetFps, fgActive, fgMultiplier, scaleForFrameGeneration,
        ce::fps_limiter_policy::DriverLowLatencyIntervalCoversGeneratedFrames(fgRuntimeMode));

    // Admission epoch: any change of the configuration that shapes the output
    // groups (limiter activity, capture source, FG state, multiplier, cadence
    // grid) re-bases the ordinal BEFORE the next classification. That way the
    // first callback of a new configuration owns a clean cadence slot, and a
    // reset can never discard the group the current callback just opened.
    const uint32_t admissionKey = (limiterActive ? 1u : 0u) | (usingCaptureSync ? 2u : 0u) |
                                  (fgActive ? 4u : 0u) | (gateEveryPresent ? 8u : 0u) |
                                  (static_cast<uint32_t>(fgMultiplier & 0x7) << 4) |
                                  (static_cast<uint32_t>(std::clamp(cadenceTargetFps, 0, 0xFFFF)) << 7) |
                                  (static_cast<uint32_t>(std::clamp(cadenceScale, 0, 0x7)) << 23);
    {
        std::lock_guard<std::mutex> admissionLock(admissionMutex_);
        if (admissionKey != lastAdmissionKey_) {
            lastAdmissionKey_ = admissionKey;
            if (groupAdmission_.Reset()) {
                groupAdmissionResetCount_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Real final-boundary call sites classify every callback into a
        // multiplier-sized output group BEFORE any locking: exactly one
        // callback per group owns a cadence slot, the remaining FG
        // multiplier-1 callbacks are the generated outputs of that already
        // admitted group and pass without their own wait. Classification is a
        // pure ordinal under a short admission mutex (never held across a
        // wait), never a time window, so a next real group arriving inside the
        // legacy dedup window cannot be confused with generated spillover -
        // the escape that let Portal RTX admit whole unpaced groups and run
        // ~146 fps against a 130 cap.
        if (gateEveryPresent) {
            boundaryCallbackCount_.fetch_add(1, std::memory_order_relaxed);
            if (groupAdmission_.Classify(ce::fps_limiter_policy::ResolveOutputGroupAdmissionMultiplier(
                    fgActive, fgMultiplier)) ==
                ce::fps_limiter_policy::OutputGroupAdmission::Decision::kPassGeneratedSlot) {
                generatedSlotPassCount_.fetch_add(1, std::memory_order_relaxed);
                lastActualWaitUs_ = 0;
                return;
            }
            pacedGroupCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Strict grid pacing: every paced present must be gated by the cadence
    // grid, even when another present thread is currently inside the cadence.
    // Blocking on the cadence lock serializes concurrent present streams so
    // each group owner gets its own grid slot (one present per target
    // interval), instead of letting the later present skip the wait and reach
    // the swapchain unpaced. The legacy try-lock path keeps its non-blocking
    // behavior for call sites whose second call is a duplicate of the same
    // frame (DXVK Present+PresentEx).
    const bool strictGrid = gateEveryPresent;
    std::unique_lock<std::mutex> cadenceLock(cadenceMutex_, std::defer_lock);
    if (strictGrid) {
        LARGE_INTEGER lockStart, lockEnd, lockFreq;
        QueryPerformanceCounter(&lockStart);
        cadenceLock.lock();
        QueryPerformanceCounter(&lockEnd);
        QueryPerformanceFrequency(&lockFreq);
        const int64_t lockWaitUs = ((lockEnd.QuadPart - lockStart.QuadPart) * 1000000) / lockFreq.QuadPart;
        if (lockWaitUs > 500) {
            ++strictGridContendedWaits_;
            if (strictGridContendedWaits_ <= 3 || (strictGridContendedWaits_ % 600) == 0) {
                TraceLog("Apply: STRICT grid serialized concurrent present lockWaitUs=%lld count=%u", lockWaitUs,
                         strictGridContendedWaits_);
                HookLog("FPS Limiter: strict grid serialized a concurrent present (lockWaitUs=%lld, count=%u)",
                        lockWaitUs, strictGridContendedWaits_);
            }
        }
    } else if (!cadenceLock.try_lock()) {
        concurrentApplySkips_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Dedup guard: DXVK fires Present+PresentEx per render frame. The second call
    // arrives within ~1ms. Skip if called within 2ms of the previous return.
    // BUT: when the FPS limiter is active and ALLOW_TEARING disables vsync,
    // frames arrive very fast (1-2ms) and dedup would skip legitimate frames.
    // Only apply dedup when the limiter is NOT active.
    if (!isActivelyLimiting_.load(std::memory_order_relaxed) && !strictGrid) {
        const int64_t kDedupTicks = qpcFrequency / 500;  // 2ms
        if (lastApplyReturnQpc != 0 && (nowQpc.QuadPart - lastApplyReturnQpc) < kDedupTicks) {
            applyDedupCount_++;
            lastActualWaitUs_ = 0;
            return;
        }
    }

    // Periodically check for native low-latency APIs (Reflex).
    // Games may load these dynamically (e.g., user enables Reflex in settings).
    // Re-check every ~250ms (15 frames at 60fps) to catch late-loaded APIs
    // and in-game Reflex toggles faster.
    nativeApiRecheckCounter_++;
    if (nativeApiRecheckCounter_ >= 15) {
        nativeApiRecheckCounter_ = 0;
        bool reflexWasAvailable = g_ReflexLimiter.IsAvailable();
        g_ReflexLimiter.Init();
        bool reflexNowAvailable = g_ReflexLimiter.IsAvailable();
        if (!reflexWasAvailable && reflexNowAvailable) {
            HookLogImportant("FPS Limiter: Reflex became available (nvapi64.dll loaded late)");
        }
    }

    // =====================================================================
    // Resolve effective limiter mode (auto fallback chain)
    // =====================================================================
    const bool externalNativeCallbacks =
        allowPostPresentReflexCadence && nativePacingBackend_.context && nativePacingBackend_.isAvailable &&
        nativePacingBackend_.setTargetFps && nativePacingBackend_.sleep;
    const bool externalNativeGameActive =
        configuredMode == LimiterModeValues::kAuto && externalNativeCallbacks &&
        nativePacingBackend_.isGameActive && nativePacingBackend_.isGameActive(nativePacingBackend_.context);

    uint32_t effectiveMode = configuredMode;

    if (configuredMode == LimiterModeValues::kAuto) {
        // Priority: Reflex (NVIDIA, game-activated) → FG fallback → basic
        // Native mode requires the game to have activated the API, not just API availability
        const bool d3dReflexAvail = g_ReflexLimiter.IsAvailable();
        const bool d3dReflexActive = g_ReflexLimiter.IsGameActivated();
        const bool reflexAvail = d3dReflexAvail || externalNativeGameActive;
        const bool reflexActive = (d3dReflexAvail && d3dReflexActive) || externalNativeGameActive;

        if (reflexAvail && reflexActive) {
            effectiveMode = LimiterModeValues::kNative;
        } else if (fgActive) {
            // No native low-latency API but FG active → FG-compatible fallback
            effectiveMode = LimiterModeValues::kFGFallback;
        } else {
            effectiveMode = LimiterModeValues::kBasic;
        }

        // Log auto mode decision (only on changes or first activation)
        static uint32_t lastLoggedAutoMode = 0;
        if (effectiveMode != lastLoggedAutoMode || !loggedActive_) {
            lastLoggedAutoMode = effectiveMode;
            const char* reason = "";
            if (effectiveMode == LimiterModeValues::kNative)
                reason = "reflex available + game activated";
            else if (effectiveMode == LimiterModeValues::kFGFallback)
                reason = "frame generation active";
            else if (effectiveMode == LimiterModeValues::kBasic)
                reason = "no native API active";

            HookLog("FPS Limiter [AUTO]: reflex=%s(%s) fg=%s → selected=%s (%s)",
                    reflexAvail ? "avail" : "n/a", reflexActive ? "active" : "inactive", fgActive ? "yes" : "no",
                    (effectiveMode == LimiterModeValues::kNative)       ? "reflex"
                    : (effectiveMode == LimiterModeValues::kFGFallback) ? "fg_fallback"
                                                                        : "basic",
                    reason);
        }
    }

    // Validate: native modes require the respective DLL to be available.
    // In explicit mode, availability is sufficient (user override).
    // In auto mode, we already checked game activation above.
    // Fall back gracefully if the selected mode is not supported on this system.
    const bool externalNativeAvailable =
        effectiveMode == LimiterModeValues::kNative && externalNativeCallbacks &&
        (externalNativeGameActive || nativePacingBackend_.isAvailable(nativePacingBackend_.context));
    if (effectiveMode == LimiterModeValues::kNative && !g_ReflexLimiter.IsAvailable() &&
        !externalNativeAvailable) {
        effectiveMode = fgActive ? LimiterModeValues::kFGFallback : LimiterModeValues::kBasic;
    }
    if (effectiveMode != LimiterModeValues::kNative) {
        loggedNativeFallback_ = false;
        reflexSleepBaselineCount_ = 0;
        reflexRecentPresentGap_ = false;
    }

    const bool explicitReflexMode = configuredMode == LimiterModeValues::kNative;
    g_ReflexLimiter.SetManualLimiterConfiguredOrActive(limiterActive && explicitReflexMode);

    // Log mode transitions - always log on first activation to confirm mode
    if (!loggedActive_ || lastTargetFps_ != effectiveTargetFps || lastUsedCaptureSync_ != usingCaptureSync ||
        lastEffectiveMode_ != effectiveMode || lastFGActive_ != fgActive || lastFGMultiplier_ != fgMultiplier ||
        lastCadenceTargetFps_ != cadenceTargetFps || lastCadenceScale_ != cadenceScale ||
        lastNativeDriverTargetFps_ != nativeDriverTargetFps) {
        // Reset pacing cadence immediately when FPS or mode changes so hot
        // config reloads apply on the next frame instead of riding stale state.
        // The output-group ordinal was already re-based by the admission epoch
        // check above, before this callback classified.
        localTargetTime_ = 0;
        localIntervalFps_ = 0;
        localIntervalRemainder_ = 0;
        localFrameCount_ = 0;
        localStatsIntervalStart_ = 0;
        localStatsFrameCount_ = 0;
        localStatsWaitedFrames_ = 0;
        localStatsLateFrames_ = 0;
        localStatsResetFrames_ = 0;
        localStatsSkippedGridSlots_ = 0;
        localStatsLateUsSum_ = 0;
        localStatsMaxLateUs_ = 0;
        lastApplyEntryQpc_ = 0;
        applyInterFrameSum_ = 0;
        applyInterFrameCount_ = 0;
        reflexPostPresentCadencePending_ = false;
        reflexPostPresentCaptureSync_ = false;
        reflexPostPresentSkipSleep_ = false;
        reflexPostPresentArmedLogged_ = false;
        externalNativePostPresentPending_ = false;

        const char* modeStr = "basic";
        if (effectiveMode == LimiterModeValues::kFGFallback)
            modeStr = "fg_fallback";
        else if (effectiveMode == LimiterModeValues::kNative)
            modeStr = "reflex";
        else if (effectiveMode == LimiterModeValues::kAuto)
            modeStr = "auto";

        // Check if native API is actually available (not just selected)
        const char* availNote = "";
        if (effectiveMode == LimiterModeValues::kNative && !g_ReflexLimiter.IsAvailable() &&
            !externalNativeAvailable)
            availNote = " [API UNAVAILABLE - will fallback]";

        TraceLog("Apply: ACTIVE sync=%s limiter=%s target=%d effective=%d group=%d/%d fg=%d fgMult=%d driver=%d",
                 usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, cadenceTargetFps,
                 cadenceScale, fgActive ? 1 : 0, fgMultiplier, nativeDriverTargetFps);
        HookLog(
            "FPS Limiter: Active (sync=%s, limiter=%s, target=%d, effective=%d, group=%d/%d, fg=%d/%dx, "
            "driver=%d, capReq=%d)%s",
            usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, cadenceTargetFps,
            cadenceScale, fgActive ? 1 : 0, fgMultiplier, nativeDriverTargetFps, captureRequested ? 1 : 0,
            availNote);
        loggedActive_ = true;
        lastTargetFps_ = effectiveTargetFps;
        lastUsedCaptureSync_ = usingCaptureSync;
        lastEffectiveMode_ = effectiveMode;
        lastFGActive_ = fgActive;
        lastFGMultiplier_ = fgMultiplier;
        lastCadenceTargetFps_ = cadenceTargetFps;
        lastCadenceScale_ = cadenceScale;
        lastNativeDriverTargetFps_ = nativeDriverTargetFps;
    }

    // =====================================================================
    // Native (Reflex) mode: delegate pacing to the driver's pipeline
    // =====================================================================
    if (effectiveMode == LimiterModeValues::kNative && externalNativeAvailable) {
        if (nativePacingBackend_.setTargetFps(nativePacingBackend_.context, nativeDriverTargetFps)) {
            externalNativePostPresentPending_ = true;
            externalNativeTargetFps_ = nativeDriverTargetFps;
            reflexLimiterActive_ = true;
            loggedNativeFallback_ = false;
            isActivelyLimiting_.store(false, std::memory_order_relaxed);
            lastActualWaitUs_ = 0;
            if (!externalNativeLoggedSuccess_) {
                const char* backendName = nativePacingBackend_.name ? nativePacingBackend_.name : "API-native";
                TraceLog("Apply: native backend armed name=%s target=%d", backendName, nativeDriverTargetFps);
                HookLog("FPS Limiter: %s armed (target=%d fps, native driver pacing handoff)", backendName,
                        nativeDriverTargetFps);
                externalNativeLoggedSuccess_ = true;
            }
            LARGE_INTEGER retQpc;
            QueryPerformanceCounter(&retQpc);
            lastApplyReturnQpc = retQpc.QuadPart;
            return;
        }
        // Do not leave an older persistent driver interval active while the
        // rational-timer path takes over after a native retarget failure.
        if (nativePacingBackend_.clear) {
            nativePacingBackend_.clear(nativePacingBackend_.context);
        }
        externalNativePostPresentPending_ = false;
        externalNativeTargetFps_ = 0;
        externalNativeLoggedSuccess_ = false;
    }

    if (effectiveMode == LimiterModeValues::kNative && g_ReflexLimiter.IsAvailable()) {
        // Lazy init: provide device from HookContext if not yet set
        if (!reflexDeviceProvided_) {
            auto* ctx = ce::GetHookContext();
            if (ctx) {
                IUnknown* dev = nullptr;
                if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX11) {
                    dev = static_cast<IUnknown*>(ctx->graphicsData.dx11.device);
                } else if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                    dev = static_cast<IUnknown*>(ctx->graphicsData.dx12.device);
                }
                if (dev) {
                    g_ReflexLimiter.SetDevice(dev);
                    reflexDeviceProvided_ = true;
                }
            }
        }

        g_ReflexLimiter.SetTargetFps(nativeDriverTargetFps);
        const bool gameSleepObserved = g_ReflexLimiter.HasObservedGameSleep();
        uint32_t reflexSleepGraceMs = 50;
        if (effectiveTargetFps > 0) {
            uint32_t frameTimeMs = static_cast<uint32_t>(1000 / effectiveTargetFps);
            if (frameTimeMs == 0) {
                frameTimeMs = 1;
            }
            const uint32_t dynamicGraceMs = frameTimeMs * 3;
            if (dynamicGraceMs > reflexSleepGraceMs) {
                reflexSleepGraceMs = dynamicGraceMs;
            }
            if (reflexSleepGraceMs > 250) {
                reflexSleepGraceMs = 250;
            }
        }
        const bool gameSleepRecent = g_ReflexLimiter.HasRecentGameSleep(reflexSleepGraceMs);
        const bool gameActivated = g_ReflexLimiter.IsGameActivated();
        const uint32_t gameSleepCount = g_ReflexLimiter.GetGameSleepCount();
        const bool recentPresentGap = HasRecentLargePresentGap(500);
        if (recentPresentGap && !reflexRecentPresentGap_) {
            reflexSleepBaselineCount_ = gameSleepCount;
        }
        reflexRecentPresentGap_ = recentPresentGap;
        const uint32_t freshSleepCount =
            (gameSleepCount > reflexSleepBaselineCount_) ? (gameSleepCount - reflexSleepBaselineCount_) : 0;
        const auto reflexDecision = ce::fps_limiter_policy::ResolveReflexPacingDecision(
            explicitReflexMode, gameSleepObserved, gameSleepRecent, freshSleepCount, recentPresentGap);
        const bool reflexHandoffReady = reflexDecision.useGameSleepHandoff;
        const bool reflexPushOk = g_ReflexLimiter.PushFpsLimit();
        const bool reflexDeviceReady = g_ReflexLimiter.HasDevice();

        if (reflexHandoffReady) {
            reflexLimiterActive_ = true;
            loggedNativeFallback_ = false;
            g_ReflexLimiter.ConfigureHybridPacing(qpcFrequency, cadenceTargetFps, cadenceScale);

            if (!reflexNativeSleepActive_) {
                reflexNativeSleepActive_ = true;
                TraceLog("Apply: REFLEX native resume");
                HookLog("FPS Limiter: Reflex Sleep resumed; returning to native pacing");
            }

            if (!reflexLoggedSuccess_) {
                TraceLog("Apply: REFLEX hybrid target=%d gameSleep=%d gameActive=%d fresh=%u", effectiveTargetFps,
                         gameSleepObserved ? 1 : 0, gameActivated ? 1 : 0, freshSleepCount);
                HookLog(
                    "FPS Limiter: Reflex hybrid active (target=%d fps, native low-latency + local pacing, "
                    "gameSleep=%d, gameActive=%d, freshSleep=%u)",
                    effectiveTargetFps, gameSleepObserved ? 1 : 0, gameActivated ? 1 : 0, freshSleepCount);
                reflexLoggedSuccess_ = true;
            }

            isActivelyLimiting_.store(false, std::memory_order_relaxed);
            lastActualWaitUs_ = 0;
            LARGE_INTEGER retQpc;
            QueryPerformanceCounter(&retQpc);
            lastApplyReturnQpc = retQpc.QuadPart;
            return;
        } else {
            g_ReflexLimiter.DisableHybridPacing();
            const bool ceOwnedSleepCandidate = reflexDecision.useExplicitLocalCadence;
            bool ceOwnedSleepOk = false;
            int64_t ceOwnedSleepUs = 0;
            if (ceOwnedSleepCandidate) {
                EnsureTimerResolution();
                isActivelyLimiting_.store(true, std::memory_order_relaxed);
                if (ce::fps_limiter_policy::ShouldRunExplicitReflexCadencePostPresent(
                        reflexDecision, allowPostPresentReflexCadence)) {
                    reflexPostPresentCadencePending_ = true;
                    reflexPostPresentTargetFps_ = effectiveTargetFps;
                    reflexPostPresentCaptureSync_ = usingCaptureSync;
                    reflexPostPresentPushOk_ = reflexPushOk;
                    reflexPostPresentDeviceReady_ = reflexDeviceReady;
                    reflexPostPresentRecentGap_ = recentPresentGap;
                    reflexPostPresentSkipSleep_ = reflexPushOk;
                    reflexLimiterActive_ = true;
                    reflexNativeSleepActive_ = false;
                    loggedNativeFallback_ = false;
                    lastActualWaitUs_ = 0;
                    if (!reflexPostPresentArmedLogged_) {
                        TraceLog(
                            "Apply: REFLEX post-present armed target=%d push=%d device=%d gap=%d "
                            "skipSleep=%d",
                            effectiveTargetFps, reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0,
                            recentPresentGap ? 1 : 0, reflexPostPresentSkipSleep_ ? 1 : 0);
                        HookLog(
                            "FPS Limiter: Reflex explicit mode armed for post-present pacing "
                            "(target=%d fps, push=%d, device=%d, skipSleep=%d)",
                            effectiveTargetFps, reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0,
                            reflexPostPresentSkipSleep_ ? 1 : 0);
                        reflexPostPresentArmedLogged_ = true;
                    }
                    LARGE_INTEGER retQpc;
                    QueryPerformanceCounter(&retQpc);
                    lastApplyReturnQpc = retQpc.QuadPart;
                    return;
                }
                const auto cadence = RunLocalCadence(cadenceTargetFps, cadenceScale, usingCaptureSync);

                LARGE_INTEGER sleepStart;
                LARGE_INTEGER sleepEnd;
                QueryPerformanceCounter(&sleepStart);
                ceOwnedSleepOk = g_ReflexLimiter.Sleep();
                QueryPerformanceCounter(&sleepEnd);
                ceOwnedSleepUs = ((sleepEnd.QuadPart - sleepStart.QuadPart) * 1000000) / qpcFrequency;

                reflexLimiterActive_ = true;
                reflexNativeSleepActive_ = false;
                loggedNativeFallback_ = false;
                lastActualWaitUs_ = cadence.actualWaitUs + ceOwnedSleepUs;
                if (!reflexLoggedSuccess_) {
                    TraceLog(
                        "Apply: REFLEX local cadence target=%d waitUs=%lld sleepUs=%lld sleepOk=%d push=%d "
                        "device=%d gap=%d",
                        effectiveTargetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                        reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0, recentPresentGap ? 1 : 0);
                    HookLog(
                        "FPS Limiter: Reflex explicit mode active (target=%d fps, local low-latency cadence + "
                        "CE-owned NvAPI Sleep, wait=%lldus, sleep=%lldus, sleepOk=%d, device=%d)",
                        effectiveTargetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                        reflexDeviceReady ? 1 : 0);
                    reflexLoggedSuccess_ = true;
                }
                if (cadence.emitStats) {
                    TraceLog("Apply: REFLEX local stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d",
                             cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps,
                             effectiveTargetFps);
                    HookLog(
                        "FPS Limiter: Reflex local cadence (%u frames): lastWait=%lldus avgFps=%.1f "
                        "instFps=%.1f target=%d sleepOk=%d",
                        cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps,
                        effectiveTargetFps, ceOwnedSleepOk ? 1 : 0);
                }
                LARGE_INTEGER retQpc;
                QueryPerformanceCounter(&retQpc);
                lastApplyReturnQpc = retQpc.QuadPart;
                return;
            }

            if (reflexNativeSleepActive_) {
                reflexSleepBaselineCount_ = gameSleepCount;
                reflexNativeSleepActive_ = false;
                TraceLog("Apply: REFLEX sleep stalled graceMs=%u", reflexSleepGraceMs);
                HookLog("FPS Limiter: Reflex Sleep paused; using timer fallback pacing");
            }
            reflexNativeSleepActive_ = false;
            reflexLimiterActive_ = false;
            if (!loggedNativeFallback_) {
                TraceLog(
                    "Apply: REFLEX timer fallback gameActive=%d gameSleep=%d push=%d sleepCount=%u fresh=%u "
                    "recent=%d gap=%d device=%d inlineHooks=%d",
                    gameActivated ? 1 : 0, gameSleepObserved ? 1 : 0, reflexPushOk ? 1 : 0, gameSleepCount,
                    freshSleepCount, gameSleepRecent ? 1 : 0, recentPresentGap ? 1 : 0, reflexDeviceReady ? 1 : 0,
                    g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                if (recentPresentGap) {
                    HookLog(
                        "FPS Limiter: Recent Present gap detected during Reflex activation; holding timer fallback "
                        "until pacing restabilizes");
                } else if (gameSleepObserved && freshSleepCount < 3) {
                    HookLog(
                        "FPS Limiter: Reflex Sleep observed but waiting for a fresh stable Sleep streak; using "
                        "timer fallback");
                } else if (reflexPushOk) {
                    HookLog(
                        "FPS Limiter: Reflex armed but native Sleep cadence is not stable yet; using timer "
                        "fallback (gameSleep=%d sleepRecent=%d sleepCount=%u fresh=%u inlineHooks=%d)",
                        gameSleepObserved ? 1 : 0, gameSleepRecent ? 1 : 0, gameSleepCount, freshSleepCount,
                        g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                } else {
                    HookLog("FPS Limiter: Reflex native mode unavailable at runtime; using timer fallback");
                }
                loggedNativeFallback_ = true;
            } else {
                static uint32_t s_fallbackDiagCounter = 0;
                s_fallbackDiagCounter++;
                if (s_fallbackDiagCounter % 600 == 0) {
                    HookLog(
                        "FPS Limiter: Reflex timer fallback diagnostic (frame %u): gameActive=%d sleepObserved=%d "
                        "sleepRecent=%d sleepCount=%u fresh=%u push=%d gap=%d inlineHooks=%d",
                        s_fallbackDiagCounter, gameActivated ? 1 : 0, gameSleepObserved ? 1 : 0,
                        gameSleepRecent ? 1 : 0, gameSleepCount, freshSleepCount, reflexPushOk ? 1 : 0,
                        recentPresentGap ? 1 : 0, g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                }
            }
        }
    } else if (reflexLimiterActive_ || reflexNativeSleepActive_ || g_ReflexLimiter.GetTargetIntervalUs() != 0) {
        // Clear any stale Reflex override even if native pacing never fully
        // handed off. Otherwise later game-managed Reflex calls can inherit
        // our old interval after FG turns off.
        ResetReflexNativePacingState();
    } else if (!explicitReflexMode) {
        g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
    }

    // =====================================================================
    // Timer-based limiting (basic or FG fallback). Both use the same SmartWait
    // mechanism and the common final-output FG adjustment above.
    // =====================================================================

    isActivelyLimiting_.store(true, std::memory_order_relaxed);

    if (effectiveTargetFps <= 0)
        effectiveTargetFps = 60;

    const bool localCadenceFirstFrame = localTargetTime_ == 0;
    if (!usingCaptureSync && !localCadenceFirstFrame && lastApplyReturnQpc != 0 && !strictGrid) {
        LARGE_INTEGER activeDedupQpc;
        QueryPerformanceCounter(&activeDedupQpc);
        int64_t activeDedupTicks = qpcFrequency / 500;  // 2ms maximum duplicate window.
        int64_t intervalTicks = qpcFrequency / effectiveTargetFps;
        if (intervalTicks < 1) {
            intervalTicks = 1;
        }
        const int64_t intervalBoundTicks = intervalTicks / 3;
        if (intervalBoundTicks > 0 && intervalBoundTicks < activeDedupTicks) {
            activeDedupTicks = intervalBoundTicks;
        }
        const int64_t minDedupTicks = qpcFrequency / 2000;  // 0.5ms minimum for timer jitter.
        if (activeDedupTicks < minDedupTicks) {
            activeDedupTicks = minDedupTicks;
        }

        const int64_t sinceReturnTicks = activeDedupQpc.QuadPart - lastApplyReturnQpc;
        if (sinceReturnTicks >= 0 && sinceReturnTicks < activeDedupTicks) {
            applyActiveDedupCount_++;
            lastActualWaitUs_ = 0;
            const int64_t sinceReturnUs = (sinceReturnTicks * 1000000) / qpcFrequency;
            const int64_t activeDedupUs = (activeDedupTicks * 1000000) / qpcFrequency;
            if (applyActiveDedupCount_ <= 12 || (applyActiveDedupCount_ % 600) == 0) {
                TraceLog(
                    "Apply: ACTIVE dedup sync=%s mode=%u configured=%u target=%d effective=%d "
                    "sinceReturnUs=%lld thresholdUs=%lld activeDedup=%u inactiveDedup=%u",
                    usingCaptureSync ? "capture" : "general", effectiveMode, configuredMode, targetFps,
                    effectiveTargetFps, sinceReturnUs, activeDedupUs, applyActiveDedupCount_, applyDedupCount_);
            }
            return;
        }
    }

    // Timer fallback/basic/FG fallback pacing is hook-local.  Waiting for
    // the helper process here is fragile because per-game config can enable
    // the limiter after startup; an unanswered event used to cost one full
    // timeout per frame before local fallback ran.
    const auto cadence = RunLocalCadence(cadenceTargetFps, cadenceScale, usingCaptureSync);
    if (localCadenceFirstFrame) {
        TraceLog(
            "Apply: LOCAL timer start sync=%s mode=%u configured=%u target=%d effective=%d group=%d/%d "
            "events=%d/%d firstWaitUs=%lld firstLateUs=%lld",
            usingCaptureSync ? "capture" : "general", effectiveMode, configuredMode, targetFps, effectiveTargetFps,
            cadenceTargetFps, cadenceScale, releaseEvent ? 1 : 0, requestEvent ? 1 : 0, cadence.scheduledWaitUs,
            cadence.lateUs);
        HookLog("FPS Limiter: Local timer cadence active (sync=%s, mode=%u, target=%d, effective=%d)",
                usingCaptureSync ? "capture" : "general", effectiveMode, targetFps, effectiveTargetFps);
    }
    if (cadence.emitStats) {
        TraceLog(
            "Apply: LOCAL timer stats frames=%u scheduledWaitUs=%lld actualWaitUs=%lld lateUs=%lld "
            "avgFps=%.1f instFps=%.1f target=%d waited=%u late=%u avgLateUs=%lld maxLateUs=%lld "
            "resets=%u phaseSkipped=%u dedup=%u activeDedup=%u",
            cadence.frameCount, cadence.scheduledWaitUs, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps,
            cadence.instantFps, effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames,
            cadence.statsAvgLateUs, cadence.statsMaxLateUs, cadence.statsResetFrames,
            cadence.statsSkippedGridSlots, applyDedupCount_, applyActiveDedupCount_);
        HookLog(
            "FPS Limiter: Local timer stats (%u frames): lastWait=%lldus late=%lldus avgFps=%.1f "
            "instFps=%.1f target=%d waited=%u lateFrames=%u resets=%u phaseSkipped=%u activeDedup=%u",
            cadence.frameCount, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps, cadence.instantFps,
            effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames, cadence.statsResetFrames,
            cadence.statsSkippedGridSlots, applyActiveDedupCount_);
        if (cadence.statsBoundaryCallbacks > 0 || cadence.statsGeneratedPasses > 0 ||
            cadence.statsConcurrentSkips > 0 || cadence.statsGroupResets > 0) {
            // Rate-limited by the 120-frame stats window. A nonzero concurrent
            // skip while a real-boundary limiter is active is an invariant
            // violation: boundary owners block on the cadence lock and
            // generated slots never touch it, so this must not increase.
            HookLog(
                "FPS Limiter: boundary admission stats: boundaryCallbacks=%u pacedGroups=%u generatedPasses=%u "
                "groupResets=%u concurrentSkips=%u%s",
                cadence.statsBoundaryCallbacks, cadence.statsPacedGroups, cadence.statsGeneratedPasses,
                cadence.statsGroupResets, cadence.statsConcurrentSkips,
                cadence.statsConcurrentSkips > 0 ? " [INVARIANT VIOLATION: unpaced lock-contention escape]" : "");
        }
    }

    // Record time Apply() returned so sequential duplicate presents
    // (e.g. DXVK Present+PresentEx) are deduped on the next call.
    QueryPerformanceCounter(&nowQpc);
    lastApplyReturnQpc = nowQpc.QuadPart;
}

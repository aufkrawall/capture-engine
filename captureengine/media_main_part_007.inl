            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", config.avSyncConfidence.c_str(),
            config.avSyncReason.c_str());
    }

    const auto qpcToUs = [&](int64_t qpcDelta) -> int64_t {
        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
    };
    const auto observeCaptureSyncPhaseSource = [&](const char* path,
                                                    ce::capture_policy::CfrCadencePhaseLockState& state,
                                                    int64_t sourceTimestampQpc) {
        if (!captureSyncPhaseLockEnabled) {
            return;
        }
        const uint64_t releasesBefore = state.releases;
        ce::capture_policy::ObserveCfrCaptureSyncSourceTimestamp(state, sourceTimestampQpc,
                                                                 captureSyncSourceIntervalTicks);
        if (state.releases != releasesBefore) {
            LogInfo(
                "[CFR PhaseLock] path=%s state=released reason=variable_source stable=%u unstable=%u "
                "multiplier=%u releases=%llu",
                path, state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.releases));
        }
    };
    const auto applyCaptureSyncPhaseTarget = [&](const char* path,
                                                 ce::capture_policy::CfrCadencePhaseLockState& state,
                                                 int64_t baseTargetQpc, int64_t sourceReferenceQpc) -> int64_t {
        const uint64_t acquisitionsBefore = state.acquisitions;
        const uint64_t releasesBefore = state.releases;
        const uint64_t rephasesBefore = state.rephases;
        const int64_t adjustedTargetQpc = ce::capture_policy::ApplyCfrCaptureSyncPhaseLock(
            state, baseTargetQpc, sourceReferenceQpc, captureSyncSourceIntervalTicks,
            captureSyncPhaseLockEnabled);
        if (state.acquisitions != acquisitionsBefore || state.releases != releasesBefore ||
            state.rephases != rephasesBefore) {
            const char* transition = state.acquisitions != acquisitionsBefore
                                         ? "acquired"
                                         : (state.rephases != rephasesBefore ? "rephased" : "released");
            LogInfo(
                "[CFR PhaseLock] path=%s state=%s offset=%lldus stable=%u unstable=%u multiplier=%u "
                "transitions=%llu/%llu/%llu",
                path, transition, static_cast<long long>(qpcToUs(state.lockedPhaseQpc)),
                state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.acquisitions),
                static_cast<unsigned long long>(state.rephases), static_cast<unsigned long long>(state.releases));
        }
        return adjustedTargetQpc;
    };
    const auto getWgcRawSelectionTimestamp = [](const QueuedFrame& frame) -> int64_t {
        return frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
    };
    const auto getWgcEffectiveContentDelayQpc = [&]() -> int64_t {
        return avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
    };
    const auto isWgcEffectiveContentDelayActive = [&]() -> bool { return getWgcEffectiveContentDelayQpc() > 0; };
    const auto getWgcSmoothnessOutputFps = [&]() -> uint32_t {
        return config.video.fps > 0 ? static_cast<uint32_t>(config.video.fps) : 0u;
    };
    const auto shouldUseWgcSmoothnessBaseConfig = [&]() -> bool {
        // Pass wgcSmoothnessDelayDesired (audio-latency delay OR configured floor) so the buffer
        // arms for video-only / low-confidence captures too, not only when audio latency is present.
        return ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                                wgcSmoothnessDelayDesired, targetIntervalTicks);
    };
    const auto getWgcSmoothnessDesiredFramesForConfig = [&]() -> uint32_t {
        if (!shouldUseWgcSmoothnessBaseConfig()) {
            return 0u;
        }
        return ce::capture_policy::GetWgcSmoothnessDesiredFrames(getWgcSmoothnessOutputFps(),
                                                                 config.wgcSmoothnessBufferMaxMs);
    };
    const auto getWgcSmoothnessRetainedFramesBudget = [&]() -> uint32_t {
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        return (g_WgcCap && desiredFrames > 0) ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
    };
    const auto isWgcSmoothnessSourceRateEligibleNow = [&]() -> bool {
        if (!ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                              wgcSmoothnessDelayDesired, targetIntervalTicks)) {
            return false;
        }
        const uint32_t inputMin250Fps = g_WgcCap ? g_WgcCap->GetInputMin250Fps() : wgcRecentInputMin250Fps;
        const uint32_t inputMin500Fps = g_WgcCap ? g_WgcCap->GetInputMin500Fps() : wgcRecentInputMin500Fps;
        return ce::capture_policy::ShouldArmWgcSmoothnessBufferForSourceRate(getWgcSmoothnessOutputFps(),
                                                                             inputMin250Fps, inputMin500Fps);
    };
    const auto shouldAttemptWgcStartupSmoothnessBufferNow = [&]() -> bool {
        return ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
            config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired, targetIntervalTicks,
            getWgcSmoothnessRetainedFramesBudget());
    };
    const auto getWgcStartupSmoothnessTargetDelayQpc = [&](bool attempted) -> int64_t {
        return attempted
                   ? ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                         getWgcSmoothnessRetainedFramesBudget(), targetIntervalTicks, getWgcSmoothnessOutputFps(),
                         config.wgcSmoothnessBufferMaxMs)
                   : 0;
    };
    const auto getWgcSmoothnessBufferReason = [&]() -> const char* {
        if (!config.wgcSmoothnessBufferEnabled) {
            return "disabled";
        }
        if (config.video.useVFR) {
            return "vfr";
        }
        if (!wgcSmoothnessDelayDesired) {
            return "sync_delay_inactive";
        }
        if (targetIntervalTicks <= 0) {
            return "invalid_target";
        }
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        if (desiredFrames == 0) {
            return "target_zero";
        }
        const uint32_t retainedFrames = getWgcSmoothnessRetainedFramesBudget();
        if (retainedFrames == 0) {
            return "vram_budget_exhausted";
        }
        if (!isWgcSmoothnessSourceRateEligibleNow()) {
            return "startup_attempt_source_rate_low";
        }
        return "startup_attempt";
    };
    const auto getWgcDelayReservoirLowWaterFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirLowWaterFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirTargetFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirTargetFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirLowWaterFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirLowWaterFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcDelayReservoirTargetFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirTargetFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcRetainedFrameCap = [&]() -> uint32_t {
        if (!g_WgcCap) {
            return 0u;
        }
        return g_WgcCap->GetSmoothnessRetainedFrameCap();
    };
    const auto updateWgcIngressPressure = [&](const char*) {
        if (!g_WgcCap) {
            return;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t retainedFrames = SaturatingToUint32(
            static_cast<uint64_t>(bufferedWgcFrames.size()) +
            static_cast<uint64_t>(std::min<size_t>(g_FrameQueue.Size(), static_cast<size_t>(UINT32_MAX))));
        const uint32_t lowWaterFrames = getWgcDelayReservoirLowWaterFrames();
        const bool delayReservoirActive = lowWaterFrames > 0;
        const bool recovering = delayReservoirActive && (wgcLowSourceModeActive || wgcLiveRecoveryModeActive ||
                                                         (wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64()) ||
                                                         retainedFrames <= lowWaterFrames);
        const bool uniformPlayoutOwnsSurplus =
            isWgcEffectiveContentDelayActive() && config.wgcActiveDelayUniformCadence;
        g_WgcCap->SetRetainedFramePressure(retainedFrames, retainedCap, lowWaterFrames, recovering,
                                           uniformPlayoutOwnsSurplus);
    };
    const auto trimBufferedWgcToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 3 || nowTick - wgcRetainedCapTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir capped: trimmedNewest=%u reason=%s retained=%zu cap=%u "
                    "reservedFree=%u poolSlots=%u (pool safety protected; surplus source frames become planned CFR "
                    "decimation/repeats, audio/PTS unchanged)",
                    trimmed, reason ? reason : "unknown", bufferedWgcFrames.size(), retainedCap,
                    g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                    g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount() : 0u);
                wgcRetainedCapTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcStartupWaitToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            LogInfo(
                "[WGC CFR] startup wait retained reservoir capped: trimmedOldest=%u reason=%s retained=%zu cap=%u "
                "reservedFree=%u poolSlots=%u",
                trimmed, reason ? reason : "startup-wait", bufferedWgcFrames.size(), retainedCap,
                g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                g_WgcCap ? g_WgcCap->GetTexturePoolSlotCount() : 0u);
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcForPoolPressure = [&](const char* reason) {
        if (!g_WgcCap) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t reservedFreeSlots = g_WgcCap->GetSmoothnessReservedFreeSlotCount();
        const uint32_t currentFreeSlots = g_WgcCap->GetPoolSlotFreeCurrentCount();
        const uint32_t trimTarget = ce::capture_policy::GetWgcPoolPressureRetainedTrimTarget(
            currentFreeSlots, reservedFreeSlots, getWgcDelayReservoirTargetFrames(), retainedCap);
        if (trimTarget == 0 || trimTarget >= retainedCap || bufferedWgcFrames.size() <= trimTarget) {
            updateWgcIngressPressure(reason);
            return 0u;
        }

        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > trimTarget) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            wgcPoolPressureTrimTotal += trimmed;
            wgcPoolPressureTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 2 || nowTick - wgcPoolPressureTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir pressure trim: trimmedNewest=%u reason=%s retained=%zu "
                    "target=%u cap=%u free=%u reservedFree=%u poolSlots=%u delayTarget=%u "
                    "(preserved active-delay target; released surplus copy-pool leases, audio/PTS unchanged)",
                    trimmed, reason ? reason : "pool-pressure", bufferedWgcFrames.size(), trimTarget, retainedCap,
                    currentFreeSlots, reservedFreeSlots, g_WgcCap->GetTexturePoolSlotCount(),
                    getWgcDelayReservoirTargetFrames());
                wgcPoolPressureTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto recordWgcDelayResidualSample =
        [&](int64_t signedResidualUs, uint64_t& samples, uint64_t& absAccumUs, int64_t& signedAccumUs,
            uint32_t& absMaxUs, uint32_t& lateMaxUs, uint32_t& earlyMaxUs, std::array<uint32_t, 256>& histogram,
            uint64_t& windowSamples, uint64_t& windowAbsAccumUs, int64_t& windowSignedAccumUs, uint32_t& windowAbsMaxUs,
            uint32_t& windowLateMaxUs, std::array<uint32_t, 256>& windowHistogram) {
            const uint32_t absResidualUs =
                SaturatingToUint32(static_cast<uint64_t>(signedResidualUs >= 0 ? signedResidualUs : -signedResidualUs));
            ++samples;
            absAccumUs += absResidualUs;
            signedAccumUs += signedResidualUs;
            absMaxUs = std::max(absMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                lateMaxUs = std::max(lateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            } else {
                earlyMaxUs = std::max(earlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedResidualUs)));
            }
            const size_t histogramBin = std::min<size_t>(histogram.size() - 1, absResidualUs / 1000u);
            ++histogram[histogramBin];
            ++windowSamples;
            windowAbsAccumUs += absResidualUs;
            windowSignedAccumUs += signedResidualUs;
            windowAbsMaxUs = std::max(windowAbsMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                windowLateMaxUs =
                    std::max(windowLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            }
            ++windowHistogram[histogramBin];
        };
    const auto recordWgcDelayRealization = [&](int64_t predictedSignedResidualUs, int64_t rawSignedResidualUs) -> bool {
        if (!isWgcEffectiveContentDelayActive() || qpcFreq.QuadPart <= 0) {
            return false;
        }
        const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
        const int64_t realizedDelaySignedUs = requestedDelayUs - predictedSignedResidualUs;
        const uint32_t realizedDelayUs =
            SaturatingToUint32(static_cast<uint64_t>(realizedDelaySignedUs > 0 ? realizedDelaySignedUs : 0));

        wgcDelayRealizedAccumUs += realizedDelayUs;
        wgcDelayRealizedMinUs = std::min(wgcDelayRealizedMinUs, realizedDelayUs);
        wgcDelayRealizedMaxUs = std::max(wgcDelayRealizedMaxUs, realizedDelayUs);
        recordWgcDelayResidualSample(predictedSignedResidualUs, wgcDelayResidualSamples, wgcDelayResidualAbsAccumUs,
                                     wgcDelayResidualSignedAccumUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualLateMaxUs,
                                     wgcDelayResidualEarlyMaxUs, wgcDelayResidualAbsHistogram,
                                     wgcDelayResidualWindowSamples, wgcDelayResidualWindowAbsAccumUs,
                                     wgcDelayResidualWindowSignedAccumUs, wgcDelayResidualWindowAbsMaxUs,
                                     wgcDelayResidualWindowLateMaxUs, wgcDelayResidualWindowAbsHistogram);
        recordWgcDelayResidualSample(rawSignedResidualUs, wgcDelayRawResidualSamples, wgcDelayRawResidualAbsAccumUs,
                                     wgcDelayRawResidualSignedAccumUs, wgcDelayRawResidualAbsMaxUs,
                                     wgcDelayRawResidualLateMaxUs, wgcDelayRawResidualEarlyMaxUs,
                                     wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualWindowSamples,
                                     wgcDelayRawResidualWindowAbsAccumUs, wgcDelayRawResidualWindowSignedAccumUs,
                                     wgcDelayRawResidualWindowAbsMaxUs, wgcDelayRawResidualWindowLateMaxUs,
                                     wgcDelayRawResidualWindowAbsHistogram);
        const int64_t rawMinusPredictedUs = rawSignedResidualUs - predictedSignedResidualUs;
        const uint32_t rawMinusPredictedAbsUs = SaturatingToUint32(
            static_cast<uint64_t>(rawMinusPredictedUs >= 0 ? rawMinusPredictedUs : -rawMinusPredictedUs));
        ++wgcDelayRawMinusPredictedSamples;
        wgcDelayRawMinusPredictedSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedAbsMaxUs = std::max(wgcDelayRawMinusPredictedAbsMaxUs, rawMinusPredictedAbsUs);
        ++wgcDelayRawMinusPredictedWindowSamples;
        wgcDelayRawMinusPredictedWindowSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedWindowAbsMaxUs =
            std::max(wgcDelayRawMinusPredictedWindowAbsMaxUs, rawMinusPredictedAbsUs);
        return true;
    };
    const auto wgcDelayResidualHistogramP95Us = [](const std::array<uint32_t, 256>& histogram,
                                                   uint64_t samples) -> uint32_t {
        if (samples == 0) {
            return 0;
        }
        const uint64_t targetRank = (samples * 95ull + 99ull) / 100ull;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < histogram.size(); ++i) {
            cumulative += histogram[i];
            if (cumulative >= targetRank) {
                return SaturatingToUint32(static_cast<uint64_t>(i) * 1000ull);
            }
        }
        return SaturatingToUint32(static_cast<uint64_t>(histogram.size() - 1) * 1000ull);
    };
    const auto wgcDelayResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualAbsHistogram, wgcDelayResidualSamples);
    };
    const auto wgcDelayResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualWindowAbsHistogram, wgcDelayResidualWindowSamples);
    };
    const auto wgcDelayRawResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualSamples);
    };
    const auto wgcDelayRawResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualWindowAbsHistogram, wgcDelayRawResidualWindowSamples);
    };

    while (g_EncoderRunning || g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) || g_FrameQueue.Size() > 0 ||
           !bufferedWgcFrames.empty() || !bufferedInjectFrames.empty()) {
        LARGE_INTEGER cycleStartQpc;
        const uint64_t cycleLiveTicksOutputStart = liveTicksOutput;
        // NOTE: cycleStartQpc is set after timer sleep below to measure
        // encode processing time, not the full loop including sleep.
        if (g_pSharedMem) {
            if (!g_Recording.load(std::memory_order_acquire)) {
                const auto phase = static_cast<CapturePipelinePhase>(
                    g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire));
                if (phase != CapturePipelinePhase::kCancelling) {
                    g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kDrain),
                                                                  std::memory_order_release);
                }
            } else if (recordingOutputLive) {
                g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kLive),
                                                              std::memory_order_release);
            }
        }
        static DWORD lastThreadLog = 0;
        if (GetTickCount() - lastThreadLog > 1000) {
            LogInfo(
                "[EncoderThread] Alive. Q=%u Bot=%d Rate=%.3f Credit=%.2f IBuf=%zu WBuf=%zu Grid=%lld Live=%d "
                "EMA=%u Fence=%.2fms Encode=%.2fms",
                (unsigned int)g_FrameQueue.Size(), (int)g_IsEncoderBottlenecked, smoothedInputPerTick,
                frameCreditAccumulator, bufferedInjectFrames.size(), bufferedWgcFrames.size(),
                static_cast<long long>(encoderGridTickCount), (int)recordingOutputLive, pacingEmaUpdates,
                smoothedInjectFenceMs, smoothedEncodeMs);
            lastThreadLog = GetTickCount();
        }

        if (g_pSharedMem) {
            UpdateAtomicPeak(g_pSharedMem->runtimeState.bufferedInjectDepthPeak,
                             static_cast<uint32_t>(bufferedInjectFrames.size()));
        }

        if (g_pSharedMem) {
            uint32_t queueDepth = (uint32_t)g_FrameQueue.Size();
            queueDepth += static_cast<uint32_t>(bufferedInjectFrames.size());
            queueDepth += static_cast<uint32_t>(bufferedWgcFrames.size());
            double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
            const uint32_t queuePressureThreshold =
                std::max<uint32_t>(8u, static_cast<uint32_t>(g_FrameQueue.Capacity() / 2));
            bool shouldThrottle = queueDepth >= queuePressureThreshold || fenceWaitMs > 16.0;

            g_pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
            g_pSharedMem->throttleCapture.store(shouldThrottle, std::memory_order_release);
            g_pSharedMem->runtimeState.hostDroppedFrames.store(static_cast<uint32_t>(g_FrameQueue.GetDroppedCount()));
            UpdateAtomicPeak(g_pSharedMem->runtimeState.encoderQueuePeakDepth, queueDepth);

            int64_t oldestBufferedTimestamp = 0;
            if (!bufferedInjectFrames.empty()) {
                oldestBufferedTimestamp = bufferedInjectFrames.front().timestamp;
            } else if (!bufferedWgcFrames.empty()) {
                oldestBufferedTimestamp = bufferedWgcFrames.front().timestamp;
            }
            if (oldestBufferedTimestamp > 0) {
                LARGE_INTEGER nowQpc;
                QueryPerformanceCounter(&nowQpc);
                uint64_t oldestAgeUs = 0;
                if (nowQpc.QuadPart > oldestBufferedTimestamp) {
                    oldestAgeUs =
                        static_cast<uint64_t>((nowQpc.QuadPart - oldestBufferedTimestamp) * 1000000 / qpcFreq.QuadPart);
                }
                wgcOldestBufferedFrameAgeUs = SaturatingToUint32(oldestAgeUs);
                g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(wgcOldestBufferedFrameAgeUs,
                                                                          std::memory_order_relaxed);
            } else {
                wgcOldestBufferedFrameAgeUs = 0;
                g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(0, std::memory_order_relaxed);
            }
        }

        uint32_t outputShortfallTicks = 0;
        const bool activeScreenGrab = IsActiveScreenGrab();
        const bool useScreenGrab = activeScreenGrab;
        const uint64_t currentWgcSourceEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
        if (activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            size_t bufferedDiscarded = 0;
            for (auto it = bufferedWgcFrames.begin(); it != bufferedWgcFrames.end();) {
                if (it->wgcSourceEpoch != currentWgcSourceEpoch) {
                    ReleaseQueuedFrameTexture(*it);
                    it = bufferedWgcFrames.erase(it);
                    ++bufferedDiscarded;
                } else {
                    ++it;
                }
            }
            const size_t queuedDiscarded = g_FrameQueue.DiscardWgcEpochNotEqual(currentWgcSourceEpoch);
            if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.wgcSourceEpoch != currentWgcSourceEpoch) {
                ResetLastQueuedFrameCache();
            }
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            lastEmittedWgcSourceQpc = 0;
            lastEmittedWgcSelectionQpc = 0;
            lastWarmupWgcSourceQpc = 0;
            wgcInputPredictor.Reset();
            wgcCfrPhaseLock.Reset();
            wgcRecentDeliveredFps = 0;
            wgcRecentDeliveredMin250Fps = 0;
            wgcRecentDeliveredMin500Fps = 0;
            wgcRecentInputMin250Fps = 0;
            wgcRecentInputMin500Fps = 0;
            wgcLowSourceModeActive = false;
            wgcLiveRecoveryModeActive = false;
            wgcSourceStarvedCurrent = false;
            smoothedWgcFreshServiceMs = 0.0;
            smoothedWgcRepeatServiceMs = 0.0;
            wgcFreshServiceSamples = wgcRepeatServiceSamples = 0;
            wgcOverloadRepeatPacer.ResetActivePacing();
            lastSuccessfulWgcCursorEmbedded = false;
            hasSuccessfulWgcCursorMetadata = false;
            privacyRuntime.ResetSource();
            ResetDuplicationCursorSuppression("WGC source epoch change");
            LogInfo(
                "[EncoderThread] WGC source epoch changed: epoch=%llu bufferedDiscarded=%zu queuedDiscarded=%zu; "
                "selection/cursor lineage rebased without changing the audio or CFR timeline",
                static_cast<unsigned long long>(currentWgcSourceEpoch), bufferedDiscarded, queuedDiscarded);
        } else if (!activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            // Standby WGC retargets are unrelated to the authoritative inject
            // pixels. Observe their publication epoch now so activating the
            // already-proven standby source does not later invalidate the
            // inject repeat fallback at the handoff boundary.
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            LogInfo("[EncoderThread] Observed standby WGC source epoch %llu while inject remained active",
                    static_cast<unsigned long long>(currentWgcSourceEpoch));
        }
        const uint32_t outputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        auto loadEncoderOverloadFlags = [&]() -> uint32_t {
            return g_pSharedMem ? g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
        };
        auto isWgcCapacityPressureActive = [&]() -> bool {
            const uint32_t overloadFlags = loadEncoderOverloadFlags();
            return g_IsEncoderBottlenecked.load(std::memory_order_relaxed) || encoderTooSlowForTargetCurrent ||
                   (overloadFlags & (ce::capture_policy::kEncoderOverloadFlagEncoder |
                                     ce::capture_policy::kEncoderOverloadFlagMux)) != 0;
        };
        auto isWgcTrueSourceStarvedForCapacityPolicy = [&]() -> bool {
            return ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
                outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                isWgcCapacityPressureActive());
        };
        auto isWgcEncoderLimitedSmoothnessMode = [&]() -> bool {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive) {
                return false;
            }
            if (wgcSourceStarvedCurrent || isWgcTrueSourceStarvedForCapacityPolicy()) {
                return false;
            }
            const uint32_t bufferedWgcFrameCount =
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
            if (!ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                    bufferedWgcFrameCount)) {
                return false;
            }
            return ce::capture_policy::IsWgcEncoderLimitedSmoothnessMode(
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed), encoderTooSlowForTargetCurrent,
                loadEncoderOverloadFlags());
        };
        if (!g_EncoderRunning && !g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) && activeScreenGrab) {
            const size_t bufferedDiscarded = bufferedWgcFrames.size();
            const size_t bufferedInjectDiscarded = bufferedInjectFrames.size();
            ClearBufferedWgcFrames();
            ClearBufferedInjectFrames();
            size_t queuedDiscarded = 0;
            QueuedFrame queuedFrame;
            while (g_FrameQueue.Pop(queuedFrame, 0)) {
                DiscardQueuedFrame(queuedFrame);
                ++queuedDiscarded;
            }
            if (bufferedDiscarded > 0 || bufferedInjectDiscarded > 0 || queuedDiscarded > 0) {
                LogInfo(
                    "[EncoderThread] WGC CFR exact-stop discarded pending frames: queued=%zu bufferedWgc=%zu "
                    "bufferedInject=%zu. "
                    "No post-stop CFR drain will be encoded.",
                    queuedDiscarded, bufferedDiscarded, bufferedInjectDiscarded);
            }
            break;
        }
        auto dropWgcVisualTimelineDebtToLiveWindow = [&](const char* reason) -> uint32_t {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive || targetIntervalTicks <= 0 ||
                qpcFreq.QuadPart <= 0) {
                return 0;
            }

            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const uint32_t maxDebtTicks = ce::capture_policy::GetWgcLiveVisualDebtLimitTicksForMode(
                targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (maxDebtTicks == 0 || outputShortfallTicks <= maxDebtTicks) {
                return 0;
            }

            const uint32_t excessTicks = outputShortfallTicks - maxDebtTicks;
            wgcVisualDebtMaxExcessTicks = std::max<uint64_t>(wgcVisualDebtMaxExcessTicks, excessTicks);

            static uint64_t s_lastWgcTimelineDebtDropLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - s_lastWgcTimelineDebtDropLogTick >= 1000) {
                LogWarn(
                    "[EncoderThread] WGC CFR visual timeline debt drop: reason=%s mode=%s excessTicks=%u "
                    "maxDebtTicks=%u maxExcessTicks=%llu shortfall=%u",
                    reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                    excessTicks, maxDebtTicks, static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                    outputShortfallTicks);
                s_lastWgcTimelineDebtDropLogTick = nowTick;
            }
            return excessTicks;
        };
        auto pruneStaleWgcVisualDebt = [&](int64_t liveNowQpc, const char* reason, bool allowDropAll,
                                           int64_t immutableSelectionTargetQpc) -> size_t {
            if (wgcWarmupUntilQpc > 0 && liveNowQpc < wgcWarmupUntilQpc) {
                return 0;
            }
            if (outputShortfallTicks > 0 && immutableSelectionTargetQpc <= 0) {
                return 0;
            }
            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const bool startupSmoothnessAttempted = shouldAttemptWgcStartupSmoothnessBufferNow();
            const int64_t startupSmoothnessTargetQpc = getWgcStartupSmoothnessTargetDelayQpc(startupSmoothnessAttempted);
            const int64_t liveVisualDebtLimitQpc = ce::capture_policy::GetWgcLiveVisualDebtLimitQpcForMode(
                targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (ce::capture_policy::ShouldProtectWgcStartupSmoothnessHistory(
                    recordingOutputLive, startupSmoothnessAttempted, startupSmoothnessTargetQpc,
                    liveVisualDebtLimitQpc)) {
                if (!wgcStartupHistoryProtectionLogged) {
                    wgcStartupHistoryProtectionLogged = true;
                    LogInfo("[EncoderThread] WGC startup history protected from the shallower live-debt window");
                }
                return 0;
            }
            const int64_t intentionalContentDelayQpc = getWgcEffectiveContentDelayQpc();
            const int64_t visualDebtFloorQpc = ce::capture_policy::GetWgcLiveVisualDebtFloorQpcForMode(
                liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode,
                intentionalContentDelayQpc);
            if (visualDebtFloorQpc <= 0) {
                return 0;
            }

            size_t dropped = 0;
            uint64_t maxDebtUs = 0;
            while (!bufferedWgcFrames.empty()) {
                const int64_t selectionTimestampQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                const int64_t nextSelectionTimestampQpc =
                    bufferedWgcFrames.size() > 1 ? GetFrameSelectionTimestamp(bufferedWgcFrames[1]) : 0;
                if (!ce::capture_policy::ShouldPruneWgcVisualDebtFrameForGrid(
                        selectionTimestampQpc, nextSelectionTimestampQpc, visualDebtFloorQpc,
                        immutableSelectionTargetQpc)) {
                    break;
                }
                if (bufferedWgcFrames.size() == 1 && !allowDropAll) {
                    break;
                }

                if (qpcFreq.QuadPart > 0) {
                    maxDebtUs = std::max<uint64_t>(
                        maxDebtUs, static_cast<uint64_t>((visualDebtFloorQpc - selectionTimestampQpc) * 1000000 /
                                                         qpcFreq.QuadPart));
                }
                QueuedFrame stale = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(stale);
                ++dropped;
                ++wgcDropStaleDebtCount;
                ++wgcDropStaleDebtTotal;
            }

            if (dropped > 0) {
                wgcDropStaleDebtMaxUs = std::max(wgcDropStaleDebtMaxUs, SaturatingToUint32(maxDebtUs));
                static uint64_t s_lastStaleWgcDebtLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastStaleWgcDebtLogTick >= 1000 || dropped >= 8) {
                    LogWarn(
                        "[EncoderThread] WGC CFR stale visual debt drop: reason=%s mode=%s dropped=%zu floorQpc=%lld "
                        "gridTargetQpc=%lld liveNowQpc=%lld contentDelay=%lldus maxDebt=%lluus remaining=%zu shortfall=%u",
                        reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                        dropped, static_cast<long long>(visualDebtFloorQpc),
                        static_cast<long long>(immutableSelectionTargetQpc), static_cast<long long>(liveNowQpc),
                        static_cast<long long>(qpcToUs(intentionalContentDelayQpc)),
                        static_cast<unsigned long long>(maxDebtUs), bufferedWgcFrames.size(), outputShortfallTicks);
                    s_lastStaleWgcDebtLogTick = nowTick;
                }
            }
            return dropped;
        };
        auto noteActivePathMismatchDiscard = [&](bool frameIsInjectMode, const char* source) {
            ++activePathMismatchDiscardThisWindow;
            ++activePathMismatchDiscardTotal;
            const uint64_t discarded = g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
            if (activePathMismatchDiscardThisWindow <= 3 || (discarded % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] Discarded %s frame on active %s path from %s (window=%u total=%llu). Preventing "
                    "mid-recording encoder mode switch.",
                    frameIsInjectMode ? "inject" : "WGC/D3D11", useScreenGrab ? "WGC" : "inject", source,
                    activePathMismatchDiscardThisWindow, static_cast<unsigned long long>(discarded));
            }
        };
        auto discardActivePathMismatchFrame = [&](QueuedFrame& mismatchedFrame, const char* source, bool queuedFrame) {

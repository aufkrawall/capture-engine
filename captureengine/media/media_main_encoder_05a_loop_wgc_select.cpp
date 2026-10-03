#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopWgcSelect() {
        if (useScreenGrab) {
        SelectWgcFrameCapture();
        SelectWgcFrameUniformPath();
        } else {
        SelectInjectFrame();
        }

        frameToProcess = nullptr;
        isDuplicate = false;
        duplicateFromDeferred = false;
        duplicateFromTimerRebase = false;
        wantsTrueRepeatLastFrame = false;

        if (popped && !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, frame.isInjectMode)) {
            discardActivePathMismatchFrame(frame, "selected frame", true);
            popped = false;
        }

        const bool canPreserveLastFrameAcrossPathHandoff =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame) &&
            MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
        if (media_main_g_HasLastFrame &&
            !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, media_main_g_LastFrame.isInjectMode) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            discardActivePathMismatchFrame(media_main_g_LastFrame, "cached last frame", false);
            media_main_g_HasLastFrame = false;
        }

        if (popped && frame.isInjectMode && media_main_g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (media_main_g_HasLastFrame && media_main_g_LastFrame.isInjectMode && media_main_g_RejectInjectFrames.load(std::memory_order_acquire) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            media_main_g_LastFrame = QueuedFrame{};
            media_main_g_HasLastFrame = false;
        }

        hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        // Warmup frames never reach the file, but the WGC/DXGI look-ahead reservoir they
        // build is handed to the live output intact. Tracking focus from the first warmup
        // tick opens the verified-focus interval before that reservoir content is captured,
        // so the live handoff no longer has to blank a reservoir it just filled.
}

void MediaEncoderSession::SelectWgcFrameCapture() {
if (!bufferedInjectFrames.empty()) {
    ClearBufferedInjectFrames();
}
smoothedInjectFenceMs = 0.0;
}

void MediaEncoderSession::SelectWgcFrameUniformPath() {
            if (!config.video.useVFR) {
        SelectWgcFrameUniform();

        SelectWgcFrameUniformTail();

                updateWgcIngressPressure(popped ? "post-select" : "post-hold");
            } else {
                // VFR: keep the existing lowest-latency newest-frame sampling.
                QueuedFrame temp;
        SelectWgcFrameUniformVfr();
            }
}

void MediaEncoderSession::SelectWgcFrameUniform() {
// CFR WGC: drain all pending captured frames and let the CFR slot
// scheduler be the only authority for selection/repeat/drop.
drainedScreenGrabFrames.clear();
drainedWgcCapturedFrames.clear();
if (auto capture = media_main_g_WgcCap.Read()) {
    const uint64_t drainSourceEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
    capture->DrainPendingFrames(drainedWgcCapturedFrames, 0);
    for (auto& capturedFrame : drainedWgcCapturedFrames) {
        if (!capturedFrame.texture) {
            continue;
        }
        if (capturedFrame.sourceEpoch != drainSourceEpoch) {
            static uint64_t s_retiredPullFrameDrops = 0;
            ++s_retiredPullFrameDrops;
            if (s_retiredPullFrameDrops <= 3 || (s_retiredPullFrameDrops % 120ull) == 0ull) {
                LogInfo(
                    "[WGC] Dropping retired-source pull frame: frameEpoch=%llu activeEpoch=%llu "
                    "discarded=%llu",
                    static_cast<unsigned long long>(capturedFrame.sourceEpoch),
                    static_cast<unsigned long long>(drainSourceEpoch),
                    static_cast<unsigned long long>(s_retiredPullFrameDrops));
            }
            ReleaseWgcCapturedFrame(capturedFrame);
            continue;
        }
        drainedScreenGrabFrames.push_back(MakeQueuedWgcFrame(std::move(capturedFrame)));
    }
}

while (media_main_g_FrameQueue.Pop(temp, 0)) {
    DiscardQueuedFrame(temp);
}

const bool sampleWgcCadenceTick = !(recordingOutputLive && encoderLateTickCount > 0);

// Phase 1: keep only frames that belong to the recording interval,
// then feed raw timestamps to predictor BEFORE moving frames to the
// buffer (std::move invalidates source object). Always feed the
// predictor (even when encoder is late) so it can calibrate the
// source FPS for Bresenham pacing and logging.
if (!drainedScreenGrabFrames.empty()) {
    const int64_t stopBoundaryQpc = !media_main_g_Recording.load(std::memory_order_acquire)
                                        ? media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire)
                                        : 0;
    size_t postStopDropped = 0;
    uint32_t postStopMaxLeadUs = 0;
    std::vector<QueuedFrame> keptFrames;
    keptFrames.reserve(drainedScreenGrabFrames.size());
    for (auto& drainedFrame : drainedScreenGrabFrames) {
        const int64_t sourceFrameQpc =
            drainedFrame.rawTimestamp > 0 ? drainedFrame.rawTimestamp : drainedFrame.timestamp;
        if (!ce::capture_policy::ShouldKeepWgcFrameForStopDrain(sourceFrameQpc, stopBoundaryQpc)) {
            if (qpcFreq.QuadPart > 0 && sourceFrameQpc > stopBoundaryQpc) {
                const uint64_t leadUs = static_cast<uint64_t>(sourceFrameQpc - stopBoundaryQpc) *
                                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                postStopMaxLeadUs = std::max(postStopMaxLeadUs, SaturatingToUint32(leadUs));
            }
            ReleaseQueuedFrameTexture(drainedFrame);
            ++postStopDropped;
            ++wgcPostStopFrameDropTotal;
            continue;
        }

        if (!drainedFrame.isInjectMode && drainedFrame.timestamp > 0) {
            wgcInputPredictor.Update(drainedFrame.timestamp, qpcFreq.QuadPart);
            // Monotonic bounded-deviation smoothing of the raw compositor timestamp.
            // WGC/DXGI timestamps are DWM composition times and arrive quantized
            // under VRR/composed presentation even when the game presents perfectly
            // smoothly; a CFR playout slaved to the raw stamps converts a surplus
            // source into constant single-tick repeats (fortistutter root cause).
            // The raw timestamp stays untouched for sync validation/diagnostics.
            drainedFrame.selectionTimestamp =
                wgcInputPredictor.SmoothMonotonicTimestamp(drainedFrame.timestamp, targetIntervalTicks);
            observeCaptureSyncPhaseSource(
                "screen_grab", wgcCfrPhaseLock, GetFrameSelectionTimestamp(drainedFrame));
            if (drainedFrame.selectionTimestamp > 0 && qpcFreq.QuadPart > 0) {
                const int64_t devQpc =
                    AbsoluteTimestampDistance(drainedFrame.selectionTimestamp, drainedFrame.timestamp);
                const uint32_t devUs = SaturatingToUint32(static_cast<uint64_t>(devQpc) * 1000000ull /
                                                          static_cast<uint64_t>(qpcFreq.QuadPart));
                ++wgcTsSmoothSamplesWindow;
                wgcTsSmoothDevAccumUsWindow += devUs;
                wgcTsSmoothDevMaxUsWindow = std::max(wgcTsSmoothDevMaxUsWindow, devUs);
                wgcTsSmoothDevMaxUsTotal = std::max(wgcTsSmoothDevMaxUsTotal, devUs);
                const uint64_t snapTotal = wgcInputPredictor.SmoothingSnapCount();
                if (snapTotal > wgcTsSmoothSnapCountTotal) {
                    wgcTsSmoothSnapCountWindow +=
                        static_cast<uint32_t>(snapTotal - wgcTsSmoothSnapCountTotal);
                    wgcTsSmoothSnapCountTotal = snapTotal;
                }
            }
            static int64_t s_lastWgcSrcQpc = 0;
            if (drainedFrame.timestamp == s_lastWgcSrcQpc) {
                ++dupTimestampCount;
            }
            s_lastWgcSrcQpc = drainedFrame.timestamp;
        }
        keptFrames.push_back(std::move(drainedFrame));
    }
    if (postStopDropped > 0) {
        wgcPostStopFrameDropMaxUs = std::max(wgcPostStopFrameDropMaxUs, postStopMaxLeadUs);
        static uint64_t s_lastPostStopWgcDropLogTick = 0;
        const uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastPostStopWgcDropLogTick >= 1000 || postStopDropped >= 4) {
            LogWarn(
                "[EncoderThread] WGC CFR post-stop frame drop: dropped=%zu stopQpc=%lld maxLead=%uus "
                "total=%llu",
                postStopDropped, static_cast<long long>(stopBoundaryQpc), postStopMaxLeadUs,
                static_cast<unsigned long long>(wgcPostStopFrameDropTotal));
            s_lastPostStopWgcDropLogTick = nowTick;
        }
    }
    drainedScreenGrabFrames.swap(keptFrames);
}

// Phase 2: append newly drained WGC frames and keep the host-side
// reserve shallow. This restores timestamp-aware selection for
// >target source cadence without letting callback bursts create a
// deep unstable queue.
for (auto& drainedFrame : drainedScreenGrabFrames) {
    bufferedWgcFrames.push_back(std::move(drainedFrame));
}

if (recordingOutputLive && !bufferedWgcFrames.empty()) {
    LARGE_INTEGER visualDebtNowQpc;
    QueryPerformanceCounter(&visualDebtNowQpc);
    int64_t visualDebtPolicyQpc = visualDebtNowQpc.QuadPart;
    if (!media_main_g_Recording.load(std::memory_order_acquire)) {
        const int64_t drainStopQpc = media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire);
        if (drainStopQpc > 0) {
            visualDebtPolicyQpc = drainStopQpc;
        }
    }
    pruneStaleWgcVisualDebt(visualDebtPolicyQpc, "live-buffer",
                            media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode, 0);
}
if (recordingOutputLive && !bufferedWgcFrames.empty()) {
    trimBufferedWgcForPoolPressure("live-pool-pressure");
}
trimBufferedWgcToRetainedCap(recordingOutputLive ? "live-buffer" : "warmup-buffer");

// Track frame arrival rate for pacing telemetry only.
if (sampleWgcCadenceTick) {
    pacingInputThisWindow += static_cast<uint32_t>(drainedScreenGrabFrames.size());
    pacingTicksThisWindow++;
    const uint32_t wgcPacingWindowSize = (pacingEmaUpdates < 6)
                                             ? std::max((uint32_t)config.video.fps / 8, 8u)
                                             : (uint32_t)config.video.fps / 2;
    if (pacingTicksThisWindow >= wgcPacingWindowSize) {
        double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
        // Adaptive alpha: fast convergence during startup, burst detection, steady-state
        double alpha = 0.5;
        if (pacingEmaUpdates < 6) {
            alpha = 0.7;
        } else if (smoothedInputPerTick > 0.01) {
            double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
            if (deviation > 0.20) {
                alpha = 0.8;
            }
        }
        smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
        pacingInputThisWindow = 0;
        pacingTicksThisWindow = 0;
        ++pacingEmaUpdates;
    }
}

if (media_main_g_WgcCap) {
    wgcRecentDeliveredFps = media_main_g_WgcCap->GetDeliveredRatePerSec();
    wgcRecentDeliveredMin250Fps = media_main_g_WgcCap->GetDeliveredMin250Fps();
    wgcRecentDeliveredMin500Fps = media_main_g_WgcCap->GetDeliveredMin500Fps();
    wgcRecentInputMin250Fps = media_main_g_WgcCap->GetInputMin250Fps();
    wgcRecentInputMin500Fps = media_main_g_WgcCap->GetInputMin500Fps();
}
const uint32_t wgcPolicySourceJitterUs =
    media_main_g_WgcCap ? SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs()) : 0u;
const uint32_t wgcPolicyPredictorJitterUs =
    wgcInputPredictor.IsCalibrated()
        ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
        : 0u;
const uint32_t wgcPolicyAverageJitterUs = std::max(wgcPolicySourceJitterUs, wgcPolicyPredictorJitterUs);

wgcCoverageDelayTicksCurrent = 0;

const uint64_t wgcPolicyNowTick = GetTickCount64();
const ce::capture_policy::WgcAdaptiveTelemetry wgcAdaptiveTelemetry = {
    outputFps,
    wgcRecentDeliveredFps,
    wgcRecentDeliveredMin250Fps,
    wgcRecentDeliveredMin500Fps,
    wgcRecentInputMin250Fps,
    wgcRecentInputMin500Fps,
    wgcPolicyAverageJitterUs,
    wgcNoFreshTickPermille,
    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
    0u,
    0.0,
};
const bool allowWgcLiveRecoveryMode =
    recordingOutputLive && media_main_g_Recording.load(std::memory_order_acquire);
static uint64_t s_lastWgcWarmupLogTick = 0;
inWgcWarmup =
    wgcWarmupUntilQpc > 0 && liveTicksOutput < static_cast<uint64_t>(std::max(24u, outputFps / 6u));
if (inWgcWarmup && s_lastWgcWarmupLogTick == 0) {
    s_lastWgcWarmupLogTick = GetTickCount64();
    LogInfo("[WGC CFR] Warmup active: %llu ticks to stabilize capture pipeline",
            static_cast<unsigned long long>(std::max(24u, outputFps / 6u)));
} else if (!inWgcWarmup && s_lastWgcWarmupLogTick > 0 &&
           (wgcPolicyNowTick - s_lastWgcWarmupLogTick) >= 1000) {
    LogInfo("[WGC CFR] Warmup ended: liveTicksOutput=%llu buffered=%zu",
            static_cast<unsigned long long>(liveTicksOutput), bufferedWgcFrames.size());
    s_lastWgcWarmupLogTick = 0;
}
const bool wgcSourceHealthTelemetryReady =
    !inWgcWarmup && allowWgcLiveRecoveryMode &&
    liveTicksOutput >= std::max<uint64_t>(8ull, outputFps / 8u) && wgcRecentDeliveredMin250Fps > 0 &&
    wgcRecentDeliveredMin500Fps > 0 && wgcRecentInputMin250Fps > 0 && wgcRecentInputMin500Fps > 0;
const bool wgcCapacityPressureForRecovery = isWgcCapacityPressureActive();
const ce::capture_policy::WgcLiveRecoveryState wgcLiveRecoveryStateCurrent =
    wgcSourceHealthTelemetryReady
        ? ce::capture_policy::ClassifyWgcLiveRecoveryState(wgcAdaptiveTelemetry, outputShortfallTicks,
                                                           wgcCapacityPressureForRecovery)
        : ce::capture_policy::WgcLiveRecoveryState::kHealthy;
wgcSourceStarvedCurrent =
    allowWgcLiveRecoveryMode &&
    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSourceStarved;
wgcSchedulerLimitedCurrent =
    allowWgcLiveRecoveryMode &&
    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSchedulerLimited;
wgcEncoderRecoveryLimitedCurrent =
    allowWgcLiveRecoveryMode &&
    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kEncoderLimited;
wgcReservePressureActive = ce::capture_policy::IsWgcReservePressureActive(
    wgcNoReserveTickCount, wgcQueueTickSampleCount, outputFps);
const ce::capture_policy::WgcLowSourceState wgcLowSourceStateCurrent =
    wgcSourceHealthTelemetryReady ? ce::capture_policy::ClassifyWgcLowSourceState(wgcAdaptiveTelemetry)
                                  : ce::capture_policy::WgcLowSourceState::kHealthy;
const bool shouldEnterWgcLowSourceMode =
    wgcLowSourceStateCurrent != ce::capture_policy::WgcLowSourceState::kHealthy;
const bool bufferedReserveRecovered =
    isWgcEffectiveContentDelayActive()
        ? ce::capture_policy::IsWgcDelayReservoirRecovered(
              bufferedWgcFrames.size(), getWgcEffectiveContentDelayQpc(), targetIntervalTicks)
        : bufferedWgcFrames.size() >= 3;
const uint64_t wgcLowSourceDurationMs =
    wgcLowSourceModeActive && wgcPolicyNowTick >= wgcLowSourceStateChangeTick
        ? (wgcPolicyNowTick - wgcLowSourceStateChangeTick)
        : 0;
const bool stableWgcUnderfeed =
    wgcSourceHealthTelemetryReady &&
    wgcLowSourceDurationMs >= ce::capture_policy::kWgcStableUnderfeedClassificationMs &&
    wgcRecentDeliveredMin250Fps > 0 && wgcRecentInputMin250Fps > 0;
const bool shouldExitWgcLowSourceMode =
    ce::capture_policy::ShouldExitWgcLowSourceMode(wgcAdaptiveTelemetry, encoderTooSlowForTargetCurrent,
                                                   bufferedReserveRecovered, wgcLowSourceDurationMs);
const auto wgcLowSourceModeUpdate = ce::capture_policy::UpdateHeldMode(
    wgcLowSourceModeActive, wgcLowSourceStateChangeTick, wgcPolicyNowTick, shouldEnterWgcLowSourceMode,
    shouldExitWgcLowSourceMode, !encoderTooSlowForTargetCurrent && bufferedReserveRecovered,
    ce::capture_policy::kWgcLowSourceEnterHoldMs, ce::capture_policy::kWgcLowSourceExitHoldMs);
wgcLowSourceModeActive = wgcLowSourceModeUpdate.active;
wgcLowSourceStateChangeTick = wgcLowSourceModeUpdate.stateChangeTick;
if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
    // Log only entries that have a reason to hold. The immediate-exit path
    // (!encoderTooSlowForTargetCurrent && bufferedReserveRecovered) can revert an
    // entry on the very next policy tick while the source stays below target, so
    // an ungated "entered" line would otherwise repeat every ~280 ms for the
    // whole episode even though each entry lasts a single tick. Those flap
    // entries are still accounted for in the session summary
    // (lowSourceImmediateExits); the real state (fps, empty permille, buffer
    // depth) is visible at 1 Hz in the CFR jitter-budget diagnostics.
    const bool wgcLowSourceEntryHolds = encoderTooSlowForTargetCurrent || !bufferedReserveRecovered;
    if (wgcLowSourceEntryHolds) {
        LogInfo(
            "[WGC CFR] Low-source mode entered: state=%s src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
            ce::capture_policy::WgcLowSourceStateToString(wgcLowSourceStateCurrent), wgcRecentDeliveredFps,
            wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
            wgcRecentInputMin500Fps, wgcNoFreshTickPermille, bufferedWgcFrames.size());
    }
} else if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kExited) {
    if (wgcLowSourceModeUpdate.immediate) {
        ++captureSessionSummary.lowSourceImmediateExits;
    } else {
        LogInfo("[WGC CFR] Low-source mode exited: src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                bufferedWgcFrames.size());
    }
}

if (wgcSourceHealthTelemetryReady) {
    const bool shouldEnterWgcLiveRecoveryMode = ce::capture_policy::ShouldEnterWgcLiveRecoveryMode(
        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery);
    const bool shouldExitWgcLiveRecoveryMode = ce::capture_policy::ShouldExitWgcLiveRecoveryMode(
        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery, stableWgcUnderfeed);
    const auto wgcLiveRecoveryModeUpdate = ce::capture_policy::UpdateHeldMode(
        wgcLiveRecoveryModeActive, wgcLiveRecoveryStateChangeTick, wgcPolicyNowTick,
        shouldEnterWgcLiveRecoveryMode, shouldExitWgcLiveRecoveryMode, false,
        ce::capture_policy::kWgcRecoveryEnterHoldMs, ce::capture_policy::kWgcRecoveryExitHoldMs);
    wgcLiveRecoveryModeActive = wgcLiveRecoveryModeUpdate.active;
    wgcLiveRecoveryStateChangeTick = wgcLiveRecoveryModeUpdate.stateChangeTick;
    if (wgcLiveRecoveryModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
        LogInfo(
            "[WGC CFR] Live-recovery entered: state=%s srcStarved=%d schedLimited=%d encLimited=%d "
            "shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
            ce::capture_policy::WgcLiveRecoveryStateToString(wgcLiveRecoveryStateCurrent),
            wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
            wgcEncoderRecoveryLimitedCurrent ? 1 : 0, outputShortfallTicks,
            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
            bufferedWgcFrames.size());
    } else if (wgcLiveRecoveryModeUpdate.transition ==
               ce::capture_policy::HeldModeTransition::kExited) {
        LogInfo(
            "[WGC CFR] Live-recovery exited: shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm "
            "buffered=%zu",
            outputShortfallTicks,
            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
            bufferedWgcFrames.size());
    }
} else {
    wgcLiveRecoveryModeActive = false;
    wgcLiveRecoveryStateChangeTick = 0;
}

const bool wgcCapacityPressureActiveCurrent = isWgcCapacityPressureActive();
const uint32_t wgcBufferedFramesForPolicy =
    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
const bool wgcTrueSourceStarvedForCapacityCurrent =
    ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
        wgcBufferedFramesForPolicy, wgcCapacityPressureActiveCurrent);
const bool wgcSourceHealthyForEncoderLimitedCurrent =
    ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
        wgcBufferedFramesForPolicy);
const bool wgcEncoderLimitedSmoothnessActiveCurrent = isWgcEncoderLimitedSmoothnessMode();
if (wgcLowSourceModeActive && wgcCapacityPressureActiveCurrent &&
    wgcSourceHealthyForEncoderLimitedCurrent && wgcEncoderLimitedSmoothnessActiveCurrent) {
    ++wgcEncoderLimitedSuppressedByLowSourceThisWindow;
    ++wgcEncoderLimitedSuppressedByLowSourceTotal;
}
if (wgcEncoderRecoveryLimitedCurrent && !wgcEncoderLimitedSmoothnessActiveCurrent &&
    !wgcTrueSourceStarvedForCapacityCurrent) {
    ++wgcCapacityPressureModeMismatchThisWindow;
    ++wgcCapacityPressureModeMismatchTotal;
    static uint64_t s_lastWgcModeMismatchLogTick = 0;
    const uint64_t mismatchNowTick = GetTickCount64();
    if (mismatchNowTick - s_lastWgcModeMismatchLogTick >= 1000) {
        LogWarn(
            "[WGC CFR] encoder-limited mode mismatch: recovery=encoder_limited smoothness=0 "
            "lowSource=%d sourceHealthy=%d trueSourceStarved=%d shortfall=%u input=%u/%u "
            "freshMiss=%upm buffered=%u overload=0x%X",
            wgcLowSourceModeActive ? 1 : 0, wgcSourceHealthyForEncoderLimitedCurrent ? 1 : 0,
            wgcTrueSourceStarvedForCapacityCurrent ? 1 : 0, outputShortfallTicks,
            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
            wgcBufferedFramesForPolicy, loadEncoderOverloadFlags());
        s_lastWgcModeMismatchLogTick = mismatchNowTick;
    }
}

const bool starvedEpisodeShouldBeActive =
    wgcSourceHealthTelemetryReady &&
    (ce::capture_policy::IsWgcDeepUnderfeed(outputFps, wgcRecentDeliveredMin250Fps,
                                            wgcRecentInputMin250Fps, wgcNoFreshTickPermille) ||
     (wgcLiveRecoveryModeActive && wgcSourceStarvedCurrent));
if (starvedEpisodeShouldBeActive) {
    if (!wgcStarvedEpisode.active) {
        wgcStarvedEpisode.Reset();
        wgcStarvedEpisode.active = true;
        wgcStarvedEpisode.startTickMs = GetTickCount64();
        LARGE_INTEGER episodeStartQpc = {};
        QueryPerformanceCounter(&episodeStartQpc);
        wgcStarvedEpisode.startQpc = episodeStartQpc.QuadPart;
        wgcStarvedEpisode.startLiveTicks = liveTicksOutput;
        wgcStarvedEpisode.startDuplicateTicks = captureSessionSummary.duplicateTicks;
        if (media_main_g_WgcCap) {
            wgcStarvedEpisode.startPoolSaturatedDrops = media_main_g_WgcCap->GetPoolSaturatedDropCount();
            wgcStarvedEpisode.startPoolOverwritePrevented =
                media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
            wgcStarvedEpisode.startIngressDecimated = media_main_g_WgcCap->GetIngressDecimatedCount();
        }
    }
    wgcStarvedEpisode.minInputFps = std::min(wgcStarvedEpisode.minInputFps, wgcRecentInputMin250Fps);
    wgcStarvedEpisode.minDeliveredFps =
        std::min(wgcStarvedEpisode.minDeliveredFps, wgcRecentDeliveredMin250Fps);
    wgcStarvedEpisode.peakFreshMissPermille =
        std::max(wgcStarvedEpisode.peakFreshMissPermille, wgcNoFreshTickPermille);
    wgcStarvedEpisode.minBufferedFrames = std::min<uint32_t>(
        wgcStarvedEpisode.minBufferedFrames,
        static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)));
} else if (wgcStarvedEpisode.active) {
    const uint64_t nowTickMs = GetTickCount64();
    const uint64_t durationMs = nowTickMs - wgcStarvedEpisode.startTickMs;
    const uint64_t outputTicks = liveTicksOutput - wgcStarvedEpisode.startLiveTicks;
    const uint64_t duplicateTicks =
        captureSessionSummary.duplicateTicks - wgcStarvedEpisode.startDuplicateTicks;
    finishWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks);
}

const bool activeDelaySevereSourceStall =
    isWgcEffectiveContentDelayActive() && wgcSourceHealthTelemetryReady &&
    ce::capture_policy::IsWgcSevereSourceStallForActiveDelay(
        outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
        wgcBufferedFramesForPolicy);
if (activeDelaySevereSourceStall) {
    const uint64_t recoveryUntil =
        wgcPolicyNowTick + ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs;
    const bool enteringRecovery = wgcActiveDelaySourceRecoveryUntilTick <= wgcPolicyNowTick;
    wgcActiveDelaySourceRecoveryUntilTick =
        std::max<uint64_t>(wgcActiveDelaySourceRecoveryUntilTick, recoveryUntil);
    if (enteringRecovery) {
        LogInfo(
            "[WGC CFR] Active-delay source recovery entered: src=%u/%u input=%u/%u empty=%upm "
            "buffered=%u holdMs=%u",
            wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
            wgcRecentInputMin500Fps, wgcNoFreshTickPermille, wgcBufferedFramesForPolicy,
            ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs);
    }
}
if (isWgcEffectiveContentDelayActive() && wgcActiveDelaySourceRecoveryUntilTick > wgcPolicyNowTick) {
    ++wgcActiveDelaySourceRecoveryTicks;
}

if (media_main_g_WgcCap && recordingOutputLive && media_main_g_Recording) {
    const uint32_t currentTargetFps = media_main_g_WgcCap->GetProducerTargetFps();
    if (currentTargetFps != 0) {
        LogError(
            "[WGC CFR] ERROR: producer contract violation: backend=%s outputFps=%u "
            "producerTargetFps=%u; forcing MinUpdateInterval=0 because finite producer intervals "
            "alias variable-rate input",
            media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, currentTargetFps);
        media_main_g_WgcCap->SetProducerTargetFps(0);
        media_main_g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
        ++wgcProducerRateRetuneCount;
        ++wgcProducerRateRetuneTotal;
    }
}

const bool scheduledWgcTelemetryTick =
    !config.video.useVFR && media_main_g_EncoderRunning && media_main_g_Recording && recordingOutputLive;
if (scheduledWgcTelemetryTick) {
    LARGE_INTEGER selectionNowQpc;
    QueryPerformanceCounter(&selectionNowQpc);
    wgcTelemetryTickArmed = true;
    wgcBufferedAtTickStart = static_cast<uint32_t>(bufferedWgcFrames.size());
    wgcReserveAvailableAtTickStart = false;
    // Uniform active-delay playout keeps its delay through below-target VRR cadence;
    // otherwise recovery collapses realized delay toward zero and can latch there.
    // The legacy reservoir path still yields its delay to live recovery.
    const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
    const bool uniformCadenceActiveDelay =
        effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
    const bool liveRecoverySuppressesDelay =
        ce::capture_policy::ShouldLiveRecoverySuppressWgcSelectionDelay(wgcLiveRecoveryModeActive,
                                                                        uniformCadenceActiveDelay);
    const bool activeDelayInspection =
        effectiveContentDelayQpc > 0 && recordingOutputLive && !liveRecoverySuppressesDelay;
    const uint32_t requiredReservoirFrames =
        activeDelayInspection ? std::max<uint32_t>(1u, getWgcDelayReservoirLowWaterFrames()) : 1u;
    const int64_t reservoirInspectionTargetQpc =
        activeDelayInspection
            ? clampWgcSelectionTargetQpc(computeDelayedWgcSelectionTargetQpc(),
                                         selectionNowQpc.QuadPart)
            : clampWgcSelectionTargetQpc(computeWgcSelectionTargetQpc(false), selectionNowQpc.QuadPart);
    inspectBufferedWgcCoverageForTarget(reservoirInspectionTargetQpc, activeDelayInspection,
                                        requiredReservoirFrames, &wgcFreshAvailableAtTickStart,
                                        &wgcReserveAvailableAtTickStart);
    wgcSelectionDelayAppliedThisTick =
        !liveRecoverySuppressesDelay &&
        ce::capture_policy::ShouldApplyWgcSelectionDelay(
            recordingOutputLive, outputShortfallTicks,
            media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), wgcReserveAvailableAtTickStart,
            effectiveContentDelayQpc > 0);
    if (wgcSelectionDelayAppliedThisTick) {
        ++wgcSelectionDelayTickCount;
    }

    const bool wgcSourceAtOrAboveTarget =
        wgcSourceHealthTelemetryReady &&
        ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
            outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps);
    const bool wgcSourceHealthyForPacing =
        !wgcTrueSourceStarvedForCapacityCurrent &&
        (wgcSourceAtOrAboveTarget || wgcSourceHealthyForEncoderLimitedCurrent);
    const bool wgcRepeatAvailableForPacer =
        media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode &&
        ((MediaEngine_RepeatLastFrameWithTimeline != nullptr) ||
         (MediaEngine_RepeatLastFrame != nullptr)) &&
        MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
    const bool wgcPacingCapacityPressure =
        wgcCapacityPressureActiveCurrent || outputShortfallTicks > 0;
    const auto overloadPacerDecision = ce::capture_policy::UpdateWgcOverloadRepeatPacer(
        wgcOverloadRepeatPacer, true, wgcSourceHealthyForPacing, wgcPacingCapacityPressure,
        wgcFreshAvailableAtTickStart, wgcRepeatAvailableForPacer, smoothedWgcFreshServiceMs,
        smoothedWgcRepeatServiceMs, frameIntervalMs, wgcFreshServiceSamples, wgcRepeatServiceSamples);
    wgcProactiveOverloadRepeatThisTick = overloadPacerDecision.repeat;

    static uint64_t s_lastWgcOverloadPacerLogTick = 0;
    const uint64_t overloadPacerNowTick = GetTickCount64();
    if (overloadPacerDecision.entered &&
        overloadPacerNowTick - s_lastWgcOverloadPacerLogTick >= 1000) {
        LogWarn(
            "[WGC CFR] Overload repeat pacer entered: reason=%s backend=%s fresh=%.2fms/%u repeat=%.2fms/%u "
            "budget=%.2fms freshFraction=%.3f shortfall=%u buffered=%zu source=%u/%u "
            "repeats=%llu maxRun=%u (CFR PTS and audio timeline unchanged)",
            overloadPacerDecision.reason,
            media_main_g_WgcCap && media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc",
            smoothedWgcFreshServiceMs, wgcFreshServiceSamples, smoothedWgcRepeatServiceMs,
            wgcRepeatServiceSamples, overloadPacerDecision.serviceBudgetMs,
            overloadPacerDecision.freshFraction, outputShortfallTicks, bufferedWgcFrames.size(),
            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps,
            static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats),
            wgcOverloadRepeatPacer.maxConsecutiveProactiveRepeats);
        s_lastWgcOverloadPacerLogTick = overloadPacerNowTick;
    } else if (overloadPacerDecision.exited &&
               overloadPacerNowTick - s_lastWgcOverloadPacerLogTick >= 1000) {
        LogInfo(
            "[WGC CFR] Overload repeat pacer exited: reason=%s fresh=%.2fms repeat=%.2fms "

            "freshFraction=%.3f shortfall=%u repeats=%llu",
            overloadPacerDecision.reason, smoothedWgcFreshServiceMs, smoothedWgcRepeatServiceMs,
            overloadPacerDecision.freshFraction, outputShortfallTicks,
            static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats));
        s_lastWgcOverloadPacerLogTick = overloadPacerNowTick;
    }
}
}

void MediaEncoderSession::SelectWgcFrameUniformVfr() {
while (media_main_g_FrameQueue.Pop(temp, 0)) {
    if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
        DiscardQueuedFrame(temp);
        continue;
    }
    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
        discardActivePathMismatchFrame(temp, "WGC VFR queue", true);
        continue;
    }
    if (popped && !frame.isInjectMode && frame.texture) {
        frame.texture->Release();
    }
    frame = std::move(temp);
    popped = true;
}
}

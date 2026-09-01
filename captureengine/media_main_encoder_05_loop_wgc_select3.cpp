#include "media_main_internal.h"
#include "media_main_encoder_session.h"

#include <cmath>
#include <cstddef>
#include <limits>

void MediaEncoderSession::SelectInjectFrame() {
if (!bufferedWgcFrames.empty()) {
    ClearBufferedWgcFrames();
}
if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && !bufferedInjectFrames.empty()) {
    ClearBufferedInjectFrames();
}

if (!config.video.useVFR) {
    // Keep multiple inject frames in reserve so the encoder usually works on
    // textures whose GPU copy has already completed instead of blocking on the
    // newest frame's fence.
    drainedInjectFrames.clear();
    QueuedFrame temp;
    while (media_main_g_FrameQueue.Pop(temp, 0)) {
        if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
            DiscardQueuedFrame(temp);
            continue;
        }
        if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
            discardActivePathMismatchFrame(temp, "inject CFR queue", true);
            continue;
        }
        drainedInjectFrames.push_back(std::move(temp));
    }

    const size_t firstNewBufferedFrame = bufferedInjectFrames.size();
    for (auto& drainedFrame : drainedInjectFrames) {
        bufferedInjectFrames.push_back(std::move(drainedFrame));
    }
    RefreshInjectFinalOutputDisplayTiming(firstNewBufferedFrame);
    for (size_t index = firstNewBufferedFrame; index < bufferedInjectFrames.size(); ++index) {
        const QueuedFrame& queuedFrame = bufferedInjectFrames[index];
        if (queuedFrame.isInjectMode && queuedFrame.timestamp > 0) {
            injectInputPredictor.Update(queuedFrame.timestamp, qpcFreq.QuadPart);
            observeCaptureSyncPhaseSource("inject", injectCfrPhaseLock, queuedFrame.timestamp);
        }
    }

    // Track frame arrival rate for source-health telemetry. Use a short
    // window during warmup/startup so the EMA is already calibrated
    // when recording goes live, then widen to half-second for
    // steady-state stability. Timestamp-target playout, not this EMA,
    // decides which source frame represents each CFR output slot.
    pacingInputThisWindow += (uint32_t)drainedInjectFrames.size();
    pacingTicksThisWindow++;
    const uint32_t pacingWindowSize = (pacingEmaUpdates < 6) ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                             : (uint32_t)config.video.fps / 2;
    if (pacingTicksThisWindow >= pacingWindowSize) {
        double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
        // Adaptive alpha: converge fast during startup (0.7), steady-state (0.5),
        // or when FPS transitions are detected (>20% deviation -> 0.8) so
        // diagnostics follow rapid source-rate changes promptly.
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

    const size_t injectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
        config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
    // Only the physical GPU/fence safety tail is protected from selection. The A/V
    // content delay is a timestamp target below; treating it as additional protected
    // frames hides every useful candidate at normal queue depth and creates trim/repeat
    // churn even when the game supplies one fresh frame per CFR tick.
    const size_t protectedInjectTailFrames =
        ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive);
    int64_t livePlayoutTargetQpc = 0;
    const int64_t leadToleranceQpc =
        ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
    if (recordingOutputLive && encoderGridStartQpc > 0 && targetIntervalTicks > 0) {
        const int64_t liveTargetQpc =
            scheduledOutputQpc > 0
                ? scheduledOutputQpc
                : ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks);
        const int64_t basePlayoutTargetQpc =
            ComputeDelayedContentGridStartQpc(liveTargetQpc, avContentDelayQpc);
        const int64_t phaseReferenceQpc =
            bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
        livePlayoutTargetQpc = applyCaptureSyncPhaseTarget(
            "inject", injectCfrPhaseLock, basePlayoutTargetQpc, phaseReferenceQpc);
    }

    const size_t minimumRequiredInjectFrames =
        injectReserveFrames + injectContentDelayFrames + 2;
    const size_t desiredBaselineMaxBufferedInjectFrames =
        std::max(ce::capture_policy::GetMaxBufferedInjectFrames(injectReserveFrames, recordingOutputLive,
                                                                recordingLiveTick, GetTickCount64()),
                 minimumRequiredInjectFrames);
    int64_t timestampPhaseQpc = 0;
    for (const auto& queuedFrame : bufferedInjectFrames) {
        if (queuedFrame.rawTimestamp > 0) {
            const int64_t correctionQpc = queuedFrame.timestamp >= queuedFrame.rawTimestamp
                                              ? queuedFrame.timestamp - queuedFrame.rawTimestamp
                                              : queuedFrame.rawTimestamp - queuedFrame.timestamp;
            timestampPhaseQpc = std::max(timestampPhaseQpc, correctionQpc);
        }
    }
    const int64_t predictedSourceIntervalQpc =
        injectInputPredictor.IsCalibrated()
            ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(
                                       injectInputPredictor.SmoothedIntervalQpc())))
            : std::max<int64_t>(1, targetIntervalTicks);
    int64_t requiredTimestampSpanQpc = 0;
    if (livePlayoutTargetQpc > 0 && !bufferedInjectFrames.empty()) {
        const int64_t newestTimestampQpc = bufferedInjectFrames.back().timestamp;
        const int64_t latestEligibleQpc =
            livePlayoutTargetQpc <= std::numeric_limits<int64_t>::max() - leadToleranceQpc
                ? livePlayoutTargetQpc + leadToleranceQpc
                : std::numeric_limits<int64_t>::max();
        if (newestTimestampQpc > latestEligibleQpc) {
            requiredTimestampSpanQpc = newestTimestampQpc - latestEligibleQpc;
        }
    }
    const size_t maximumRetentionLimit =
        ce::capture_policy::GetInjectRetentionCeiling(
            minimumRequiredInjectFrames, static_cast<size_t>(FRAME_RING_SIZE),
            static_cast<size_t>(SHARED_TEXTURE_SLOT_COUNT));
    const size_t baselineMaxBufferedInjectFrames =
        std::min(desiredBaselineMaxBufferedInjectFrames, maximumRetentionLimit);
    const size_t maxBufferedInjectFrames =
        recordingOutputLive
            ? ce::capture_policy::GetInjectTimestampRetentionLimit(
                  baselineMaxBufferedInjectFrames, injectReserveFrames, requiredTimestampSpanQpc,
                  predictedSourceIntervalQpc, maximumRetentionLimit)
            : baselineMaxBufferedInjectFrames;
    injectTimestampRetentionLimit = maxBufferedInjectFrames;
    injectTimestampPhaseCurrentQpc = timestampPhaseQpc;
    injectTimestampPhaseMaxQpc = std::max(injectTimestampPhaseMaxQpc, timestampPhaseQpc);
    injectTimestampPhaseReservePeak =
        std::max(injectTimestampPhaseReservePeak,
                 maxBufferedInjectFrames > baselineMaxBufferedInjectFrames
                     ? maxBufferedInjectFrames - baselineMaxBufferedInjectFrames
                     : 0);
    maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
    uint32_t trimmedInjectFrames = 0;
    while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
        const bool preserveFront =
            ce::capture_policy::ShouldPreserveInjectFrontAtBufferCap(
                bufferedInjectFrames.front().timestamp, livePlayoutTargetQpc, leadToleranceQpc);
        const size_t trimIndex =
            preserveFront && bufferedInjectFrames.size() > protectedInjectTailFrames + 1
                ? bufferedInjectFrames.size() - protectedInjectTailFrames - 1
                : 0;
        auto trimIt = bufferedInjectFrames.begin() + static_cast<std::ptrdiff_t>(trimIndex);
        QueuedFrame staleFrame = std::move(*trimIt);
        bufferedInjectFrames.erase(trimIt);
        DiscardQueuedFrame(staleFrame);
        if (trimIndex != 0) {
            ++injectFrontPreserveTrimTotal;
        }
        ++trimmedInjectFrames;
        ++injectBufferCapTrimTotal;
    }
    if (trimmedInjectFrames > 0) {
        pendingInjectTrimmedLogCount += trimmedInjectFrames;
        media_main_g_InjectBufferedTrimmedFrames.fetch_add(trimmedInjectFrames, std::memory_order_relaxed);
        if (media_main_g_pSharedMem) {
            media_main_g_pSharedMem->runtimeState.injectTrimmedFrames.fetch_add(trimmedInjectFrames,
                                                                     std::memory_order_relaxed);
        }
        if (lastDeferredLineage.IsValid() && !bufferedInjectFrames.empty() &&
            !std::any_of(bufferedInjectFrames.begin(), bufferedInjectFrames.end(),
                         [&](const QueuedFrame& candidate) {
                             return MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                         })) {
            lastDeferredLineage = {};
        }
    }
    DWORD now = GetTickCount();
    if (pendingInjectTrimmedLogCount > 0 && now - lastInjectTrimLog >= 1000) {
        LogInfo(
            "[EncoderThread] Trimmed %u inject frame(s) at the hard buffer cap "
            "(peak=%zu cap=%zu fenceTail=%zu delayFrames=%zu phaseUs=%lld requiredSpanUs=%lld "
            "sourceIntervalUs=%lld preserveFrontTotal=%llu total=%llu)",
            pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
            protectedInjectTailFrames, injectContentDelayFrames,
            static_cast<long long>(qpcToUs(timestampPhaseQpc)),
            static_cast<long long>(qpcToUs(requiredTimestampSpanQpc)),
            static_cast<long long>(qpcToUs(predictedSourceIntervalQpc)),
            static_cast<unsigned long long>(injectFrontPreserveTrimTotal),
            static_cast<unsigned long long>(injectBufferCapTrimTotal));
        pendingInjectTrimmedLogCount = 0;
        maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
        lastInjectTrimLog = now;
    }

    auto recordInjectTargetDrop = [&](QueuedFrame& stale) {
        DiscardQueuedFrame(stale);
        media_main_g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
        if (media_main_g_pSharedMem) {
            media_main_g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
        }
        ++injectTargetSupersededThisWindow;
        ++injectTargetSupersededTotal;
    };
    auto eligibleInjectFrameCount = [&]() -> size_t {
        return bufferedInjectFrames.size() > protectedInjectTailFrames
                   ? bufferedInjectFrames.size() - protectedInjectTailFrames
                   : 0;
    };
    auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
        return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                       lastEmittedInjectSourceQpc);
    };

    // Remove only frames that can never be emitted again. Unlike the old wall-age trim,
    // this is relative to committed source lineage and cannot delete an intentional
    // delayed frame merely because the encoder thread is currently later than it.
    while (eligibleInjectFrameCount() > 0 && !isFreshInjectCandidate(bufferedInjectFrames.front())) {
        QueuedFrame obsolete = std::move(bufferedInjectFrames.front());
        bufferedInjectFrames.pop_front();
        recordInjectTargetDrop(obsolete);
    }

    if (!media_main_g_EncoderRunning && !bufferedInjectFrames.empty()) {
        frame = std::move(bufferedInjectFrames.front());
        bufferedInjectFrames.pop_front();
        popped = true;
        lastDeferredLineage = {};
    } else if (!recordingOutputLive || encoderGridStartQpc <= 0 || targetIntervalTicks <= 0) {
        // Warmup/startup: the readiness gate below builds the content-delay history. Pop
        // the oldest eligible source so the eventual first live frame is causal.
        if (eligibleInjectFrameCount() > 0) {
            frame = std::move(bufferedInjectFrames.front());
            bufferedInjectFrames.pop_front();
            popped = true;
            lastDeferredLineage = {};
        }
    } else {
        const size_t availableCount = eligibleInjectFrameCount();
        const int64_t playoutTargetQpc = livePlayoutTargetQpc;
        auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
            return isFreshInjectCandidate(candidate) &&
                   !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
        };
        size_t bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                          playoutTargetQpc, isAllowedCandidate);
        bool usedDeferredFallback = false;
        if (bestIdx >= availableCount) {
            bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                      playoutTargetQpc, isFreshInjectCandidate);
            usedDeferredFallback = lastDeferredLineage.IsValid() && bestIdx < availableCount;
        }

        if (bestIdx < availableCount) {
            const int64_t selectedTimestamp = bufferedInjectFrames[bestIdx].timestamp;
            const auto decision = ce::capture_policy::DecideCfrNearestPlayout(
                selectedTimestamp, playoutTargetQpc, leadToleranceQpc, lastEmittedInjectSourceQpc);
            if (decision.emit) {
                if (updateInjectOverloadRepeatPacer(true).repeat) {
                    // Keep every candidate for a later immutable output slot.
                    // The scheduled slot is emitted from the cached frame.
                    return;
                }
                if (bestIdx > 0) {
                    ++selectionLogCounter;
                    if (selectionLogCounter <= 12 || (selectionLogCounter % 240) == 0) {
                        LogInfo(
                            "[EncoderThread] Inject target select tick=%lld targetQpc=%lld chose idx=%zu "
                            "frame=%u tex=%d ts=%lld oldest=%lld avail=%zu fenceTail=%zu "
                            "delayFrames=%zu delayUs=%lld%s",
                            static_cast<long long>(selectionGridTick),
                            static_cast<long long>(playoutTargetQpc), bestIdx,
                            bufferedInjectFrames[bestIdx].frameIndex,
                            bufferedInjectFrames[bestIdx].textureIndex,
                            static_cast<long long>(selectedTimestamp),
                            static_cast<long long>(bufferedInjectFrames.front().timestamp), availableCount,
                            protectedInjectTailFrames, injectContentDelayFrames,
                            static_cast<long long>(qpcToUs(avContentDelayQpc)),
                            usedDeferredFallback ? " fallback=deferred-only" : "");
                    }
                }
                for (size_t i = 0; i < bestIdx; ++i) {
                    QueuedFrame superseded = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    recordInjectTargetDrop(superseded);
                }
                frame = std::move(bufferedInjectFrames.front());
                bufferedInjectFrames.pop_front();
                popped = true;
                lastDeferredLineage = {};
                ++injectTargetSelectThisWindow;
                ++injectTargetSelectTotal;
                if (qpcFreq.QuadPart > 0) {
                    const uint64_t residualUs =
                        ce::capture_policy::GetCfrTimestampDistanceQpc(selectedTimestamp,
                                                                      playoutTargetQpc) *
                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                    injectTargetResidualMaxUs =
                        std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                }
            } else if (decision.hold) {
                updateInjectOverloadRepeatPacer(false);
                ++injectTargetHoldThisWindow;
                ++injectTargetHoldTotal;
                ++injectTargetHoldWithCandidateThisWindow;
                ++injectTargetHoldWithCandidateTotal;
                const int64_t futureLeadQpc = selectedTimestamp - playoutTargetQpc;
                if (qpcFreq.QuadPart > 0 && futureLeadQpc > qpcFreq.QuadPart / 4) {
                    static uint64_t s_largeFutureHoldLogCount = 0;
                    ++s_largeFutureHoldLogCount;
                    if (s_largeFutureHoldLogCount <= 5 ||
                        (s_largeFutureHoldLogCount % 120ull) == 0ull) {
                        const QueuedFrame& heldFrame = bufferedInjectFrames[bestIdx];
                        LogWarn(
                            "[EncoderThread] Inject candidate held far in the future: leadUs=%lld "
                            "targetQpc=%lld frame=%u tex=%d timestamp=%lld rawTimestamp=%lld "
                            "captureFlags=0x%X avail=%zu fenceTail=%zu count=%llu",
                            static_cast<long long>(qpcToUs(futureLeadQpc)),
                            static_cast<long long>(playoutTargetQpc), heldFrame.frameIndex,
                            heldFrame.textureIndex, static_cast<long long>(heldFrame.timestamp),
                            static_cast<long long>(heldFrame.rawTimestamp), heldFrame.captureFlags,
                            availableCount, protectedInjectTailFrames,
                            static_cast<unsigned long long>(s_largeFutureHoldLogCount));
                    }
                }
            } else {
                updateInjectOverloadRepeatPacer(false);
            }
        } else {
            updateInjectOverloadRepeatPacer(false);
            ++injectTargetHoldThisWindow;
            ++injectTargetHoldTotal;
        }
    }
} else {
    // VFR: keep the existing newest-frame sampling for the lowest latency.
    QueuedFrame temp;
    while (media_main_g_FrameQueue.Pop(temp, 0)) {
        if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
            DiscardQueuedFrame(temp);
            continue;
        }
        if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
            discardActivePathMismatchFrame(temp, "inject VFR queue", true);
            continue;
        }
        if (popped && !frame.isInjectMode && frame.texture) {
            frame.texture->Release();
        }
        frame = std::move(temp);
        popped = true;
    }
}
}

ce::cursor::CaptureState MediaEncoderSession::selectCursorStateForScheduledQpc(int64_t scheduledQpc, const QueuedFrame& referenceFrame, const char* outputKind) {

ce::cursor::CaptureState cursorState = referenceFrame.cursorState;
if (config.video.captureCursor && scheduledQpc > 0) {
    const uint32_t captureWidth = referenceFrame.cursorState.captureWidth != 0
                                      ? referenceFrame.cursorState.captureWidth
                                      : referenceFrame.width;
    const uint32_t captureHeight = referenceFrame.cursorState.captureHeight != 0
                                       ? referenceFrame.cursorState.captureHeight
                                       : referenceFrame.height;
    const bool cursorEmbedded = useScreenGrab && referenceFrame.wgcCursorEmbedded;
    bool useDxgiPointerTimeline = false;
    if (useScreenGrab) {
        const auto capture = media_main_g_WgcCap.Read();
        useDxgiPointerTimeline =
            capture && capture->IsUsingDesktopDuplication() &&
            media_main_g_DxgiCursorTimelinePublished.load(std::memory_order_acquire) != 0;
    }
    // Source-embedded ownership belongs to the delayed reference
    // frame, not to this live pointer sample. Publishing it here
    // would delay suppression a second time and hide the cursor
    // after an embedded -> hardware-plane transition.
    ce::cursor::Timeline& timeline = useScreenGrab ? media_main_g_WgcCursorTimeline : media_main_g_InjectCursorTimeline;
    ce::cursor::CaptureState liveState;
    if (!useDxgiPointerTimeline) {
        liveState =
            CaptureCursorSnapshot(scheduledQpc, referenceFrame.captureLeft, referenceFrame.captureTop,
                                  captureWidth, captureHeight, false);
        timeline.Publish(liveState);
    }
    const int64_t cursorTargetQpc =
        useScreenGrab ? std::max<int64_t>(1, scheduledQpc - getWgcEffectiveContentDelayQpc())
                      : scheduledQpc;
    if (!timeline.SelectAtOrBefore(cursorTargetQpc, &cursorState)) {
        cursorState = useDxgiPointerTimeline ? referenceFrame.cursorState : liveState;
    }
    // Pixel ownership is authoritative for exactly this selected
    // source frame. Remove stale source ownership from the cursor
    // history when the selected frame has a separate hardware
    // pointer, while preserving a real CURSOR_SUPPRESSED state.
    cursorState.SetSourceEmbedded(cursorEmbedded);

    static uint64_t s_cursorTimelineLogCount = 0;
    ++s_cursorTimelineLogCount;
    if (s_cursorTimelineLogCount <= 5 || (s_cursorTimelineLogCount % 600ull) == 0ull) {
        LogInfo(
            "[Cursor] CFR timeline backend=%s output=%s scheduled=%lld target=%lld source=%lld "
            "selected=%lld observed=%lld deltaUs=%lld dpi=%u size=%ux%u bounds=(%d,%d %ux%u) "
            "visible=%d embedded=%d fallback=%d coord=%s samples=%s",
            useScreenGrab ? "screen-grab" : "inject", outputKind ? outputKind : "unknown",
            static_cast<long long>(scheduledQpc), static_cast<long long>(cursorTargetQpc),
            static_cast<long long>(referenceFrame.cursorState.associationQpc),
            static_cast<long long>(cursorState.associationQpc),
            static_cast<long long>(cursorState.observedQpc),
            static_cast<long long>(qpcFreq.QuadPart > 0
                                       ? ((cursorTargetQpc - cursorState.associationQpc) * 1000000) /
                                             qpcFreq.QuadPart
                                       : 0),
            cursorState.dpi, cursorState.requestedWidth, cursorState.requestedHeight,
            cursorState.captureLeft, cursorState.captureTop, cursorState.captureWidth,
            cursorState.captureHeight, cursorState.IsVisible() ? 1 : 0,
            cursorState.IsSourceEmbedded() ? 1 : 0,
            (cursorState.flags & ce::cursor::kStateHandleVisibilityFallback) != 0 ? 1 : 0,
            cursorState.PositionIsShapeTopLeft() ? "shape-top-left" : "hotspot",
            useDxgiPointerTimeline ? "dxgi-qpc" : "grid-query");
    }
}
return cursorState;

}

MediaEncoderSession::ScreenGrabPrivacyContext MediaEncoderSession::sampleScreenGrabPrivacyContext() {

ScreenGrabPrivacyContext context;
HWND confirmedWindow = nullptr;
HMONITOR confirmedMonitor = nullptr;
const auto captureBefore = media_main_g_WgcCap.Read();
if (captureBefore) {
    captureBefore->GetTargetIdentity(&context.targetWindow, &context.targetMonitor);
}
context.focus = ce::screen_grab_privacy::CaptureStableFullscreenFocus();
const auto captureAfter = media_main_g_WgcCap.Read();
if (captureAfter) {
    captureAfter->GetTargetIdentity(&confirmedWindow, &confirmedMonitor);
}
context.stableCaptureTarget = captureBefore && captureAfter && captureBefore.get() == captureAfter.get() &&
                              context.targetWindow == confirmedWindow &&
                              context.targetMonitor == confirmedMonitor;
return context;

}

void MediaEncoderSession::observeScreenGrabPrivacyWarmup() {

if (!privacyRuntime.IsEnabled()) {
    return;
}
const auto context = sampleScreenGrabPrivacyContext();
privacyRuntime.ObserveWarmup(useScreenGrab, context.targetWindow, context.targetMonitor,
                             context.stableCaptureTarget, context.focus);

}

ce::screen_grab_privacy::GateDecision MediaEncoderSession::evaluateScreenGrabPrivacy(const QueuedFrame* freshFrame) {

if (!privacyRuntime.IsEnabled()) {
    return ce::screen_grab_privacy::GateDecision{};
}
const auto context = sampleScreenGrabPrivacyContext();
const int64_t freshFrameQpc =
    freshFrame ? (freshFrame->rawTimestamp > 0 ? freshFrame->rawTimestamp : freshFrame->timestamp) : 0;
return privacyRuntime.Evaluate(useScreenGrab, context.targetWindow, context.targetMonitor,
                               context.stableCaptureTarget, context.focus, freshFrame != nullptr,
                               freshFrameQpc);

}

void MediaEncoderSession::requestPrivacyFailClosedStop(const char* reason) {

LogError(
    "[PrivacyBlackout] FAIL-CLOSED: %s; cached video is invalidated and recording will stop rather than "
    "expose captured pixels",
    reason ? reason : "opaque-black submission failed");
privacyRuntime.ResetSource();
media_main_g_PrivacyFailClosedStopRequested.store(true, std::memory_order_release);
media_main_g_EncoderRunning.store(false, std::memory_order_release);

}

bool MediaEncoderSession::submitPrivacyBlackFrame(const QueuedFrame& referenceFrame, int64_t mediaTimestampQpc, int64_t scheduledQpc, int64_t timelineElapsedUs) {

if (!privacyRuntime.SubmitBlack(referenceFrame.texture, referenceFrame.isHDR, mediaTimestampQpc,
                                scheduledQpc, timelineElapsedUs, useScreenGrab && !config.video.useVFR)) {
    requestPrivacyFailClosedStop("opaque-black frame submission failed");
    return false;
}
return true;

}

bool MediaEncoderSession::repeatLastFrameForScheduledQpc(int64_t scheduledQpc) {

if (useScreenGrab && privacyRuntime.IsEnabled()) {
    const auto privacyDecision = evaluateScreenGrabPrivacy(nullptr);
    if (privacyDecision.useBlackFrame) {
        if (!media_main_g_HasLastFrame || media_main_g_LastFrame.isInjectMode) {
            requestPrivacyFailClosedStop("no screen-grab texture is available for an opaque-black tick");
            return false;
        }
        return submitPrivacyBlackFrame(media_main_g_LastFrame, scheduledQpc,
                                       scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc));
    }
}
ce::cursor::CaptureState cursorState;
if (config.video.captureCursor && media_main_g_HasLastFrame && !privacyRuntime.RepeatCacheIsBlack()) {
    cursorState = selectCursorStateForScheduledQpc(scheduledQpc, media_main_g_LastFrame, "repeat");
}
bool succeeded = false;
if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
    succeeded = MediaEngine_RepeatLastFrameWithTimeline(
        scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc), &cursorState);
} else {
    succeeded = MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc, &cursorState);
}
if (succeeded && useScreenGrab && privacyRuntime.IsEnabled()) {
    privacyRuntime.CommitRepeatOutput();
}
return succeeded;

}

bool MediaEncoderSession::recoverScheduledFreshEncodeFailure(bool scheduledCfrTick, bool freshEncodeSucceeded, bool freshEncodeDeferred, int64_t scheduledQpc, const QueuedFrame* failedFrame, const char* context) {

const bool repeatCacheAvailable = MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
if (!ce::capture_policy::ShouldRepeatAfterScheduledFreshEncodeFailure(
        scheduledCfrTick, freshEncodeSucceeded, freshEncodeDeferred, hasRepeatLastFramePath,
        repeatCacheAvailable)) {
    return false;
}

// The failed WGC attempt may have changed cursor suppression to
// match pixels that were never emitted. Restore the metadata that
// belongs to the cached successful source frame before repeating.
if (failedFrame && !failedFrame->isInjectMode && hasSuccessfulWgcCursorMetadata) {
    SyncDuplicationCursorSuppression(lastSuccessfulWgcCursorEmbedded);
}

const bool repeatSucceeded = repeatLastFrameForScheduledQpc(scheduledQpc);
const bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
if (!repeatSucceeded || repeatDeferred) {
    LogWarn(
        "[EncoderThread] CFR fresh encode failed and cached-repeat recovery also failed: context=%s "
        "scheduledQpc=%lld repeatSucceeded=%d repeatDeferred=%d",
        context ? context : "unknown", static_cast<long long>(scheduledQpc), repeatSucceeded ? 1 : 0,
        repeatDeferred ? 1 : 0);
    return false;
}

static uint64_t s_freshEncodeRecoveryCount = 0;
++s_freshEncodeRecoveryCount;
if (s_freshEncodeRecoveryCount <= 5 || (s_freshEncodeRecoveryCount % 120ull) == 0ull) {
    LogWarn(
        "[EncoderThread] CFR fresh encode failure recovered with cached duplicate: context=%s "
        "scheduledQpc=%lld recoveryCount=%llu",
        context ? context : "unknown", static_cast<long long>(scheduledQpc),
        static_cast<unsigned long long>(s_freshEncodeRecoveryCount));
}
return true;

}

void MediaEncoderSession::releaseWgcLeaseAfterMediaEngineCopy(QueuedFrame& encodedFrame, const char* context) {

if (encodedFrame.isInjectMode || !encodedFrame.wgcPoolLease.IsValid()) {
    return;
}

if (!hasRepeatLastFramePath) {
    static bool s_loggedHeldForFallback = false;
    if (!s_loggedHeldForFallback) {
        LogWarn(
            "[WGC] Holding encoded pool slot lease because media-engine repeat cache is unavailable "
            "(slot=%u generation=%llu context=%s). Pool pressure can rise on fallback duplicate paths.",
            encodedFrame.wgcPoolSlot, static_cast<unsigned long long>(encodedFrame.wgcPoolGeneration),
            context);
        s_loggedHeldForFallback = true;
    }
    return;
}

const uint32_t slot = encodedFrame.wgcPoolSlot;
const uint64_t generation = encodedFrame.wgcPoolGeneration;
encodedFrame.wgcPoolLease.Reset();
encodedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
encodedFrame.wgcPoolGeneration = 0;

static uint64_t s_releaseLogCount = 0;
++s_releaseLogCount;
if (s_releaseLogCount <= 5 || (s_releaseLogCount % 1000ull) == 0ull) {
    LogInfo(
        "[WGC] Pool slot lease released after media-engine copy: slot=%u generation=%llu context=%s "
        "releaseCount=%llu",
        slot, static_cast<unsigned long long>(generation), context,
        static_cast<unsigned long long>(s_releaseLogCount));
}

}

ce::capture_policy::CfrOverloadRepeatPacerDecision
MediaEncoderSession::updateInjectOverloadRepeatPacer(bool freshCandidateAvailable) {
    auto& runtime = injectOverloadRepeatRuntime;
    const double targetFps = static_cast<double>(std::max(config.video.fps, 1));
    const double predictedFps = injectInputPredictor.GetPredictedFps(qpcFreq.QuadPart);
    // Never spend a scarce source frame to relieve encoder pressure. This
    // drops out quickly when FG/MFG is suspended for a cutscene, while a 2x ->
    // 4x transition remains comfortably above the near-target floor.
    const bool sourceHealthy = ce::capture_policy::IsCfrSourceHealthyForOverloadPacing(
        smoothedInputPerTick, injectInputPredictor.IsCalibrated(), predictedFps, targetFps);
    const bool repeatAvailable =
        media_main_g_HasLastFrame && media_main_g_LastFrame.isInjectMode &&
        MediaEngine_RepeatLastFrame && MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
    const uint32_t overloadFlags = loadEncoderOverloadFlags();
    const bool capacityPressure =
        outputShortfallTicks > 0 || overloadFlags != 0 ||
        media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
    const auto decision = ce::capture_policy::UpdateCfrOverloadRepeatPacer(
        runtime.pacer, recordingOutputLive && !activeScreenGrab && !config.video.useVFR,
        sourceHealthy, capacityPressure, freshCandidateAvailable, repeatAvailable,
        runtime.freshServiceMs, runtime.repeatServiceMs, frameIntervalMs,
        runtime.freshServiceSamples, runtime.repeatServiceSamples);
    injectProactiveOverloadRepeatThisTick = decision.repeat;

    if (decision.entered || decision.exited) {
        LogInfo(
            "[Inject CFR] Overload repeat pacer %s: reason=%s fresh=%.2fms/%u repeat=%.2fms/%u "
            "budget=%.2fms freshFraction=%.3f shortfall=%u buffered=%zu source=%.1ffps/%.2f "
            "repeats=%llu probes=%llu (CFR PTS and audio timeline unchanged)",
            decision.entered ? "entered" : "exited", decision.reason,
            runtime.freshServiceMs, runtime.freshServiceSamples, runtime.repeatServiceMs,
            runtime.repeatServiceSamples, decision.serviceBudgetMs, decision.freshFraction,
            outputShortfallTicks, bufferedInjectFrames.size(), predictedFps, smoothedInputPerTick,
            static_cast<unsigned long long>(runtime.pacer.proactiveRepeats),
            static_cast<unsigned long long>(runtime.pacer.probeRepeats));
    } else if (decision.probing &&
               (runtime.pacer.probeRepeats <= 3 || (runtime.pacer.probeRepeats % 8ull) == 0ull)) {
        LogInfo(
            "[Inject CFR] Measuring cached-repeat service: probe=%llu fresh=%.2fms/%u "
            "shortfall=%u buffered=%zu",
            static_cast<unsigned long long>(runtime.pacer.probeRepeats), runtime.freshServiceMs,
            runtime.freshServiceSamples, outputShortfallTicks, bufferedInjectFrames.size());
    }
    return decision;
}

void MediaEncoderSession::observeInjectFreshService(double wallServiceMs, double pureServiceMs) {
    ce::capture_policy::UpdateCfrServiceTimeEma(
        wallServiceMs, pureServiceMs, media_main_kEncodeEmaAlpha,
        injectOverloadRepeatRuntime.freshServiceMs, injectOverloadRepeatRuntime.freshServiceSamples);
}

void MediaEncoderSession::observeInjectRepeatService(double wallServiceMs, double pureServiceMs) {
    ce::capture_policy::UpdateCfrServiceTimeEma(
        wallServiceMs, pureServiceMs, media_main_kEncodeEmaAlpha,
        injectOverloadRepeatRuntime.repeatServiceMs, injectOverloadRepeatRuntime.repeatServiceSamples);
}

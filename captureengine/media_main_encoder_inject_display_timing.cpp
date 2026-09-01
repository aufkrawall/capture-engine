#include "media_main_internal.h"
#include "media_main_encoder_session.h"

#include <limits>

void MediaEncoderSession::RefreshInjectFinalOutputDisplayTiming(size_t firstNewBufferedFrame) {
    if (!media_main_g_pSharedMem || qpcFreq.QuadPart <= 0)
        return;

    bool sawFinalOutput = false;
    auto resetCorrelation = [&]() {
        injectDisplayTimingFallbackCount += injectDisplayTimingObservations.size();
        for (const auto& observation : injectDisplayTimingObservations) {
            for (auto& buffered : bufferedInjectFrames) {
                if (buffered.ringIndex == observation.ringIndex &&
                    buffered.frameIndex == observation.frameIndex) {
                    buffered.captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                    break;
                }
            }
        }
        injectDisplayTimingObservations.clear();
        injectDisplayTimingLastMatchedSequence = 0;
        injectDisplayTimingOffsetValid = false;
        injectDisplayTimingOffsetQpc = 0;
        injectDisplayTimingPhaseMismatchStreak = 0;
    };

    const uint64_t publicationGeneration =
        media_main_g_pSharedMem->displayTiming.publicationGeneration.load(std::memory_order_acquire);
    if ((publicationGeneration & 1u) == 0) {
        const uint32_t activeGeneration = static_cast<uint32_t>(publicationGeneration);
        if (injectDisplayTimingActiveGeneration != activeGeneration) {
            resetCorrelation();
            injectDisplayTimingActiveGeneration = activeGeneration;
            for (auto& queuedFrame : bufferedInjectFrames) {
                if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK) != 0 &&
                    queuedFrame.displayTimingGeneration != activeGeneration) {
                    queuedFrame.captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                }
            }
        }
    }
    firstNewBufferedFrame = std::min(firstNewBufferedFrame, bufferedInjectFrames.size());

    // A game may temporarily suspend DLSS-G around a cutscene and switch the
    // capture source between final displayed outputs and ordinary application
    // Presents. Detect that boundary from the frame metadata. Correlation from
    // the old path must not calibrate the new one, while the base-Present path
    // is rebased once to the preceding adjusted timestamp so source lineage
    // stays continuous instead of freezing until the CPU clock catches up with
    // the old display phase.
    auto addTimestampOffset = [](int64_t timestampQpc, int64_t offsetQpc) {
        if (offsetQpc > 0 && timestampQpc > std::numeric_limits<int64_t>::max() - offsetQpc)
            return std::numeric_limits<int64_t>::max();
        if (offsetQpc < 0 && timestampQpc < std::numeric_limits<int64_t>::min() - offsetQpc)
            return std::numeric_limits<int64_t>::min();
        return timestampQpc + offsetQpc;
    };
    int64_t previousScannedTimestampQpc =
        firstNewBufferedFrame > 0 ? bufferedInjectFrames[firstNewBufferedFrame - 1].timestamp
                                  : lastEmittedInjectSourceQpc;
    for (size_t index = firstNewBufferedFrame; index < bufferedInjectFrames.size(); ++index) {
        QueuedFrame& queuedFrame = bufferedInjectFrames[index];
        const bool finalOutput =
            (queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT) != 0;
        const int64_t rawTimestamp =
            queuedFrame.rawTimestamp > 0 ? queuedFrame.rawTimestamp : queuedFrame.timestamp;
        if (!injectTimestampPathKnown) {
            injectTimestampPathKnown = true;
            injectTimestampFinalOutputPathActive = finalOutput;
            injectNonFinalTimestampOffsetQpc = 0;
        } else if (finalOutput != injectTimestampFinalOutputPathActive) {
            const bool previousFinalOutput = injectTimestampFinalOutputPathActive;

            resetCorrelation();
            injectTimestampFinalOutputPathActive = finalOutput;
            ++injectTimestampPathTransitionCount;
            injectNonFinalTimestampOffsetQpc =
                !finalOutput
                    ? ce::capture_policy::GetCapturePathContinuityOffsetQpc(
                          previousScannedTimestampQpc, rawTimestamp)
                    : 0;
            if (injectTimestampPathTransitionCount <= 8 ||
                (injectTimestampPathTransitionCount % 120ull) == 0) {
                LogInfo(
                    "[Inject Timestamp Path] transition=%s->%s frame=%u rawTimestamp=%lld "
                    "continuityOffsetUs=%lld count=%llu",
                    previousFinalOutput ? "final-output" : "base-present",
                    finalOutput ? "final-output" : "base-present", queuedFrame.frameIndex,
                    static_cast<long long>(rawTimestamp),
                    static_cast<long long>(qpcToUs(injectNonFinalTimestampOffsetQpc)),
                    static_cast<unsigned long long>(injectTimestampPathTransitionCount));
            }
        }

        if (rawTimestamp > 0) {
            int64_t provisionalTimestampQpc = rawTimestamp;
            if (!finalOutput) {
                provisionalTimestampQpc =
                    addTimestampOffset(rawTimestamp, injectNonFinalTimestampOffsetQpc);
            }
            if (previousScannedTimestampQpc > 0 &&
                provisionalTimestampQpc <= previousScannedTimestampQpc) {
                provisionalTimestampQpc =
                    previousScannedTimestampQpc == std::numeric_limits<int64_t>::max()
                        ? previousScannedTimestampQpc
                        : previousScannedTimestampQpc + 1;
            }
            queuedFrame.timestamp = provisionalTimestampQpc;
            previousScannedTimestampQpc = provisionalTimestampQpc;
        }
    }

    // Queue drains can straddle several rapid FG on/off transitions. Only the
    // newest contiguous path segment may consume the current correlation
    // state. Timestamps on older segments are already frozen in their own
    // domain and remain valid selection history.
    size_t activePathSegmentStart = bufferedInjectFrames.size();
    while (activePathSegmentStart > 0) {
        const bool finalOutput =
            (bufferedInjectFrames[activePathSegmentStart - 1].captureFlags &
             SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT) != 0;
        if (finalOutput != injectTimestampFinalOutputPathActive)
            break;
        --activePathSegmentStart;
    }
    for (size_t index = 0; index < activePathSegmentStart; ++index) {
        bufferedInjectFrames[index].captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
    }
    const size_t firstNewActivePathFrame = std::max(firstNewBufferedFrame, activePathSegmentStart);
    for (size_t index = firstNewActivePathFrame; index < bufferedInjectFrames.size(); ++index) {
        const QueuedFrame& queuedFrame = bufferedInjectFrames[index];
        if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT) == 0)
            continue;
        sawFinalOutput = true;
        if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK) == 0) {
            continue;
        }

        // Keep correlation evidence after the texture/frame itself has been
        // selected and released. NVIDIA's ETW sample is deliberately reordered
        // for 24 ms, while low-latency CFR may consume the frame sooner; a late
        // exact sample still calibrates the display phase of future outputs.
        if (injectDisplayTimingObservations.size() >= DISPLAY_TIMING_RING_SIZE) {
            const auto expired = injectDisplayTimingObservations.front();
            injectDisplayTimingObservations.pop_front();
            for (auto& buffered : bufferedInjectFrames) {
                if (buffered.ringIndex == expired.ringIndex &&
                    buffered.frameIndex == expired.frameIndex) {
                    buffered.captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                    break;
                }
            }
            ++injectDisplayTimingFallbackCount;
        }
        injectDisplayTimingObservations.push_back(
            {queuedFrame.displayTimingSequence, queuedFrame.displayTimingGeneration,
             queuedFrame.rawTimestamp > 0 ? queuedFrame.rawTimestamp : queuedFrame.timestamp,
             queuedFrame.ringIndex, queuedFrame.frameIndex});
    }

    auto findBufferedObservation = [&](const InjectDisplayTimingObservation& observation) -> QueuedFrame* {
        for (auto& queuedFrame : bufferedInjectFrames) {
            if (queuedFrame.ringIndex == observation.ringIndex &&
                queuedFrame.frameIndex == observation.frameIndex) {
                return &queuedFrame;
            }
        }
        return nullptr;
    };

    for (auto it = injectDisplayTimingObservations.begin(); it != injectDisplayTimingObservations.end();) {
        const int64_t targetTimestampQpc =
            injectDisplayTimingOffsetValid
                ? it->virtualTimestampQpc + injectDisplayTimingOffsetQpc
                : it->virtualTimestampQpc;
        uint64_t matchedSequence = 0;
        int64_t displayTimestampQpc = 0;
        const int64_t maximumDistanceQpc =
            injectDisplayTimingOffsetValid ? std::max<int64_t>(1, qpcFreq.QuadPart / 50)
                                           : std::max<int64_t>(1, qpcFreq.QuadPart / 2);
        const auto resolution = ce::capture_policy::ResolveDisplayTimingAfterWatermark(
            media_main_g_pSharedMem->displayTiming, it->publicationWatermark, it->generation,
            injectDisplayTimingLastMatchedSequence + 1, targetTimestampQpc, qpcFreq.QuadPart,
            maximumDistanceQpc, &matchedSequence, &displayTimestampQpc);
        if (resolution == ce::capture_policy::DisplayTimingResolution::kPending) {
            // Correlations are ordered. Letting a later frame consume a sample
            // while the older one is pending would phase-shift the whole tail
            // when its delayed ETW event finally arrives.
            break;
        }

        QueuedFrame* buffered = findBufferedObservation(*it);
        if (resolution == ce::capture_policy::DisplayTimingResolution::kResolved) {
            injectDisplayTimingPhaseMismatchStreak = 0;
            injectDisplayTimingLastMatchedSequence = matchedSequence;
            const int64_t measuredOffsetQpc = displayTimestampQpc - it->virtualTimestampQpc;
            if (injectDisplayTimingOffsetValid) {
                injectDisplayTimingOffsetQpc = (injectDisplayTimingOffsetQpc * 7 + measuredOffsetQpc) / 8;
            } else {
                injectDisplayTimingOffsetQpc = measuredOffsetQpc;
                injectDisplayTimingOffsetValid = true;
            }
            if (buffered) {
                buffered->timestamp =
                    ce::capture_policy::NormalizeFinalOutputDisplayTimestampQpc(
                        it->virtualTimestampQpc, displayTimestampQpc,
                        injectDisplayTimingOffsetQpc);
                buffered->captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                buffered->captureFlags |= SHARED_FRAME_CAPTURE_DISPLAY_TIMING_RESOLVED;
            }
            ++injectDisplayTimingResolvedCount;
        } else {
            if (buffered)
                buffered->captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
            ++injectDisplayTimingFallbackCount;
            if (resolution == ce::capture_policy::DisplayTimingResolution::kPhaseMismatch &&
                injectDisplayTimingOffsetValid) {
                ++injectDisplayTimingPhaseMismatchStreak;
                if (ce::capture_policy::ShouldReacquireFinalOutputDisplayPhase(
                        injectDisplayTimingPhaseMismatchStreak)) {
                    const int64_t retiredPhaseQpc = injectDisplayTimingOffsetQpc;
                    injectDisplayTimingOffsetValid = false;
                    injectDisplayTimingOffsetQpc = 0;
                    injectDisplayTimingPhaseMismatchStreak = 0;
                    ++injectDisplayTimingPhaseReacquireCount;
                    LogInfo(
                        "[Inject DLSS FG] display transport phase reacquire: retiredPhaseUs=%lld "
                        "count=%llu (virtual CFR cadence remained authoritative)",
                        static_cast<long long>(qpcToUs(retiredPhaseQpc)),
                        static_cast<unsigned long long>(injectDisplayTimingPhaseReacquireCount));
                }
            } else {
                injectDisplayTimingPhaseMismatchStreak = 0;
            }
        }
        it = injectDisplayTimingObservations.erase(it);
        sawFinalOutput = true;
    }

    const uint64_t pendingThisPass = injectDisplayTimingObservations.size();
    injectDisplayTimingPendingCount += pendingThisPass;
    ce::capture_policy::FinalOutputTimestampOrderState timestampOrder;
    timestampOrder.previousQpc = lastEmittedInjectSourceQpc;
    for (size_t index = 0; index < bufferedInjectFrames.size(); ++index) {
        QueuedFrame& queuedFrame = bufferedInjectFrames[index];
        const bool finalOutput =
            (queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT) != 0;
        const bool activeFinalOutput = finalOutput && injectTimestampFinalOutputPathActive &&
                                       index >= activePathSegmentStart;
        if (!activeFinalOutput) {
            timestampOrder.previousQpc = std::max(timestampOrder.previousQpc, queuedFrame.timestamp);
            timestampOrder.accumulatedShiftQpc = 0;
            sawFinalOutput = sawFinalOutput || finalOutput;
            continue;
        }

        sawFinalOutput = true;
        const int64_t virtualTimestamp =
            queuedFrame.rawTimestamp > 0 ? queuedFrame.rawTimestamp : queuedFrame.timestamp;
        const bool alreadyResolved =
            (queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_RESOLVED) != 0;
        int64_t candidateTimestamp =
            alreadyResolved ? queuedFrame.timestamp : virtualTimestamp;

        // Resolved frames retain display-cadence residuals around the virtual
        // clock. Pending frames remain on that virtual clock; the common
        // virtual-to-display phase is correlation evidence only and must never
        // deepen the recording's leased texture history.
        queuedFrame.timestamp =
            ce::capture_policy::PreserveFinalOutputTimestampOrder(timestampOrder, candidateTimestamp);
    }

    const DWORD now = GetTickCount();
    if (sawFinalOutput && now - injectDisplayTimingLastLog >= 1000) {
        LogInfo(
            "[Inject DLSS FG] final-output timing: resolved=%llu fallback=%llu pendingNow=%llu "
            "pendingPassSum=%llu correlationPhaseUs=%lld timestampCorrectionUs=%lld displayStatus=%u "
            "writeSequence=%llu retentionCap=%zu phaseReservePeak=%zu path=%s transitions=%llu "
            "phaseReacquire=%llu mismatchStreak=%u",
            static_cast<unsigned long long>(injectDisplayTimingResolvedCount),
            static_cast<unsigned long long>(injectDisplayTimingFallbackCount),
            static_cast<unsigned long long>(pendingThisPass),
            static_cast<unsigned long long>(injectDisplayTimingPendingCount),
            static_cast<long long>(injectDisplayTimingOffsetValid ? qpcToUs(injectDisplayTimingOffsetQpc) : 0),
            static_cast<long long>(qpcToUs(injectTimestampPhaseCurrentQpc)),
            static_cast<unsigned>(media_main_g_pSharedMem->displayTiming.GetStatus()),
            static_cast<unsigned long long>(
                media_main_g_pSharedMem->displayTiming.writeSequence.load(std::memory_order_acquire)),
            injectTimestampRetentionLimit, injectTimestampPhaseReservePeak,
            injectTimestampFinalOutputPathActive ? "final-output" : "base-present",
            static_cast<unsigned long long>(injectTimestampPathTransitionCount),
            static_cast<unsigned long long>(injectDisplayTimingPhaseReacquireCount),
            injectDisplayTimingPhaseMismatchStreak);
        injectDisplayTimingLastLog = now;
    }
}

#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::RefreshInjectFinalOutputDisplayTiming(size_t firstNewBufferedFrame) {
    if (!media_main_g_pSharedMem || qpcFreq.QuadPart <= 0)
        return;

    bool sawFinalOutput = false;
    const uint64_t publicationGeneration =
        media_main_g_pSharedMem->displayTiming.publicationGeneration.load(std::memory_order_acquire);
    if ((publicationGeneration & 1u) == 0) {
        const uint32_t activeGeneration = static_cast<uint32_t>(publicationGeneration);
        if (injectDisplayTimingActiveGeneration != activeGeneration) {
            injectDisplayTimingFallbackCount += injectDisplayTimingObservations.size();
            injectDisplayTimingObservations.clear();
            injectDisplayTimingActiveGeneration = activeGeneration;
            injectDisplayTimingLastMatchedSequence = 0;
            injectDisplayTimingOffsetValid = false;
            injectDisplayTimingOffsetQpc = 0;
            for (auto& queuedFrame : bufferedInjectFrames) {
                if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK) != 0 &&
                    queuedFrame.displayTimingGeneration != activeGeneration) {
                    queuedFrame.captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                }
            }
        }
    }
    firstNewBufferedFrame = std::min(firstNewBufferedFrame, bufferedInjectFrames.size());
    for (size_t index = firstNewBufferedFrame; index < bufferedInjectFrames.size(); ++index) {
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
                                           : std::max<int64_t>(1, qpcFreq.QuadPart / 4);
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
            injectDisplayTimingLastMatchedSequence = matchedSequence;
            const int64_t measuredOffsetQpc = displayTimestampQpc - it->virtualTimestampQpc;
            if (injectDisplayTimingOffsetValid) {
                injectDisplayTimingOffsetQpc = (injectDisplayTimingOffsetQpc * 7 + measuredOffsetQpc) / 8;
            } else {
                injectDisplayTimingOffsetQpc = measuredOffsetQpc;
                injectDisplayTimingOffsetValid = true;
            }
            if (buffered) {
                buffered->timestamp = displayTimestampQpc;
                buffered->captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
                buffered->captureFlags |= SHARED_FRAME_CAPTURE_DISPLAY_TIMING_RESOLVED;
            }
            ++injectDisplayTimingResolvedCount;
        } else {
            if (buffered)
                buffered->captureFlags &= ~SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
            ++injectDisplayTimingFallbackCount;
        }
        it = injectDisplayTimingObservations.erase(it);
        sawFinalOutput = true;
    }

    const uint64_t pendingThisPass = injectDisplayTimingObservations.size();
    injectDisplayTimingPendingCount += pendingThisPass;
    ce::capture_policy::FinalOutputTimestampOrderState timestampOrder;
    timestampOrder.previousQpc = lastEmittedInjectSourceQpc;
    for (auto& queuedFrame : bufferedInjectFrames) {
        if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT) == 0) {
            timestampOrder.previousQpc = std::max(timestampOrder.previousQpc, queuedFrame.timestamp);
            continue;
        }

        sawFinalOutput = true;
        const int64_t virtualTimestamp =
            queuedFrame.rawTimestamp > 0 ? queuedFrame.rawTimestamp : queuedFrame.timestamp;
        const bool alreadyResolved =
            (queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_RESOLVED) != 0;
        int64_t candidateTimestamp = alreadyResolved
                                         ? queuedFrame.timestamp
                                         : (injectDisplayTimingOffsetValid
                                                ? virtualTimestamp + injectDisplayTimingOffsetQpc
                                                : virtualTimestamp);

        // Pending frames follow the most recently measured display phase. The
        // virtual intervals remain authoritative, so a delayed ETW packet cannot
        // make the buffered source order regress.
        if ((queuedFrame.captureFlags & SHARED_FRAME_CAPTURE_DISPLAY_TIMING_RESOLVED) == 0 &&
            injectDisplayTimingOffsetValid) {
            candidateTimestamp = virtualTimestamp + injectDisplayTimingOffsetQpc;
        }
        queuedFrame.timestamp =
            ce::capture_policy::PreserveFinalOutputTimestampOrder(timestampOrder, candidateTimestamp);
    }

    const DWORD now = GetTickCount();
    if (sawFinalOutput && now - injectDisplayTimingLastLog >= 1000) {
        LogInfo(
            "[Inject DLSS FG] final-output timing: resolved=%llu fallback=%llu pendingNow=%llu "
            "pendingPassSum=%llu phaseOffsetUs=%lld displayStatus=%u writeSequence=%llu",
            static_cast<unsigned long long>(injectDisplayTimingResolvedCount),
            static_cast<unsigned long long>(injectDisplayTimingFallbackCount),
            static_cast<unsigned long long>(pendingThisPass),
            static_cast<unsigned long long>(injectDisplayTimingPendingCount),
            static_cast<long long>(injectDisplayTimingOffsetValid ? qpcToUs(injectDisplayTimingOffsetQpc) : 0),
            static_cast<unsigned>(media_main_g_pSharedMem->displayTiming.GetStatus()),
            static_cast<unsigned long long>(
                media_main_g_pSharedMem->displayTiming.writeSequence.load(std::memory_order_acquire)));
        injectDisplayTimingLastLog = now;
    }
}

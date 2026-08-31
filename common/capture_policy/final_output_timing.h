#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "../display_timing_shared.h"

namespace ce::capture_policy {

// Streamline can issue all Presents in one generated-output group in a short
// CPU burst even though the driver schedules those images on separate refresh
// intervals. This clock preserves output order and spreads the burst without
// sleeping or making the Present thread wait. Source-present boundaries refine
// the interval after each complete group.
struct FinalOutputTimelineState {
    std::atomic<int64_t> lastTimestampQpc{0};
    std::atomic<int64_t> estimatedIntervalQpc{0};
    std::atomic<int64_t> lastSourcePresentQpc{0};
    std::atomic<int64_t> sourceGroupAuthorityQpc{0};
    std::atomic<uint32_t> callbacksSinceSourcePresent{0};
    std::atomic<bool> sourceGroupIntervalValid{false};
    std::atomic<bool> captureEpochActive{false};
    std::atomic<uint64_t> sourceGroupExpiryCount{0};
    std::atomic<uint64_t> virtualLeadClampCount{0};
};

// Kept in the common policy layer so final-output producers can embed the
// state without pulling the hook logging/runtime declarations into public API
// headers. The hook-side capture-pacing helper aliases this exact type.
struct FinalOutputCadenceState {
    std::atomic<int64_t> lastCaptureUs{0};
    std::atomic<uint64_t> pacedCaptureSkipCount{0};

    void Reset() {
        lastCaptureUs.store(0, std::memory_order_release);
        pacedCaptureSkipCount.store(0, std::memory_order_release);
    }
};

inline void ResetFinalOutputTimelineClock(FinalOutputTimelineState& state) {
    state.lastTimestampQpc.store(0, std::memory_order_release);
    state.estimatedIntervalQpc.store(0, std::memory_order_release);
    state.lastSourcePresentQpc.store(0, std::memory_order_release);
    state.sourceGroupAuthorityQpc.store(0, std::memory_order_release);
    state.callbacksSinceSourcePresent.store(0, std::memory_order_release);
    state.sourceGroupIntervalValid.store(false, std::memory_order_release);
    state.sourceGroupExpiryCount.store(0, std::memory_order_release);
    state.virtualLeadClampCount.store(0, std::memory_order_release);
}

inline void ResetFinalOutputTimeline(FinalOutputTimelineState& state) {
    ResetFinalOutputTimelineClock(state);
    state.captureEpochActive.store(false, std::memory_order_release);
}

// Final-output callbacks also service the visible overlay while recording is
// idle. Never carry that idle clock phase into a later recording: a stale
// source-group estimate can otherwise place the first captured frame far in the
// future before media has any opportunity to calibrate it against display ETW.
inline bool UpdateFinalOutputCaptureEpoch(FinalOutputTimelineState& state, bool captureActive) {
    const bool wasActive = state.captureEpochActive.exchange(captureActive, std::memory_order_acq_rel);
    if (!captureActive || wasActive)
        return false;

    ResetFinalOutputTimelineClock(state);
    state.captureEpochActive.store(true, std::memory_order_release);
    return true;
}

// A fixed/dynamic MFG factor change should not leave the virtual clock one
// whole old-multiplier group ahead or behind. Preserve its phase and scale the
// strongest interval estimate by the exact ratio; the next complete source
// group remains authoritative and will replace this transition estimate.
inline void AdjustFinalOutputTimelineForMultiplierChange(FinalOutputTimelineState& state,
                                                         uint32_t previousMultiplier,
                                                         uint32_t currentMultiplier,
                                                         int64_t adjustmentQpc) {
    if (previousMultiplier < 2 || currentMultiplier < 2 || previousMultiplier == currentMultiplier)
        return;

    int64_t previousInterval = state.estimatedIntervalQpc.load(std::memory_order_acquire);
    if (previousInterval <= 0)
        return;
    const int64_t scaledInterval = std::max<int64_t>(
        1, previousInterval * static_cast<int64_t>(previousMultiplier) /
               static_cast<int64_t>(currentMultiplier));
    const int64_t authorityQpc = adjustmentQpc > 0
                                     ? adjustmentQpc
                                     : state.lastTimestampQpc.load(std::memory_order_acquire);
    state.estimatedIntervalQpc.store(scaledInterval, std::memory_order_release);
    state.sourceGroupAuthorityQpc.store(authorityQpc, std::memory_order_release);
    state.sourceGroupIntervalValid.store(authorityQpc > 0, std::memory_order_release);
}

// VkSetPresentConfigNV is carried only on the first call of a metered batch,
// but its configuration applies to the next numFramesPerBatch Present calls.
// Consume that batch with one atomic cursor so every actual WSI output remains
// identifiable even when shared Streamline state is temporarily unavailable.
inline bool ConsumeFinalOutputMeteredBatchPresent(std::atomic<uint32_t>& remainingPresents,
                                                  uint32_t newBatchSize) {
    if (newBatchSize >= 2)
        remainingPresents.store(newBatchSize, std::memory_order_release);

    uint32_t remaining = remainingPresents.load(std::memory_order_acquire);
    while (remaining != 0) {
        if (remainingPresents.compare_exchange_weak(remaining, remaining - 1,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

inline int64_t FinalOutputIntervalFromFps(int64_t qpcFrequency, float outputFps) {
    if (qpcFrequency <= 0 || outputFps < 10.0f || outputFps > 1000.0f)
        return 0;
    return std::max<int64_t>(1, std::llround(static_cast<double>(qpcFrequency) /
                                             static_cast<double>(outputFps)));
}

inline void RefineFinalOutputInterval(FinalOutputTimelineState& state, int64_t sampleIntervalQpc,
                                      int64_t qpcFrequency, uint32_t oldWeight) {
    if (sampleIntervalQpc <= 0 || qpcFrequency <= 0 || sampleIntervalQpc < qpcFrequency / 1000 ||
        sampleIntervalQpc > qpcFrequency / 10) {
        return;
    }

    int64_t previous = state.estimatedIntervalQpc.load(std::memory_order_acquire);
    for (;;) {
        int64_t refined = sampleIntervalQpc;
        if (previous > 0 && sampleIntervalQpc >= previous / 2 && sampleIntervalQpc <= previous * 2) {
            refined = (previous * static_cast<int64_t>(oldWeight) + sampleIntervalQpc) /
                      static_cast<int64_t>(oldWeight + 1);
        }
        if (state.estimatedIntervalQpc.compare_exchange_weak(previous, refined, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
            return;
        }
    }
}

inline int64_t NextFinalOutputTimestampQpc(FinalOutputTimelineState& state, int64_t callbackQpc,
                                           int64_t qpcFrequency, float observedOutputFps,
                                           bool countForSourceGroup = true) {
    if (countForSourceGroup)
        state.callbacksSinceSourcePresent.fetch_add(1, std::memory_order_relaxed);

    const int64_t observedInterval = FinalOutputIntervalFromFps(qpcFrequency, observedOutputFps);
    bool sourceGroupExpired = false;
    if (qpcFrequency > 0 && state.sourceGroupIntervalValid.load(std::memory_order_acquire)) {
        const int64_t authorityQpc = state.sourceGroupAuthorityQpc.load(std::memory_order_acquire);
        const int64_t currentEstimate = state.estimatedIntervalQpc.load(std::memory_order_acquire);
        const int64_t authorityTimeout =
            std::max<int64_t>(qpcFrequency / 4, std::max<int64_t>(1, currentEstimate) * 8);
        if (authorityQpc > 0 && callbackQpc > authorityQpc &&
            callbackQpc - authorityQpc > authorityTimeout) {
            bool expected = true;
            if (state.sourceGroupIntervalValid.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel, std::memory_order_acquire)) {
                state.sourceGroupExpiryCount.fetch_add(1, std::memory_order_relaxed);
                sourceGroupExpired = true;
            }
        }
    }
    // Presentation metrics are only a startup fallback. Streamline can issue
    // its runtime Presents in a CPU burst, so those metrics can temporarily
    // resemble the base rate or an arbitrarily high rate. A complete source
    // group is authoritative only while source boundaries keep arriving. Some
    // Streamline topologies hand every later Present to a worker, so a last
    // transition-era group must expire instead of owning the clock forever.
    if (observedInterval > 0 && !state.sourceGroupIntervalValid.load(std::memory_order_acquire))
        RefineFinalOutputInterval(state, observedInterval, qpcFrequency, sourceGroupExpired ? 0 : 7);

    int64_t interval = state.estimatedIntervalQpc.load(std::memory_order_acquire);
    if (interval <= 0)
        interval = qpcFrequency > 0 ? std::max<int64_t>(1, qpcFrequency / 120) : 1;

    int64_t previous = state.lastTimestampQpc.load(std::memory_order_acquire);
    for (;;) {
        int64_t next = callbackQpc;
        bool clampedVirtualLead = false;
        if (previous > 0) {
            next = previous + interval;
            // A real source stall starts a new phase. Ordinary Streamline burst
            // gaps remain below this threshold and retain the virtual cadence.
            if (callbackQpc > next + interval * 8) {
                next = callbackQpc;
            } else if (callbackQpc > 0) {
                // DLSS 4x can burst at most four outputs for one source frame.
                // One extra interval of slack covers boundary jitter, but no
                // physically meaningful schedule can run arbitrarily ahead of
                // the callback that supplied its pixels. Keep timestamps
                // monotonic while compressing any already-invalid lead.
                const int64_t maximumVirtualLead = std::max<int64_t>(1, interval * 4);
                const int64_t latestAllowedTimestamp = callbackQpc + maximumVirtualLead;
                if (next > latestAllowedTimestamp) {
                    next = std::max(previous + 1, latestAllowedTimestamp);
                    clampedVirtualLead = true;
                }
            }
        }
        if (state.lastTimestampQpc.compare_exchange_weak(previous, next, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
            if (clampedVirtualLead)
                state.virtualLeadClampCount.fetch_add(1, std::memory_order_relaxed);
            return next;
        }
    }
}

inline void ObserveFinalOutputSourcePresent(FinalOutputTimelineState& state, int64_t sourcePresentQpc,
                                            int64_t qpcFrequency) {
    const uint32_t outputsInCompletedGroup =
        state.callbacksSinceSourcePresent.exchange(0, std::memory_order_acq_rel);
    const int64_t previousSourceQpc =
        state.lastSourcePresentQpc.exchange(sourcePresentQpc, std::memory_order_acq_rel);
    if (previousSourceQpc <= 0 || sourcePresentQpc <= previousSourceQpc || outputsInCompletedGroup == 0)
        return;

    const int64_t groupInterval = sourcePresentQpc - previousSourceQpc;
    const int64_t outputInterval = groupInterval / static_cast<int64_t>(outputsInCompletedGroup);
    if (outputInterval <= 0 || qpcFrequency <= 0 || outputInterval < qpcFrequency / 1000 ||
        outputInterval > qpcFrequency / 10) {
        return;
    }
    // A completed source group supplies an exact count/time observation and is
    // stronger than the presentation-FPS fallback used during startup.
    RefineFinalOutputInterval(state, outputInterval, qpcFrequency, 0);
    state.sourceGroupAuthorityQpc.store(sourcePresentQpc, std::memory_order_release);
    state.sourceGroupIntervalValid.store(true, std::memory_order_release);
}

struct DisplayTimingPublicationWatermark {
    uint64_t sequence = 0;
    uint32_t generation = 0;
    bool valid = false;

    explicit operator bool() const {
        return valid;
    }
};

// Snapshot the publication cursor immediately before the runtime Present. The
// corresponding screen-change event must be newer than this watermark, but it
// is deliberately *not* assumed to be the next sequence: the ETW reducer keeps
// a reorder window and can still publish older Presents first.
inline DisplayTimingPublicationWatermark CaptureDisplayTimingPublicationWatermark(
    const SharedDisplayTiming& timing) {
    const uint64_t generationBefore = timing.publicationGeneration.load(std::memory_order_acquire);
    if ((generationBefore & 1u) != 0)
        return {};
    const DisplayTimingStatus status = timing.GetStatus();
    if (status != DisplayTimingStatus::Starting && status != DisplayTimingStatus::Active)
        return {};
    const uint64_t writeSequence = timing.writeSequence.load(std::memory_order_acquire);
    const uint64_t generationAfter = timing.publicationGeneration.load(std::memory_order_acquire);
    if (generationBefore != generationAfter)
        return {};

    DisplayTimingPublicationWatermark result;
    result.sequence = writeSequence;
    result.generation = static_cast<uint32_t>(generationBefore);
    result.valid = true;
    return result;
}

enum class DisplayTimingResolution {
    kPending,
    kResolved,
    kInvalid,
};

struct FinalOutputTimestampOrderState {
    int64_t previousQpc = 0;
    int64_t accumulatedShiftQpc = 0;
};

// Rebase a newly selected capture path exactly once when it enters a timestamp
// domain behind the preceding path (display-correlated final output -> CPU
// base-Present is the common case). The fixed segment offset preserves cadence
// without allowing ordinary encoder backlog to masquerade as a path change.
inline int64_t GetCapturePathContinuityOffsetQpc(int64_t previousAdjustedQpc,
                                                int64_t firstRawQpc) {
    if (previousAdjustedQpc <= 0 || firstRawQpc <= 0 || previousAdjustedQpc < firstRawQpc) {
        return 0;
    }
    return previousAdjustedQpc - firstRawQpc + 1;
}

// A delayed display sample can move the pending source timeline behind a frame
// the encoder already committed. Shift the remaining buffered run as a unit so
// order stays strict without compressing its generated-output intervals.
inline int64_t PreserveFinalOutputTimestampOrder(FinalOutputTimestampOrderState& state,
                                                 int64_t candidateQpc) {
    int64_t shiftedQpc = candidateQpc + state.accumulatedShiftQpc;
    if (state.previousQpc > 0 && shiftedQpc <= state.previousQpc) {
        const int64_t adjustmentQpc = state.previousQpc - shiftedQpc + 1;
        state.accumulatedShiftQpc += adjustmentQpc;
        shiftedQpc += adjustmentQpc;
    }
    state.previousQpc = shiftedQpc;
    return shiftedQpc;
}

// Resolve one virtual final-output timestamp against ordered display samples
// published after its pre-Present watermark. Waiting until a sample at or
// beyond the target exists makes the nearest choice stable. minimumSequence
// prevents a later captured output from reusing an earlier output's sample.
inline DisplayTimingResolution ResolveDisplayTimingAfterWatermark(
    const SharedDisplayTiming& timing, uint64_t publicationWatermark, uint32_t generation,
    uint64_t minimumSequence, int64_t targetTimestampQpc, int64_t qpcFrequency,
    int64_t maximumDistanceQpc, uint64_t* matchedSequence, int64_t* timestampQpc) {
    if (targetTimestampQpc <= 0 || qpcFrequency <= 0 || maximumDistanceQpc <= 0 ||
        !matchedSequence || !timestampQpc)
        return DisplayTimingResolution::kInvalid;
    if (publicationWatermark == UINT64_MAX)
        return DisplayTimingResolution::kInvalid;

    const uint64_t generationBefore = timing.publicationGeneration.load(std::memory_order_acquire);
    if ((generationBefore & 1u) != 0 || static_cast<uint32_t>(generationBefore) != generation)
        return DisplayTimingResolution::kInvalid;

    const uint64_t writeSequence = timing.writeSequence.load(std::memory_order_acquire);
    const uint64_t firstAllowedSequence =
        std::max(publicationWatermark + 1, std::max<uint64_t>(1, minimumSequence));
    if (firstAllowedSequence > writeSequence) {
        const DisplayTimingStatus status = timing.GetStatus();
        return status == DisplayTimingStatus::Starting || status == DisplayTimingStatus::Active
                   ? DisplayTimingResolution::kPending
                   : DisplayTimingResolution::kInvalid;
    }

    const uint64_t earliestAvailable =
        writeSequence >= DISPLAY_TIMING_RING_SIZE ? writeSequence - DISPLAY_TIMING_RING_SIZE + 1 : 1;
    if (firstAllowedSequence < earliestAvailable)
        return DisplayTimingResolution::kInvalid;

    uint64_t previousSequence = 0;
    int64_t previousTimestampQpc = 0;
    uint64_t selectedSequence = 0;
    int64_t selectedTimestampQpc = 0;
    for (uint64_t sequence = firstAllowedSequence;; ++sequence) {
        int64_t screenTimeUs = 0;
        if (!timing.Read(sequence, screenTimeUs))
            return DisplayTimingResolution::kInvalid;
        const int64_t candidateTimestampQpc = DisplayTimingUsToQpc(screenTimeUs, qpcFrequency);
        if (candidateTimestampQpc <= 0)
            return DisplayTimingResolution::kInvalid;
        if (candidateTimestampQpc < targetTimestampQpc) {
            previousSequence = sequence;
            previousTimestampQpc = candidateTimestampQpc;
        } else {
            selectedSequence = sequence;
            selectedTimestampQpc = candidateTimestampQpc;
            if (previousSequence != 0 &&
                targetTimestampQpc - previousTimestampQpc <= candidateTimestampQpc - targetTimestampQpc) {
                selectedSequence = previousSequence;
                selectedTimestampQpc = previousTimestampQpc;
            }
            break;
        }

        if (sequence == writeSequence)
            break;
    }

    const uint64_t generationAfter = timing.publicationGeneration.load(std::memory_order_acquire);
    if (generationBefore != generationAfter)
        return DisplayTimingResolution::kInvalid;
    if (selectedSequence == 0) {
        const DisplayTimingStatus status = timing.GetStatus();
        return status == DisplayTimingStatus::Starting || status == DisplayTimingStatus::Active
                   ? DisplayTimingResolution::kPending
                   : DisplayTimingResolution::kInvalid;
    }

    // Once the consumer has calibrated the CPU-Present to display phase, a
    // regular generated-output stream makes the nearest sample much closer
    // than one display interval. Startup deliberately supplies a wider bound:
    // NVIDIA can hold several already-Presented images in its hardware pacer,
    // so rejecting the first offset at 20 ms would prevent phase calibration
    // forever. The caller owns that confidence-dependent bound.
    const int64_t distanceQpc = selectedTimestampQpc >= targetTimestampQpc
                                    ? selectedTimestampQpc - targetTimestampQpc
                                    : targetTimestampQpc - selectedTimestampQpc;
    if (distanceQpc > maximumDistanceQpc)
        return DisplayTimingResolution::kInvalid;

    *matchedSequence = selectedSequence;
    *timestampQpc = selectedTimestampQpc;
    return DisplayTimingResolution::kResolved;
}

}  // namespace ce::capture_policy

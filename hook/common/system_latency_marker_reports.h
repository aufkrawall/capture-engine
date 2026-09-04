#pragma once

// The marker path: turning a graphics runtime's own low-latency report into
// published samples.
//
// Split out of system_latency_metrics.h, which carries the correlator itself
// and includes this file at its end. The definitions are out of line so the
// class stays readable; either header may be included first.

#include "system_latency_metrics.h"

namespace ce::system_latency {

inline bool Tracker::IsValidNativeFrame(const NativeFrameReport& frame) {
    const int64_t simulationStartUs = ToSignedTimestamp(frame.simulationStartTimeUs);
    const int64_t presentStartUs = ToSignedTimestamp(frame.presentStartTimeUs);
    return simulationStartUs > 0 && presentStartUs >= simulationStartUs &&
           presentStartUs - simulationStartUs <= kMaximumIntervalUs;
}

inline uint64_t Tracker::NativeReadyTimeUs(const NativeFrameReport& frame) {
    if (frame.gpuRenderEndTimeUs >= frame.presentStartTimeUs &&
        frame.gpuRenderEndTimeUs - frame.presentStartTimeUs <= static_cast<uint64_t>(kMaximumTotalLatencyUs)) {
        return frame.gpuRenderEndTimeUs;
    }
    return frame.presentStartTimeUs;
}

inline int64_t Tracker::MedianSimulationInterval(
    const std::array<NativeFrameReport, NativeReport::kCapacity>& frames, size_t count) {
    std::array<int64_t, NativeReport::kCapacity - 1> intervals{};
    size_t intervalCount = 0;
    for (size_t i = 1; i < count; ++i) {
        const int64_t previous = ToSignedTimestamp(frames[i - 1].simulationStartTimeUs);
        const int64_t current = ToSignedTimestamp(frames[i].simulationStartTimeUs);
        const int64_t deltaUs = current - previous;
        if (deltaUs >= kDuplicateThresholdUs && deltaUs <= kMaximumIntervalUs)
            intervals[intervalCount++] = deltaUs;
    }
    if (intervalCount == 0)
        return 0;
    std::sort(intervals.begin(), intervals.begin() + intervalCount);
    return intervals[intervalCount / 2];
}

inline void Tracker::SubmitNativeReport(const NativeReport& report) {
    if (fgType_.load(std::memory_order_relaxed) == 2)
        return;
    std::array<NativeFrameReport, NativeReport::kCapacity> validFrames{};
    size_t validCount = 0;
    const size_t reportCount = (std::min)(report.count, report.frames.size());
    for (size_t i = 0; i < reportCount; ++i) {
        const auto& frame = report.frames[i];
        if (!IsValidNativeFrame(frame))
            continue;
        validFrames[validCount++] = frame;
    }
    if (validCount == 0)
        return;

    std::sort(validFrames.begin(), validFrames.begin() + validCount,
              [](const NativeFrameReport& a, const NativeFrameReport& b) {
                  return a.presentStartTimeUs < b.presentStartTimeUs;
              });
    int64_t samplingIntervalUs = MedianSimulationInterval(validFrames, validCount);

    std::lock_guard<std::mutex> lock(mutex_);
    if (samplingIntervalUs <= 0)
        samplingIntervalUs = ResolveWorkIntervalLocked();
    markerIntervalUs_ = samplingIntervalUs;
    markerCadenceTrusted_ = !IsMarkerCadenceOutputRateLocked(samplingIntervalUs);
    if (!markerCadenceTrusted_) {
        nativeEstimatedSamples_.Clear();
        ++markerReportsRejectedForOutputCadence_;
        return;
    }
    // NVIDIA documents the average input-to-frame-start heuristic as
    // invalid below 10 FPS. Fail closed instead of publishing a number
    // whose estimated input slice is outside that supported regime.
    if (samplingIntervalUs <= 0 || samplingIntervalUs > kMaximumSamplingIntervalUs)
        return;
    for (size_t displayIndex = 0; displayIndex < displays_.Size(); ++displayIndex) {
        const int64_t screenTimeUs = displays_.At(displayIndex);
        if (screenTimeUs <= lastNativeDisplayTimeUs_)
            continue;

        // The sensor associates every displayed transition with the
        // runtime PresentStart that produced it. Use that causal boundary
        // when available: DLSS-G's asynchronous pacer can let markers for
        // newer application frames occur before an older frame reaches
        // the screen, so screenTime alone selects a future frame.
        int64_t markerCutoffUs = screenTimeUs;
        bool usedAssociation = false;
        if (displayIndex < displayPresentStarts_.Size()) {
            const int64_t associatedPresentStartUs = displayPresentStarts_.At(displayIndex);
            if (associatedPresentStartUs > 0 && associatedPresentStartUs <= screenTimeUs) {
                markerCutoffUs = associatedPresentStartUs;
                usedAssociation = true;
            }
        }

        const NativeFrameReport* candidate = nullptr;
        for (size_t frameIndex = 0; frameIndex < validCount; ++frameIndex) {
            const auto& frame = validFrames[frameIndex];
            if (frame.presentStartTimeUs <= static_cast<uint64_t>(lastNativePresentTimeUs_))
                continue;
            if (frame.presentStartTimeUs > static_cast<uint64_t>(markerCutoffUs))
                continue;
            if (NativeReadyTimeUs(frame) > static_cast<uint64_t>(screenTimeUs))
                continue;
            if (!candidate || frame.presentStartTimeUs > candidate->presentStartTimeUs)
                candidate = &frame;
        }
        if (!candidate)
            continue;

        // The generator's pacing thread presents this content after the
        // application has already submitted a newer frame, so the newest
        // marker at or before that present belongs to a simulation this
        // frame cannot be showing. Step back one application frame - the
        // one the generator is holding while it displays the frames it
        // derived from it - but keep the watermark on the unheld frame so
        // the remaining displays of the same group are not counted again.
        const NativeFrameReport* consumed = candidate;
        int64_t extraGeneratorHoldUs = 0;
        if (IsGeneratorPacingOutputLocked()) {
            const NativeFrameReport* held = nullptr;
            for (size_t frameIndex = 0; frameIndex < validCount; ++frameIndex) {
                const auto& frame = validFrames[frameIndex];
                if (frame.presentStartTimeUs >= candidate->presentStartTimeUs)
                    continue;
                if (!held || frame.presentStartTimeUs > held->presentStartTimeUs)
                    held = &frame;
            }
            if (held) {
                const int64_t candidateAgeUs = markerCutoffUs - ToSignedTimestamp(candidate->presentStartTimeUs);
                const int64_t renderTimeUs = ToSignedTimestamp(candidate->presentStartTimeUs) -
                                             ToSignedTimestamp(candidate->simulationStartTimeUs);
                const bool synchronousPresent = usedAssociation && candidateAgeUs <= 1000;
                if (!synchronousPresent && candidateAgeUs < (std::max)(renderTimeUs, samplingIntervalUs / 3)) {
                    candidate = held;
                }
            } else {
                const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
                if (fgMultiplier >= 2) {
                    const int64_t displayIntervalUs = MedianRing(displayIntervals_);
                    const int64_t effectiveDisplayIntervalUs =
                        displayIntervalUs > 0 ? displayIntervalUs : (samplingIntervalUs / fgMultiplier);
                    extraGeneratorHoldUs = (fgMultiplier - 1) * effectiveDisplayIntervalUs;
                }
            }
        }

        const int64_t presentTimeUs =
            usedAssociation ? markerCutoffUs : ToSignedTimestamp(candidate->presentStartTimeUs);
        const int64_t presentToDisplayUs = screenTimeUs - presentTimeUs;
        if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs) {
            ++samplesRejected_;
            ++samplesRejectedPresentToDisplay_;
            continue;
        }

        const int64_t simulationStartUs =
            ToSignedTimestamp(candidate->simulationStartTimeUs) - extraGeneratorHoldUs;
        int64_t displayedSamplingIntervalUs = samplingIntervalUs;
        const int64_t displayedSimulationIntervalUs = simulationStartUs - lastNativeSimulationStartTimeUs_;
        if (lastNativeSimulationStartTimeUs_ > 0 &&
            displayedSimulationIntervalUs >= kDuplicateThresholdUs &&
            displayedSimulationIntervalUs <= kMaximumIntervalUs) {
            displayedSamplingIntervalUs = (std::max)(
                displayedSamplingIntervalUs,
                MedianRingWithCandidate(nativeDisplayedSimulationIntervals_, displayedSimulationIntervalUs));
        }

        // PCL uses half a base sampling interval for average input wait,
        // then adds every complete base interval whose frame was dropped.
        // Equivalently: displayed interval minus half the base interval.
        const int64_t estimatedInputWaitUs = displayedSamplingIntervalUs - samplingIntervalUs / 2;
        if (estimatedInputWaitUs > 0) {
            const int64_t estimatedInputTimeUs = simulationStartUs - estimatedInputWaitUs;
            if (estimatedInputTimeUs > 0 && screenTimeUs >= estimatedInputTimeUs) {
                const int64_t totalUs = screenTimeUs - estimatedInputTimeUs;
                if (IsValidTotalLatency(totalUs)) {
                    nativeEstimatedSamples_.Add(static_cast<float>(totalUs) / 1000.0f, screenTimeUs);
                    nativeSimulationToDisplayUs_ = screenTimeUs - simulationStartUs;
                    nativeInputWaitUs_ = estimatedInputWaitUs;
                    nativeSamplingIntervalUs_ = samplingIntervalUs;
                    nativePresentToDisplayUs_ = presentToDisplayUs;
                    nativeUsedAssociation_ = usedAssociation;
                    nativeGeneratorHoldApplied_ = candidate != consumed || extraGeneratorHoldUs > 0;
                } else {
                    ++samplesRejected_;
                    ++samplesRejectedTotalLatency_;
                }
            }
        }

        if (lastNativeSimulationStartTimeUs_ > 0 &&
            displayedSimulationIntervalUs >= kDuplicateThresholdUs &&
            displayedSimulationIntervalUs <= kMaximumIntervalUs) {
            nativeDisplayedSimulationIntervals_.Push(displayedSimulationIntervalUs);
        }
        if (lastNativeSimulationStartTimeUs_ == 0 || simulationStartUs > lastNativeSimulationStartTimeUs_)
            lastNativeSimulationStartTimeUs_ = simulationStartUs;
        lastNativePresentTimeUs_ = ToSignedTimestamp(consumed->presentStartTimeUs);
        lastNativeDisplayTimeUs_ = screenTimeUs;
    }
}

}  // namespace ce::system_latency

#pragma once

// Correlator for the overlay's PC-latency readout.
//
// The published number is the time from the average input arrival to the frame
// carrying it reaching the screen. It is assembled from three independent
// observations, all in the same QueryPerformanceCounter microsecond domain:
//
//   frame begin -> Present call -> screen
//   (hooks)        (hooks)         (display-timing sensor, ETW)
//
// plus a modelled average input wait for the part no runtime reports. When the
// game emits Reflex/PCL markers the first two links are replaced by the game's
// own simulation-start and present-start timestamps.
//
// The pairing between a Present and the screen transition it produced is the
// load-bearing part. The sensor associates every displayed transition with the
// runtime PresentStart that caused it; without that association a render-ahead
// queue is invisible, because by the time a frame is scanned out the game has
// already called Present for the frames queued behind it.
//
// The hook's own Present observations are deliberately not part of that chain
// when the association is available. They are not one per displayed frame: a
// 144 Hz Talos session entered the wrapper 327 times a second against ~100
// published display transitions, and under frame generation the wrapper is
// entered by the generator's pacing thread at the output rate while the
// application renders at a fraction of it. The sensor's runtime PresentStart is
// the same quantity measured where it is unambiguous.

#include "system_latency_types.h"
#include "system_latency_windows.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace ce::system_latency {

class Tracker {
public:
    // frameBeginUs is the most recent low-latency boundary at or before this
    // present. With FG off every observed Present is an application source
    // frame. With FG on the final-output Present stream is deliberately kept
    // separate and ObserveApplicationPresent supplies the classified source.
    void ObservePresent(int64_t presentTimeUs, int64_t frameBeginUs = 0,
                        FrameBeginKind frameBeginKind = FrameBeginKind::Modelled) {
        // Native-report processing is infrequent but can scan up to 64 frames.
        // Never make the present hot path wait behind that diagnostic work.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            // A skipped present leaves a hole the display association has to
            // resolve against an older frame, so make the loss visible instead
            // of letting it look like extra latency.
            droppedPresents_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (presentTimeUs <= 0)
            return;

        if (!presents_.Empty()) {
            const int64_t previous = presents_.Back();
            if (presentTimeUs <= previous) {
                if (previous - presentTimeUs > kClockResetThresholdUs)
                    ResetMeasurementsLocked();
                else
                    return;
            } else {
                const int64_t intervalUs = presentTimeUs - previous;
                if (intervalUs < kDuplicateThresholdUs)
                    return;
                if (intervalUs <= kMaximumIntervalUs)
                    presentIntervals_.Push(intervalUs);
            }
        }

        presents_.Push(presentTimeUs);
        if (fgMultiplier_.load(std::memory_order_relaxed) < 2)
            RecordApplicationPresentLocked(presentTimeUs, frameBeginUs, frameBeginKind);
    }

    void ObserveApplicationPresent(int64_t presentTimeUs, int64_t frameBeginUs = 0,
                                   FrameBeginKind frameBeginKind = FrameBeginKind::Modelled) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            droppedApplicationPresents_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        RecordApplicationPresentLocked(presentTimeUs, frameBeginUs, frameBeginKind);
    }

    void ObserveDisplay(int64_t screenTimeUs, int64_t presentStartTimeUs = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (screenTimeUs <= 0)
            return;
        if (!displays_.Empty() && screenTimeUs <= displays_.Back()) {
            if (displays_.Back() - screenTimeUs > kClockResetThresholdUs)
                ResetMeasurementsLocked();
            else
                return;
        }

        if (!displays_.Empty()) {
            const int64_t displayIntervalUs = screenTimeUs - displays_.Back();
            if (displayIntervalUs >= kDuplicateThresholdUs && displayIntervalUs <= kMaximumIntervalUs)
                displayIntervals_.Push(displayIntervalUs);
        }
        displays_.Push(screenTimeUs);
        displayPresentStarts_.Push(presentStartTimeUs);
        ++displaysObserved_;
        if (presentStartTimeUs > 0)
            ++displaysWithAssociation_;
        UpdateObservedProductionStateLocked(screenTimeUs);
        UpdateFallbackLocked(screenTimeUs, presentStartTimeUs);
    }

    void SetFrameGeneration(float baseFps, int multiplier) {
        // FG publication happens on the Present path. Keep this metadata
        // lock-free so an infrequent native-report scan can never stall it.
        fgBaseFps_.store(std::isfinite(baseFps) && baseFps > 0.0f ? baseFps : 0.0f,
                         std::memory_order_relaxed);
        const int normalizedMultiplier = multiplier >= 2 ? multiplier : 1;
        const int previousMultiplier = fgMultiplier_.exchange(normalizedMultiplier, std::memory_order_relaxed);
        if (previousMultiplier != normalizedMultiplier) {
            std::lock_guard<std::mutex> lock(mutex_);
            ResetMeasurementsLocked();
            ++measurementEpochResets_;
        }
    }

    void SubmitNativeReport(const NativeReport& report) {
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
            if (IsGeneratorPacingOutputLocked()) {
                const NativeFrameReport* held = nullptr;
                for (size_t frameIndex = 0; frameIndex < validCount; ++frameIndex) {
                    const auto& frame = validFrames[frameIndex];
                    if (frame.presentStartTimeUs >= candidate->presentStartTimeUs)
                        continue;
                    if (!held || frame.presentStartTimeUs > held->presentStartTimeUs)
                        held = &frame;
                }
                if (held)
                    candidate = held;
            }

            const int64_t presentTimeUs = ToSignedTimestamp(candidate->presentStartTimeUs);
            const int64_t presentToDisplayUs = screenTimeUs - presentTimeUs;
            if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs) {
                ++samplesRejected_;
                continue;
            }

            const int64_t simulationStartUs = ToSignedTimestamp(candidate->simulationStartTimeUs);
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
                        nativeGeneratorHoldApplied_ = candidate != consumed;
                    } else {
                        ++samplesRejected_;
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

    Snapshot GetSnapshot(int64_t currentQpcUs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot snapshot{};
        snapshot.frameGenerationConfigured = fgMultiplier_.load(std::memory_order_relaxed) >= 2;
        snapshot.frameGenerationStateKnown = observedProductionState_ >= 0;
        snapshot.frameGenerationObserved = observedProductionState_ == 1;
        // During a mode/production transition neither old samples nor nominal
        // FG metadata can identify the pipeline on screen. Withhold the number
        // until measured application/display cadence resolves it.
        if (snapshot.frameGenerationConfigured && !snapshot.frameGenerationStateKnown) {
            publishedSource_ = Source::Unavailable;
            return snapshot;
        }
        if (nativeEstimatedSamples_.IsFresh(currentQpcUs))
            snapshot = nativeEstimatedSamples_.MakeSnapshot(Source::ReflexMarkers);
        else if (fallbackSamples_.IsFresh(currentQpcUs))
            snapshot = fallbackSamples_.MakeSnapshot(Source::Estimated);
        snapshot.frameGenerationConfigured = fgMultiplier_.load(std::memory_order_relaxed) >= 2;
        snapshot.frameGenerationStateKnown = observedProductionState_ >= 0;
        snapshot.frameGenerationObserved = observedProductionState_ == 1;
        if (snapshot.source != publishedSource_) {
            if (publishedSource_ != Source::Unavailable && snapshot.source != Source::Unavailable)
                ++sourceTransitions_;
            publishedSource_ = snapshot.source;
        }
        return snapshot;
    }

    // currentQpcUs selects which windows still count as fresh, matching the
    // GetSnapshot call the caller is reporting on.
    Diagnostics GetDiagnostics(int64_t currentQpcUs = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Diagnostics diagnostics{};
        if (currentQpcUs > 0) {
            const SampleWindow& crossCheck =
                publishedSource_ == Source::ReflexMarkers ? fallbackSamples_ : nativeEstimatedSamples_;
            const Source crossCheckSource =
                publishedSource_ == Source::ReflexMarkers ? Source::Estimated : Source::ReflexMarkers;
            if (publishedSource_ != Source::Unavailable && crossCheck.IsFresh(currentQpcUs)) {
                diagnostics.crossCheckSource = crossCheckSource;
                diagnostics.crossCheckMilliseconds = crossCheck.MakeSnapshot(crossCheckSource).milliseconds;
            }
        }
        diagnostics.displaysObserved = displaysObserved_;
        diagnostics.displaysWithPresentAssociation = displaysWithAssociation_;
        diagnostics.displaysWithoutMatchedPresent = displaysWithoutMatchedPresent_;
        diagnostics.presentsDroppedUnderContention = droppedPresents_.load(std::memory_order_relaxed) +
                                                     droppedApplicationPresents_.load(std::memory_order_relaxed);
        diagnostics.samplesRejectedOutOfRange = samplesRejected_;
        diagnostics.sourceTransitions = sourceTransitions_;
        if (publishedSource_ == Source::ReflexMarkers) {
            diagnostics.lastAnchorToPresentUs = nativeSimulationToDisplayUs_ - nativePresentToDisplayUs_;
            diagnostics.lastPresentToDisplayUs = nativePresentToDisplayUs_;
            diagnostics.lastInputWaitUs = nativeInputWaitUs_;
            diagnostics.lastBaseIntervalUs = nativeSamplingIntervalUs_;
            diagnostics.lastFrameBeginKind = FrameBeginKind::Modelled;
            diagnostics.lastMarkerUsedAssociation = nativeUsedAssociation_;
            diagnostics.generatorHoldApplied = nativeGeneratorHoldApplied_;
        } else {
            diagnostics.lastAnchorToPresentUs = lastAnchorToPresentUs_;
            diagnostics.lastPresentToDisplayUs = lastPresentToDisplayUs_;
            diagnostics.lastInputWaitUs = lastInputWaitUs_;
            diagnostics.lastBaseIntervalUs = lastBaseIntervalUs_;
            diagnostics.lastFrameBeginKind = lastFrameBeginKind_;
            diagnostics.generatorHoldApplied = lastGeneratorHoldApplied_;
        }
        diagnostics.displayIntervalUs = MedianRing(displayIntervals_);
        diagnostics.applicationIntervalUs = MedianRing(applicationPresentIntervals_);
        diagnostics.frameBeginIntervalUs = MedianRing(frameBeginIntervals_);
        diagnostics.markerIntervalUs = markerIntervalUs_;
        if (diagnostics.displayIntervalUs > 0 && diagnostics.applicationIntervalUs > 0) {
            diagnostics.observedOutputRatioPermille = static_cast<int>(
                (diagnostics.applicationIntervalUs * 1000 + diagnostics.displayIntervalUs / 2) /
                diagnostics.displayIntervalUs);
        }
        diagnostics.frameGenerationObserved = observedProductionState_ == 1;
        diagnostics.markerCadenceTrusted = markerCadenceTrusted_;
        diagnostics.markerReportsRejectedForOutputCadence = markerReportsRejectedForOutputCadence_;
        diagnostics.measurementEpochResets = measurementEpochResets_;
        return diagnostics;
    }

    void ResetDisplayHistory() {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetMeasurementsLocked();
        ++measurementEpochResets_;
    }

private:
    static constexpr int64_t kDuplicateThresholdUs = 100;
    static constexpr int64_t kApplicationDuplicateThresholdUs = 1'000;
    static constexpr int64_t kClockResetThresholdUs = 1'000'000;
    static constexpr int64_t kMaximumIntervalUs = 250'000;
    static constexpr int64_t kMaximumSamplingIntervalUs = 100'000;
    static constexpr int64_t kMaximumPresentToDisplayUs = 250'000;
    static constexpr int64_t kMaximumTotalLatencyUs = 500'000;
    static constexpr size_t kCadenceEvidenceCount = 6;

    static int64_t ToSignedTimestamp(uint64_t timestampUs) {
        if (timestampUs == 0 || timestampUs > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            return 0;
        return static_cast<int64_t>(timestampUs);
    }

    static bool IsValidNativeFrame(const NativeFrameReport& frame) {
        const int64_t simulationStartUs = ToSignedTimestamp(frame.simulationStartTimeUs);
        const int64_t presentStartUs = ToSignedTimestamp(frame.presentStartTimeUs);
        return simulationStartUs > 0 && presentStartUs >= simulationStartUs &&
               presentStartUs - simulationStartUs <= kMaximumIntervalUs;
    }

    static uint64_t NativeReadyTimeUs(const NativeFrameReport& frame) {
        if (frame.gpuRenderEndTimeUs >= frame.presentStartTimeUs &&
            frame.gpuRenderEndTimeUs - frame.presentStartTimeUs <= static_cast<uint64_t>(kMaximumTotalLatencyUs)) {
            return frame.gpuRenderEndTimeUs;
        }
        return frame.presentStartTimeUs;
    }

    static bool IsValidTotalLatency(int64_t totalUs) {
        return totalUs > 0 && totalUs <= kMaximumTotalLatencyUs;
    }

    static int64_t MedianSimulationInterval(
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

    int64_t ResolveFgBaseIntervalLocked() const {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const float fgBaseFps = fgBaseFps_.load(std::memory_order_relaxed);
        if (fgMultiplier < 2 || fgBaseFps <= 0.0f)
            return 0;
        return std::llround(1'000'000.0 / static_cast<double>(fgBaseFps));
    }

    int64_t ResolveWorkIntervalLocked() const {
        const int64_t applicationIntervalUs = MedianRing(applicationPresentIntervals_);
        if (applicationIntervalUs > 0)
            return applicationIntervalUs;
        // Last resort. The hook's Present cadence is not the application's:
        // it counts generated presents and, in some configurations, several
        // wrapper entries per displayed frame.
        return (std::max)(MedianRing(presentIntervals_), ResolveFgBaseIntervalLocked());
    }

    bool IsGeneratorPacingOutputLocked() const {
        return observedProductionState_ == 1;
    }

    bool IsMarkerCadenceOutputRateLocked(int64_t markerIntervalUs) const {
        if (!IsGeneratorPacingOutputLocked())
            return false;
        const int64_t applicationIntervalUs = MedianRing(applicationPresentIntervals_);
        const int64_t displayIntervalUs = MedianRing(displayIntervals_);
        if (markerIntervalUs <= 0 || applicationIntervalUs <= 0 || displayIntervalUs <= 0)
            return false;
        // A true source-frame marker follows the classified application
        // cadence. Output-rate PCL/Reflex streams are substantially faster and
        // cannot identify which generated display carries a given simulation.
        return markerIntervalUs * 4 < applicationIntervalUs * 3;
    }

    // Index in applicationPresents_ of the application frame whose simulation produced
    // the content of the runtime present at runtimePresentUs.
    //
    // The newest application source at or before that present is right only when the
    // application itself issued it: the wrapper's Present is entered before the
    // next frame can begin, so no newer boundary exists yet. A generator's
    // pacing thread presents on its own schedule, and by then the application
    // has already begun the next frame - it must have, because the generator
    // interpolates towards a frame that is complete. So exactly one boundary
    // has to be stepped back, which is the application frame the generator is
    // holding while it shows the frames derived from it.
    bool MatchApplicationPresentLocked(int64_t runtimePresentUs, size_t& matchedIndex, bool& holdApplied) const {
        holdApplied = false;
        bool found = false;
        for (size_t i = applicationPresents_.Size(); i > 0; --i) {
            if (applicationPresents_.At(i - 1) <= runtimePresentUs) {
                matchedIndex = i - 1;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        if (matchedIndex > 0 && IsGeneratorPacingOutputLocked()) {
            --matchedIndex;
            holdApplied = true;
        }
        return true;
    }

    void RecordApplicationPresentLocked(int64_t presentTimeUs, int64_t frameBeginUs, FrameBeginKind frameBeginKind) {
        if (presentTimeUs <= 0)
            return;
        if (!applicationPresents_.Empty()) {
            const int64_t intervalUs = presentTimeUs - applicationPresents_.Back();
            if (intervalUs < kApplicationDuplicateThresholdUs)
                return;
            if (intervalUs <= 0 || intervalUs > kMaximumIntervalUs)
                return;
            applicationPresentIntervals_.Push(intervalUs);
        }

        int64_t anchorUs = 0;
        FrameBeginKind anchorKind = FrameBeginKind::Modelled;
        if (frameBeginKind != FrameBeginKind::Modelled && frameBeginUs > 0 && frameBeginUs <= presentTimeUs &&
            presentTimeUs - frameBeginUs <= kMaximumIntervalUs) {
            anchorUs = frameBeginUs;
            anchorKind = frameBeginKind;
            if (lastFrameBeginUs_ > 0 && frameBeginUs > lastFrameBeginUs_) {
                const int64_t intervalUs = frameBeginUs - lastFrameBeginUs_;
                if (intervalUs >= kDuplicateThresholdUs && intervalUs <= kMaximumIntervalUs)
                    frameBeginIntervals_.Push(intervalUs);
            }
            if (frameBeginUs > lastFrameBeginUs_)
                lastFrameBeginUs_ = frameBeginUs;
        }

        applicationPresents_.Push(presentTimeUs);
        applicationAnchors_.Push(anchorUs);
        applicationAnchorKinds_.Push(static_cast<int64_t>(anchorKind));
    }

    void ResetSampleEpochLocked(int64_t displayWatermarkUs) {
        fallbackDisplayedInputIntervals_.Clear();
        nativeDisplayedSimulationIntervals_.Clear();
        nativeEstimatedSamples_.Clear();
        fallbackSamples_.Clear();
        lastFallbackPresentTimeUs_ = 0;
        lastFallbackAnchorUs_ = 0;
        lastNativePresentTimeUs_ = 0;
        lastNativeDisplayTimeUs_ = displayWatermarkUs;
        lastNativeSimulationStartTimeUs_ = 0;
        lastAnchorToPresentUs_ = 0;
        lastPresentToDisplayUs_ = 0;
        lastInputWaitUs_ = 0;
        lastBaseIntervalUs_ = 0;
        ++measurementEpochResets_;
    }

    void UpdateObservedProductionStateLocked(int64_t displayWatermarkUs) {
        int newState = -1;
        if (fgMultiplier_.load(std::memory_order_relaxed) < 2) {
            newState = 0;
        } else if (applicationPresentIntervals_.Size() >= kCadenceEvidenceCount &&
                   displayIntervals_.Size() >= kCadenceEvidenceCount) {
            const int64_t applicationIntervalUs = MedianRing(applicationPresentIntervals_);
            const int64_t displayIntervalUs = MedianRing(displayIntervals_);
            if (applicationIntervalUs > 0 && displayIntervalUs > 0)
                newState = applicationIntervalUs * 2 >= displayIntervalUs * 3 ? 1 : 0;
        }
        if (newState < 0 || newState == observedProductionState_)
            return;
        if (observedProductionState_ < 0) {
            observedProductionState_ = newState;
            if (newState == 1)
                ResetSampleEpochLocked(0);
            return;
        }
        ResetSampleEpochLocked(displayWatermarkUs);
        observedProductionState_ = newState;
    }

    // Returns false when this displayed transition cannot be attributed to an
    // observed Present.
    bool MatchPresentLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs, size_t& matchedIndex) const {
        // The runtime emits PresentStart inside the Present call the wrapper
        // observed, so the newest present at or before it is that same frame,
        // however many later frames the game has already queued behind it.
        // Without the association there is no way to tell a queued frame from a
        // superseded one, so the newest present before the screen transition
        // stays the documented degraded behaviour.
        const int64_t matchCutoffUs = associatedPresentStartUs > 0 ? associatedPresentStartUs : screenTimeUs;
        for (size_t i = presents_.Size(); i > 0; --i) {
            const int64_t candidateUs = presents_.At(i - 1);
            if (candidateUs <= matchCutoffUs && candidateUs > lastFallbackPresentTimeUs_) {
                matchedIndex = i - 1;
                return true;
            }
        }
        return false;
    }

    void UpdateFallbackLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs) {
        // The runtime PresentStart the sensor paired with this transition is
        // the authoritative present time. Fall back to the hook's own Present
        // observations only when no association exists, which is the
        // documented degraded path.
        int64_t runtimePresentUs = associatedPresentStartUs;
        if (runtimePresentUs <= 0 || runtimePresentUs > screenTimeUs) {
            size_t matchedIndex = 0;
            if (!MatchPresentLocked(screenTimeUs, 0, matchedIndex)) {
                ++displaysWithoutMatchedPresent_;
                return;
            }
            runtimePresentUs = presents_.At(matchedIndex);
        }
        if (runtimePresentUs <= lastFallbackPresentTimeUs_)
            return;

        size_t applicationIndex = 0;
        bool holdApplied = false;
        const bool haveApplicationFrame =
            MatchApplicationPresentLocked(runtimePresentUs, applicationIndex, holdApplied);
        const int64_t anchorUs = haveApplicationFrame ? applicationAnchors_.At(applicationIndex) : 0;
        const bool anchorUsable = anchorUs > 0 && anchorUs <= runtimePresentUs &&
                                  runtimePresentUs - anchorUs <= kMaximumIntervalUs;

        // Interval between the input-sampling points of consecutively displayed
        // frames. Measured between frame-begin boundaries when they exist,
        // because generated frames sample no input of their own and therefore
        // add no sampling point: consecutive displays from one application
        // frame share its boundary and contribute nothing here.
        if (lastFallbackPresentTimeUs_ > 0) {
            const bool useAnchors = anchorUsable && lastFallbackAnchorUs_ > 0;
            const int64_t previousInputUs = useAnchors ? lastFallbackAnchorUs_ : lastFallbackPresentTimeUs_;
            const int64_t currentInputUs = useAnchors ? anchorUs : runtimePresentUs;
            const int64_t displayedInputIntervalUs = currentInputUs - previousInputUs;
            if (displayedInputIntervalUs >= kDuplicateThresholdUs &&
                displayedInputIntervalUs <= kMaximumIntervalUs) {
                fallbackDisplayedInputIntervals_.Push(displayedInputIntervalUs);
            }
        }
        lastFallbackPresentTimeUs_ = runtimePresentUs;
        lastFallbackAnchorUs_ = anchorUsable ? anchorUs : 0;

        const int64_t presentToDisplayUs = screenTimeUs - runtimePresentUs;
        const int64_t baseIntervalUs = ResolveWorkIntervalLocked();
        if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs || baseIntervalUs <= 0 ||
            baseIntervalUs > kMaximumSamplingIntervalUs) {
            ++samplesRejected_;
            return;
        }

        // Simulation and render work, plus whatever a generator held the frame
        // for: measured from the frame's own boundary when one was observed,
        // otherwise approximated as one base interval. The measured form is
        // what makes the estimate sensitive to a low-latency mode, which
        // shortens this span without changing cadence.
        int64_t anchorToPresentUs = baseIntervalUs;
        FrameBeginKind frameBeginKind = FrameBeginKind::Modelled;
        if (anchorUsable) {
            anchorToPresentUs = runtimePresentUs - anchorUs;
            frameBeginKind = static_cast<FrameBeginKind>(applicationAnchorKinds_.At(applicationIndex));
        } else if (haveApplicationFrame) {
            // No observed input boundary: model one application interval of CPU
            // work, but retain the measured time the generator held that source
            // frame before the associated final-output Present.
            anchorToPresentUs = baseIntervalUs + runtimePresentUs - applicationPresents_.At(applicationIndex);
        }

        // Input wait is half a base interval plus every full interval whose
        // sampling point never reached the screen, matching PCL's dropped-frame
        // treatment.
        const int64_t displayedInputIntervalUs =
            (std::max)(baseIntervalUs, MedianRing(fallbackDisplayedInputIntervals_));
        const int64_t estimatedInputWaitUs = displayedInputIntervalUs - baseIntervalUs / 2;
        const int64_t totalUs = presentToDisplayUs + anchorToPresentUs + estimatedInputWaitUs;
        if (!IsValidTotalLatency(totalUs)) {
            ++samplesRejected_;
            return;
        }
        fallbackSamples_.Add(static_cast<float>(totalUs) / 1000.0f, screenTimeUs);
        lastAnchorToPresentUs_ = anchorToPresentUs;
        lastPresentToDisplayUs_ = presentToDisplayUs;
        lastInputWaitUs_ = estimatedInputWaitUs;
        lastBaseIntervalUs_ = baseIntervalUs;
        lastFrameBeginKind_ = frameBeginKind;
        lastGeneratorHoldApplied_ = holdApplied;
    }

    void ResetMeasurementsLocked() {
        presents_.Clear();
        presentIntervals_.Clear();
        applicationPresents_.Clear();
        applicationPresentIntervals_.Clear();
        applicationAnchors_.Clear();
        applicationAnchorKinds_.Clear();
        frameBeginIntervals_.Clear();
        displays_.Clear();
        displayPresentStarts_.Clear();
        displayIntervals_.Clear();
        fallbackDisplayedInputIntervals_.Clear();
        nativeDisplayedSimulationIntervals_.Clear();
        nativeEstimatedSamples_.Clear();
        fallbackSamples_.Clear();
        lastFallbackPresentTimeUs_ = 0;
        lastFallbackAnchorUs_ = 0;
        lastFrameBeginUs_ = 0;
        lastNativePresentTimeUs_ = 0;
        lastNativeDisplayTimeUs_ = 0;
        lastNativeSimulationStartTimeUs_ = 0;
        observedProductionState_ = -1;
        markerIntervalUs_ = 0;
        markerCadenceTrusted_ = true;
        lastGeneratorHoldApplied_ = false;
        nativeGeneratorHoldApplied_ = false;
    }

    mutable std::mutex mutex_;
    ValueRing<256> presents_;
    ValueRing<32> presentIntervals_;
    ValueRing<64> applicationPresents_;
    ValueRing<32> applicationPresentIntervals_;
    ValueRing<64> applicationAnchors_;
    ValueRing<64> applicationAnchorKinds_;
    ValueRing<32> frameBeginIntervals_;
    ValueRing<256> displays_;
    ValueRing<256> displayPresentStarts_;
    ValueRing<32> displayIntervals_;
    ValueRing<32> fallbackDisplayedInputIntervals_;
    ValueRing<32> nativeDisplayedSimulationIntervals_;
    SampleWindow nativeEstimatedSamples_;
    SampleWindow fallbackSamples_;
    int64_t lastFallbackPresentTimeUs_ = 0;
    int64_t lastFallbackAnchorUs_ = 0;
    int64_t lastFrameBeginUs_ = 0;
    int64_t lastNativePresentTimeUs_ = 0;
    int64_t lastNativeDisplayTimeUs_ = 0;
    int64_t lastNativeSimulationStartTimeUs_ = 0;
    std::atomic<float> fgBaseFps_{0.0f};
    std::atomic<int> fgMultiplier_{1};

    // Diagnostics. Counters survive a display-generation reset so a session's
    // measurement quality stays legible across backend transitions.
    std::atomic<uint64_t> droppedPresents_{0};
    std::atomic<uint64_t> droppedApplicationPresents_{0};
    uint64_t displaysObserved_ = 0;
    uint64_t displaysWithAssociation_ = 0;
    uint64_t displaysWithoutMatchedPresent_ = 0;
    uint64_t samplesRejected_ = 0;
    mutable uint64_t sourceTransitions_ = 0;
    mutable Source publishedSource_ = Source::Unavailable;
    int64_t lastAnchorToPresentUs_ = 0;
    int64_t lastPresentToDisplayUs_ = 0;
    int64_t lastInputWaitUs_ = 0;
    int64_t lastBaseIntervalUs_ = 0;
    FrameBeginKind lastFrameBeginKind_ = FrameBeginKind::Modelled;
    bool lastGeneratorHoldApplied_ = false;
    int64_t nativeSimulationToDisplayUs_ = 0;
    int64_t nativePresentToDisplayUs_ = 0;
    int64_t nativeInputWaitUs_ = 0;
    int64_t nativeSamplingIntervalUs_ = 0;
    bool nativeUsedAssociation_ = false;
    bool nativeGeneratorHoldApplied_ = false;
    int observedProductionState_ = -1;
    int64_t markerIntervalUs_ = 0;
    bool markerCadenceTrusted_ = true;
    uint64_t markerReportsRejectedForOutputCadence_ = 0;
    uint64_t measurementEpochResets_ = 0;
};

}  // namespace ce::system_latency

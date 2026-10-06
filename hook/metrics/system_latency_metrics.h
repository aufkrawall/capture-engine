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

#include "system_latency_frame_begin.h"
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
        FrameBeginObservation frameBegin;
        frameBegin.beginUs = frameBeginUs;
        frameBegin.kind = frameBeginKind;
        ObservePresent(presentTimeUs, frameBegin, 0);
    }

    // threadId is the presenting thread (0 = unknown); it decides which input
    // retrievals can belong to this frame.
    void ObservePresent(int64_t presentTimeUs, const FrameBeginObservation& frameBegin, uint32_t threadId) {
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
            RecordApplicationPresentLocked(presentTimeUs, frameBegin, threadId);
    }

    void ObserveApplicationPresent(int64_t presentTimeUs, int64_t frameBeginUs = 0,
                                   FrameBeginKind frameBeginKind = FrameBeginKind::Modelled) {
        FrameBeginObservation frameBegin;
        frameBegin.beginUs = frameBeginUs;
        frameBegin.kind = frameBeginKind;
        ObserveApplicationPresent(presentTimeUs, frameBegin, 0);
    }

    void ObserveApplicationPresent(int64_t presentTimeUs, const FrameBeginObservation& frameBegin,
                                   uint32_t threadId) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            droppedApplicationPresents_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        RecordApplicationPresentLocked(presentTimeUs, frameBegin, threadId);
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
            else if (displayIntervalUs > kMaximumIntervalUs)
                queueDepthMeasurable_ = false;
        }
        ObserveFramesQueuedAheadLocked(presentStartTimeUs);
        displays_.Push(screenTimeUs);
        displayPresentStarts_.Push(presentStartTimeUs);
        ++displaysSinceQueueSeed_;
        RejectImpossibleQueueCountLocked();
        ++displaysObserved_;
        if (presentStartTimeUs > 0)
            ++displaysWithAssociation_;
        UpdateObservedProductionStateLocked(screenTimeUs);
        UpdateFallbackLocked(screenTimeUs, presentStartTimeUs);
    }

    // A consumer that knows it could not deliver every displayed transition -
    // a publication ring it fell behind on, say - must say so: the in-flight
    // count is conservation over both streams, and an uncounted retirement
    // inflates it permanently.
    void NoteDisplayStreamGap() {
        std::lock_guard<std::mutex> lock(mutex_);
        queueDepthMeasurable_ = false;
    }

    void SetFrameGeneration(float baseFps, int multiplier, int fgType = 0) {
        // FG publication happens on the Present path. Keep this metadata
        // lock-free so an infrequent native-report scan can never stall it.
        fgBaseFps_.store(std::isfinite(baseFps) && baseFps > 0.0f ? baseFps : 0.0f,
                         std::memory_order_relaxed);
        const int normalizedMultiplier = multiplier >= 2 ? multiplier : 1;
        const int previousMultiplier = fgMultiplier_.exchange(normalizedMultiplier, std::memory_order_relaxed);
        const int previousFgType = fgType_.exchange(fgType, std::memory_order_relaxed);
        if (previousMultiplier != normalizedMultiplier || previousFgType != fgType) {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool generatorJustStarted = previousMultiplier < 2 && normalizedMultiplier >= 2;
            ResetMeasurementsLocked();
            ++measurementEpochResets_;
            // The runtime has produced nothing yet, so its queue is empty and the
            // depth the game fills it to becomes measurable from here.
            queueDepthMeasurable_ = generatorJustStarted;
        }
    }

    // Definition in system_latency_marker_reports.h.
    void SubmitNativeReport(const NativeReport& report);

    Snapshot GetSnapshot(int64_t currentQpcUs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot snapshot{};
        if (nativeEstimatedSamples_.IsFresh(currentQpcUs))
            snapshot = nativeEstimatedSamples_.MakeSnapshot(Source::ReflexMarkers);
        else if (fallbackSamples_.IsFresh(currentQpcUs))
            snapshot = fallbackSamples_.MakeSnapshot(Source::Estimated);
        snapshot.frameGenerationConfigured = fgMultiplier_.load(std::memory_order_relaxed) >= 2;
        snapshot.frameGenerationStateKnown = true;
        snapshot.frameGenerationObserved = IsGeneratorPacingOutputLocked();
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
        diagnostics.samplesRejectedPresentToDisplay = samplesRejectedPresentToDisplay_;
        diagnostics.samplesRejectedBaseInterval = samplesRejectedBaseInterval_;
        diagnostics.samplesRejectedTotalLatency = samplesRejectedTotalLatency_;
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
            diagnostics.generatorHoldMeasured = lastGeneratorHoldMeasured_;
        }
        diagnostics.applicationFramesInFlight = static_cast<uint32_t>(InFlightApplicationFramesLocked());
        diagnostics.displayIntervalUs = MedianRing(displayIntervals_);
        diagnostics.framesQueuedAhead = static_cast<int>(MedianRing(framesQueuedAhead_));
        diagnostics.framesQueuedAheadMax = static_cast<int>(MaximumRing(framesQueuedAhead_));
        diagnostics.applicationIntervalUs = ResolveWorkIntervalLocked();
        diagnostics.applicationPresentStreamFresh = IsApplicationPresentStreamFreshLocked();
        diagnostics.frameBeginIntervalUs = MedianRing(frameBeginIntervals_);
        diagnostics.markerIntervalUs = markerIntervalUs_;
        if (diagnostics.displayIntervalUs > 0 && diagnostics.applicationIntervalUs > 0) {
            diagnostics.observedOutputRatioPermille = static_cast<int>(
                (diagnostics.applicationIntervalUs * 1000 + diagnostics.displayIntervalUs / 2) /
                diagnostics.displayIntervalUs);
        }
        diagnostics.frameGenerationObserved = IsGeneratorPacingOutputLocked();
        diagnostics.markerCadenceTrusted = markerCadenceTrusted_;
        diagnostics.markerReportsRejectedForOutputCadence = markerReportsRejectedForOutputCadence_;
        diagnostics.measurementEpochResets = measurementEpochResets_;
        diagnostics.queueDepthCountsRejected = queueDepthCountsRejected_;
        diagnostics.anchorKindSamples = anchorKindSamples_;
        diagnostics.sleepAnchorsOnOtherThread = sleepAnchorsOnOtherThread_;
        diagnostics.markerAnchorsStale = markerAnchorsStale_;
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
    // A generator holding more than this many application frames is reporting a
    // drifted count, not a pipeline.
    static constexpr size_t kMaximumQueueDepth = 8;

    static int64_t ToSignedTimestamp(uint64_t timestampUs) {
        if (timestampUs == 0 || timestampUs > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            return 0;
        return static_cast<int64_t>(timestampUs);
    }

    // Definitions in system_latency_marker_reports.h.
    static bool IsValidNativeFrame(const NativeFrameReport& frame);
    static uint64_t NativeReadyTimeUs(const NativeFrameReport& frame);

    static bool IsValidTotalLatency(int64_t totalUs) {
        return totalUs > 0 && totalUs <= kMaximumTotalLatencyUs;
    }

    // Definition in system_latency_marker_reports.h.
    static int64_t MedianSimulationInterval(
        const std::array<NativeFrameReport, NativeReport::kCapacity>& frames, size_t count);

    int64_t ResolveFgBaseIntervalLocked() const {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const float fgBaseFps = fgBaseFps_.load(std::memory_order_relaxed);
        if (fgMultiplier < 2 || fgBaseFps <= 0.0f)
            return 0;
        return std::llround(1'000'000.0 / static_cast<double>(fgBaseFps));
    }

    // The application-source cadence is a measurement only while that stream is
    // still arriving. A generator that takes presentation over from the game
    // can let the game's own Present reach DXGI during its startup only: The
    // Witcher 3 under bridged DLSS-G (session 20261001_105517) delivered ~1.6 s
    // of them at hitch cadence, then none. The frozen 141 ms median exceeded the
    // sampling bound, so every estimate was rejected, and made the game's
    // 28.9 ms PCL markers look like output-rate markers, so the marker path was
    // rejected too: no PC latency for the rest of the session.
    bool IsApplicationPresentStreamFreshLocked() const {
        if (applicationPresents_.Empty())
            return false;
        const int64_t newestApplicationUs = applicationPresents_.Back();
        int64_t newestObservationUs = newestApplicationUs;
        if (!presents_.Empty())
            newestObservationUs = (std::max)(newestObservationUs, presents_.Back());
        if (!displays_.Empty())
            newestObservationUs = (std::max)(newestObservationUs, displays_.Back());
        return newestObservationUs - newestApplicationUs <= kMaximumIntervalUs;
    }

    int64_t ResolveWorkIntervalLocked() const {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const int64_t fgBaseInterval = ResolveFgBaseIntervalLocked();
        const int64_t applicationIntervalUs =
            IsApplicationPresentStreamFreshLocked() ? MedianRing(applicationPresentIntervals_) : 0;
        if (fgMultiplier >= 2) {
            const int64_t appInterval = applicationIntervalUs;
            if (appInterval > 0 && (fgBaseInterval <= 0 || appInterval >= fgBaseInterval / 2))
                return appInterval;
            if (markerCadenceTrusted_ && markerIntervalUs_ > 0)
                return markerIntervalUs_;
            if (fgBaseInterval > 0)
                return fgBaseInterval;
            const int64_t displayInterval = MedianRing(displayIntervals_);
            if (displayInterval > 0)
                return displayInterval * fgMultiplier;
        }
        if (applicationIntervalUs > 0)
            return applicationIntervalUs;
        if (markerCadenceTrusted_ && markerIntervalUs_ > 0)
            return markerIntervalUs_;
        return (std::max)(MedianRing(presentIntervals_), ResolveFgBaseIntervalLocked());
    }

    bool IsGeneratorPacingOutputLocked() const {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        if (fgMultiplier < 2)
            return false;
        const int64_t displayIntervalUs = MedianRing(displayIntervals_);
        if (displayIntervalUs <= 0)
            return true;
        const int64_t baseIntervalUs = ResolveWorkIntervalLocked();
        if (baseIntervalUs <= 0)
            return true;
        return baseIntervalUs * 10 >= displayIntervalUs * 135 / 10;
    }

    bool IsMarkerCadenceOutputRateLocked(int64_t markerIntervalUs) const {
        if (!IsGeneratorPacingOutputLocked())
            return false;
        const int64_t baseIntervalUs = ResolveWorkIntervalLocked();
        if (baseIntervalUs <= 0 || markerIntervalUs <= 0)
            return false;
        if (markerIntervalUs * 4 < baseIntervalUs * 3)
            return true;
        const int64_t displayIntervalUs = MedianRing(displayIntervals_);
        if (displayIntervalUs > 0 && markerIntervalUs <= displayIntervalUs * 13 / 10)
            return true;
        return false;
    }

    // Frames that reached the screen while this one waited between its runtime
    // PresentStart and its own display: the depth of the queue below Present
    // that it sat behind. A vsync-capped frame rate shows here as a full flip
    // queue; free-running VRR and low-latency modes keep it near zero.
    void ObserveFramesQueuedAheadLocked(int64_t presentStartTimeUs) {
        if (presentStartTimeUs <= 0 || displays_.Empty())
            return;
        int64_t aheadCount = 0;
        for (size_t i = displays_.Size(); i > 0 && displays_.At(i - 1) > presentStartTimeUs; --i)
            ++aheadCount;
        framesQueuedAhead_.Push(aheadCount);
    }

    // Application frames the game has handed to the generator that have not yet
    // reached the screen, counted by conservation: every application frame is
    // eventually displayed exactly fgMultiplier times, so the cumulative
    // difference between the two streams is the number still in flight.
    //
    // Only the difference carries the depth - both streams run at the same rate
    // in steady state - so the count means something only from a point where the
    // queue was empty. Frame generation switching on is such a point: the runtime
    // has produced nothing yet, and the depth then emerges as the number of
    // application frames issued before the first group reaches the screen.
    // Anywhere else the queue state is unknowable from timestamps, and the
    // documented single-frame hold stands.
    size_t InFlightApplicationFramesLocked() const {
        if (!queueDepthMeasurable_)
            return 0;
        const int fgMultiplier = (std::max)(fgMultiplier_.load(std::memory_order_relaxed), 1);
        const uint64_t retired = displaysSinceQueueSeed_ / static_cast<uint64_t>(fgMultiplier);
        if (applicationPresentsSinceQueueSeed_ <= retired)
            return 0;
        const uint64_t inFlight = applicationPresentsSinceQueueSeed_ - retired;
        // The clamp only trims the frame in transit (RejectImpossibleQueueCountLocked
        // abandons anything beyond it).
        return static_cast<size_t>((std::min)(inFlight, static_cast<uint64_t>(kMaximumQueueDepth)));
    }

    // Conservation only holds while every application frame reaches the screen
    // fgMultiplier times. FSR FG violates that right after switching on: GTA
    // (session 20260925_233000) presented 25-35 frames during a ~500 ms stretch
    // with no display at all, the count saturated at the cap, and the published
    // latency read ~110-150 ms for the rest of the session instead of ~30 ms. A
    // count outside the physically possible range can never come back into it,
    // so it is abandoned until the next known-empty seed. One frame of slack on
    // each side: the newest frame is presented before the older frame's outputs
    // are counted (so a full queue transiently reads one deeper), and a runtime
    // present of the frame before the seed can retire one frame too many.
    void RejectImpossibleQueueCountLocked() {
        if (!queueDepthMeasurable_)
            return;
        const int fgMultiplier = (std::max)(fgMultiplier_.load(std::memory_order_relaxed), 1);
        const uint64_t retired = displaysSinceQueueSeed_ / static_cast<uint64_t>(fgMultiplier);
        const bool tooDeep = applicationPresentsSinceQueueSeed_ > retired + kMaximumQueueDepth + 1;
        const bool overRetired = retired > applicationPresentsSinceQueueSeed_ + 1;
        if (!tooDeep && !overRetired)
            return;
        queueDepthMeasurable_ = false;
        ++queueDepthCountsRejected_;
    }

    // Index in applicationPresents_ of the application frame whose simulation produced
    // the content of the runtime present at runtimePresentUs.
    //
    // The newest application source at or before that present is right only when the
    // application itself issued it: the wrapper's Present is entered before the
    // next frame can begin, so no newer boundary exists yet. A generator's
    // pacing thread presents on its own schedule, and by then the application
    // has already begun the next frame - it must have, because the generator
    // interpolates towards a frame that is complete. So at least one boundary
    // has to be stepped back, which is the application frame the generator is
    // holding while it shows the frames derived from it.
    //
    // One is the whole answer only when the queue behind that hold is one frame
    // deep, which is what a low-latency mode enforces and nothing else does. A
    // game running ahead into a generator's own queue is exactly as far behind
    // the screen as that queue is deep, so step back the measured depth and keep
    // the single-frame hold as the floor. A stepped anchor older than the
    // correlator's interval bound is not evidence of latency, it is evidence the
    // count drifted, so it falls back toward the floor.
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
        if (!found || runtimePresentUs - applicationPresents_.At(matchedIndex) > kMaximumIntervalUs)
            return false;
        if (matchedIndex == 0 || !IsGeneratorPacingOutputLocked())
            return true;
        const size_t inFlight = InFlightApplicationFramesLocked();
        size_t stepBack = inFlight > 1 ? inFlight - 1 : 1;
        stepBack = (std::min)(stepBack, matchedIndex);
        while (stepBack > 1 &&
               runtimePresentUs - applicationPresents_.At(matchedIndex - stepBack) > kMaximumIntervalUs) {
            --stepBack;
        }
        matchedIndex -= stepBack;
        holdApplied = true;
        return true;
    }

    void RecordApplicationPresentLocked(int64_t presentTimeUs, const FrameBeginObservation& frameBegin,
                                        uint32_t threadId) {
        if (presentTimeUs <= 0)
            return;
        int64_t previousApplicationPresentUs = 0;
        if (!applicationPresents_.Empty()) {
            const int64_t previous = applicationPresents_.Back();
            previousApplicationPresentUs = previous;
            if (presentTimeUs <= previous) {
                if (previous - presentTimeUs > kClockResetThresholdUs)
                    ResetMeasurementsLocked();
                return;
            }
            const int64_t intervalUs = presentTimeUs - previous;
            const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
            const int64_t minIntervalUs = fgMultiplier >= 2 ? 3'000 : kApplicationDuplicateThresholdUs;
            if (intervalUs < minIntervalUs)
                return;
            if (intervalUs > kMaximumIntervalUs) {
                applicationPresentsSinceQueueSeed_ = 0;
                displaysSinceQueueSeed_ = 0;
                queueDepthMeasurable_ = true;
            }
            if (intervalUs <= kMaximumIntervalUs)
                applicationPresentIntervals_.Push(intervalUs);
        }

        // A marker pair whose PresentStart is not newer than the previous
        // application Present labelled an earlier frame: this one emitted none.
        FrameBeginKind frameBeginKind = frameBegin.kind;
        if (frameBeginKind == FrameBeginKind::SimulationMarker && previousApplicationPresentUs > 0 &&
            frameBegin.markerPresentUs <= previousApplicationPresentUs) {
            frameBeginKind = FrameBeginKind::Modelled;
            ++markerAnchorsStale_;
        }
        if (frameBeginKind == FrameBeginKind::Modelled && frameBegin.sleepOnOtherThread)
            ++sleepAnchorsOnOtherThread_;
        const int64_t frameBeginUs = frameBeginKind == FrameBeginKind::Modelled ? 0 : frameBegin.beginUs;

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
        ++applicationPresentsSinceQueueSeed_;
        RejectImpossibleQueueCountLocked();
        applicationAnchors_.Push(anchorUs);
        applicationAnchorKinds_.Push(static_cast<int64_t>(anchorKind));
        applicationPresentThreads_.Push(threadId);
    }

    void ResetSampleEpochLocked(int64_t displayWatermarkUs) {
        fallbackDisplayedInputIntervals_.Clear(); nativeDisplayedSimulationIntervals_.Clear();
        nativeEstimatedSamples_.Clear(); fallbackSamples_.Clear();
        lastFallbackPresentTimeUs_ = 0; lastFallbackAnchorUs_ = 0;
        lastNativePresentTimeUs_ = 0; lastNativeDisplayTimeUs_ = displayWatermarkUs;
        lastNativeSimulationStartTimeUs_ = 0;
        lastAnchorToPresentUs_ = 0; lastPresentToDisplayUs_ = 0;
        lastInputWaitUs_ = 0; lastBaseIntervalUs_ = 0;
        ++measurementEpochResets_;
    }

    void UpdateObservedProductionStateLocked(int64_t displayWatermarkUs) {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const int newState = fgMultiplier >= 2 ? 1 : 0;
        if (newState == observedProductionState_)
            return;
        observedProductionState_ = newState;
    }

    // Definitions in system_latency_fallback.h.
    bool MatchPresentLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs, size_t& matchedIndex) const;
    void UpdateFallbackLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs);
    bool ResolveMeasuredAnchorLocked(size_t applicationIndex, int64_t& anchorUs, FrameBeginKind& kind) const;
    int64_t LearnedAnchorSpanLocked(int64_t applicationPresentUs) const;

    void ResetMeasurementsLocked() {
        presents_.Clear(); presentIntervals_.Clear();
        applicationPresents_.Clear(); applicationPresentIntervals_.Clear();
        applicationAnchors_.Clear(); applicationAnchorKinds_.Clear(); applicationPresentThreads_.Clear();
        frameBeginIntervals_.Clear();
        measuredAnchorSpans_.Clear();
        lastMeasuredAnchorPresentUs_ = 0;
        displays_.Clear(); displayPresentStarts_.Clear(); displayIntervals_.Clear(); framesQueuedAhead_.Clear();
        fallbackDisplayedInputIntervals_.Clear(); nativeDisplayedSimulationIntervals_.Clear();
        nativeEstimatedSamples_.Clear(); fallbackSamples_.Clear();
        lastFallbackPresentTimeUs_ = 0;
        lastFallbackAnchorUs_ = 0;
        lastFrameBeginUs_ = 0;
        lastNativePresentTimeUs_ = 0;
        lastNativeDisplayTimeUs_ = 0;
        lastNativeSimulationStartTimeUs_ = 0;
        observedProductionState_ = fgMultiplier_.load(std::memory_order_relaxed) >= 2 ? 1 : 0;
        markerIntervalUs_ = 0;
        markerCadenceTrusted_ = true;
        lastGeneratorHoldApplied_ = false;
        lastGeneratorHoldMeasured_ = false;
        applicationPresentsSinceQueueSeed_ = 0;
        displaysSinceQueueSeed_ = 0;
        queueDepthMeasurable_ = false;
        nativeGeneratorHoldApplied_ = false;
    }

    mutable std::mutex mutex_;
    ValueRing<256> presents_;
    ValueRing<32> presentIntervals_;
    ValueRing<256> applicationPresents_;
    ValueRing<32> applicationPresentIntervals_;
    ValueRing<256> applicationAnchors_;
    ValueRing<256> applicationAnchorKinds_;
    ValueRing<256> applicationPresentThreads_;
    // Measured anchor-to-application-Present spans, for frames without one.
    ValueRing<32> measuredAnchorSpans_;
    int64_t lastMeasuredAnchorPresentUs_ = 0;
    ValueRing<32> frameBeginIntervals_;
    ValueRing<256> displays_;
    ValueRing<256> displayPresentStarts_;
    ValueRing<32> displayIntervals_;
    ValueRing<32> framesQueuedAhead_;
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
    std::atomic<int> fgType_{0};

    // Diagnostics. Counters survive a display-generation reset so a session's
    // measurement quality stays legible across backend transitions.
    std::atomic<uint64_t> droppedPresents_{0};
    std::atomic<uint64_t> droppedApplicationPresents_{0};
    uint64_t displaysObserved_ = 0;
    uint64_t displaysWithAssociation_ = 0;
    uint64_t displaysWithoutMatchedPresent_ = 0;
    uint64_t samplesRejected_ = 0;
    uint64_t samplesRejectedPresentToDisplay_ = 0;
    uint64_t samplesRejectedBaseInterval_ = 0;
    uint64_t samplesRejectedTotalLatency_ = 0;
    mutable uint64_t sourceTransitions_ = 0;
    mutable Source publishedSource_ = Source::Unavailable;
    int64_t lastAnchorToPresentUs_ = 0;
    int64_t lastPresentToDisplayUs_ = 0;
    int64_t lastInputWaitUs_ = 0;
    int64_t lastBaseIntervalUs_ = 0;
    FrameBeginKind lastFrameBeginKind_ = FrameBeginKind::Modelled;
    bool lastGeneratorHoldApplied_ = false;
    bool lastGeneratorHoldMeasured_ = false;
    uint64_t applicationPresentsSinceQueueSeed_ = 0;
    uint64_t displaysSinceQueueSeed_ = 0;
    bool queueDepthMeasurable_ = false;
    int64_t nativeSimulationToDisplayUs_ = 0;
    int64_t nativePresentToDisplayUs_ = 0;
    int64_t nativeInputWaitUs_ = 0;
    int64_t nativeSamplingIntervalUs_ = 0;
    bool nativeUsedAssociation_ = false;
    bool nativeGeneratorHoldApplied_ = false;
    int observedProductionState_ = 0;
    int64_t markerIntervalUs_ = 0;
    bool markerCadenceTrusted_ = true;
    uint64_t markerReportsRejectedForOutputCadence_ = 0;
    uint64_t measurementEpochResets_ = 0;
    uint64_t queueDepthCountsRejected_ = 0;
    std::array<uint64_t, kFrameBeginKindCount> anchorKindSamples_{};
    uint64_t sleepAnchorsOnOtherThread_ = 0;
    uint64_t markerAnchorsStale_ = 0;
};

}  // namespace ce::system_latency

// Out-of-line marker-path definitions. Included last so the class is complete;
// the include is mutual and guarded, so either header may be included first.
#include "system_latency_fallback.h"
#include "system_latency_marker_reports.h"

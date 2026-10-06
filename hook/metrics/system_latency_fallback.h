#pragma once

// The estimate path: turning a displayed transition, the runtime Present that
// produced it and the application frame behind that Present into a sample.
//
// Split out of system_latency_metrics.h, which carries the correlator itself
// and includes this file at its end. The definitions are out of line so the
// class stays readable; either header may be included first.

#include "system_latency_metrics.h"

namespace ce::system_latency {

// Returns false when this displayed transition cannot be attributed to an
// observed Present.
inline bool Tracker::MatchPresentLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs,
                                        size_t& matchedIndex) const {
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

// The presenting thread's last input retrieval since its previous application
// Present: the point this frame read its input. Only the presenting thread's
// own retrievals qualify - a game thread running ahead of a render thread has
// retrieved input for later frames by the time this one is presented.
inline bool Tracker::FindInputRetrievalLocked(size_t applicationIndex, int64_t& retrievalUs) {
    const int64_t threadId = applicationPresentThreads_.At(applicationIndex);
    const int64_t presentUs = applicationPresents_.At(applicationIndex);
    const int64_t windowStartUs =
        applicationIndex > 0 ? applicationPresents_.At(applicationIndex - 1) : presentUs - kMaximumIntervalUs;
    bool otherThread = false;
    for (size_t i = inputRetrievals_.Size(); i > 0; --i) {
        const int64_t candidateUs = inputRetrievals_.At(i - 1);
        if (candidateUs > presentUs)
            continue;
        if (candidateUs <= windowStartUs)
            break;
        if (threadId != 0 && inputRetrievalThreads_.At(i - 1) == threadId) {
            retrievalUs = candidateUs;
            return true;
        }
        otherThread = true;
    }
    if (otherThread)
        ++inputRetrievalFramesOnOtherThread_;
    return false;
}

// The application frame's own input boundary, when one was observed for it:
// a frame-ID-matched marker or same-thread sleep recorded at Present time,
// otherwise the presenting thread's input retrieval.
inline bool Tracker::ResolveMeasuredAnchorLocked(size_t applicationIndex, int64_t& anchorUs, FrameBeginKind& kind) {
    anchorUs = applicationAnchors_.At(applicationIndex);
    kind = static_cast<FrameBeginKind>(applicationAnchorKinds_.At(applicationIndex));
    if (anchorUs > 0 && kind != FrameBeginKind::Modelled)
        return true;
    if (FindInputRetrievalLocked(applicationIndex, anchorUs)) {
        kind = FrameBeginKind::InputRetrieval;
        return true;
    }
    anchorUs = 0;
    kind = FrameBeginKind::Modelled;
    return false;
}

// A frame without a boundary of its own, in a stream whose neighbouring frames
// had one, has the span those frames measured - not one full interval. Only
// while the measurements are recent: a stale span describes another scene.
inline int64_t Tracker::LearnedAnchorSpanLocked(int64_t applicationPresentUs) const {
    static constexpr size_t kMinimumLearnedSpans = 8;
    if (measuredAnchorSpans_.Size() < kMinimumLearnedSpans || lastMeasuredAnchorPresentUs_ <= 0)
        return 0;
    const int64_t ageUs = applicationPresentUs - lastMeasuredAnchorPresentUs_;
    if (ageUs > kSampleFreshnessUs || ageUs < -kSampleFreshnessUs)
        return 0;
    return MedianRing(measuredAnchorSpans_);
}

inline void Tracker::UpdateFallbackLocked(int64_t screenTimeUs, int64_t associatedPresentStartUs) {
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
    if (runtimePresentUs <= lastFallbackPresentTimeUs_) {
        if (lastFallbackPresentTimeUs_ - runtimePresentUs > kClockResetThresholdUs)
            ResetMeasurementsLocked();
        return;
    }

    size_t applicationIndex = 0;
    bool holdApplied = false;
    const bool haveApplicationFrame = MatchApplicationPresentLocked(runtimePresentUs, applicationIndex, holdApplied);
    int64_t anchorUs = 0;
    FrameBeginKind anchorKind = FrameBeginKind::Modelled;
    const bool anchorUsable = haveApplicationFrame &&
                              ResolveMeasuredAnchorLocked(applicationIndex, anchorUs, anchorKind) &&
                              anchorUs <= runtimePresentUs && runtimePresentUs - anchorUs <= kMaximumIntervalUs;
    if (!anchorUsable)
        anchorUs = 0;

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
        if (displayedInputIntervalUs >= kDuplicateThresholdUs && displayedInputIntervalUs <= kMaximumIntervalUs) {
            fallbackDisplayedInputIntervals_.Push(displayedInputIntervalUs);
        }
    }
    lastFallbackPresentTimeUs_ = runtimePresentUs;
    lastFallbackAnchorUs_ = anchorUs;

    const int64_t presentToDisplayUs = screenTimeUs - runtimePresentUs;
    const int64_t baseIntervalUs = ResolveWorkIntervalLocked();
    if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs) {
        ++samplesRejected_;
        ++samplesRejectedPresentToDisplay_;
        return;
    }
    if (baseIntervalUs <= 0 || baseIntervalUs > kMaximumSamplingIntervalUs) {
        ++samplesRejected_;
        ++samplesRejectedBaseInterval_;
        return;
    }

    // Simulation and render work, plus whatever a generator held the frame
    // for: measured from the frame's own boundary when one was observed,
    // otherwise learned from neighbouring frames or approximated as one base
    // interval. The measured form is what makes the estimate sensitive to a
    // low-latency mode, which shortens this span without changing cadence.
    int64_t anchorToPresentUs = baseIntervalUs;
    // The step back onto the held application frame is what makes the hold
    // measured in both branches below; only the expected-hold addition
    // further down is a model.
    bool holdMeasured = holdApplied;
    FrameBeginKind frameBeginKind = FrameBeginKind::Modelled;
    if (anchorUsable) {
        anchorToPresentUs = runtimePresentUs - anchorUs;
        frameBeginKind = anchorKind;
        const int64_t applicationPresentUs = applicationPresents_.At(applicationIndex);
        if (applicationPresentUs >= anchorUs) {
            measuredAnchorSpans_.Push(applicationPresentUs - anchorUs);
            lastMeasuredAnchorPresentUs_ = applicationPresentUs;
        }
    } else if (haveApplicationFrame) {
        // No observed input boundary. The span from the application's Present
        // to the runtime Present that carries the frame is measured either way:
        // under a generator it is the time the generator held the source frame,
        // without one it is the wait inside the application's own Present call
        // (CE's limiter or flip-queue pacing).
        const int64_t applicationPresentUs = applicationPresents_.At(applicationIndex);
        const int64_t holdUs = runtimePresentUs - applicationPresentUs;
        const int64_t learnedSpanUs = LearnedAnchorSpanLocked(applicationPresentUs);
        if (holdUs < 0 || holdUs > kMaximumIntervalUs) {
            holdMeasured = false;
        } else if (learnedSpanUs > 0) {
            anchorToPresentUs = learnedSpanUs + holdUs;
            frameBeginKind = FrameBeginKind::Learned;
        } else if (IsGeneratorPacingOutputLocked()) {
            anchorToPresentUs = baseIntervalUs + holdUs;
        }
        // Without a generator the modelled interval stays as it is: it is the
        // application's own Present-to-Present period, which already contains
        // that in-call wait. Adding it again published CE's own limiter wait
        // twice (Strange Brigade at a 90 fps cap: 11.1 ms interval + 9.3 ms
        // wait for a frame the game built in 1.8 ms).
    }

    if (IsGeneratorPacingOutputLocked()) {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const int64_t displayIntervalUs = MedianRing(displayIntervals_);
        const int64_t effectiveDisplayIntervalUs =
            displayIntervalUs > 0 ? displayIntervalUs : (baseIntervalUs / fgMultiplier);
        const int64_t expectedGeneratorHoldUs = (fgMultiplier - 1) * effectiveDisplayIntervalUs;
        if (!holdApplied) {
            anchorToPresentUs += expectedGeneratorHoldUs;
            holdApplied = true;
        }
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
        ++samplesRejectedTotalLatency_;
        return;
    }
    fallbackSamples_.Add(static_cast<float>(totalUs) / 1000.0f, screenTimeUs);
    ++anchorKindSamples_[static_cast<size_t>(frameBeginKind)];
    lastAnchorToPresentUs_ = anchorToPresentUs;
    lastPresentToDisplayUs_ = presentToDisplayUs;
    lastInputWaitUs_ = estimatedInputWaitUs;
    lastBaseIntervalUs_ = baseIntervalUs;
    lastFrameBeginKind_ = frameBeginKind;
    lastGeneratorHoldApplied_ = holdApplied;
    lastGeneratorHoldMeasured_ = holdMeasured;
}

}  // namespace ce::system_latency

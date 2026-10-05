#pragma once

#include "common/capture/time_units.h"
#include "common/capture/frame_timing_utils.h"
#include "mediaengine/audio/audio_time_utils.h"

namespace ce::media::timing {
using namespace ce::time;

inline AudioHundredNanoseconds AudioAnchor(QpcTicks qpc, QpcFrequency frequency) {
    return AudioHundredNanoseconds{qpc.count() > 0 && frequency.count() > 0
        ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
            static_cast<uint64_t>(qpc.count()), static_cast<uint64_t>(frequency.count()))) : 0};
}

inline Milliseconds DiagnosticTimestamp(QpcTicks qpc, QpcFrequency frequency) {
    // Preserve the legacy raw diagnostic fallback when a trusted frequency is unavailable.
    if (frequency.count() <= 0)
        return Milliseconds{qpc.count()};
    return Milliseconds{(qpc.count() / frequency.count()) * 1000 +
                        ((qpc.count() % frequency.count()) * 1000) / frequency.count()};
}

enum class SourceClock { Inject, ScreenGrab };
struct FirstOutputAnchor {
    Milliseconds source;
    Milliseconds audio;
    AudioHundredNanoseconds audioQpc;
};

// Confined to the existing mux lock. Sampling cursors retain the original VFR/CFR policy;
// only an accepted output commits the first anchor and the separate committed timeline.
class SubmissionTiming {
public:
    bool HasFirstOutput() const { return firstCommitted_; }
    FirstOutputAnchor FirstAnchor() const { return firstAnchor_; }
    template <typename Apply>
    bool CommitFirst(FirstOutputAnchor anchor, Apply&& apply) {
        if (firstCommitted_)
            return false;
        firstAnchor_ = anchor;
        firstCommitted_ = true;
        apply(anchor);
        return true;
    }
    Microseconds ResolveCandidate(SourceClock source, bool vfr, QpcFrequency frequency,
                                  QpcTicks timestamp, Microseconds steady, Microseconds scheduled) {
        auto& state = source == SourceClock::Inject ? inject_ : screenGrab_;
        if (vfr)
            return Microseconds{ComputeSourceDrivenElapsedUs(frequency.count(), timestamp.count(), steady.count(), state)};
        return Microseconds{source == SourceClock::ScreenGrab
            ? ResolveAuthoritativeCfrTimelineElapsedUs(steady.count(), scheduled.count(), state.lastElapsedUs)
            : ResolveCfrTimelineElapsedUs(steady.count(), scheduled.count(), state.lastElapsedUs)};
    }
    void CommitElapsed(SourceClock source, Microseconds elapsed) {
        if (elapsed.count() < 0)
            return;
        auto& state = source == SourceClock::Inject ? inject_ : screenGrab_;
        state.lastElapsedUs = std::max(state.lastElapsedUs, elapsed.count());
        (source == SourceClock::Inject ? committedInject_ : committedScreenGrab_) = elapsed;
    }
    Microseconds SamplingElapsed(SourceClock source) const {
        return Microseconds{source == SourceClock::Inject ? inject_.lastElapsedUs : screenGrab_.lastElapsedUs};
    }
    Microseconds CommittedElapsed(SourceClock source) const {
        return source == SourceClock::Inject ? committedInject_ : committedScreenGrab_;
    }
    void Reset() { *this = SubmissionTiming{}; }
private:
    bool firstCommitted_ = false;
    FirstOutputAnchor firstAnchor_;
    SourceTimelineState inject_;
    SourceTimelineState screenGrab_;
    Microseconds committedInject_;
    Microseconds committedScreenGrab_;
};

}  // namespace ce::media::timing

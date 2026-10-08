#pragma once

#include <cstddef>
#include <cstdint>

// Routing policy and frame accounting for the post-process pass (sharpen + display gamma).
//
// The pass submits CE work on the game's queue. While DLSS-G / FSR FG are starting, settling or
// being torn down, such submissions can hang the device (Witcher 3 session 20261008_172629: GPU
// fault on the first DLSS FG off), so the normal-route renderer has to stay out of those frames.
// This header keeps that decision, and the bookkeeping that makes every uncorrected frame visible
// in the log, free of any D3D12 state so it can be tested exhaustively.
namespace ce::post_process_route {

// Why the draw routing set skipOverlayDraw for the frame.
enum class SkipCause : uint8_t {
    None = 0,
    FocusLossHold,         // swapchain not presentable (occluded / minimized / zero-sized)
    FgTransitionCooldown,  // FG mode switch in flight
    PostSlWarmup,          // DLSS FG start: PostSL route not yet proven stable
    PostSlRecent,          // PostSL route fired within the last few presents and owns the frames
};

// What happened to one normal-route frame.
enum class Outcome : uint8_t {
    Applied = 0,
    AppliedOverlayUnavailable,  // overlay init pending/failed, pass ran without the overlay chain
    AlreadyApplied,             // another call within the same Present already corrected it
    CoveredByPostSl,            // the PostSL route runs the pass for these frames itself
    CoveredByRuntimeRoute,      // FSR/runtime-owned output route runs the pass at its final output
    SkippedFocusLossHold,
    SkippedFgTransition,
    SkippedUnclassified,
    SkippedOverlayUnavailable,  // overlay init pending and the state is not quiescent enough
    Failed,                     // the pass was asked to run and could not
    NotReached,                 // the frame left ProcessFrame before the post-process decision
};

constexpr size_t kOutcomeCount = static_cast<size_t>(Outcome::NotReached) + 1;

// Result of one call into the renderer.
enum class PassResult : uint8_t { NotRequested = 0, Applied, AlreadyApplied, Failed, Unavailable };

constexpr bool IsGap(Outcome outcome) {
    switch (outcome) {
        case Outcome::SkippedFocusLossHold:
        case Outcome::SkippedFgTransition:
        case Outcome::SkippedUnclassified:
        case Outcome::SkippedOverlayUnavailable:
        case Outcome::Failed:
        case Outcome::NotReached:
            return true;
        default:
            return false;
    }
}

constexpr bool IsCovered(Outcome outcome) {
    return outcome == Outcome::CoveredByPostSl || outcome == Outcome::CoveredByRuntimeRoute;
}

constexpr const char* Name(Outcome outcome) {
    switch (outcome) {
        case Outcome::Applied: return "applied";
        case Outcome::AppliedOverlayUnavailable: return "applied-without-overlay";
        case Outcome::AlreadyApplied: return "already-applied";
        case Outcome::CoveredByPostSl: return "covered-by-postsl";
        case Outcome::CoveredByRuntimeRoute: return "covered-by-runtime-route";
        case Outcome::SkippedFocusLossHold: return "focus-loss-hold";
        case Outcome::SkippedFgTransition: return "fg-transition";
        case Outcome::SkippedUnclassified: return "unclassified-skip";
        case Outcome::SkippedOverlayUnavailable: return "overlay-unavailable";
        case Outcome::Failed: return "pass-failed";
        case Outcome::NotReached: return "left-before-decision";
    }
    return "unknown";
}

constexpr Outcome FromPassResult(PassResult result, bool withoutOverlay) {
    switch (result) {
        case PassResult::Applied:
            return withoutOverlay ? Outcome::AppliedOverlayUnavailable : Outcome::Applied;
        case PassResult::AlreadyApplied:
            return Outcome::AlreadyApplied;
        case PassResult::Failed:
        case PassResult::Unavailable:
            return Outcome::Failed;
        case PassResult::NotRequested:
            break;
    }
    return Outcome::Applied;  // NotRequested frames are dropped before they are recorded
}

// Routes that run the pass outside the normal-route frame transaction, at the frame's final output.
enum class RuntimeRoute : uint8_t {
    PostSl = 0,       // DLSS FG: the PostSL render path, before the overlay is composited
    FsrCallback,      // FSR FG: AMD's present callback, onto the frame generation output
    FsrOverlayOutput  // FSR FG without a callback: CE's own submission onto the output
};
constexpr size_t kRuntimeRouteCount = 3;

constexpr const char* RouteName(RuntimeRoute route) {
    switch (route) {
        case RuntimeRoute::PostSl: return "postsl";
        case RuntimeRoute::FsrCallback: return "fsr-callback";
        case RuntimeRoute::FsrOverlayOutput: return "fsr-overlay-output";
    }
    return "unknown";
}

// --- Normal route (after the overlay init) -------------------------------------------------------

struct NormalRouteInputs {
    bool skipOverlayDraw = false;
    SkipCause cause = SkipCause::None;
    // DLSS-G toggle-ON before the first confirmed PostSL render: the overlay itself is drawn on the
    // game queue pre-SL for these frames (it is the overlay's only route), so the pass runs under
    // exactly the same submission conditions.
    bool preSlDrawKeptThroughToggleOn = false;
    // A runtime-owned (FSR FG) swapchain: the pass runs at that route's final output instead.
    bool separateGpuWorkBlocked = false;
};

constexpr bool NormalRouteMayPostProcess(const NormalRouteInputs& in) {
    if (in.separateGpuWorkBlocked)
        return false;
    if (!in.skipOverlayDraw)
        return true;
    // A held (not presentable) swapchain never takes CE work, whatever else is kept alive.
    return in.preSlDrawKeptThroughToggleOn && in.cause != SkipCause::FocusLossHold;
}

// Classifies a frame NormalRouteMayPostProcess refused.
constexpr Outcome ClassifyNormalRouteSkip(const NormalRouteInputs& in) {
    if (in.separateGpuWorkBlocked)
        return Outcome::CoveredByRuntimeRoute;
    switch (in.cause) {
        // The PostSL route is confirmed rendering in both: it runs the pass on every output it presents. Its
        // failures are counted per route (RuntimeRoute::PostSl), where the displayed frames are.
        case SkipCause::PostSlRecent:
        case SkipCause::PostSlWarmup: return Outcome::CoveredByPostSl;
        case SkipCause::FocusLossHold: return Outcome::SkippedFocusLossHold;
        case SkipCause::FgTransitionCooldown: return Outcome::SkippedFgTransition;
        case SkipCause::None: break;
    }
    return Outcome::SkippedUnclassified;
}

// --- Overlay unavailable (init deferred / failed) ------------------------------------------------

// The pass does not depend on the overlay, so it should keep correcting frames while overlay init is
// deferred (startup grace, resume settle, init back-off after failures). It may only do so in a state
// where CE work on the game queue is as safe as in a plain game: no FG runtime, no transition in
// flight, no Streamline teardown grace, a presentable swapchain on a healthy device.
struct OverlayUnavailableInputs {
    bool focusLossHold = false;
    bool deviceLost = false;
    bool insideExecuteCommandLists = false;
    bool fgTransitionCooldown = false;
    bool streamlineOffGrace = false;
    bool streamlineFgRunning = false;
    bool fgActive = false;
    bool runtimeOwnsSwapchain = false;
    bool postSlRouteActive = false;
    bool postSlKeepAlive = false;
};

constexpr bool MayPostProcessWithoutOverlay(const OverlayUnavailableInputs& in) {
    return !(in.focusLossHold || in.deviceLost || in.insideExecuteCommandLists || in.fgTransitionCooldown ||
             in.streamlineOffGrace || in.streamlineFgRunning || in.fgActive || in.runtimeOwnsSwapchain ||
             in.postSlRouteActive || in.postSlKeepAlive);
}

// --- Frame accounting ----------------------------------------------------------------------------

// Counts normal-route frames by outcome and reports when uncorrected frames deserve a log line, so a
// log shows how many frames were missed and why without one line per frame: the end of the first runs
// of consecutive uncorrected frames (then every 64th run), a progress line inside a run that does not
// end, and a periodic summary while uncorrected frames keep appearing. Frame counted, not time
// counted: deterministic and free of clocks. Not thread safe; the caller serializes.
class FrameLedger {
public:
    static constexpr uint32_t kLoggedRunBurst = 16;       // log the end of the first runs ...
    static constexpr uint32_t kLoggedRunStride = 64;      // ... then every 64th
    static constexpr uint64_t kHeartbeatFrames = 1200;    // progress line inside a run that never ends
    static constexpr uint64_t kSummaryFrames = 7200;      // totals, only while gaps keep appearing

    struct Report {
        Outcome outcome = Outcome::Applied;  // outcome of the frame just noted
        bool logRunEnd = false;
        Outcome endedOutcome = Outcome::Applied;
        uint64_t endedFrames = 0;    // length of the run that just ended
        uint64_t endedRunIndex = 0;  // 1-based index of that run
        bool logHeartbeat = false;
        uint64_t runFrames = 0;      // length of the current run
        uint64_t runIndex = 0;
        bool logSummary = false;
    };

    Report Note(Outcome outcome) {
        Report report;
        report.outcome = outcome;
        const bool gap = IsGap(outcome);
        ++totalFrames_;
        ++byOutcome_[static_cast<size_t>(outcome)];
        if (gap) {
            ++gapFrames_;
        } else if (IsCovered(outcome)) {
            ++coveredFrames_;
        } else {
            ++correctedFrames_;
        }

        if (inRun_ && (!gap || outcome != runOutcome_)) {
            report.logRunEnd = runIndex_ <= kLoggedRunBurst || (runIndex_ % kLoggedRunStride) == 0;
            report.endedOutcome = runOutcome_;
            report.endedFrames = runFrames_;
            report.endedRunIndex = runIndex_;
            inRun_ = false;
        }
        if (gap && !inRun_) {
            inRun_ = true;
            runOutcome_ = outcome;
            runFrames_ = 0;
            ++runIndex_;
        }
        if (inRun_) {
            ++runFrames_;
            report.runFrames = runFrames_;
            report.runIndex = runIndex_;
            report.logHeartbeat = (runFrames_ % kHeartbeatFrames) == 0;
        }
        if (totalFrames_ - framesAtLastSummary_ >= kSummaryFrames) {
            framesAtLastSummary_ = totalFrames_;
            report.logSummary = gapFrames_ != gapFramesAtLastSummary_;
            gapFramesAtLastSummary_ = gapFrames_;
        }
        return report;
    }

    uint64_t CorrectedFrames() const { return correctedFrames_; }
    uint64_t CoveredFrames() const { return coveredFrames_; }
    uint64_t GapFrames() const { return gapFrames_; }
    uint64_t GapRuns() const { return runIndex_; }
    uint64_t TotalFrames() const { return totalFrames_; }
    uint64_t OutcomeCount(Outcome outcome) const { return byOutcome_[static_cast<size_t>(outcome)]; }

private:
    uint64_t byOutcome_[kOutcomeCount] = {};
    uint64_t totalFrames_ = 0;
    uint64_t correctedFrames_ = 0;
    uint64_t coveredFrames_ = 0;
    uint64_t gapFrames_ = 0;
    uint64_t runIndex_ = 0;
    uint64_t runFrames_ = 0;
    uint64_t framesAtLastSummary_ = 0;
    uint64_t gapFramesAtLastSummary_ = 0;
    Outcome runOutcome_ = Outcome::Applied;
    bool inRun_ = false;
};

}  // namespace ce::post_process_route

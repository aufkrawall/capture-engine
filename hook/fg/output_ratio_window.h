#pragma once

#include <cstdint>

namespace ce::fg_runtime {

// Where an application frame boundary was seen.
enum class ApplicationFrameSource : uint8_t {
    kPresentHook,  // the game's own Present reached CE's DXGI hook (DX12_ObserveApplicationSourcePresentTiming)
    kTitleMarker,  // the title's Streamline PCL PresentStart marker, on its own thread
};

inline const char* ApplicationFrameSourceName(ApplicationFrameSource source) {
    return source == ApplicationFrameSource::kTitleMarker ? "title-marker" : "present-hook";
}

// Outputs per application frame, measured over windows of frames: the half of the DLSS-G idle proof
// that does not trust the runtime's own report (FGCompatibility::IsDLSSGGenerationIdle).
//
// It was a streak of source groups with exactly one final output each. That needs the game's own
// Present at CE's DXGI hook, and after an FSR FG phase Talos's fresh DLSS-G swapchain presented
// every output from Streamline's thread while the game's Present into Streamline never reached the
// hook: no group ever completed, so DLSS-G could not be proven idle there (20261019_182822). The
// title's PresentStart marker is that frame boundary on every route, but it runs on the game thread,
// asynchronous to Streamline's output thread, so per-frame counts jitter (an idle 1,1,1 reads 2,0,1
// whenever an output lands across the boundary) and an exact per-frame test would flap. A ratio over
// a window is immune: boundary jitter moves at most a couple of outputs between windows. The same
// jitter is why these markers do not feed the final-output timeline, whose interval follows every
// completed group (final_output_timing.h).
//
// Exactly one frame source counts: once the title's markers are seen, present-hook frames are
// ignored. Counting both would halve the ratio and make 2x generation look idle.
class OutputRatioWindow {
public:
    static constexpr uint32_t kWindowFrames = 32;
    static constexpr uint32_t kBoundarySlackOutputs = 2;

    struct Verdict {
        bool changed = false;
        bool single = false;     // about one output per application frame
        uint32_t outputs = 0;    // in the window (or partial window) that decided
        uint32_t frames = 0;
    };

    void NoteOutput() {
        if (windowOutputs_ != UINT32_MAX)  // no frame source at all must not wrap the count
            ++windowOutputs_;
    }

    Verdict NoteFrame(ApplicationFrameSource source) {
        if (source == ApplicationFrameSource::kTitleMarker) {
            if (!titleMarkers_) {
                titleMarkers_ = true;
                Restart();
                return Set(false, 0, 0);
            }
        } else if (titleMarkers_) {
            return {};
        }

        ++windowFrames_;
        // Generation resuming must not wait for the window: more outputs than frames, beyond what a
        // boundary can shift, ends the single-output verdict at once.
        if (single_ && windowOutputs_ > windowFrames_ + windowFrames_ / 4 + kBoundarySlackOutputs)
            return Set(false, windowOutputs_, windowFrames_);
        if (windowFrames_ < kWindowFrames)
            return {};

        // A window with far fewer outputs than frames is no proof either: outputs went uncounted.
        const uint64_t outputs4 = static_cast<uint64_t>(windowOutputs_) * 4;
        const bool single = outputs4 >= static_cast<uint64_t>(windowFrames_) * 3 &&
                            outputs4 <= static_cast<uint64_t>(windowFrames_) * 5;
        const uint32_t outputs = windowOutputs_;
        const uint32_t frames = windowFrames_;
        Restart();
        return Set(single, outputs, frames);
    }

    bool Single() const { return single_; }
    bool TitleMarkers() const { return titleMarkers_; }

    void Reset() {
        Restart();
        single_ = false;
        titleMarkers_ = false;
    }

private:
    void Restart() {
        windowOutputs_ = 0;
        windowFrames_ = 0;
    }

    Verdict Set(bool single, uint32_t outputs, uint32_t frames) {
        Verdict verdict{single != single_, single, outputs, frames};
        single_ = single;
        return verdict;
    }

    uint32_t windowOutputs_ = 0;
    uint32_t windowFrames_ = 0;
    bool single_ = false;
    bool titleMarkers_ = false;
};

}  // namespace ce::fg_runtime

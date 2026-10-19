#pragma once

#include <array>
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
// a window is immune: boundary jitter only moves outputs between neighbouring frames. The same
// jitter is why these markers do not feed the final-output timeline, whose interval follows every
// completed group (final_output_timing.h).
//
// Exactly one frame source counts: once the title's markers are seen, present-hook frames are
// ignored. Counting both would halve the ratio and make 2x generation look idle.
class OutputRatioWindow {
public:
    // A sliding window: the verdict covers exactly the last kWindowFrames application frames, so idleness is proven
    // that many frames after it begins (0.4 s at 40 fps). Tumbling 32-frame windows took up to two windows, and the
    // game showed its base rate for about a second at every menu entry (Talos 20261019_195220).
    static constexpr uint32_t kWindowFrames = 16;

    struct Verdict {
        bool changed = false;
        bool single = false;   // about one output per application frame
        uint32_t outputs = 0;  // in the window that decided
        uint32_t frames = 0;
    };

    void NoteOutput() {
        if (pendingOutputs_ != UINT16_MAX)  // no frame source at all must not wrap the count
            ++pendingOutputs_;
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

        // Outputs since the previous boundary belong to this frame; a boundary asynchronous to the outputs only
        // moves one between neighbouring frames, which the window sum does not see.
        if (filled_ == kWindowFrames)
            windowOutputs_ -= perFrame_[head_];
        else
            ++filled_;
        perFrame_[head_] = pendingOutputs_;
        windowOutputs_ += pendingOutputs_;
        pendingOutputs_ = 0;
        head_ = (head_ + 1) % kWindowFrames;
        if (filled_ < kWindowFrames)
            return {};

        // 3/4..5/4 outputs per frame. Fewer is no proof either: outputs went uncounted.
        const uint64_t outputs4 = static_cast<uint64_t>(windowOutputs_) * 4;
        const bool single = outputs4 >= static_cast<uint64_t>(kWindowFrames) * 3 &&
                            outputs4 <= static_cast<uint64_t>(kWindowFrames) * 5;
        return Set(single, windowOutputs_, filled_);
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
        perFrame_ = {};
        windowOutputs_ = 0;
        pendingOutputs_ = 0;
        filled_ = 0;
        head_ = 0;
    }

    Verdict Set(bool single, uint32_t outputs, uint32_t frames) {
        Verdict verdict{single != single_, single, outputs, frames};
        single_ = single;
        return verdict;
    }

    std::array<uint16_t, kWindowFrames> perFrame_{};
    uint32_t windowOutputs_ = 0;
    uint16_t pendingOutputs_ = 0;
    uint32_t filled_ = 0;
    uint32_t head_ = 0;
    bool single_ = false;
    bool titleMarkers_ = false;
};

}  // namespace ce::fg_runtime

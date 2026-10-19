#pragma once

#include <cstdint>

namespace ce::dx12_overlay_policy {

// Whether the frame-classification queue still sees the game's submissions.
//
// Classification counts command lists on ONE queue (GetFrameClassificationQueue, normally the
// game's original Present queue): only submissions made in step with the Presents tell an
// application frame from a frame-generation runtime's. A game can stop submitting there while it
// keeps rendering on another DIRECT queue. After an FSR FG phase, Talos's render thread kept its
// 2-3 lists per frame on the primary queue while the original queue saw nothing, so every
// Present counted zero for the rest of the session and no line said where the work had gone
// (20261019_182822: `cmdLists=0` with `consInterp` rising for 35 s).
//
// Following that other queue is rejected. Its submissions are not in step with the Presents, so
// per-Present counts there have spurious zero windows, and the ECL-pattern heuristic latches FSR
// FG after ten of them. This tracker only reports the state; consumers that must not read a zero
// count as FG evidence decide that from the runtime state (MayFrameGenerationProduceThisPresent).
class FrameClassificationStarvation {
public:
    static constexpr uint64_t kStarvedPresents = 120;
    static constexpr uint64_t kReminderPresents = 3000;

    enum class Event : uint8_t { kNone, kStarved, kStillStarved, kRecovered };

    struct Report {
        Event event = Event::kNone;
        uint64_t presents = 0;          // consecutive zero-count Presents
        uint64_t otherSubmissions = 0;  // command lists the other queue carried meanwhile
    };

    // Per Present: command lists counted since the previous Present on the classification queue
    // and on the other known application queue. A run in which the other queue is silent too is
    // a game that renders nothing, which the zero count describes correctly.
    Report Observe(uint32_t classified, uint32_t other) {
        Report report;
        if (classified > 0) {
            if (starved_)
                report = {Event::kRecovered, run_, otherDuringRun_};
            starved_ = false;
            run_ = 0;
            otherDuringRun_ = 0;
            return report;
        }
        ++run_;
        otherDuringRun_ += other;
        if (!starved_) {
            if (run_ >= kStarvedPresents && otherDuringRun_ > 0) {
                starved_ = true;
                report = {Event::kStarved, run_, otherDuringRun_};
            }
        } else if (run_ % kReminderPresents == 0) {
            report = {Event::kStillStarved, run_, otherDuringRun_};
        }
        return report;
    }

    bool Starved() const { return starved_; }

private:
    bool starved_ = false;
    uint64_t run_ = 0;
    uint64_t otherDuringRun_ = 0;
};

}  // namespace ce::dx12_overlay_policy

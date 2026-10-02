#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>

#include "display_timing_correlation.h"
#include "display_timing_policy.h"

// Screen times for a process whose presents the desktop compositor draws
// instead of flipping them itself.
//
// Such a present never completes as a flip of its own: the process's surface
// becomes ready when its present packet finishes on the GPU, and the frame
// reaches the screen with the first compositor frame built after that. The
// compositor's own presents do flip, so its completion is the composed frame's
// screen time. A compositor frame carries only the newest ready surface of the
// process; older ready frames were replaced before any composition showed them.
//
// DOOM Eternal session `20260928_044654` (native Vulkan, 4K -> 1440p mode): once
// NVIDIA's WSI composed the game's frames, the display-change stream had no
// sample of the game at all, so the overlay's latency estimate went to
// "PC Latency -" and the frame-time graph fell back to presentation timing.
//
// This is a bound, not an exact identity: a surface that became ready while the
// compositor was already building its frame is attributed to that frame, which
// can put the screen time one refresh early. The compositor's submission is the
// latest point that is observable without the compositor's own trace provider.

struct ComposedFrame {
    uint32_t processId = 0;
    uint64_t associationId = 0;
    uint32_t submitSequence = 0;
    int64_t submitTimestamp = 0;
    int64_t presentStartTimestamp = 0;
    int64_t readyTimestamp = 0;
};

class ComposedPresentation {
public:
    // Only bounds a runaway producer; claims and the completion bound drain it.
    static constexpr std::size_t kMaxReadyFrames = 64;

    // processId's presents are composed; compositorPid is the compositor whose
    // flips carry them (0 when it could not be identified: nothing is claimed).
    void Begin(uint32_t processId, uint32_t compositorPid) {
        processId_ = processId;
        compositorPid_ = compositorPid;
        ready_.clear();
    }

    void End() {
        processId_ = 0;
        compositorPid_ = 0;
        ready_.clear();
    }

    bool active() const noexcept { return processId_ != 0; }
    uint32_t processId() const noexcept { return processId_; }
    uint32_t compositorPid() const noexcept { return compositorPid_; }
    bool IsCompositor(uint32_t processId) const noexcept {
        return compositorPid_ != 0 && processId == compositorPid_;
    }

    // The composed process's present packet finished at readyTimestamp.
    // maxAge is the submit-to-completion bound; a packet completing outside it
    // (or before its own submission) is another engine's packet that happens
    // to carry the same submit sequence.
    bool ObserveReady(uint32_t submitSequence, const SubmitAssociation& association, int64_t readyTimestamp,
                      int64_t maxAge) {
        if (!active() || association.processId != processId_ ||
            !IsPlausibleSubmitCompletion(association.timestamp, readyTimestamp, maxAge)) {
            return false;
        }
        for (const ComposedFrame& frame : ready_) {
            if (frame.associationId == association.associationId)
                return false;
        }
        if (ready_.size() >= kMaxReadyFrames) {
            ready_.pop_front();
            ++superseded_;
        }
        const ComposedFrame frame = {association.processId, association.associationId, submitSequence,
                                     association.timestamp, association.presentStartTimestamp, readyTimestamp};
        auto position = ready_.end();
        while (position != ready_.begin() && std::prev(position)->readyTimestamp > readyTimestamp)
            --position;
        ready_.insert(position, frame);
        ++readyObserved_;
        return true;
    }

    // The frame a compositor present submitted at compositorSubmitTimestamp
    // shows: the newest one ready by then. Older ready frames are dropped as
    // replaced; frames that became ready later wait for a later composition.
    std::optional<ComposedFrame> TakeFrameShownBy(int64_t compositorSubmitTimestamp) {
        std::size_t newest = ready_.size();
        for (std::size_t i = 0; i < ready_.size() && ready_[i].readyTimestamp <= compositorSubmitTimestamp; ++i)
            newest = i;
        if (newest == ready_.size())
            return std::nullopt;
        const ComposedFrame frame = ready_[newest];
        superseded_ += newest;
        ready_.erase(ready_.begin(),
                     ready_.begin() + static_cast<std::deque<ComposedFrame>::difference_type>(newest + 1));
        ++claimed_;
        return frame;
    }

    void PruneBefore(int64_t cutoff) {
        while (!ready_.empty() && ready_.front().readyTimestamp < cutoff)
            ready_.pop_front();
    }

    std::size_t pending() const noexcept { return ready_.size(); }
    uint64_t readyObserved() const noexcept { return readyObserved_; }
    uint64_t claimed() const noexcept { return claimed_; }
    uint64_t superseded() const noexcept { return superseded_; }

private:
    uint32_t processId_ = 0;
    uint32_t compositorPid_ = 0;
    std::deque<ComposedFrame> ready_;
    uint64_t readyObserved_ = 0;
    uint64_t claimed_ = 0;
    uint64_t superseded_ = 0;
};

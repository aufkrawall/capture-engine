#pragma once

#include <cstddef>
#include <cstdint>

// Stack-overflow attribution for crash.log: which code the overflowing recursion runs through, and whether
// any of it is CE's. A foreign self-recursion and a loop CE takes part in look alike at the fault address
// (session 20261024_131043: 75k frames of OptiScaler's own detour reached through its second detour of the
// same function; 20261023_173151: OptiScaler's loop with CE's hook once in it), so the line counts CE's
// frames over the whole walk. Fed one return address at a time, innermost first: the crash path allocates
// nothing however deep the stack is.
namespace ce::crash_stack {

struct Frame {
    uintptr_t returnAddress = 0;
    uintptr_t imageBase = 0;  // 0 when no loaded image claims the address
};

class OverflowFrameSummary {
   public:
    static constexpr size_t kHeadFrames = 64;
    static constexpr size_t kMaxCyclePeriod = 16;
    // Frames between the fault and the cycle (the faulting function's own callee, a dispatch thunk).
    static constexpr size_t kMaxCycleStart = 8;
    static constexpr size_t kTailFrames = 4;
    static constexpr size_t kNoDepth = static_cast<size_t>(-1);

    void Add(const Frame& frame, bool ownModule) {
        if (ownModule) {
            if (ownFrames_ == 0) {
                firstOwnDepth_ = frames_;
            }
            ++ownFrames_;
        }
        if (frames_ < kHeadFrames) {
            head_[frames_] = frame;
        } else if (cycleOpen_ && cyclePeriod_ != 0 &&
                   frame.returnAddress ==
                       head_[cycleStart_ + (frames_ - cycleStart_) % cyclePeriod_].returnAddress) {
            ++cycleFrames_;
        } else {
            cycleOpen_ = false;
        }
        tail_[frames_ % kTailFrames] = frame;
        ++frames_;
        if (frames_ == kHeadFrames) {
            DetectCycle();
        }
    }

    // A walk shorter than kHeadFrames still gets its cycle judged on what it has.
    void Finish() {
        if (frames_ < kHeadFrames) {
            DetectCycle();
        }
    }

    size_t Frames() const { return frames_; }
    size_t OwnFrames() const { return ownFrames_; }
    size_t FirstOwnDepth() const { return firstOwnDepth_; }
    // Smallest period that repeats over the innermost frames; 0 when they do not repeat.
    size_t CyclePeriod() const { return cyclePeriod_; }
    // Depth of the cycle's first frame (0 when it starts right at the fault).
    size_t CycleStart() const { return cycleStart_; }
    // Frames from CycleStart() on that follow that period without a break.
    size_t CycleFrames() const { return cycleFrames_; }
    // The cycle's frames when CyclePeriod() != 0 (index < period), else the innermost frames.
    const Frame& CycleFrame(size_t index) const { return head_[cycleStart_ + index]; }
    size_t TailCount() const { return frames_ < kTailFrames ? frames_ : kTailFrames; }
    // index 0 is the outermost frame (the thread's start).
    const Frame& Tail(size_t index) const { return tail_[(frames_ - 1 - index) % kTailFrames]; }

   private:
    void DetectCycle() {
        const size_t count = frames_ < kHeadFrames ? frames_ : kHeadFrames;
        cyclePeriod_ = 0;
        cycleStart_ = 0;
        cycleFrames_ = 0;
        cycleOpen_ = false;
        for (size_t start = 0; start <= kMaxCycleStart; ++start) {
            for (size_t period = 1; period <= kMaxCyclePeriod && start + 2 * period <= count; ++period) {
                bool repeats = true;
                for (size_t i = start; i + period < count && repeats; ++i) {
                    repeats = head_[i].returnAddress == head_[i + period].returnAddress;
                }
                if (repeats) {
                    cyclePeriod_ = period;
                    cycleStart_ = start;
                    cycleFrames_ = count - start;
                    cycleOpen_ = true;
                    return;
                }
            }
        }
    }

    Frame head_[kHeadFrames] = {};
    Frame tail_[kTailFrames] = {};
    size_t frames_ = 0;
    size_t ownFrames_ = 0;
    size_t firstOwnDepth_ = kNoDepth;
    size_t cyclePeriod_ = 0;
    size_t cycleStart_ = 0;
    size_t cycleFrames_ = 0;
    bool cycleOpen_ = false;
};

}  // namespace ce::crash_stack

#pragma once

#include <cstdint>

#include "hook/fg/fg_runtime_state.h"
#include "hook/pacing/fps_limiter_policy.h"

// How the FPS limiter paces a present interposer (NVIDIA Smooth Motion), which hands the application a proxy and
// presents a frame's outputs on its own worker threads.
//
// Where CE sees the application's Present (its swapchain wrapper, DX12's normal case), that Present owns the cadence on
// the game thread and the outputs pass, as under FSR FG (ce::runtime_input_site_policy). The driver then spaces the
// outputs itself: without a limiter its flips run half an interval ahead of the screen and stay flat (Trine 5 DX11,
// 109 fps, display stddev 78-250 us, announced flip lead 4345 us).
//
// Where CE only sees the outputs (Trine 5 DX11, Vulkan), holding them is the only pacing there is, and how it holds them
// decides the graph. Waiting on one output per frame leaves the driver's flip schedule with nothing it can place: at a
// 140 cap the flip lead collapsed to 770 us and the display read stddev 1315 us, p1 3700 us, p99 8700 us (120 cap: stddev
// 1436 us, p1 400 us), while the waited presents themselves were exact to 13 us (20261106_181807). Every output on its own
// slot of the output grid reads 133-237 us at the same cap in the same game (20261106_151156, where the undetected status
// happened to pace them that way). So every output of a capped frame gets its slot.
//
// Below the cap CE has nothing to add and leaves the outputs to the driver: an output group whose first output missed
// its slot passes whole.
namespace ce::interposer_output_pacing {

// Which output of its application frame a present is; 0 opens the frame's group. All of a frame's outputs come from one
// worker, so a thread change opens a group whatever the count says, and a worker that got two frames in a row is split
// by the multiplier (runs of 4 and 6 at 2x in the Trine 5 sessions). A dedicated presenter thread has only the count,
// and its phase does not matter: all outputs of a capped frame sit on one equal grid.
class OutputGroupTracker {
public:
    uint32_t Next(uint32_t threadId, int multiplier) {
        const uint32_t groupSize = multiplier > 1 ? static_cast<uint32_t>(multiplier) : 1u;
        if (threadId != lastThreadId_ || position_ >= groupSize) {
            position_ = 0;
        }
        lastThreadId_ = threadId;
        return position_++;
    }

    void Reset() {
        position_ = 0;
        lastThreadId_ = 0;
    }

private:
    uint32_t position_ = 0;
    uint32_t lastThreadId_ = 0;
};

// The slots of the current group's later outputs. Armed only by a first output that waited for its slot, which is what
// proves CE is the limit for this frame.
class GroupSchedule {
public:
    void Arm(int64_t deadlineTick, int64_t nextDeadlineTick, int multiplier) {
        deadlineTick_ = deadlineTick;
        intervalTicks_ = nextDeadlineTick - deadlineTick;
        multiplier_ = multiplier;
    }

    void Clear() {
        *this = {};
    }

    // 0 when this output has no slot and passes.
    int64_t OutputDeadline(uint32_t outputIndex) const {
        if (multiplier_ < 2 || intervalTicks_ <= 0 || outputIndex == 0 ||
            outputIndex >= static_cast<uint32_t>(multiplier_)) {
            return 0;
        }
        return deadlineTick_ + intervalTicks_ * outputIndex / multiplier_;
    }

private:
    int64_t deadlineTick_ = 0;
    int64_t intervalTicks_ = 0;
    int multiplier_ = 0;
};

// CE's swapchain wrapper is on the game thread, in front of the interposer: its site owns the cadence.
inline fps_limiter_policy::PresentSite ResolveWrapperPresentSite(bool presentInterposerOwnsPresent) {
    return presentInterposerOwnsPresent ? fps_limiter_policy::PresentSite::kRuntimeInputPresent
                                        : fps_limiter_policy::PresentSite::kUniqueApplicationPresent;
}

// The site of a native Vulkan present or acquire. CE's layer sits below VK_LAYER_NV_present, so Smooth Motion's outputs
// arrive there; every other generator's outputs keep the plain output groups, whose spacing is that generator's own.
inline fps_limiter_policy::PresentSite ResolveVulkanFinalOutputSite(fg_runtime::RuntimeMode runtimeMode) {
    return runtimeMode == fg_runtime::RuntimeMode::kNvidiaSmoothMotion
               ? fps_limiter_policy::PresentSite::kInterposerOutputPresent
               : fps_limiter_policy::PresentSite::kFinalOutputBoundary;
}

}  // namespace ce::interposer_output_pacing

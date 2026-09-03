#pragma once

// Where the FG switch app's own frame time goes, per heartbeat window.
//
// A frame-rate A/B against an injected overlay says how much a frame lost, never
// which call lost it. These phases are the app's own call sites around the
// frame-generation runtime, so the same numbers can be read with and without an
// overlay injected: if the loss lands in Present it is pacing or the GPU, if it
// lands in ffxConfigure or ExecuteCommandLists it is a hook on that call, and if
// it lands in "other" it is the app itself.
//
// Wall time, deliberately: a hook that BLOCKS the calling thread is the case that
// CPU-cycle accounting cannot see.

#include <windows.h>

#include <cstdint>

namespace testapp::fg {

enum class FramePhase : int {
    kPresent = 0,       // the (possibly proxied) swapchain Present
    kFfxConfigure,      // ffxConfigure: per-frame enable refresh + UI registration
    kFfxDispatch,       // ffxDispatch: upscale + frame-generation prepare
    kExecuteCommandLists,
    kCount,
};

class FramePhaseTimers {
public:
    void Add(FramePhase phase, int64_t ticks) {
        const int index = static_cast<int>(phase);
        if (index < 0 || index >= static_cast<int>(FramePhase::kCount)) {
            return;
        }
        ticks_[index] += ticks;
        ++calls_[index];
    }

    // Microseconds per frame in this phase over the window, then reset. Returns 0
    // for a window with no frames rather than dividing by zero.
    double TakeUsPerFrame(FramePhase phase, uint64_t frames) {
        const int index = static_cast<int>(phase);
        if (index < 0 || index >= static_cast<int>(FramePhase::kCount)) {
            return 0.0;
        }
        const int64_t ticks = ticks_[index];
        ticks_[index] = 0;
        calls_[index] = 0;
        if (frames == 0) {
            return 0.0;
        }
        return static_cast<double>(ticks) * TicksToUs() / static_cast<double>(frames);
    }

    static int64_t Now() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    static double TicksToUs() {
        static const double s_scale = [] {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return frequency.QuadPart > 0 ? 1000000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
        }();
        return s_scale;
    }

private:
    int64_t ticks_[static_cast<int>(FramePhase::kCount)] = {};
    uint64_t calls_[static_cast<int>(FramePhase::kCount)] = {};
};

// The phases are recorded from the render thread only, so no synchronization is
// needed and the measurement cannot perturb what it measures.
class ScopedFramePhase {
public:
    ScopedFramePhase(FramePhaseTimers& timers, FramePhase phase)
        : timers_(timers), phase_(phase), enterTicks_(FramePhaseTimers::Now()) {}

    ~ScopedFramePhase() { timers_.Add(phase_, FramePhaseTimers::Now() - enterTicks_); }

    ScopedFramePhase(const ScopedFramePhase&) = delete;
    ScopedFramePhase& operator=(const ScopedFramePhase&) = delete;

private:
    FramePhaseTimers& timers_;
    FramePhase phase_;
    int64_t enterTicks_;
};

}  // namespace testapp::fg

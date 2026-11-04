#pragma once

#include <atomic>
#include <cstdint>

// Present-interposer output cadence.
//
// A present interposer (NVIDIA Smooth Motion's NvPresent64) hands the application a proxy
// swapchain and presents its own PRIVATE chain once per frame it actually puts on screen. The
// application's present stream therefore stays 1x no matter how many frames the driver generates,
// which is why the command-list population and present-gap heuristics in fg_detection can never
// see Smooth Motion in DX12: they only ever observe the application's stream.
//
// The ratio between the two streams is direct structural evidence and needs no heuristic at all.
// Strange Brigade DX12, session 20260914_105853: every application present is followed by exactly
// two presents on the interposer's private chain.
namespace ce::present_interposer {

// Minimum application presents in a window before a verdict is trustworthy. Below this a startup
// burst or an alt-tab gap can produce any ratio at all.
inline constexpr int kMinApplicationPresentsForVerdict = 20;
// Highest generation factor CE reports. Present interposers ship 2x today; the clamp keeps a
// miscounted window from publishing an absurd multiplier into the overlay.
inline constexpr int kMaxInterposerMultiplier = 4;
inline constexpr int64_t kWindowUs = 1000000;
// An application frame closes a window once it is a window old, so a window twice that old had none for a whole one.
inline constexpr int64_t kApplicationStoppedWindowUs = 2 * kWindowUs;

struct CadenceVerdict {
    bool generating = false;
    int multiplier = 1;
    float outputFps = 0.0f;
    float applicationFps = 0.0f;
    // The window held enough evidence to say whether frames are generated. Too few application frames say nothing.
    bool decided = false;
    // Decided from outputs alone: they kept coming for a whole window with no application frame behind them.
    bool applicationStopped = false;
};

// Pure: is the interposer generating frames in this window, and by what factor?
//
// Below 1.5x the interposer is present in the chain but forwarding one output per application
// frame — Smooth Motion loaded and not engaged, which must read as "no frame generation" rather
// than as a 1x generator.
inline bool IsInterposerGeneratingFrames(int applicationPresents, int outputPresents) {
    if (applicationPresents < kMinApplicationPresentsForVerdict || outputPresents <= applicationPresents) {
        return false;
    }
    return outputPresents * 2 >= applicationPresents * 3;
}

// Pure: nearest whole generation factor for a window already known to be generating.
inline int ResolveInterposerMultiplier(int applicationPresents, int outputPresents) {
    if (applicationPresents <= 0) {
        return 1;
    }
    // Round half up: (out + app/2) / app.
    const int multiplier = (outputPresents * 2 + applicationPresents) / (applicationPresents * 2);
    if (multiplier < 2) {
        return 2;
    }
    return multiplier > kMaxInterposerMultiplier ? kMaxInterposerMultiplier : multiplier;
}

// Pure: the full verdict for one measurement window.
inline CadenceVerdict ClassifyInterposerCadence(int applicationPresents, int outputPresents, int64_t windowUs) {
    CadenceVerdict verdict;
    if (applicationPresents < kMinApplicationPresentsForVerdict || windowUs <= 0) {
        return verdict;
    }
    const float windowSeconds = static_cast<float>(windowUs) / 1000000.0f;
    verdict.decided = true;
    verdict.applicationFps = static_cast<float>(applicationPresents) / windowSeconds;
    verdict.outputFps = static_cast<float>(outputPresents) / windowSeconds;
    verdict.generating = IsInterposerGeneratingFrames(applicationPresents, outputPresents);
    verdict.multiplier = verdict.generating ? ResolveInterposerMultiplier(applicationPresents, outputPresents) : 1;
    return verdict;
}

// Pure: outputs with no application frame behind them. Nothing is being generated from the application's frames.
inline CadenceVerdict ClassifyStoppedApplication(int outputPresents, int64_t windowUs) {
    CadenceVerdict verdict;
    verdict.decided = true;
    verdict.applicationStopped = true;
    if (windowUs > 0) {
        verdict.outputFps = static_cast<float>(outputPresents) * 1000000.0f / static_cast<float>(windowUs);
    }
    return verdict;
}

// Counts both streams over a sliding window. An interposer's two Note* calls come from the same render thread in
// present order (its private present happens inside the application's); a runtime's outputs come from its presenter
// thread too (XeFG), so its owner serializes window changes and verdict publication together. The interposer's
// counters stay relaxed: a present landing in the neighbouring window is one present's worth of skew.
class CadenceTracker {
public:
    void NoteOutputPresent() {
        outputPresents_.fetch_add(1, std::memory_order_relaxed);
    }

    // The form for a runtime whose outputs can outlive the application's frames. Talos stops sending XeFG present
    // IDs in a menu and XeFG keeps presenting (20261104_183540), so no application frame would ever close the window
    // and report it. Returns true and fills `out` when the output closed one on that evidence.
    bool NoteOutputPresent(int64_t nowUs, CadenceVerdict* out) {
        const int outputPresents = outputPresents_.fetch_add(1, std::memory_order_relaxed) + 1;
        int64_t windowStartUs = windowStartUs_.load(std::memory_order_relaxed);
        if (windowStartUs < 0) {
            // A toggle can happen in a menu that never sends a present ID. Outputs must be able to start its window.
            windowStartUs_.store(nowUs, std::memory_order_relaxed);
            return false;
        }
        if (nowUs - windowStartUs < kApplicationStoppedWindowUs ||
            outputPresents < kMinApplicationPresentsForVerdict ||
            !windowStartUs_.compare_exchange_strong(windowStartUs, nowUs, std::memory_order_relaxed)) {
            return false;
        }
        outputPresents_.store(0, std::memory_order_relaxed);
        applicationPresents_.store(0, std::memory_order_relaxed);
        applicationStopped_.store(true, std::memory_order_relaxed);
        if (out) {
            *out = ClassifyStoppedApplication(outputPresents, nowUs - windowStartUs);
        }
        return true;
    }

    // Returns true and fills `out` when the application present closed a measurement window.
    bool NoteApplicationPresent(int64_t nowUs, CadenceVerdict* out) {
        const bool firstApplicationFrame = !applicationWindowStarted_.exchange(true, std::memory_order_relaxed);
        const int applicationPresents = applicationPresents_.fetch_add(1, std::memory_order_relaxed) + 1;
        int64_t windowStartUs = windowStartUs_.load(std::memory_order_relaxed);
        if (firstApplicationFrame || windowStartUs < 0) {
            // Only what follows belongs to the window. Talos sends XeFG its first present ID when it enables
            // generation: 185 outputs from before that read as 4x (20261104_183540).
            outputPresents_.store(0, std::memory_order_relaxed);
            windowStartUs_.store(nowUs, std::memory_order_relaxed);
            const bool resumed = applicationStopped_.exchange(false, std::memory_order_relaxed);
            if (resumed && out) {
                *out = CadenceVerdict{};
            }
            return resumed;
        }
        // Back from a stop nothing is measured yet, and the undecided verdict says so now, not a window later.
        const bool resumed = applicationStopped_.load(std::memory_order_relaxed);
        const int64_t elapsedUs = nowUs - windowStartUs;
        if ((!resumed && elapsedUs < kWindowUs) ||
            !windowStartUs_.compare_exchange_strong(windowStartUs, nowUs, std::memory_order_relaxed)) {
            return false;
        }
        applicationStopped_.store(false, std::memory_order_relaxed);
        const int outputPresents = outputPresents_.exchange(0, std::memory_order_relaxed);
        applicationPresents_.store(0, std::memory_order_relaxed);
        const CadenceVerdict verdict =
            resumed ? CadenceVerdict{} : ClassifyInterposerCadence(applicationPresents, outputPresents, elapsedUs);
        if (out) {
            *out = verdict;
        }
        return true;
    }

    // A swapchain teardown or an alt-tab makes the partial window meaningless.
    void Reset() {
        applicationPresents_.store(0, std::memory_order_relaxed);
        outputPresents_.store(0, std::memory_order_relaxed);
        windowStartUs_.store(kNoWindow, std::memory_order_relaxed);
        applicationStopped_.store(false, std::memory_order_relaxed);
        applicationWindowStarted_.store(false, std::memory_order_relaxed);
    }

private:
    // A timestamp sentinel rather than 0: a QPC-derived microsecond stamp of 0 is legal.
    static constexpr int64_t kNoWindow = -1;

    std::atomic<int> applicationPresents_{0};
    std::atomic<int> outputPresents_{0};
    std::atomic<int64_t> windowStartUs_{kNoWindow};
    std::atomic<bool> applicationStopped_{false};
    std::atomic<bool> applicationWindowStarted_{false};
};

}  // namespace ce::present_interposer

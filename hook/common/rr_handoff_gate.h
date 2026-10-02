#pragma once

#include <atomic>
#include <cstdint>

// Whether DLSS Ray Reconstruction is actually rendering in this process, judged
// from evaluation evidence rather than from configuration.
//
// The UE5 RR preset turns off the engine's own Lumen/SSR reflection denoisers
// because RR replaces them. That is only right while RR runs: under TSR, under
// ordinary DLSS SR, or after an RR->SR fallback the same writes leave raw,
// undenoised reflections on screen. The UE5 override layer therefore hands
// those CVars to CE only while this gate says RR is rendering, and back to the
// game's own values otherwise.
namespace ce::rr_handoff {

// Bumped by every successful upscaler evaluation the NGX and Streamline hooks
// observe. Monotonic, so a reader judges a window by its delta and an
// evaluation can never be missed between two samples.
inline std::atomic<uint64_t> g_rayReconstructionEvaluations{0};
inline std::atomic<uint64_t> g_superResolutionEvaluations{0};

// Presented frames with no upscaler evaluation at all before RR is declared
// stopped. A frame count, not a time: the absence of evaluations only means
// something against frames the game demonstrably rendered (a paused or
// minimised game presents nothing and keeps its verdict). Large enough that a
// menu or loading screen that skips the scene for a moment does not flap the
// hand-off, small enough that a switch to TSR is honoured within seconds.
inline constexpr uint64_t kPresentsWithoutUpscalerBeforeOff = 120;

struct Sample {
    uint64_t rayReconstructionEvaluations = 0;
    uint64_t superResolutionEvaluations = 0;
    uint64_t presents = 0;
};

class Gate {
 public:
    // Feeds one observation window. Returns true when the verdict changed.
    //
    // - any RR evaluation in the window: rendering. A title that runs RR on one
    //   view and SR on another (or alternates per frame) keeps RR's verdict,
    //   so per-frame interleaving can never flap it.
    // - SR evaluations only: not rendering (an RR->SR switch or fallback).
    // - no upscaler evaluation: not rendering once enough presented frames
    //   prove the game kept rendering without DLSS (TSR, DLSS disabled).
    bool Observe(const Sample& sample) noexcept {
        const uint64_t rayReconstruction =
            sample.rayReconstructionEvaluations - last_.rayReconstructionEvaluations;
        const uint64_t superResolution = sample.superResolutionEvaluations - last_.superResolutionEvaluations;
        // A present counter that went backwards (never expected) proves nothing.
        const uint64_t presents = sample.presents >= last_.presents ? sample.presents - last_.presents : 0;
        last_ = sample;

        bool next = rendering_;
        if (rayReconstruction) {
            next = true;
            presentsWithoutUpscaler_ = 0;
        } else if (superResolution) {
            next = false;
            presentsWithoutUpscaler_ = 0;
        } else {
            presentsWithoutUpscaler_ += presents;
            if (presentsWithoutUpscaler_ >= kPresentsWithoutUpscalerBeforeOff)
                next = false;
        }
        const bool changed = next != rendering_;
        rendering_ = next;
        return changed;
    }

    bool Rendering() const noexcept { return rendering_; }
    uint64_t PresentsWithoutUpscaler() const noexcept { return presentsWithoutUpscaler_; }

 private:
    Sample last_{};
    bool rendering_ = false;
    uint64_t presentsWithoutUpscaler_ = 0;
};

}  // namespace ce::rr_handoff

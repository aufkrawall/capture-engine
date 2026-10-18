#pragma once

#include <cstdint>

#include "hook/pacing/fps_limiter_policy.h"

// Who paces while a frame-generation runtime presents from its own thread.
//
// FSR FG takes the game's Present on its proxy swapchain and later issues the
// real DXGI presents - one generated and one application output per rendered
// frame at 2x - from AMD's presenter thread, spaced by AMD's own busy-wait
// pacer. AMD asks for the rendered rate to be limited and the output spacing
// left to that pacer (FidelityFX frame-interpolation swapchain documentation,
// "Frame pacing and presentation"). Pacing the output stream instead puts a
// second pacer behind the first one on AMD's presenter thread, and the game is
// then only throttled by the runtime's queue filling up and blocking it inside
// its Present: two pacers in series plus a full queue's latency.
//
// So the game-thread proxy Present (PresentSite::kRuntimeInputPresent) owns the
// cadence whenever it is live, and every other site passes. Liveness is counted
// in presents, never measured on a clock: a runtime emits at most the maximum
// FG multiplier of outputs per application frame, so an output stream that
// carries several whole groups without a single proxy Present proves the proxy
// no longer carries the game's frames (its hook is gone or the game moved to
// another swapchain) and the output sites pace again.
namespace ce::runtime_input_site_policy {

// The largest frame-generation multiplier CE paces (FGCompatibility clamps to 4).
inline constexpr uint32_t kMaxFrameGenerationMultiplier = 4;
// Whole groups of slack between the proxy Present and the outputs it produces:
// the runtime presents a frame's outputs after the game already handed it the
// next one, and queues up to a couple of frames.
inline constexpr uint32_t kOutputGroupsBeforeOwnershipLapses = 3;
inline constexpr uint32_t kOutputsBeforeOwnershipLapses =
    kMaxFrameGenerationMultiplier * kOutputGroupsBeforeOwnershipLapses;

// No proxy Present observed since the limiter was (re)started.
inline constexpr uint32_t kNoRuntimeInputSeen = UINT32_MAX;

// Whether a non-input Apply() passes without pacing, given how many non-input
// Apply() calls preceded it since the last proxy Present.
inline bool RuntimeInputOwnsPacing(uint32_t outputsSinceRuntimeInput) {
    return outputsSinceRuntimeInput != kNoRuntimeInputSeen &&
           outputsSinceRuntimeInput < kOutputsBeforeOwnershipLapses;
}

// The capture source the proxy site compares capture sync against: what the
// output sites would have used. A callback-proven runtime output is what DX12
// inject capture records (final), as is a published final-output route.
inline bool RuntimeInputCaptureSourceIsFinalOutput(bool finalOutputRoutePublished, bool runtimeOutputStreamIsFinal) {
    return finalOutputRoutePublished || runtimeOutputStreamIsFinal;
}

// Generated outputs a frame-generation runtime presents for one application
// frame before that frame's own real output (AMD: generated, then real).
inline uint32_t GeneratedOutputsPerFrame(bool frameGenerationActive, int multiplier) {
    return frameGenerationActive && multiplier > 1 ? static_cast<uint32_t>(multiplier - 1) : 0;
}

// Whether the game thread's release after its proxy Present waits until the
// runtime presented the frame's generated outputs.
//
// The runtime's presents queue behind the game's GPU work. Released the moment
// the runtime took its frame, the game builds the next one while the runtime
// interpolates, composes and presents the generated output, and that Present
// waits on the GPU until the game's frame work drains. Talos, FSR FG 2x at a 120
// cap (session 20261018_200054, vsync forced or not): CE's callback commands for
// the generated output finished 0.48 ms after its callback, before AMD presented
// it, yet it reached the screen 1.86 ms after Present against 0.47 ms for real
// outputs; the next queued command on AMD's present queue started 1.4 ms late,
// and per frame that release tracked the moment the game finished its frame
// (correlation 0.87-0.89). Display intervals alternated 6.5/10.1 ms.
//
// Releasing the game once the generated outputs are presented puts its frame
// work in the gap before the real output instead, and ages the finished frame
// less. The release is bounded by the frame's measured work budget before its
// deadline, so a runtime that presents late or not at all can never make the
// frame late; that bound is the back edge whenever the work is not measurable.
inline bool ShouldGateReleaseOnGeneratedOutputs(ce::fps_limiter_policy::PresentSite site, bool frameGenerationActive,
                                                int multiplier, bool callSiteRunsPostPresentCadence,
                                                bool explicitPostPresentCadencePending) {
    return site == ce::fps_limiter_policy::PresentSite::kRuntimeInputPresent &&
           GeneratedOutputsPerFrame(frameGenerationActive, multiplier) > 0 && callSiteRunsPostPresentCadence &&
           !explicitPostPresentCadencePending;
}

}  // namespace ce::runtime_input_site_policy

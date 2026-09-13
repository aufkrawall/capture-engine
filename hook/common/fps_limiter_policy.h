#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>

#include "fg_runtime_state.h"

namespace ce::fps_limiter_policy {

struct ReflexPacingDecision {
    bool useGameSleepHandoff = false;
    bool useGameSleepWarmup = false;
    bool useExplicitLocalCadence = false;
};

inline uint32_t RebaseGameSleepBaselineForCounterEpoch(uint32_t baselineCount,
                                                       uint32_t currentSleepCount,
                                                       uint32_t previouslyEvaluatedSleepCount) {
    // Reflex activation epochs reset their observation counter. Preserve a
    // disruption baseline only within the same epoch; otherwise a high old
    // baseline can prevent a fresh 1/2/3-call streak from ever handing off.
    return currentSleepCount < previouslyEvaluatedSleepCount ? 0 : baselineCount;
}

inline ReflexPacingDecision ResolveReflexPacingDecision(bool explicitReflexMode, bool gameActivated,
                                                        bool gameSleepObserved, bool gameSleepRecent,
                                                        bool gameSleepAdvanced, uint32_t freshSleepCount,
                                                        bool recentPresentGap) {
    ReflexPacingDecision decision;
    (void)recentPresentGap;
    // The caller re-bases freshSleepCount at the edge of a Present gap. Once
    // three successful Sleep calls have occurred after that edge, extending
    // the local fallback for the whole gap-grace window only overlays a second
    // cadence on the newly healthy native one. That showed up as temporal
    // jitter around cutscene/FG transitions. Fresh post-gap Sleep evidence is
    // the recovery proof; a merely old observed Sleep is still insufficient.
    decision.useGameSleepHandoff =
        gameActivated && gameSleepObserved && gameSleepRecent && freshSleepCount >= 3;
    // Do not overlay CE's fallback cadence on the exact recovery frames where
    // a newly successful game Sleep already owns pacing. This is deliberately
    // progress-qualified rather than merely recency-qualified: if Sleep stops
    // again before the stable streak is complete, fallback resumes on the
    // very next evaluated frame instead of waiting out the recency grace.
    decision.useGameSleepWarmup = gameActivated && gameSleepObserved && gameSleepRecent &&
                                  gameSleepAdvanced && freshSleepCount < 3;
    decision.useExplicitLocalCadence =
        explicitReflexMode && !decision.useGameSleepHandoff && !decision.useGameSleepWarmup;
    return decision;
}

inline bool ShouldRunExplicitReflexCadencePostPresent(const ReflexPacingDecision& decision,
                                                      bool callSiteSupportsPostPresentCadence) {
    return decision.useExplicitLocalCadence && callSiteSupportsPostPresentCadence;
}

inline bool ShouldReturnNvApiReflexWrapper(bool manualReflexLimiterConfiguredOrActive, bool callerIsStreamlineRuntime,
                                           bool callerIsThirdPartyOverlay, bool callerIsSystemModule,
                                           bool callerIsCaptureHookModule) {
    return manualReflexLimiterConfiguredOrActive && !callerIsStreamlineRuntime && !callerIsThirdPartyOverlay &&
           !callerIsSystemModule && !callerIsCaptureHookModule;
}

inline bool IsManualReflexLimiterConfigured(bool generalEnabled, int generalFps, uint32_t generalMode,
                                            bool captureSyncEnabled, uint32_t captureSyncMode,
                                            uint32_t nativeModeValue) {
    return (generalEnabled && generalFps > 0 && generalMode == nativeModeValue) ||
           (captureSyncEnabled && captureSyncMode == nativeModeValue);
}

inline bool ShouldScaleTargetForFrameGeneration(bool usingCaptureSync, bool injectVideoCaptureRequested,
                                                bool injectFinalOutputAvailable) {
    // Ordinary inject capture publishes application-rendered frames, while
    // WGC/DXGI and the explicit DX12/Vulkan final-output routes observe the
    // presented stream including generated frames.
    return !usingCaptureSync || !injectVideoCaptureRequested || injectFinalOutputAvailable;
}

// Streamline's requested/available state can report DLSS-G ON before the game
// starts executing the frame-generation pipeline. Dividing the limiter target
// from that nominal state caps an ordinary one-Present render loop at
// target/multiplier (Gothic Remake sat at 60 fps under a 120 fps cap while its
// pre-menu DLSS-G state still produced no generated frames). A successful,
// recent game-owned low-latency sleep is a protocol-level production signal:
// DLSS-G requires Reflex, while Vulkan publishes the equivalent native pacing
// ownership through its backend. FSR FG and Smooth Motion runtime detection is
// already based on an active presentation path and does not need this extra
// Streamline qualification.
inline bool IsFrameGenerationProducingForPacing(ce::fg_runtime::RuntimeMode runtimeMode,
                                                bool runtimeFrameGenerationActive,
                                                bool recentActivatedD3DGameSleep,
                                                bool externalNativeGameActive) {
    if (!runtimeFrameGenerationActive) {
        return false;
    }
    if (runtimeMode != ce::fg_runtime::RuntimeMode::kDLSSFG) {
        return true;
    }
    return recentActivatedD3DGameSleep || externalNativeGameActive;
}

enum class LimiterConstraintSource : uint8_t {
    kNone,
    kCaptureSync,
    kGeneral,
};

struct LimiterTargetSelection {
    LimiterConstraintSource source = LimiterConstraintSource::kNone;
    int targetFps = 0;
    int captureTargetFps = 0;
    int captureOutputEquivalentFps = 0;
    int generalTargetFps = 0;
    bool captureSourceIsFinalOutput = false;

    bool IsActive() const {
        return source != LimiterConstraintSource::kNone;
    }

    bool UsesCaptureSync() const {
        return source == LimiterConstraintSource::kCaptureSync;
    }
};

inline int SaturatingPositiveProduct(int value, int multiplier) {
    if (value <= 0 || multiplier <= 0) {
        return 0;
    }
    if (value > INT_MAX / multiplier) {
        return INT_MAX;
    }
    return value * multiplier;
}

// Capture sync and the general limiter are simultaneous constraints, not a
// priority list. Compare them in the final-output domain so starting a base-
// frame inject capture cannot silently replace a stricter displayed-rate cap.
// Equal constraints prefer capture sync to retain its stable CFR grid phase.
inline LimiterTargetSelection ResolveLimiterTargetSelection(
    bool captureRequested, bool captureSyncEnabled, int captureFps, int captureSyncMultiplier,
    bool useVfr, bool generalEnabled, int generalFps, bool frameGenerationActive,
    int frameGenerationMultiplier, bool injectVideoCaptureRequested,
    bool injectFinalOutputAvailable) {
    LimiterTargetSelection selection;
    const bool captureAvailable = captureRequested && captureSyncEnabled && !useVfr &&
                                  captureFps > 0 && captureSyncMultiplier >= 1 &&
                                  captureSyncMultiplier <= 8;
    const bool generalAvailable = generalEnabled && generalFps > 0;

    selection.generalTargetFps = generalAvailable ? generalFps : 0;
    if (captureAvailable) {
        selection.captureTargetFps = SaturatingPositiveProduct(captureFps, captureSyncMultiplier);
        selection.captureSourceIsFinalOutput =
            !injectVideoCaptureRequested || injectFinalOutputAvailable;
        selection.captureOutputEquivalentFps = selection.captureTargetFps;
        if (frameGenerationActive && frameGenerationMultiplier >= 2 &&
            !selection.captureSourceIsFinalOutput) {
            selection.captureOutputEquivalentFps = SaturatingPositiveProduct(
                selection.captureTargetFps, std::min(frameGenerationMultiplier, 4));
        }
    }

    if (captureAvailable &&
        (!generalAvailable || selection.captureOutputEquivalentFps <= generalFps)) {
        selection.source = LimiterConstraintSource::kCaptureSync;
        selection.targetFps = selection.captureTargetFps;
    } else if (generalAvailable) {
        selection.source = LimiterConstraintSource::kGeneral;
        selection.targetFps = generalFps;
    }
    return selection;
}

inline int ResolveFrameGenerationBaseTarget(int outputTargetFps, bool frameGenerationActive, int multiplier,
                                            bool scaleForFrameGeneration) {
    if (outputTargetFps <= 0)
        return outputTargetFps;
    if (!frameGenerationActive || multiplier < 2 || !scaleForFrameGeneration)
        return outputTargetFps;
    return std::max(1, outputTargetFps / std::min(multiplier, 4));
}

// Size of one output group at a real final presentation/acquire boundary: the
// FG multiplier while frame generation is active, otherwise every callback is
// its own group owner. Deliberately ordinal-based, never time-based.
inline int ResolveOutputGroupAdmissionMultiplier(bool frameGenerationActive, int frameGenerationMultiplier) {
    if (!frameGenerationActive || frameGenerationMultiplier < 2) {
        return 1;
    }
    return std::min(frameGenerationMultiplier, 4);
}

// Scale of the local rational cadence interval: the group period is
// frequency * cadenceScale / configuredTarget. Final-output observers scale by
// the FG multiplier so target/multiplier groups per second yield exactly the
// configured output rate (130/3 = 43.333... groups/s under a 130 cap), while
// ordinary/base inject capture-sync keeps scale 1 because that source contains
// only application-rendered frames and its target already IS the base rate.
inline int ResolveCadenceScaleMultiplier(bool frameGenerationActive, int frameGenerationMultiplier,
                                         bool scaleForFrameGeneration) {
    if (!frameGenerationActive || !scaleForFrameGeneration || frameGenerationMultiplier < 2) {
        return 1;
    }
    return std::min(frameGenerationMultiplier, 4);
}

// NVIDIA's driver-owned low-latency interval - `minimumIntervalUs` in
// NvAPI_D3D_SetSleepMode / NvAPI_Vulkan_SetSleepMode / vkSetLatencySleepModeNV -
// is frame-generation aware for NVIDIA's OWN generated frames: the driver
// stretches the application's render loop by the active DLSS-G/MFG factor so
// the interval constrains the FINAL presented rate. Third-party generated
// frames (FSR FG) are invisible to it; there the interval throttles the game's
// Reflex sleep, i.e. the render loop itself, and the base target is correct.
inline bool DriverLowLatencyIntervalCoversGeneratedFrames(ce::fg_runtime::RuntimeMode runtimeMode) {
    return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG ||
           runtimeMode == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}

// Target handed to a driver-owned low-latency interval. CE's own cadence paces
// base frames, but a frame-generation-aware driver interval must receive the
// OUTPUT rate: giving it the FG-divided base target applies the divisor twice.
// Portal RTX with 3x MFG under a 130 cap received 43, the driver then paced the
// render loop at 43/3 = 14.3 fps, and the game displayed 43 fps instead of 130.
inline int ResolveNativeDriverPacingTargetFps(int configuredTargetFps, int baseTargetFps,
                                              bool frameGenerationActive, int multiplier,
                                              bool scaleForFrameGeneration,
                                              bool driverIntervalCoversGeneratedFrames) {
    if (configuredTargetFps <= 0 || baseTargetFps <= 0) {
        return baseTargetFps;
    }
    if (!frameGenerationActive || multiplier < 2 || !driverIntervalCoversGeneratedFrames) {
        return baseTargetFps;
    }
    if (scaleForFrameGeneration) {
        // The configured cap already denotes the final output rate.
        return configuredTargetFps;
    }
    // Ordinary/base inject capture sync configures the application-rendered
    // rate; the equivalent output rate that the driver interval expects is
    // base * multiplier. Explicit final-output inject routes take the
    // scaleForFrameGeneration branch above instead.
    const int clampedMultiplier = std::min(multiplier, 4);
    if (configuredTargetFps > INT_MAX / clampedMultiplier) {
        return configuredTargetFps;
    }
    return configuredTargetFps * clampedMultiplier;
}

// What a call site can promise about its own Apply() entries. This is a
// structural contract owned by the hook that calls Apply(), never a property
// measured from a clock.
//
// kDuplicateProne is the legacy default: the site can fire more than once for
// a single logical frame (DXVK Present+PresentEx, the D3D9/D3D8/DDraw/OpenGL
// wrappers), so the extra call really is the same frame and the duplicate-
// present time window still owns the classification.
//
// kUniqueApplicationPresent is a site that structurally cannot deliver a
// second entry for one application present: DXGI's Present/Present1 detours
// and the swapchain wrapper are mutually exclusive (IsInWrapperPresent) and
// guarded by IsRecursivePresent(), so nested and cross-thread re-entries
// return before Apply() is ever reached. Those sites must be gated by the
// cadence grid, because a duplicate window there can only misfire: Strange
// Brigade DX12 renders a new frame in 1-2ms, so ~46 genuine presents per
// second landed inside the 2ms window and reached the swapchain completely
// unpaced (~130 fps against a 90 fps cap, alternating short/long frame times).
//
// kFinalOutputBoundary is a site that observes EVERY final presented output,
// generated frames included (native-Vulkan vkQueuePresentKHR /
// vkAcquireNextImageKHR). Only those sites may own multiplier-sized output-
// group admission.
enum class PresentSite : uint8_t {
    kDuplicateProne = 0,
    kUniqueApplicationPresent = 1,
    kFinalOutputBoundary = 2,
};

// Whether every Apply() entry from this site must take a cadence-grid slot
// instead of the legacy duplicate-present time window.
//
// A kUniqueApplicationPresent site only owns the application's own present
// stream. While a frame-generation runtime is producing, that same DXGI stream
// also carries runtime-owned generated presents (FFX presents an interpolated
// frame from its own proxy swapchain), and the site cannot tell them apart
// from the application's frames. CE's cadence paces base frames there, so
// gating every entry would both spend a base-rate grid slot on a generated
// present - halving the output - and block the runtime's own presenter thread
// inside CE's cadence lock, which is the FFX freeze class CE must never enter.
// Until those sites can classify a generated present structurally the way a
// final-output boundary does, frame generation keeps the established
// behaviour. This is NOT the rejected `strictGrid = boundary && !FGActive`
// escape from Portal RTX: a real final-output boundary stays strict and is
// owned by OutputGroupAdmission below.
inline bool ShouldGateEveryApplyOnCadenceGrid(PresentSite site, bool frameGenerationActive) {
    switch (site) {
        case PresentSite::kFinalOutputBoundary:
            return true;
        case PresentSite::kUniqueApplicationPresent:
            return !frameGenerationActive;
        case PresentSite::kDuplicateProne:
            break;
    }
    return false;
}

// Where a fully-owned cadence period spends its idle time.
//
// The deadline decides when a frame is PRESENTED. It does not have to decide
// when the game is allowed to BUILD that frame, and spending the whole wait
// after the game already finished rendering is what ages the frame: Strange
// Brigade DX12 under a 90 fps cap rendered a frame in a median 1.8ms (stddev
// 0.15ms) and then sat in CE's present hook for a median 9.3ms of every 11.1ms
// period. The overlay's PC-latency chain measured exactly that shape -
// anchorToPresent 20.5ms against a 0.4ms present-to-display, where a
// front-edge limiter in the same scene read 11.2ms/7.5ms.
//
// Releasing the game `budget` before the deadline moves that idle to the FRONT
// of the period: the frame is built last and presented immediately. The
// present still lands on the same absolute grid slot, because whatever the
// budget over-reserved is simply waited out before Present as before. The
// budget is therefore a latency control only, never a rate or correctness one:
// a budget of a whole interval, a skipped post-present release, or an
// unmeasurable work time all degrade to the original back-edge behaviour with
// the cap and the grid phase intact.
//
// The ceiling is the observed high-water of recent frames rather than a
// percentile: overrunning the budget makes the present late, and a late
// present re-phases the general cadence. One hitch saturates the ceiling and
// parks the placement back at the back edge until it ages out, which is the
// safe direction.
inline int64_t ResolveFrameWorkBudgetUs(int64_t observedWorkCeilingUs, int64_t fineMarginUs, int64_t intervalUs,
                                        size_t sampleCount, size_t minimumSamples) {
    if (intervalUs <= 0) {
        return 0;
    }
    if (sampleCount < minimumSamples || observedWorkCeilingUs < 0) {
        // Not measurable yet: reserve the whole period, which is the original
        // back-edge placement expressed as a budget. Sample count is the only
        // measurability signal - a ceiling that rounds to zero is a real
        // measurement of a frame that costs less than the timer margin, and the
        // margin alone is the right reservation for it.
        return intervalUs;
    }
    const int64_t margin = fineMarginUs > 0 ? fineMarginUs : 0;
    if (observedWorkCeilingUs > intervalUs - margin) {
        return intervalUs;
    }
    return observedWorkCeilingUs + margin;
}

// A cadence wait may only be front-loaded where CE owns the whole period.
//
// - The call site must gate every entry on the grid (PresentSite contract), so
//   exactly one release belongs to one present.
// - The call site must run the post-present half; the flag that promises an
//   explicit post-present cadence is that same promise.
// - Frame generation disqualifies it for the same reason it disqualifies the
//   strict grid on a kUniqueApplicationPresent site: the present stream is not
//   CE's to re-phase, and blocking after a runtime-owned present is the FFX
//   freeze class.
// - An explicit Reflex/native post-present cadence already owns the slot.
// - Capture sync is excluded. Removing the game's slack before the deadline
//   raises the rate of presents that miss it (Strange Brigade DX12 went from
//   0.24% to 0.63% late frames), and under capture sync a missed deadline
//   skips whole CFR grid slots rather than costing a fraction of a millisecond
//   of frame time. While a recording is the product, the capture grid outranks
//   input latency.
inline bool ShouldFrontLoadCadenceWait(bool gatedOnCadenceGrid, bool callSiteRunsPostPresentCadence,
                                       bool frameGenerationActive, bool explicitPostPresentCadencePending,
                                       bool usingCaptureSync) {
    return gatedOnCadenceGrid && callSiteRunsPostPresentCadence && !frameGenerationActive &&
           !explicitPostPresentCadencePending && !usingCaptureSync;
}

// Extra reservation above the observed work ceiling, learned from the presents
// that actually missed their deadline.
//
// A ceiling taken over a sliding window of N samples is, by construction,
// exceeded by roughly one in N+1 later frames: at 90 fps with a 64-sample ring
// that is over one missed deadline per second, which is exactly where a 1% low
// is measured. Strange Brigade DX12 showed it - front-loading moved the
// published 1% low from 86.9 to 84.9 fps, the 0.1% low from 86.5 to 83.2, and
// the overlay's frame-time stddev from 148us to 225us, with the limiter
// reporting late frames of 24-455us where the back edge reported none.
//
// Widening the reservation for everyone would pay latency the game never
// needed. Growing it by what a real overrun actually cost, and decaying it
// while none occur, pays only what this game demonstrates it needs - and while
// the presentation queue still holds the frame (6.8ms on that session) those
// microseconds do not reach the screen at all.
inline int64_t GrowFrontLoadHeadroomUs(int64_t headroomUs, int64_t lateUs, int64_t intervalUs) {
    // Lateness of a whole interval or more is a hitch, not a budget overrun:
    // the frame could not have been started early enough to make that deadline
    // and reserving for it would park the placement at the back edge forever.
    if (lateUs <= 0 || intervalUs <= 0 || lateUs >= intervalUs) {
        return headroomUs;
    }
    return lateUs > headroomUs ? lateUs : headroomUs;
}

// Decay applied once per clean observation window so a one-off overrun cannot
// hold the reservation for the rest of the session. Integer division reaches
// zero on its own, so the placement fully recovers rather than asymptoting.
inline int64_t DecayFrontLoadHeadroomUs(int64_t headroomUs) {
    if (headroomUs <= 0) {
        return 0;
    }
    const int64_t step = headroomUs / 8;
    return headroomUs - (step > 0 ? step : 1);
}

// Whether the frame's own GPU work still has to finish after the deadline, and
// by how much.
//
// Front-loading budgets the CPU half of a frame, but the flip cannot happen
// until the GPU half finishes too. Strange Brigade DX12 is GPU-bound - a
// 1.8ms CPU frame in front of roughly 8.5ms of GPU work - so a CPU-sized
// budget released the game far too late and the GPU ran past the deadline.
// Session `20260913_132320` measured the consequence exactly: present-to-
// display rose 0.4 -> 6.8ms, and because the screen time was then set by GPU
// completion instead of by CE's grid, the game's own frame-to-frame variance
// reached the display timeline. The overlay publishes its percentiles from
// screen times, which is why its 1% low fell 86.9 -> 85.0 fps and its
// frame-time stddev rose 148 -> 202us while the PRESENT timeline actually
// improved (stddev 194 -> 169us).
//
// The placement is worth nothing below that threshold. Writing L for
// input-to-photon, B for the budget, W for the frame's whole CPU+GPU work and
// F for the irreducible flip latency:
//
//   B >= W  ->  L = B + F        and every screen time is pinned to the grid
//   B <  W  ->  L = W + F        and screen times follow GPU completion
//
// so shrinking the budget below W buys exactly zero latency and pays for it in
// jitter. The optimum is B = W: the lowest latency the frame can have, with the
// grid still deciding when it is shown. That is what this looks for, and the
// excess of present-to-display over its own floor is how far away it is.
inline int64_t ResolveFrontLoadGpuExcessUs(int64_t recentPresentToDisplayUs, int64_t floorPresentToDisplayUs,
                                           int64_t marginUs) {
    if (recentPresentToDisplayUs <= 0 || floorPresentToDisplayUs < 0) {
        return 0;
    }
    const int64_t excess = recentPresentToDisplayUs - floorPresentToDisplayUs;
    // Below the timer margin the frame is finishing early enough that the grid,
    // not the GPU, is still deciding the screen time.
    return excess > marginUs ? excess : 0;
}

// The reservation walks back in by one timer margin per clean window rather
// than by a fraction of itself. A proportional decay would periodically put the
// GPU a large step past the deadline just to discover it no longer fits there;
// a bounded probe costs at most one margin of straddle for one window, and
// still finds a lighter scene's lower optimum over time.
inline int64_t DecayFrontLoadGpuHeadroomUs(int64_t headroomUs, int64_t marginUs) {
    if (headroomUs <= 0) {
        return 0;
    }
    const int64_t step = marginUs > 0 ? marginUs : 1;
    return headroomUs > step ? headroomUs - step : 0;
}

// Front-loading is only allowed where the GPU half can be observed. Without
// present-to-display evidence there is no way to tell whether releasing the
// game later pushes its GPU work past the deadline, and the relation above
// says a budget that lands below W buys no latency at all - so guessing can
// only lose. The back edge stays the proven default.
inline bool HasUsableGpuCompletionEvidence(size_t presentToDisplaySamples, size_t minimumSamples,
                                           bool floorSeeded) {
    return floorSeeded && presentToDisplaySamples >= minimumSamples;
}

// Deterministic multiplier-sized output-group admission for real final
// presentation boundaries (native-Vulkan vkQueuePresentKHR /
// vkAcquireNextImageKHR). Exactly one callback per group of `multiplier`
// consecutive real-boundary callbacks owns a cadence slot; the remaining
// multiplier-1 callbacks are the generated outputs of that already admitted
// group. Classification is a pure ordinal and never reads a clock, so a next
// real group arriving inside a time window cannot be confused with generated
// spillover - the escape that let Portal RTX run ~146 fps against a 130 cap.
class OutputGroupAdmission {
public:
    enum class Decision {
        kPaceGroup,          // This callback owns the next base-group cadence slot.
        kPassGeneratedSlot,  // One of the remaining multiplier-1 generated output slots.
    };

    Decision Classify(int multiplier) {
        if (multiplier < 2) {
            // Every real-boundary callback is its own paced group owner.
            ordinal_ = 0;
            return Decision::kPaceGroup;
        }
        const Decision decision = (ordinal_ == 0) ? Decision::kPaceGroup : Decision::kPassGeneratedSlot;
        if (++ordinal_ >= static_cast<uint32_t>(multiplier)) {
            ordinal_ = 0;
        }
        return decision;
    }

    // Discards any partial group. Returns true when generated slots had
    // already been handed out, so the caller can surface a group reset in the
    // limiter diagnostics instead of silently re-basing the admission.
    bool Reset() {
        const bool hadPartialGroup = ordinal_ != 0;
        ordinal_ = 0;
        return hadPartialGroup;
    }

    uint32_t PendingGeneratedSlots() const {
        return ordinal_;
    }

private:
    uint32_t ordinal_ = 0;
};

inline int64_t NextRationalIntervalTicks(int64_t frequency, int fps, int64_t& remainder) {
    if (frequency <= 0 || fps <= 0) {
        remainder = 0;
        return 1;
    }
    int64_t ticks = frequency / fps;
    remainder += frequency % fps;
    if (remainder >= fps) {
        remainder -= fps;
        ++ticks;
    }
    return ticks > 0 ? ticks : 1;
}

// Exact rational group-cadence variant of NextRationalIntervalTicks: the
// interval is frequency * cadenceScale / fps, so e.g. a 130 fps output cap
// with a 3x multiplier paces 130/3 = 43.333... groups/s with zero long-term
// drift instead of flooring to 43 groups/s (129 output fps). The Bresenham
// remainder distributes the sub-tick fraction across intervals.
inline int64_t NextRationalGroupIntervalTicks(int64_t frequency, int fps, int cadenceScale, int64_t& remainder) {
    if (frequency <= 0 || fps <= 0 || cadenceScale <= 0) {
        remainder = 0;
        return 1;
    }
    const int64_t scale = static_cast<int64_t>(cadenceScale);
    if (frequency > INT64_MAX / scale) {
        // Guard the multiplication even though real QPC frequencies are small;
        // degrade to the unscaled interval rather than overflow.
        remainder = 0;
        return std::max<int64_t>(1, frequency / fps);
    }
    const int64_t scaledFrequency = frequency * scale;
    int64_t ticks = scaledFrequency / fps;
    remainder += scaledFrequency % fps;
    if (remainder >= fps) {
        remainder -= fps;
        ++ticks;
    }
    return ticks > 0 ? ticks : 1;
}

struct PhasePreservingLateAdvance {
    int64_t nextTargetQpc = 0;
    uint32_t skippedGridSlots = 0;
};

inline PhasePreservingLateAdvance AdvanceCaptureSyncDeadlineAfterLateFrame(int64_t currentTargetQpc,
                                                                           int64_t nowQpc, int64_t frequency,
                                                                           int fps, int cadenceScale,
                                                                           int64_t& remainder) {
    PhasePreservingLateAdvance result{currentTargetQpc, 0};
    if (currentTargetQpc <= 0 || nowQpc <= 0 || frequency <= 0 || fps <= 0) {
        result.nextTargetQpc = nowQpc + NextRationalGroupIntervalTicks(frequency, fps, cadenceScale, remainder);
        result.skippedGridSlots = 1;
        return result;
    }

    // Keep the original rational-grid phase after a hitch. A half-interval guard prevents an
    // immediate short catch-up Present, while advancing by whole grid slots avoids the permanent
    // phase rebase that otherwise makes a matched capture/output cadence straddle the CFR
    // selector's half-frame boundary for the rest of the recording. With frame generation the
    // grid is the scaled output-group grid, so whole GROUP slots are skipped.
    const int64_t halfInterval = std::max<int64_t>(1, (frequency / fps) * std::max<int64_t>(1, cadenceScale) / 2);
    const int64_t minimumNextQpc = nowQpc <= INT64_MAX - halfInterval ? nowQpc + halfInterval : INT64_MAX;
    do {
        const int64_t step = NextRationalGroupIntervalTicks(frequency, fps, cadenceScale, remainder);
        if (result.nextTargetQpc > INT64_MAX - step) {
            result.nextTargetQpc = minimumNextQpc;
            ++result.skippedGridSlots;
            break;
        }
        result.nextTargetQpc += step;
        ++result.skippedGridSlots;
    } while (result.nextTargetQpc < minimumNextQpc);
    return result;
}

}  // namespace ce::fps_limiter_policy

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include "hook/ngx/rr_handoff_gate.h"
#include "hook/overrides/ue5_cvar_override_policy.h"

namespace {

const ce::ue5_cvar::Spec* FindSpec(std::string_view name) {
    for (const auto& spec : ce::ue5_cvar::kSpecs) {
        if (name == spec.name)
            return &spec;
    }
    return nullptr;
}

ce::ue5_cvar::Settings PresetSettings(uint8_t preset) {
    ce::ue5_cvar::Settings settings;
    settings.rayReconstructionOptimalSettings = preset;
    return settings;
}

}  // namespace

// Before any evaluation evidence the engine's own reflection denoisers stay in
// charge: CE must never assume RR is rendering just because the preset is on.
TEST(UE5RayReconstructionHandoffGateTest, StartsNotRenderingAndFollowsEvaluations) {
    ce::rr_handoff::Gate gate;
    EXPECT_FALSE(gate.Rendering());
    EXPECT_FALSE(gate.Observe({0, 0, 50}));
    EXPECT_FALSE(gate.Rendering());

    EXPECT_TRUE(gate.Observe({3, 0, 60}));
    EXPECT_TRUE(gate.Rendering());
    EXPECT_FALSE(gate.Observe({9, 0, 70})) << "a steady state is not a transition";

    // An RR->SR switch or fallback hands the reflections back at once.
    EXPECT_TRUE(gate.Observe({9, 4, 80}));
    EXPECT_FALSE(gate.Rendering());
    EXPECT_TRUE(gate.Observe({10, 4, 90}));
    EXPECT_TRUE(gate.Rendering());
}

// A title that runs RR on one view and SR on another evaluates both in every
// window; the verdict must stay on RR rather than flapping between them.
TEST(UE5RayReconstructionHandoffGateTest, InterleavedRrAndSrEvaluationsNeverFlap) {
    ce::rr_handoff::Gate gate;
    ASSERT_TRUE(gate.Observe({1, 1, 10}));
    for (uint64_t window = 2; window < 50; ++window) {
        EXPECT_FALSE(gate.Observe({window, window, window * 10})) << window;
        EXPECT_TRUE(gate.Rendering()) << window;
    }
}

// No upscaler evaluation at all only means "RR stopped" against frames the game
// demonstrably presented (TSR, DLSS switched off). A loading screen shorter than
// the threshold, or a paused game that presents nothing, keeps the verdict.
TEST(UE5RayReconstructionHandoffGateTest, AbsenceNeedsPresentedFramesToTurnOff) {
    ce::rr_handoff::Gate gate;
    ASSERT_TRUE(gate.Observe({5, 0, 1000}));

    const uint64_t threshold = ce::rr_handoff::kPresentsWithoutUpscalerBeforeOff;
    EXPECT_FALSE(gate.Observe({5, 0, 1000 + threshold - 1}));
    EXPECT_TRUE(gate.Rendering());
    for (int idle = 0; idle < 100; ++idle)
        EXPECT_FALSE(gate.Observe({5, 0, 1000 + threshold - 1})) << "a game presenting nothing proves nothing";
    EXPECT_TRUE(gate.Rendering());

    EXPECT_TRUE(gate.Observe({5, 0, 1000 + threshold}));
    EXPECT_FALSE(gate.Rendering());
    EXPECT_EQ(gate.PresentsWithoutUpscaler(), threshold);

    // An RR evaluation resets the absence count, so a short scene-less phase
    // after RR resumed starts counting from zero again.
    ASSERT_TRUE(gate.Observe({6, 0, 1000 + threshold + 1}));
    EXPECT_EQ(gate.PresentsWithoutUpscaler(), 0u);
    EXPECT_FALSE(gate.Observe({6, 0, 1000 + threshold + 1 + threshold / 2}));
    EXPECT_TRUE(gate.Rendering());
}

TEST(UE5RayReconstructionHandoffGateTest, BackwardsPresentCounterIsNotAbsenceEvidence) {
    ce::rr_handoff::Gate gate;
    ASSERT_TRUE(gate.Observe({1, 0, 5000}));
    EXPECT_FALSE(gate.Observe({1, 0, 10}));
    EXPECT_TRUE(gate.Rendering());
    EXPECT_EQ(gate.PresentsWithoutUpscaler(), 0u);
}

// Exactly the temporal and screen-space reflection stages are handed off. The
// bilateral filter stays a plain preset write: NVIDIA documents that UE 5.2/5.3
// can assert on a later resolution change after it was changed at runtime, and a
// hand-off necessarily changes it while rendering.
TEST(UE5RayReconstructionHandoffPolicyTest, HandsOffOnlyTheRuntimeSafeReflectionDenoisers) {
    const auto settings = PresetSettings(ce::ue5_cvar::kRayReconstructionPresetFull);
    std::size_t handoffs = 0;
    for (const auto& spec : ce::ue5_cvar::kSpecs) {
        const auto resolved = ce::ue5_cvar::Resolve(spec, settings);
        if (!resolved.handoff)
            continue;
        ++handoffs;
        EXPECT_TRUE(resolved.enabled) << spec.name;
        EXPECT_FALSE(resolved.floor) << spec.name;
        EXPECT_EQ(static_cast<int32_t>(resolved.bits), 0) << spec.name;
    }
    EXPECT_EQ(handoffs, 3u);
    for (std::string_view name : {"r.Lumen.Reflections.ScreenSpaceReconstruction", "r.Lumen.Reflections.Temporal",
                                  "r.SSR.Temporal"}) {
        const auto* spec = FindSpec(name);
        ASSERT_NE(spec, nullptr) << name;
        EXPECT_TRUE(ce::ue5_cvar::Resolve(*spec, settings).handoff) << name;
        EXPECT_TRUE(ce::ue5_cvar::Resolve(*spec, PresetSettings(ce::ue5_cvar::kRayReconstructionPresetLight)).enabled)
            << name << " must be installed from light upward so a hand-off never needs a rescan";
        EXPECT_FALSE(ce::ue5_cvar::Resolve(*spec, PresetSettings(ce::ue5_cvar::kRayReconstructionPresetOff)).enabled)
            << name;
    }
    const auto* bilateral = FindSpec("r.Lumen.Reflections.BilateralFilter");
    ASSERT_NE(bilateral, nullptr);
    EXPECT_TRUE(ce::ue5_cvar::Resolve(*bilateral, settings).enabled);
    EXPECT_FALSE(ce::ue5_cvar::Resolve(*bilateral, settings).handoff);
}

// An explicit custom entry is the user's exact value and is never handed back.
TEST(UE5RayReconstructionHandoffPolicyTest, CustomValueIsNeverHandedOff) {
    auto settings = PresetSettings(ce::ue5_cvar::kRayReconstructionPresetLight);
    const std::size_t index = ce::ue5_cvar::FindSpecIndex("r.Lumen.Reflections.Temporal");
    ASSERT_LT(index, ce::ue5_cvar::kSpecs.size());
    settings.customCVarOverrideMask = uint64_t{1} << index;
    settings.customCVarOverrideValues[index] = 0;
    const auto resolved = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[index], settings);
    ASSERT_TRUE(resolved.enabled);
    EXPECT_FALSE(resolved.handoff);
}

TEST(UE5RayReconstructionHandoffPolicyTest, HandoffBitsPassTheGameValueThroughUntilRrRenders) {
    EXPECT_EQ(ce::ue5_cvar::HandoffBits(0, true, true, 1), 0u);
    EXPECT_EQ(ce::ue5_cvar::HandoffBits(0, false, true, 1), 1u);
    EXPECT_EQ(ce::ue5_cvar::HandoffBits(0, false, false, 1), 0u)
        << "with nothing observed yet the configured value is all there is";
    EXPECT_EQ(ce::ue5_cvar::HandoffBits(0, true, false, 1), 0u);
}

// Both register as 0, so as preset entries they were no-ops on a stock engine
// and only ever overrode a title that enabled them on purpose. They remain
// reachable as explicit custom entries.
TEST(UE5RayReconstructionHandoffPolicyTest, ZeroDefaultTemporalSwitchesAreCustomOnly) {
    for (std::string_view name : {"r.Lumen.ScreenProbeGather.Temporal.RejectBasedOnNormal",
                                  "r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp"}) {
        const std::size_t index = ce::ue5_cvar::FindSpecIndex(name);
        ASSERT_LT(index, ce::ue5_cvar::kSpecs.size()) << name;
        auto settings = PresetSettings(ce::ue5_cvar::kRayReconstructionPresetFull);
        settings.forceRayReconstruction = true;
        EXPECT_FALSE(ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[index], settings).enabled) << name;
        settings.customCVarOverrideMask = uint64_t{1} << index;
        settings.customCVarOverrideValues[index] = 1;
        const auto custom = ce::ue5_cvar::Resolve(ce::ue5_cvar::kSpecs[index], settings);
        ASSERT_TRUE(custom.enabled) << name;
        EXPECT_EQ(static_cast<int32_t>(custom.bits), 1) << name;
    }
}

// Pins the measured cost classification so a later edit cannot move a paid
// entry back into a level documented as free.
TEST(UE5RayReconstructionHandoffPolicyTest, PaidEntriesSitInPaidTiers) {
    struct Expected {
        std::string_view name;
        uint8_t lowestPreset;
        double value;
    };
    constexpr Expected expected[] = {
        // Restores the 5.6 default that 5.7 changed; free relative to the engine.
        {"r.Lumen.ScreenProbeGather.IntegrateDownsampleFactor", ce::ue5_cvar::kRayReconstructionPresetMedium, 1.0},
        // FrameIndex % N: a jitter cycle, not a ray count.
        {"r.Lumen.ScreenProbeGather.Temporal.MaxRayDirections", ce::ue5_cvar::kRayReconstructionPresetMedium, 16.0},
        // SurfaceCacheTexels / Factor per frame against defaults 64 and 32.
        {"r.LumenScene.Radiosity.UpdateFactor", ce::ue5_cvar::kRayReconstructionPresetHigh, 16.0},
        {"r.LumenScene.DirectLighting.UpdateFactor", ce::ue5_cvar::kRayReconstructionPresetHigh, 16.0},
        {"r.Lumen.ScreenProbeGather.StochasticInterpolation", ce::ue5_cvar::kRayReconstructionPresetHigh, 0.0},
        // Resolution squared traces per probe.
        {"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", ce::ue5_cvar::kRayReconstructionPresetFull, 16.0},
        // Full-resolution MegaLights, in both engine spellings.
        {"r.MegaLights.DownsampleMode", ce::ue5_cvar::kRayReconstructionPresetFull, 0.0},
        {"r.MegaLights.DownsampleFactor", ce::ue5_cvar::kRayReconstructionPresetFull, 1.0},
    };
    for (const auto& item : expected) {
        const auto* spec = FindSpec(item.name);
        ASSERT_NE(spec, nullptr) << item.name;
        EXPECT_EQ(spec->type, ce::ue5_cvar::ValueType::Int32) << item.name;
        for (uint8_t preset = ce::ue5_cvar::kRayReconstructionPresetOff;
             preset <= ce::ue5_cvar::kRayReconstructionPresetFull; ++preset) {
            const auto resolved = ce::ue5_cvar::Resolve(*spec, PresetSettings(preset));
            EXPECT_EQ(resolved.enabled, preset >= item.lowestPreset) << item.name << " preset=" << int(preset);
            if (resolved.enabled)
                EXPECT_EQ(static_cast<int32_t>(resolved.bits), static_cast<int32_t>(item.value)) << item.name;
        }
    }
}

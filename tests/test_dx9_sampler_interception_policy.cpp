#include <gtest/gtest.h>

#include "hook/d3d9/dx9_sampler_interception_policy.h"
#include "hook/d3d9/dx9_state_block_sampler_policy.h"

namespace arm = ce::dx9_sampler_interception;
namespace sb = ce::dx9_sampler_state;

namespace {

constexpr size_t kMinFilter = 4;
constexpr size_t kMaxAnisotropy = 8;

// Sampler 0 as CE keeps it while forcing 16x AF on a LINEAR material.
sb::SamplerState ForcedSampler() {
    sb::SamplerState sampler;
    sampler.initialized = true;
    sampler.bootstrapAttempted = true;
    sampler.reconciled = true;
    sampler.logical[kMinFilter] = D3DTEXF_LINEAR;
    sampler.physical[kMinFilter] = D3DTEXF_ANISOTROPIC;
    sampler.physical[kMaxAnisotropy] = 16;
    sampler.textureBound = true;
    sampler.textureMipLevels = 10;
    return sampler;
}

}  // namespace

// The per-draw calls are what made a CPU-bound DX9 game (Borderlands 2) ~15 %
// slower with nothing forced: without an override only the rare state-block
// lifecycle calls may be hooked.
TEST(Dx9SamplerInterceptionPolicyTest, NoOverrideHooksOnlyTheStateBlockLifecycle) {
    arm::VTableArming arming;
    EXPECT_EQ(arming.OnDeviceHooks(false), arm::kLifecycleSlots);
    EXPECT_FALSE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnPresent(false), 0u);
    EXPECT_EQ(arming.OnDeviceHooks(false), 0u) << "a second device on the vtable adds nothing";
    EXPECT_FALSE(arming.PerDrawArmed());

    const uint32_t perDraw = arm::kSlotSetTexture | arm::kSlotSetSamplerState | arm::kSlotGetSamplerState |
                             arm::kSlotStateBlockApply;
    EXPECT_EQ(arm::kPerDrawSlots & perDraw, perDraw);
    EXPECT_EQ(arm::kPerDrawSlots & arm::kLifecycleSlots, 0u);
}

TEST(Dx9SamplerInterceptionPolicyTest, OverrideConfiguredLaterArmsThePerDrawSlotsOnceAtPresent) {
    arm::VTableArming arming;
    ASSERT_EQ(arming.OnDeviceHooks(false), arm::kLifecycleSlots);
    EXPECT_EQ(arming.OnPresent(true), arm::kPerDrawSlots);
    EXPECT_TRUE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnPresent(true), 0u) << "handed out once: a failed install is not retried per Present";
    // Switching the override off restores the application's values through the
    // armed detours, so the vtable stays armed.
    EXPECT_EQ(arming.OnPresent(false), 0u);
    EXPECT_TRUE(arming.PerDrawArmed());
}

TEST(Dx9SamplerInterceptionPolicyTest, OverrideAtDeviceCreationArmsEverythingAtOnce) {
    arm::VTableArming arming;
    EXPECT_EQ(arming.OnDeviceHooks(true), arm::kLifecycleSlots | arm::kPerDrawSlots);
    EXPECT_TRUE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnDeviceHooks(true), 0u);
    EXPECT_EQ(arming.OnPresent(true), 0u);
}

// The shadow may start from the D3D9 defaults only while CE sees every Set*:
// a Reset between a config change and the arming Present must not seed it.
TEST(Dx9SamplerInterceptionPolicyTest, ShadowAndRecordingsAreTrustedOnlyWhileIntercepted) {
    EXPECT_TRUE(arm::ShadowTracksFromDefaults(true, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(false, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(true, false));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(false, false));

    EXPECT_TRUE(arm::RecordingSnapshotTrusted(true));
    EXPECT_FALSE(arm::RecordingSnapshotTrusted(false)) << "a recording begun before arming missed Set* calls";
}

// With AF forced, engines re-set every sampler state per draw. Such a call
// changes nothing and is answered without the AF policy or the driver.
TEST(Dx9SamplerInterceptionPolicyTest, RedundantSetIsSkippedOnlyOnAReconciledTrackedSampler) {
    const sb::SamplerState forced = ForcedSampler();
    EXPECT_TRUE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_LINEAR));
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_POINT)) << "a real change";
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_ANISOTROPIC))
        << "the forced physical value is not the application's";
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, sb::kStateCount, 0));

    sb::SamplerState unreconciled = forced;
    unreconciled.reconciled = false;
    EXPECT_FALSE(sb::IsRedundantLogicalSet(unreconciled, kMinFilter, D3DTEXF_LINEAR));

    sb::SamplerState untracked = forced;
    untracked.initialized = false;
    EXPECT_FALSE(sb::IsRedundantLogicalSet(untracked, kMinFilter, D3DTEXF_LINEAR));

    sb::SamplerState invalidated = forced;
    sb::InvalidateStageSamplerShadow(invalidated);
    EXPECT_FALSE(sb::IsRedundantLogicalSet(invalidated, kMinFilter, D3DTEXF_LINEAR));
}

// A state-block Apply changes the device behind the Set* calls; the merge must
// clear `reconciled` so the next Set reaches the policy again.
TEST(Dx9SamplerInterceptionPolicyTest, StateBlockApplyEndsTheRedundantSetShortcut) {
    sb::SamplerShadow shadow;
    shadow[0] = ForcedSampler();
    sb::StateBlockSnapshot block = sb::MakeStateBlockSnapshot(sb::CoverageForStateBlockType(D3DSBT_PIXELSTATE));
    sb::CaptureStateBlock(block, shadow);
    ASSERT_TRUE(sb::IsRedundantLogicalSet(shadow[0], kMinFilter, D3DTEXF_LINEAR));

    sb::ApplyStateBlockToShadow(block, shadow);
    EXPECT_FALSE(shadow[0].reconciled);
    EXPECT_FALSE(sb::IsRedundantLogicalSet(shadow[0], kMinFilter, D3DTEXF_LINEAR));
}

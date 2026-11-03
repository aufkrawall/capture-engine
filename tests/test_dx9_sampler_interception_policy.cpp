#include <gtest/gtest.h>

#include "hook/d3d9/dx9_sampler_interception_policy.h"

namespace arm = ce::dx9_sampler_interception;

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

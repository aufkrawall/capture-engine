#include <gtest/gtest.h>

#include "hook/overrides/sampler_override/hook_arming_policy.h"

namespace arm = ce::sampler_hook_arming;

// The shared rule for D3D6/7, D3D8 and D3D9: without a sampler override only
// the state-block recording pair is hooked; the per-draw override hooks follow
// once, at device creation or at the first frame boundary with an override.
TEST(SamplerHookArmingPolicyTest, NoOverrideArmsOnlyTheRecordingPairOnce) {
    arm::VTableArming arming;
    const arm::ArmStep first = arming.OnDeviceHooks(false);
    EXPECT_TRUE(first.lifecycle);
    EXPECT_FALSE(first.overrides);
    EXPECT_FALSE(arming.OverridesArmed());

    const arm::ArmStep second = arming.OnDeviceHooks(false);
    EXPECT_FALSE(second.lifecycle) << "a second device on the vtable adds nothing";
    EXPECT_FALSE(second.overrides);
    EXPECT_FALSE(arming.OnFrame(false).overrides);
}

TEST(SamplerHookArmingPolicyTest, OverrideConfiguredLaterArmsAtTheNextFrameOnly) {
    arm::VTableArming arming;
    ASSERT_TRUE(arming.OnDeviceHooks(false).lifecycle);
    EXPECT_TRUE(arming.OnFrame(true).overrides);
    EXPECT_TRUE(arming.OverridesArmed());
    EXPECT_FALSE(arming.OnFrame(true).overrides) << "handed out once: a failed install is not retried per frame";
    EXPECT_FALSE(arming.OnFrame(false).overrides);
    EXPECT_TRUE(arming.OverridesArmed()) << "switching the override off keeps the detours that restore the values";
}

TEST(SamplerHookArmingPolicyTest, OverrideAtDeviceCreationArmsBothAtOnce) {
    arm::VTableArming arming;
    const arm::ArmStep step = arming.OnDeviceHooks(true);
    EXPECT_TRUE(step.lifecycle);
    EXPECT_TRUE(step.overrides);
    EXPECT_FALSE(arming.OnDeviceHooks(true).overrides);
}

TEST(SamplerHookArmingPolicyTest, ShadowAndRecordingsAreTrustedOnlyWhileIntercepted) {
    EXPECT_TRUE(arm::ShadowTracksFromDefaults(true, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(false, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(true, false));
    EXPECT_TRUE(arm::RecordingSnapshotTrusted(true));
    EXPECT_FALSE(arm::RecordingSnapshotTrusted(false));
}

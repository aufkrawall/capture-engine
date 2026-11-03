#include <gtest/gtest.h>

#include <string>

#include "hook/overrides/sampler_override/hook_arming_policy.h"
#include "source_fragment_reader.h"

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

// D3D8 and D3D6/7 follow the rule: their per-draw texture-stage hooks are
// installed only by the override step, which the device install hands out only
// with an override and the frame boundary arms before the configuration refresh.
TEST(SamplerHookArmingSourceTest, LegacyApisHookTextureStageStateOnlyWhenArmed) {
    using ce::test_source::FindSource;
    using ce::test_source::ReadFile;
    const std::string dx8 = ReadFile(FindSource("hook", "dx8_hook_detours.cpp"));
    const std::string legacy = ReadFile(FindSource("hook", "ddraw_hook_legacy_d3d_install.cpp"));
    const std::string ddraw = ReadFile(FindSource("hook", "ddraw_hook_detours_legacy_d3d.cpp"));
    ASSERT_FALSE(dx8.empty());
    ASSERT_FALSE(legacy.empty());
    ASSERT_FALSE(ddraw.empty());

    // D3D8: the slot is patched in the override step only, and the step runs from the arming answer.
    const size_t dx8Override = dx8.find("void HookD3D8OverrideSlotsLocked(");
    const size_t dx8OverrideEnd = dx8.find("D3D8SamplerVTableRecord* FindOrCreateD3D8SamplerVTableLocked(");
    ASSERT_NE(dx8Override, std::string::npos);
    ASSERT_NE(dx8OverrideEnd, std::string::npos);
    const size_t dx8SetHook = dx8.find("&vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]");
    EXPECT_GT(dx8SetHook, dx8Override);
    EXPECT_LT(dx8SetHook, dx8OverrideEnd);
    EXPECT_EQ(dx8.find("&vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]", dx8SetHook + 1), std::string::npos);
    EXPECT_NE(dx8.find("if (step.overrides)\n            HookD3D8OverrideSlotsLocked(record, vtable);"),
              std::string::npos);
    const size_t dx8Arm = dx8.find("    ArmD3D8SamplerHooksAtPresent(device);");
    const size_t dx8Refresh = dx8.find("ce::legacy_d3d_sampler_state::RefreshConfiguration(", dx8Arm);
    ASSERT_NE(dx8Arm, std::string::npos);
    EXPECT_NE(dx8Refresh, std::string::npos) << "arm before the refresh, so it reconciles at once";

    // D3D6/7: likewise, and EndScene is the frame boundary.
    const size_t legacyOverride = legacy.find("void HookLegacyD3DOverrideSlotsLocked(");
    const size_t legacyOverrideEnd = legacy.find("}  // namespace", legacyOverride);
    ASSERT_NE(legacyOverride, std::string::npos);
    for (const char* detour : {"&DetourSetTextureStageState7", "&DetourSetTextureStageState6",
                               "&DetourD3D7ApplyStateBlock", "InstallD3D7StateBlockTrackingHooks(record, vtable);"}) {
        const size_t at = legacy.find(detour);
        EXPECT_GT(at, legacyOverride) << detour;
        EXPECT_LT(at, legacyOverrideEnd) << detour;
    }
    EXPECT_NE(legacy.find("if (step.overrides)\n            HookLegacyD3DOverrideSlotsLocked("), std::string::npos);
    for (const char* endScene : {"HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(", "HRESULT STDMETHODCALLTYPE DetourD3D6EndScene("}) {
        const size_t body = ddraw.find(endScene);
        ASSERT_NE(body, std::string::npos) << endScene;
        const size_t armAt = ddraw.find("ArmLegacyD3DSamplerHooksAtEndScene(record);", body);
        const size_t refreshAt = ddraw.find("RefreshConfiguration(", body);
        EXPECT_LT(armAt, refreshAt) << endScene;
        EXPECT_LT(ddraw.find("ReportLegacyD3DUse(", body), refreshAt) << "EndScene still reports the API in use";
    }
}

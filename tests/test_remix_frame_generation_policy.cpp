#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/remix_frame_generation_policy.h"
#include "source_fragment_reader.h"

TEST(RemixFrameGenerationPolicyTest, RecognizesOnlyTheUpstreamRemixScheduleOption) {
    EXPECT_TRUE(ce::remix_fg::IsScheduleOption("rtx.dlfg.maxInterpolatedFrames"));
    EXPECT_FALSE(ce::remix_fg::IsScheduleOption("DLSSG.MultiFrameCount"));
    EXPECT_FALSE(ce::remix_fg::IsScheduleOption("rtx.dlfg.enable"));

    EXPECT_TRUE(ce::remix_fg::ShouldOverrideConfigVariable("rtx.dlfg.maxInterpolatedFrames", 2));
    EXPECT_FALSE(ce::remix_fg::ShouldOverrideConfigVariable("rtx.dlfg.maxInterpolatedFrames", 0));
    EXPECT_FALSE(ce::remix_fg::ShouldOverrideConfigVariable("rtx.dlfg.maxInterpolatedFrames", 4));
}

TEST(RemixFrameGenerationPolicyTest, ReassertsWhenNgxAndTheUpstreamSchedulerCanDiffer) {
    using ce::remix_fg::ShouldReassertFromNgx;

    EXPECT_TRUE(ShouldReassertFromNgx("DLSSG.MultiFrameCount", 1, 2, 2, UINT32_MAX));
    EXPECT_FALSE(ShouldReassertFromNgx("DLSSG.MultiFrameCount", 1, 2, 2, 1));
    EXPECT_TRUE(ShouldReassertFromNgx("MultiFrameCount", 2, 2, 0, 2));
    EXPECT_FALSE(ShouldReassertFromNgx("DLSSG.MultiFrameCount", 2, 2, 2, 1));
    EXPECT_FALSE(ShouldReassertFromNgx("FrameGenerationMultiplier", 3, 2, 0, 2));
    EXPECT_FALSE(ShouldReassertFromNgx("DLSSG.MultiFrameIndex", 1, 2, 0, 2));
}

TEST(RemixFrameGenerationPolicyTest, FormatsThePublicConfigValueWithoutAllocation) {
    ASSERT_NE(ce::remix_fg::GeneratedFrameCountString(1), nullptr);
    ASSERT_NE(ce::remix_fg::GeneratedFrameCountString(2), nullptr);
    ASSERT_NE(ce::remix_fg::GeneratedFrameCountString(3), nullptr);
    EXPECT_STREQ(ce::remix_fg::GeneratedFrameCountString(1), "1");
    EXPECT_STREQ(ce::remix_fg::GeneratedFrameCountString(2), "2");
    EXPECT_STREQ(ce::remix_fg::GeneratedFrameCountString(3), "3");
    EXPECT_EQ(ce::remix_fg::GeneratedFrameCountString(0), nullptr);
    EXPECT_EQ(ce::remix_fg::GeneratedFrameCountString(4), nullptr);
}

TEST(RemixFrameGenerationPolicyTest, HooksTheLegitimateRemixInterfaceWithoutSyntheticD3D9Startup) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "apis" / "remix_hook.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("RegisterDynamicHookFiltered("), std::string::npos);
    EXPECT_NE(text.find("\"remixapi_InitializeLibrary\""), std::string::npos);
    EXPECT_NE(text.find("interfacePrefix->setConfigVariable = &HookedSetConfigVariable"), std::string::npos);
    EXPECT_NE(text.find("ce::remix_fg::kScheduleOption"), std::string::npos);
    EXPECT_EQ(text.find("Direct3DCreate9Ex"), std::string::npos);
}

TEST(RemixFrameGenerationPolicyTest, RegistersBeforeTheGetProcAddressRouterIsArmed) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "main_hookthread.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());
    const size_t registerHook = text.find("RemixHook::RegisterDynamicHooks();");
    const size_t armPreset = text.find("ArmNgxFgPresetOverrideIfConfigured(\"config.ini\")");
    ASSERT_NE(registerHook, std::string::npos);
    ASSERT_NE(armPreset, std::string::npos);
    EXPECT_LT(registerHook, armPreset);
}

TEST(RemixFrameGenerationPolicyTest, TracksLateModuleLoadAndUnloadWithoutAProcessNameRule) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "main_overlay_detect.cpp";
    ASSERT_TRUE(fs::exists(source));

    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("RemixHook::OnModuleLoaded(module, moduleNameOrPath)"), std::string::npos);
    EXPECT_NE(text.find("RemixHook::OnModuleUnloaded(data->DllBase, data->SizeOfImage, base)"),
              std::string::npos);
    EXPECT_EQ(text.find("PortalRTX"), std::string::npos);
    EXPECT_EQ(text.find("NvRemixBridge.exe"), std::string::npos);
}

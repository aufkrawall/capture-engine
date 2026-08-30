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

// The Portal RTX FIFO stutter (session 20260830_175147): CE forced
// VK_PRESENT_MODE_FIFO_KHR and unlinked VK_NV_present_metering from every
// present, yet the driver kept presenting the FIFO swapchain with
// SyncInterval=0 plus DXGI_PRESENT_ALLOW_TEARING because the capability was
// still enabled on the device. Withholding the capability is only half the
// answer: the runtime's own option has to agree, or it never falls back to the
// CPU pacer that spreads a generated group across the rendered frame it
// belongs to.
TEST(RemixFrameGenerationPolicyTest, DisablesHardwarePresentMeteringOnlyForVblankPacedProfiles) {
    using ce::remix_fg::IsPresentMeteringOption;
    using ce::remix_fg::RequestsVblankPacedPresentation;
    using ce::remix_fg::ShouldForcePresentMeteringOff;

    EXPECT_TRUE(IsPresentMeteringOption("rtx.dlfg.enablePresentMetering"));
    EXPECT_FALSE(IsPresentMeteringOption("rtx.dlfg.maxInterpolatedFrames"));
    EXPECT_FALSE(IsPresentMeteringOption("rtx.dlfg.enable"));

    // The same two spellings the Vulkan layer maps onto FIFO and FIFO_RELAXED,
    // so the layer and the runtime cannot disagree about one setting.
    EXPECT_TRUE(RequestsVblankPacedPresentation("fifo"));
    EXPECT_TRUE(RequestsVblankPacedPresentation("adaptive"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("mailbox"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("off"));
    EXPECT_FALSE(RequestsVblankPacedPresentation("default"));
    EXPECT_FALSE(RequestsVblankPacedPresentation(""));
    EXPECT_FALSE(RequestsVblankPacedPresentation("FIFO"));

    EXPECT_TRUE(ShouldForcePresentMeteringOff("rtx.dlfg.enablePresentMetering", "fifo"));
    EXPECT_TRUE(ShouldForcePresentMeteringOff("rtx.dlfg.enablePresentMetering", "adaptive"));
    // A profile that never asked for a rate contract keeps the runtime's own
    // pacing choice, hardware metering included.
    EXPECT_FALSE(ShouldForcePresentMeteringOff("rtx.dlfg.enablePresentMetering", "off"));
    EXPECT_FALSE(ShouldForcePresentMeteringOff("rtx.dlfg.enablePresentMetering", "mailbox"));
    EXPECT_FALSE(ShouldForcePresentMeteringOff("rtx.dlfg.enablePresentMetering", "default"));
    // No other option is ever rewritten.
    EXPECT_FALSE(ShouldForcePresentMeteringOff("rtx.dlfg.maxInterpolatedFrames", "fifo"));
    EXPECT_FALSE(ShouldForcePresentMeteringOff("rtx.vsync", "fifo"));
}

TEST(RemixFrameGenerationPolicyTest, PresentMeteringIsDisabledWithTheRuntimeSpelling) {
    // Remix parses its own config values; "False" is the spelling its own
    // rtx.conf uses for a disabled boolean option.
    EXPECT_STREQ(ce::remix_fg::kPresentMeteringDisabledValue, "False");
    EXPECT_STREQ(ce::remix_fg::kPresentMeteringOption, "rtx.dlfg.enablePresentMetering");
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

TEST(RemixFrameGenerationPolicyTest, NegotiatesExactKnownPublicApiVersionsWithTailCapacity) {
    EXPECT_EQ(ce::remix_fg::MakePublicApiVersion(0, 5, 1), 0x0000000000050001ull);
    EXPECT_EQ(ce::remix_fg::MakePublicApiVersion(0, 6, 4), 0x0000000000060004ull);
    ASSERT_EQ(ce::remix_fg::kKnownPublicApiVersions.size(), 2u);
    EXPECT_EQ(ce::remix_fg::kKnownPublicApiVersions[0],
              ce::remix_fg::MakePublicApiVersion(0, 6, 4));
    EXPECT_EQ(ce::remix_fg::kKnownPublicApiVersions[1],
              ce::remix_fg::MakePublicApiVersion(0, 5, 1));
    EXPECT_GE(ce::remix_fg::kPublicInterfaceStorageFunctionCount, 64u);
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
    EXPECT_NE(text.find("CaptureSetterThroughPublicInterface"), std::string::npos);
    EXPECT_NE(text.find("initializer(&info, static_cast<void*>(interfaceFunctions.data()))"),
              std::string::npos);
    EXPECT_NE(text.find("IsProviderOwnedFunction"), std::string::npos);
    EXPECT_NE(text.find("kKnownPublicApiVersions"), std::string::npos);
    EXPECT_EQ(text.find("CreateToolhelp32Snapshot"), std::string::npos);
    EXPECT_NE(text.find("ce::remix_fg::kScheduleOption"), std::string::npos);
    EXPECT_EQ(text.find("Direct3DCreate9Ex"), std::string::npos);
    EXPECT_EQ(text.find("NvRemixBridge.exe"), std::string::npos);
    EXPECT_EQ(text.find("PortalRTX"), std::string::npos);
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

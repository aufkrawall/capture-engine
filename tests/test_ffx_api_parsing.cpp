#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "../hook/apis/ffx_hook.h"
#include "../hook/common/ffx_api_parsing.h"

namespace {

void DummyFFXDetour() {}
void OtherDummyFFXDetour() {}

template <size_t N>
void FillInlineDetourSnapshot(std::array<unsigned char, N>& snapshot, const void* target, const void* detour) {
    snapshot.fill(0);
#ifdef _WIN64
    static_assert(N >= 14);
    snapshot[0] = 0xFF;
    snapshot[1] = 0x25;
    std::memcpy(snapshot.data() + 6, &detour, sizeof(detour));
#else
    static_assert(N >= 5);
    snapshot[0] = 0xE9;
    const auto* targetBytes = reinterpret_cast<const std::uint8_t*>(target);
    const auto* detourBytes = reinterpret_cast<const std::uint8_t*>(detour);
    const int32_t relativeTarget = static_cast<int32_t>(detourBytes - (targetBytes + 5));
    std::memcpy(snapshot.data() + 1, &relativeTarget, sizeof(relativeTarget));
#endif
}

TEST(FFXApiParsingTest, RecognizesEnabledFrameGenerationConfigure) {
    ce::ffx_api::ConfigureDescFrameGeneration desc{};
    desc.header.type = ce::ffx_api::kConfigureDescTypeFrameGeneration;
    desc.frameGenerationEnabled = true;
    desc.frameID = 42;

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc.header);
    EXPECT_TRUE(parsed.recognized);
    EXPECT_TRUE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 42u);
}

TEST(FFXApiParsingTest, RecognizesDisabledFrameGenerationConfigure) {
    ce::ffx_api::ConfigureDescFrameGeneration desc{};
    desc.header.type = ce::ffx_api::kConfigureDescTypeFrameGeneration;
    desc.frameGenerationEnabled = false;
    desc.frameID = 77;

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc.header);
    EXPECT_TRUE(parsed.recognized);
    EXPECT_FALSE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 77u);
}

TEST(FFXApiParsingTest, IgnoresNonFrameGenerationConfigure) {
    ce::ffx_api::ApiHeader desc{};
    desc.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGenerationSwapchain, 0x08u);

    const auto parsed = ce::ffx_api::ParseFrameGenerationConfigureState(&desc);
    EXPECT_FALSE(parsed.recognized);
    EXPECT_FALSE(parsed.enabled);
    EXPECT_EQ(parsed.frameId, 0u);
}

TEST(FFXApiParsingTest, ConfigureTypeRemainsDistinctFromCreateContextType) {
    EXPECT_NE(ce::ffx_api::kConfigureDescTypeFrameGeneration,
              ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGeneration, 0x01u));
}

TEST(FFXApiParsingTest, PresentCallbackParsingExposesRuntimeCompositionSurface) {
    ce::ffx_api::CallbackDescFrameGenerationPresent desc{};
    desc.header.type = ce::ffx_api::kCallbackDescTypeFrameGenerationPresent;
    desc.device = reinterpret_cast<void*>(0x1234);
    desc.commandList = reinterpret_cast<void*>(0x5678);
    desc.outputSwapChainBuffer.resource = reinterpret_cast<void*>(0x9abc);
    desc.currentBackBuffer.resource = reinterpret_cast<void*>(0xdef0);
    desc.currentUI.resource = reinterpret_cast<void*>(0x1111);
    desc.isGeneratedFrame = true;
    desc.frameID = 99;

    EXPECT_EQ(desc.header.type, ce::ffx_api::kCallbackDescTypeFrameGenerationPresent);
    EXPECT_EQ(desc.device, reinterpret_cast<void*>(0x1234));
    EXPECT_EQ(desc.commandList, reinterpret_cast<void*>(0x5678));
    EXPECT_EQ(desc.outputSwapChainBuffer.resource, reinterpret_cast<void*>(0x9abc));
    EXPECT_TRUE(desc.isGeneratedFrame);
    EXPECT_EQ(desc.frameID, 99u);
}

TEST(FFXApiParsingTest, PresentCallbackPremulAlphaDefaultsFalseWithoutExtension) {
    ce::ffx_api::CallbackDescFrameGenerationPresent desc{};
    desc.header.type = ce::ffx_api::kCallbackDescTypeFrameGenerationPresent;

    EXPECT_FALSE(ce::ffx_api::ResolvePresentCallbackUsePremulAlpha(&desc));
}

TEST(FFXApiParsingTest, PresentCallbackPremulAlphaReadsLinkedExtension) {
    ce::ffx_api::CallbackDescFrameGenerationPresent desc{};
    desc.header.type = ce::ffx_api::kCallbackDescTypeFrameGenerationPresent;
    ce::ffx_api::CallbackDescFrameGenerationPresentPremulAlpha premul{};
    premul.header.type = ce::ffx_api::kCallbackDescTypeFrameGenerationPresentPremulAlpha;
    premul.usePremulAlpha = true;
    desc.header.pNext = &premul.header;

    EXPECT_TRUE(ce::ffx_api::ResolvePresentCallbackUsePremulAlpha(&desc));
}

TEST(FFXApiParsingTest, OfficialAMDFFXRuntimeModulesUseNonInlineExportHooks) {
    EXPECT_TRUE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("amd_fidelityfx_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(
        "C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("AMD_FidelityFX_FG.dll"));

    EXPECT_FALSE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("amd_fidelityfx_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldInlineHookFFXExportsForModule(
        "C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldPatchFFXImportsForModule("amd_fidelityfx_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldPatchFFXImportsForModule(
        "C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint("amd_fidelityfx_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(
        "C:\\Games\\Talos\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldInlineHookFFXExportsForModule(nullptr));
    EXPECT_FALSE(ce::ffx_api::ShouldPatchFFXImportsForModule(nullptr));
    EXPECT_FALSE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(nullptr));
}

TEST(FFXApiParsingTest, ProxyAndLegacyFFXModulesCanStillUseInlineExportHooks) {
    EXPECT_FALSE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("ffx_frameinterpolation_x64.dll"));
    EXPECT_FALSE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("nvngx_dlssg.dll"));
    EXPECT_FALSE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("fsr3fg.dll"));

    EXPECT_TRUE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("ffx_frameinterpolation_x64.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("ffx_framegeneration.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("nvngx_dlssg.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("fsr3mod.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldPatchFFXImportsForModule("ffx_framegeneration.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldPatchFFXImportsForModule("nvngx_dlssg.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldPatchFFXImportsForModule("fsr3mod.dll"));
}

TEST(FFXHookValidationTest, ProbeRecognizesExpectedInlineDetourSnapshot) {
#ifdef _WIN64
    std::array<unsigned char, 14> snapshot{};
#else
    std::array<unsigned char, 5> snapshot{};
#endif
    FillInlineDetourSnapshot(snapshot, snapshot.data(), reinterpret_cast<void*>(&DummyFFXDetour));

    const auto probeResult =
        FFXHook::detail::ProbeExpectedInlineDetourInstalled(snapshot.data(), reinterpret_cast<void*>(&DummyFFXDetour));
    EXPECT_EQ(probeResult.state, FFXHook::detail::InlineDetourProbeState::kInstalledExpected);
    EXPECT_EQ(probeResult.win32Error, ERROR_SUCCESS);
}

TEST(FFXHookValidationTest, SnapshotRejectsChangedDetourTarget) {
#ifdef _WIN64
    std::array<unsigned char, 14> snapshot{};
#else
    std::array<unsigned char, 5> snapshot{};
#endif
    FillInlineDetourSnapshot(snapshot, snapshot.data(), reinterpret_cast<void*>(&DummyFFXDetour));

    EXPECT_FALSE(FFXHook::detail::SnapshotMatchesExpectedInlineDetour(snapshot.data(), snapshot.data(), snapshot.size(),
                                                                      reinterpret_cast<void*>(&OtherDummyFFXDetour)));
}

TEST(FFXHookValidationTest, ProbeHandlesUnreadableTargetGracefully) {
    const auto probeResult = FFXHook::detail::ProbeExpectedInlineDetourInstalled(
        reinterpret_cast<void*>(static_cast<uintptr_t>(1)), reinterpret_cast<void*>(&DummyFFXDetour));
    EXPECT_EQ(probeResult.state, FFXHook::detail::InlineDetourProbeState::kUnreadableTarget);
    EXPECT_NE(probeResult.win32Error, ERROR_SUCCESS);
}

}  // namespace

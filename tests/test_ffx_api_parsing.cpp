#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "../hook/apis/ffx_cached_pointer_router.h"
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
    std::memcpy(snapshot.data() + 6, reinterpret_cast<const void*>(&detour), sizeof(detour));
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

TEST(FFXApiParsingTest, ClassifiesGenericFrameGenerationBackendFromCreateChain) {
    ce::ffx_api::ApiHeader create{};
    create.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGeneration, 0x01u);
    ce::ffx_api::ApiHeader backend{};
    create.pNext = &backend;

    backend.type = ce::ffx_api::kCreateContextDescTypeBackendVulkan;
    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&create), ce::ffx_api::BackendApi::kVulkan);
    EXPECT_FALSE(ce::ffx_api::ShouldUseDX12FrameGenerationInterop(ce::ffx_api::ParseCreateContextBackend(&create)));

    backend.type = ce::ffx_api::kCreateContextDescTypeBackendDX12;
    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&create), ce::ffx_api::BackendApi::kDX12);
    EXPECT_TRUE(ce::ffx_api::ShouldUseDX12FrameGenerationInterop(ce::ffx_api::ParseCreateContextBackend(&create)));

    backend.type = ce::ffx_api::kCreateContextDescTypeBackendVulkanModern;
    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&create), ce::ffx_api::BackendApi::kVulkan);
}

TEST(FFXApiParsingTest, ClassifiesBackendSpecificFrameGenerationSwapchainFamilies) {
    ce::ffx_api::ApiHeader dx12Swapchain{};
    dx12Swapchain.type = ce::ffx_api::kCreateContextDescTypeFrameGenerationSwapChainWrapDX12;
    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&dx12Swapchain), ce::ffx_api::BackendApi::kDX12);

    ce::ffx_api::ApiHeader vulkanSwapchain{};
    vulkanSwapchain.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGenerationSwapchainVulkan, 0x01u);
    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&vulkanSwapchain), ce::ffx_api::BackendApi::kVulkan);
    EXPECT_TRUE(ce::ffx_api::IsFrameGenerationEffectType(vulkanSwapchain.type));
}

TEST(FFXApiParsingTest, UnknownBackendPreservesLegacyDX12Interop) {
    ce::ffx_api::ApiHeader create{};
    create.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGeneration, 0x01u);

    EXPECT_EQ(ce::ffx_api::ParseCreateContextBackend(&create), ce::ffx_api::BackendApi::kUnknown);
    EXPECT_TRUE(ce::ffx_api::ShouldUseDX12FrameGenerationInterop(ce::ffx_api::ParseCreateContextBackend(&create)));
}

TEST(FFXApiParsingTest, ParsesAllDX12FrameGenerationSwapChainCreationQueues) {
    void* wrapSwapChain = reinterpret_cast<void*>(0x1100);
    ce::ffx_api::CreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = ce::ffx_api::kCreateContextDescTypeFrameGenerationSwapChainWrapDX12;
    wrap.swapChain = &wrapSwapChain;
    wrap.gameQueue = reinterpret_cast<void*>(0x1200);

    const auto parsedWrap = ce::ffx_api::ParseFrameGenerationSwapChainCreateState(&wrap.header);
    EXPECT_TRUE(parsedWrap.recognized);
    EXPECT_EQ(parsedWrap.swapChainOutput, &wrapSwapChain);
    EXPECT_EQ(parsedWrap.gameQueue, wrap.gameQueue);

    void* newSwapChain = nullptr;
    ce::ffx_api::CreateContextDescFrameGenerationSwapChainNewDX12 createNew{};
    createNew.header.type = ce::ffx_api::kCreateContextDescTypeFrameGenerationSwapChainNewDX12;
    createNew.swapChain = &newSwapChain;
    createNew.gameQueue = reinterpret_cast<void*>(0x2200);

    const auto parsedNew = ce::ffx_api::ParseFrameGenerationSwapChainCreateState(&createNew.header);
    EXPECT_TRUE(parsedNew.recognized);
    EXPECT_EQ(parsedNew.swapChainOutput, &newSwapChain);
    EXPECT_EQ(parsedNew.gameQueue, createNew.gameQueue);

    void* hwndSwapChain = nullptr;
    ce::ffx_api::CreateContextDescFrameGenerationSwapChainForHwndDX12 createForHwnd{};
    createForHwnd.header.type = ce::ffx_api::kCreateContextDescTypeFrameGenerationSwapChainForHwndDX12;
    createForHwnd.swapChain = &hwndSwapChain;
    createForHwnd.gameQueue = reinterpret_cast<void*>(0x3200);

    const auto parsedForHwnd = ce::ffx_api::ParseFrameGenerationSwapChainCreateState(&createForHwnd.header);
    EXPECT_TRUE(parsedForHwnd.recognized);
    EXPECT_EQ(parsedForHwnd.swapChainOutput, &hwndSwapChain);
    EXPECT_EQ(parsedForHwnd.gameQueue, createForHwnd.gameQueue);
}

TEST(FFXApiParsingTest, IgnoresNonSwapChainCreateDescriptors) {
    ce::ffx_api::ApiHeader versionQuery{};
    versionQuery.type = ce::ffx_api::MakeEffectSubId(ce::ffx_api::kEffectIdFrameGenerationSwapchain, 0x0bu);

    const auto parsed = ce::ffx_api::ParseFrameGenerationSwapChainCreateState(&versionQuery);
    EXPECT_FALSE(parsed.recognized);
    EXPECT_EQ(parsed.swapChainOutput, nullptr);
    EXPECT_EQ(parsed.gameQueue, nullptr);
}

TEST(FFXCachedPointerRouterTest, MatchesOnlyExactLiveOriginalExportPointers) {
    ce::ffx_cached_pointer_router::Route routes[] = {
        {"ffxCreateContext", reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000)},
        {"ffxConfigure", reinterpret_cast<void*>(0x3000), reinterpret_cast<void*>(0x4000)},
        {"invalid", nullptr, reinterpret_cast<void*>(0x5000)},
        {"identity", reinterpret_cast<void*>(0x6000), reinterpret_cast<void*>(0x6000)},
    };

    EXPECT_EQ(ce::ffx_cached_pointer_router::detail::FindMatchingRoute(reinterpret_cast<void*>(0x1000), routes,
                                                                       _countof(routes)),
              0);
    EXPECT_EQ(ce::ffx_cached_pointer_router::detail::FindMatchingRoute(reinterpret_cast<void*>(0x3000), routes,
                                                                       _countof(routes)),
              1);
    EXPECT_EQ(ce::ffx_cached_pointer_router::detail::FindMatchingRoute(reinterpret_cast<void*>(0x6000), routes,
                                                                       _countof(routes)),
              -1);
    EXPECT_EQ(ce::ffx_cached_pointer_router::detail::FindMatchingRoute(reinterpret_cast<void*>(0x7000), routes,
                                                                       _countof(routes)),
              -1);
}

TEST(FFXCachedPointerRouterTest, ScansOnlyWritableNonExecutablePersistentSections) {
    EXPECT_TRUE(ce::ffx_cached_pointer_router::detail::IsWritableNonExecutableSection(IMAGE_SCN_MEM_READ |
                                                                                      IMAGE_SCN_MEM_WRITE));
    EXPECT_FALSE(ce::ffx_cached_pointer_router::detail::IsWritableNonExecutableSection(
        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE));
    EXPECT_FALSE(ce::ffx_cached_pointer_router::detail::IsWritableNonExecutableSection(IMAGE_SCN_MEM_READ));
    EXPECT_FALSE(ce::ffx_cached_pointer_router::detail::IsWritableNonExecutableSection(
        IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_DISCARDABLE));
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

TEST(FFXApiParsingTest, OfficialAMDFFXRuntimeModulesSkipInlineJumpsButArmConfigureFallback) {
    EXPECT_TRUE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("amd_fidelityfx_dx12.dll"));
    EXPECT_TRUE(
        ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName("AMD_FidelityFX_FG.dll"));

    EXPECT_FALSE(ce::ffx_api::ShouldInlineHookFFXExportsForModule("amd_fidelityfx_dx12.dll"));
    EXPECT_FALSE(
        ce::ffx_api::ShouldInlineHookFFXExportsForModule("C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldPatchFFXImportsForModule("amd_fidelityfx_dx12.dll"));
    EXPECT_TRUE(
        ce::ffx_api::ShouldPatchFFXImportsForModule("C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint("amd_fidelityfx_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(
        "C:\\Games\\Talos\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint("AMD_FidelityFX_FG.dll"));
    EXPECT_FALSE(ce::ffx_api::ShouldInlineHookFFXExportsForModule(nullptr));
    EXPECT_FALSE(ce::ffx_api::ShouldPatchFFXImportsForModule(nullptr));
    EXPECT_FALSE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(nullptr));
    EXPECT_FALSE(ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint("amd_fidelityfx_vk.dll"));
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

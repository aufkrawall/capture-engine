#include "test_dxgi_shared_shared.h"

TEST(DXGISharedSourceTest, OverlayCallerCannotTurnAGameOutputIntoAHelperSwapchain) {
    for (const char* file : {"dxgi_shared_present_core.cpp", "dxgi_shared_present1.cpp"}) {
        const std::string source = ce::test_source::ReadFile(
            std::filesystem::current_path() / "hook" / "present" / file);
        EXPECT_NE(source.find("ShouldBypassThirdPartyOverlayPresent(pSwapChain,"), std::string::npos);
    }
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/present/dxgi_shared_overlay_admission.cpp");
    const size_t admission = source.find("ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(");
    ASSERT_NE(admission, std::string::npos);
    const size_t end = source.find(";", admission);
    ASSERT_NE(end, std::string::npos);
    const std::string decision = source.substr(admission, end - admission);
    EXPECT_EQ(decision.find("callerFromThirdPartyOverlay"), std::string::npos);
    EXPECT_NE(decision.find("knownThirdPartyOverlaySwapchain"), std::string::npos);
    EXPECT_NE(decision.find("startupBlockingOverlayTaggedSwapchain"), std::string::npos);
}

TEST(DXGISharedTest, OverlayHelpersRemainExcludedUntilAStartupWrapperReturnsGameOwnership) {
    using ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain;
    EXPECT_FALSE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(false, false, false));
    EXPECT_FALSE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(false, false, true));
    EXPECT_TRUE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(true, false, false));
    EXPECT_TRUE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(true, false, true));
    EXPECT_TRUE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(true, true, true));
    EXPECT_FALSE(ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(true, true, false));
}

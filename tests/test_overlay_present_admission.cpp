#include "test_dxgi_shared_shared.h"

TEST(DXGISharedSourceTest, OverlayCallerCannotTurnAGameOutputIntoAHelperSwapchain) {
    for (const char* file : {"dxgi_shared_present_core.cpp", "dxgi_shared_present1.cpp"}) {
        const std::string source = ce::test_source::ReadFile(
            std::filesystem::current_path() / "hook" / "present" / file);
        const size_t call = source.find("ShouldBypassThirdPartyOverlayPresent(pSwapChain,");
        ASSERT_NE(call, std::string::npos) << file;
        // The Present chain-participant evidence (which overlay forwarded into CE
        // on this call) must keep flowing in from both Present methods.
        const size_t callEnd = source.find(";", call);
        ASSERT_NE(callEnd, std::string::npos) << file;
        EXPECT_NE(source.substr(call, callEnd - call).find("detourCallerModulePath"), std::string::npos) << file;
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

// The inject-after-CE order (RTSS loading after Capture Engine already prepended over the
// Present entry) is exactly the single-late-overlay case: the install-time leave-entry
// decision cannot cover it and the two participants share the five entry bytes through
// their save/restore cycles. That state must be named for ANY late overlay - not only for
// a second one - and the entry dance must be observable from outside the Present path.
TEST(DXGISharedSourceTest, LateForeignOverlayJoinIsNamedForASingleOverlayAndProbesTheEntry) {
    namespace fs = std::filesystem;
    const auto detect = ce::test_source::ReadFile(fs::current_path() / "hook" / "runtime" / "main_overlay_detect.cpp");
    const size_t join = detect.find("joined a Present entry CE already prepended over");
    ASSERT_NE(join, std::string::npos);
    const size_t gate = detect.find("TrackedOverlaySubset::kOverlay) >=");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, join);
    EXPECT_NE(detect.find("TrackedOverlaySubset::kOverlay) >= 1"), std::string::npos);
    EXPECT_EQ(detect.find("TrackedOverlaySubset::kOverlay) >= 2"), std::string::npos);
    EXPECT_NE(detect.find("DXGIShared::ArmPresentEntryChainStateProbe(matched)"), std::string::npos);

    // The samples belong to the hook thread (between presents), never the Present path.
    const auto hookThread = ce::test_source::ReadFile(fs::current_path() / "hook" / "runtime" / "main_hookthread.cpp");
    EXPECT_NE(hookThread.find("DXGIShared::ServicePresentEntryChainStateProbe()"), std::string::npos);
}

#include "test_dxgi_shared_shared.h"
#include "hook/present/dxgi_present_hook_policy.h"

TEST(DXGISharedTest, NativePresentEntriesAreReservedBeforeAnOverlayLoads) {
    using ce::dxgi_present_hook::ShouldReserveEntryForLateOverlays;
    EXPECT_TRUE(ShouldReserveEntryForLateOverlays(true, true, false));
    EXPECT_FALSE(ShouldReserveEntryForLateOverlays(false, true, false));
    EXPECT_FALSE(ShouldReserveEntryForLateOverlays(true, false, false));
    EXPECT_FALSE(ShouldReserveEntryForLateOverlays(true, true, true));
    EXPECT_EQ(ce::dxgi_present_hook::kReservedEntryBytes, 14);
}

TEST(DXGISharedTest, NativeEntryReservationPreservesIndependentFrontendVTableInterception) {
    using ce::dxgi_present_hook::ShouldKeepVTablePristineForReservedEntry;
    EXPECT_TRUE(ShouldKeepVTablePristineForReservedEntry(true, false));
    EXPECT_TRUE(ShouldKeepVTablePristineForReservedEntry(true, true));
    EXPECT_FALSE(ShouldKeepVTablePristineForReservedEntry(false, false));
    EXPECT_TRUE(ShouldKeepVTablePristineForReservedEntry(false, true));
}

TEST(DXGISharedSourceTest, NativeEntryReservationPrecedesTheOrdinaryPrepend) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/present/dxgi_shared_hooks_present.cpp");
    const size_t reserve = source.find("ce::dxgi_present_hook::ShouldReserveEntryForLateOverlays(");
    ASSERT_NE(reserve, std::string::npos);
    EXPECT_NE(source.find("IsAddressInsideSystemDXGI(presentAddr)"), std::string::npos);
    EXPECT_NE(source.find("IsAddressInsideSystemDXGI(present1Addr)"), std::string::npos);
    const size_t body = source.find("InstallPresentBodyHooksBelowForeignChain(presentAddr", reserve);
    ASSERT_NE(body, std::string::npos);
    const size_t prepend = source.find("InlineHook::InstallPublishedBatch(inlineHooks", body);
    ASSERT_NE(prepend, std::string::npos);
    EXPECT_LT(body, prepend);
    EXPECT_NE(source.find("ce::dxgi_present_hook::kReservedEntryBytes", reserve), std::string::npos);
}

TEST(DXGISharedSourceTest, EntryProbeResolvesForeignJumpsWithoutAnOwnedPrepend) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/present/dxgi_shared_hooks.cpp");
    const size_t probe = source.find("void ServicePresentEntryChainStateProbe()");
    ASSERT_NE(probe, std::string::npos);
    const size_t ownership = source.find("InlineHook::IsInstalledEntryPatchIntact(entry, &jumpTarget)", probe);
    ASSERT_NE(ownership, std::string::npos);
    EXPECT_NE(source.find("ResolveE9JmpTarget(entry)", ownership), std::string::npos);
    EXPECT_NE(source.find("ResolveFF25JmpTarget(entry)", ownership), std::string::npos);
}

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

TEST(DXGISharedSourceTest, PresentAdmissionWalksTheStackOnlyForTaggedHelperSwapchains) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/present/dxgi_shared_overlay_admission.cpp");
    // The startup-blocking stack walk is a CaptureStackBackTrace plus per-frame
    // address lookups per D3D12 Present. It stays off the common path: only a
    // swapchain that is both creation-tagged and helper-owned may consult it.
    const size_t guarded = source.find("knownThirdPartyOverlaySwapchain && startupBlockingOverlayTaggedSwapchain &&");
    ASSERT_NE(guarded, std::string::npos);
    const size_t walk = source.find("HasStartupBlockingOverlayModuleInCurrentStack()", guarded);
    ASSERT_NE(walk, std::string::npos);
    EXPECT_LT(walk, source.find(";", guarded));
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

TEST(DXGISharedTest, StartupBlockingPresentEvidenceCannotAffectAnUntaggedSwapchain) {
    using ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass;
    // The stack evidence is the only expensive input; nothing but a tagged
    // helper swapchain can turn it into a decision, which is what lets the
    // admission path skip the walk entirely on the common Present paths.
    EXPECT_FALSE(ShouldKeepStartupBlockingOverlaySwapchainBypass(false, false, true));
    EXPECT_FALSE(ShouldKeepStartupBlockingOverlaySwapchainBypass(true, false, true));
    EXPECT_FALSE(ShouldKeepStartupBlockingOverlaySwapchainBypass(false, true, true));
    EXPECT_TRUE(ShouldKeepStartupBlockingOverlaySwapchainBypass(true, true, true));
    EXPECT_FALSE(ShouldKeepStartupBlockingOverlaySwapchainBypass(true, true, false));
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

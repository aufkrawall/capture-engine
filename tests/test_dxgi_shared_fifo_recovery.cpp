#include "test_dxgi_shared_shared.h"

namespace {

void ExpectFinalOutputVSyncBeforeBypass(const std::string& source, const char* finalBranch,
                                       const char* bypassDeclaration) {
    const size_t branch = source.find(finalBranch);
    ASSERT_NE(branch, std::string::npos);
    const size_t vsync = source.find("ProcessVSyncOverride(SyncInterval, Flags);", branch);
    const size_t bypass = source.find(bypassDeclaration, branch);
    ASSERT_NE(vsync, std::string::npos);
    ASSERT_NE(bypass, std::string::npos);
    EXPECT_LT(vsync, bypass);
}

}  // namespace

TEST(DXGISharedSourceTest, RecoveredStreamlineFinalOutputsApplyConfiguredVSyncAtThePhysicalBoundary) {
    const std::string present = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook" / "common" / "dxgi_shared_present_routing.cpp");
    const std::string present1 = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook" / "common" / "dxgi_shared_present1.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());

    ExpectFinalOutputVSyncBeforeBypass(
        present, "ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(",
        "PFN_Present presentBypass = EnsurePresentBypassTrampoline();");
    ExpectFinalOutputVSyncBeforeBypass(
        present, "if (ctx.streamlineSyntheticReentrant)",
        "PFN_Present presentBypass = EnsurePresentBypassTrampoline();");
    ExpectFinalOutputVSyncBeforeBypass(
        present1, "ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(",
        "PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();");
    ExpectFinalOutputVSyncBeforeBypass(
        present1, "if (streamlineSyntheticReentrant)",
        "PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();");

    // Startup handoff is still the application/proxy transport. It must reach
    // its bypass before any final-output sync override; sync=1 there is the
    // documented DLSS-G pacer/device-hang boundary.
    const size_t startup = present.find("ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(");
    const size_t startupBypass = present.find("PFN_Present presentBypass = EnsurePresentBypassTrampoline();", startup);
    const size_t firstVSync = present.find("ProcessVSyncOverride(SyncInterval, Flags);", startup);
    ASSERT_NE(startupBypass, std::string::npos);
    ASSERT_NE(firstVSync, std::string::npos);
    EXPECT_LT(startupBypass, firstVSync);
}

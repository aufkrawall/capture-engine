#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_overlay_policy/ecl_caller_gate.h"
#include "source_fragment_reader.h"

// Whose ExecuteCommandLists an overlay's return address is (ecl_caller_gate.h). Leaving an overlay's own lists out of
// the game's count matters little; leaving the game's out because an overlay hooks above CE would starve frame
// classification, so that side is what these tests pin.
namespace {
using ce::dx12_overlay_policy::IsOverlayOwnedEclSubmission;
int ceEntry;
int followerEntry;
}  // namespace

TEST(EclCallerGateTest, AnOverlayAboveCeCanForwardTheGamesSubmission) {
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, &followerEntry, &ceEntry));
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(false, &followerEntry, &ceEntry));
}

TEST(EclCallerGateTest, CeAtTheCurrentQueueEntryDistinguishesAnOverlaysOwnLists) {
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(false, &ceEntry, &ceEntry));
    EXPECT_TRUE(IsOverlayOwnedEclSubmission(true, &ceEntry, &ceEntry));
}

TEST(EclCallerGateTest, DelayedHookInstallationNeedsNoModuleLoadToInvalidateDirectCallerEvidence) {
    const void* liveEntry = &ceEntry;
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(false, liveEntry, &ceEntry));
    EXPECT_TRUE(IsOverlayOwnedEclSubmission(true, liveEntry, &ceEntry));
    // An already-loaded overlay installs its hook after the application's direct call.
    liveEntry = &followerEntry;
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, liveEntry, &ceEntry));
    // Removing that hook restores CE's proof immediately too.
    liveEntry = &ceEntry;
    EXPECT_TRUE(IsOverlayOwnedEclSubmission(true, liveEntry, &ceEntry));
}

TEST(EclCallerGateTest, ADirectCallOnAnotherQueueCannotExcludeTheGamesForwardedLists) {
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(false, &ceEntry, &ceEntry));
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, &followerEntry, &ceEntry));
    EXPECT_TRUE(IsOverlayOwnedEclSubmission(true, &ceEntry, &ceEntry));
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, &followerEntry, &ceEntry));
}

TEST(EclCallerGateTest, MissingEntryEvidenceKeepsTheSubmissionCounted) {
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, nullptr, &ceEntry));
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, nullptr, nullptr));
    EXPECT_FALSE(IsOverlayOwnedEclSubmission(true, &followerEntry, nullptr));
}

// The classifier cannot read the caller itself: a helper's return address is the detour (2026-11-04).
TEST(EclCallerGateSourceTest, TheDetourReadsItsOwnReturnAddressForTheClassifier) {
    namespace fs = std::filesystem;
    const std::string ecl = ce::test_source::ReadFile(fs::current_path() / "hook" / "d3d12" / "dx12_hook_ecl.cpp");
    const std::string helper =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "d3d12" / "dx12_hook_swapchain_create.cpp");
    ASSERT_FALSE(ecl.empty());
    ASSERT_FALSE(helper.empty());
    EXPECT_NE(ecl.find("IsECLCallerFromThirdPartyOverlay(CE_RETURN_ADDRESS(),"), std::string::npos);
    const size_t classifier = helper.find("bool IsECLCallerFromThirdPartyOverlay(");
    ASSERT_NE(classifier, std::string::npos);
    const size_t end = helper.find("\n}\n", classifier);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(helper.substr(classifier, end - classifier).find("CE_RETURN_ADDRESS()"), std::string::npos);
}

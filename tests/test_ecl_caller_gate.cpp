#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_overlay_policy/ecl_caller_gate.h"
#include "source_fragment_reader.h"

// Whose ExecuteCommandLists an overlay's return address is (ecl_caller_gate.h). Leaving an overlay's own lists out of
// the game's count matters little; leaving the game's out because an overlay hooks above CE would starve frame
// classification, so that side is what these tests pin.
namespace {
using ce::dx12_overlay_policy::EclCallerGate;
constexpr uint64_t kModules = 7;
}  // namespace

TEST(EclCallerGateTest, AnOverlayWithNoDirectCallerSeenIsTheGamesRoute) {
    EclCallerGate gate;
    for (int submission = 0; submission < 100; ++submission) {
        EXPECT_FALSE(gate.Observe(true, kModules));
    }
}

TEST(EclCallerGateTest, OnceTheGameIsSeenDirectlyAnOverlaysListsAreItsOwn) {
    EclCallerGate gate;
    EXPECT_FALSE(gate.Observe(false, kModules));
    EXPECT_TRUE(gate.Observe(true, kModules));
    EXPECT_FALSE(gate.Observe(false, kModules));
    EXPECT_TRUE(gate.Observe(true, kModules));
}

TEST(EclCallerGateTest, AModuleLoadingMayHaveHookedInAboveCeSoItIsProvedAgain) {
    EclCallerGate gate;
    EXPECT_FALSE(gate.Observe(false, kModules));
    EXPECT_TRUE(gate.Observe(true, kModules));
    EXPECT_FALSE(gate.Observe(true, kModules + 1)) << "the new overlay now calls for the game";
    EXPECT_FALSE(gate.Observe(true, kModules + 1));
    EXPECT_FALSE(gate.Observe(false, kModules + 1));
    EXPECT_TRUE(gate.Observe(true, kModules + 1));
}

TEST(EclCallerGateTest, AnOverlaysFirstSubmissionBeforeAnyGameListIsCounted) {
    EclCallerGate gate;
    EXPECT_FALSE(gate.Observe(true, kModules));
    EXPECT_FALSE(gate.Observe(false, kModules));
    EXPECT_TRUE(gate.Observe(true, kModules));
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

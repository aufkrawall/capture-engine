#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::ClassifyPresentOverlayState;
using ce::dx12_overlay_policy::PresentOverlayState;
using ce::dx12_overlay_policy::SwapchainPresentLedger;

constexpr uintptr_t kNativeOff = 0x1000;
constexpr uintptr_t kStreamlineProxy = 0x2000;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(SwapchainPresentLedgerTest, ClassifiesDrawnInheritedAndMissingPresents) {
    EXPECT_EQ(ClassifyPresentOverlayState(true, true), PresentOverlayState::kDrawn);
    EXPECT_EQ(ClassifyPresentOverlayState(true, false), PresentOverlayState::kDrawn);
    EXPECT_EQ(ClassifyPresentOverlayState(false, true), PresentOverlayState::kInherited);
    EXPECT_EQ(ClassifyPresentOverlayState(false, false), PresentOverlayState::kMissing);
}

// The switch-spam case: the departing chain's final image stays on screen until the
// replacement presents, so its last state and the silent gap are what the log must name.
TEST(SwapchainPresentLedgerTest, HandoffReportsDepartingTailArrivingFirstPresentAndGap) {
    SwapchainPresentLedger ledger;
    ledger.NoteCreated(kNativeOff, 1'000);
    EXPECT_FALSE(ledger.NotePresent(kNativeOff, PresentOverlayState::kDrawn, 1u << 1, 2'000).handoff);
    ledger.NotePresent(kNativeOff, PresentOverlayState::kDrawn, 1u << 1, 9'000);
    ledger.NotePresent(kNativeOff, PresentOverlayState::kMissing, 0, 16'000);
    ledger.NotePresent(kNativeOff, PresentOverlayState::kMissing, 0, 23'000);

    ledger.NoteCreated(kStreamlineProxy, 30'000);
    const auto event = ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kDrawn, 1u << 2, 373'000);

    ASSERT_TRUE(event.handoff);
    EXPECT_EQ(event.departing.swapchain, kNativeOff);
    EXPECT_EQ(event.departing.presents, 4u);
    EXPECT_EQ(event.departing.lastState, PresentOverlayState::kMissing);
    EXPECT_EQ(event.departing.tailMissing, 2u);
    EXPECT_EQ(event.departing.missingPresents, 2u);
    EXPECT_EQ(event.current.swapchain, kStreamlineProxy);
    EXPECT_EQ(event.current.firstState, PresentOverlayState::kDrawn);
    EXPECT_EQ(event.current.firstRouteMask, 1u << 2);
    EXPECT_EQ(event.current.createdUs, 30'000u);
    EXPECT_EQ(event.noPresentGapUs, 350'000u);
    EXPECT_FALSE(event.overlayArrivedLate);
}

// DXGI reused a released swapchain's address for its replacement in the spam session
// (000001B64BFB4020 twice). A creation must start a new lifetime at the same address.
TEST(SwapchainPresentLedgerTest, ReusedAddressIsANewLifetimeAndStillAHandoff) {
    SwapchainPresentLedger ledger;
    ledger.NoteCreated(kStreamlineProxy, 0);
    ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kDrawn, 1u << 2, 1'000);
    ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kMissing, 0, 2'000);

    ledger.NoteCreated(kStreamlineProxy, 3'000);
    const auto event = ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kMissing, 0, 10'000);

    ASSERT_TRUE(event.handoff);
    EXPECT_EQ(event.departing.swapchain, kStreamlineProxy);
    EXPECT_EQ(event.departing.presents, 2u);
    EXPECT_EQ(event.departing.lastState, PresentOverlayState::kMissing);
    EXPECT_NE(event.departing.lifetime, event.current.lifetime);
    EXPECT_EQ(event.current.presents, 1u);
    EXPECT_EQ(ledger.Lifetimes(), 2u);
}

// A runtime presenting on two live chains is not a handoff; only a new lifetime is.
TEST(SwapchainPresentLedgerTest, AlternatingBetweenKnownChainsIsNotAHandoff) {
    SwapchainPresentLedger ledger;
    ledger.NotePresent(kNativeOff, PresentOverlayState::kDrawn, 1u << 1, 1'000);
    EXPECT_TRUE(ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kDrawn, 1u << 2, 2'000).handoff);
    EXPECT_FALSE(ledger.NotePresent(kNativeOff, PresentOverlayState::kDrawn, 1u << 1, 3'000).handoff);
    EXPECT_FALSE(ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kDrawn, 1u << 2, 4'000).handoff);
}

TEST(SwapchainPresentLedgerTest, ReportsWhenTheOverlayFirstReachesALateChain) {
    SwapchainPresentLedger ledger;
    ledger.NoteCreated(kStreamlineProxy, 0);
    EXPECT_FALSE(ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kMissing, 0, 1'000).overlayArrivedLate);
    EXPECT_FALSE(ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kMissing, 0, 8'000).overlayArrivedLate);
    const auto event = ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kInherited, 0, 15'000);
    ASSERT_TRUE(event.overlayArrivedLate);
    EXPECT_EQ(event.current.presentsBeforeFirstOverlay, 2u);
    EXPECT_EQ(event.current.firstOverlayUs - event.current.firstPresentUs, 14'000u);
    EXPECT_FALSE(ledger.NotePresent(kStreamlineProxy, PresentOverlayState::kMissing, 0, 22'000).overlayArrivedLate);
}

TEST(SwapchainPresentLedgerTest, EvictionKeepsTheDepartingChainForTheNextHandoff) {
    SwapchainPresentLedger ledger;
    uint64_t now = 0;
    for (uintptr_t sc = 1; sc <= SwapchainPresentLedger::kCapacity + 3; ++sc) {
        ledger.NoteCreated(sc * 0x100, ++now);
        ledger.NotePresent(sc * 0x100, PresentOverlayState::kDrawn, 1u << 1, ++now);
    }
    const uintptr_t last = (SwapchainPresentLedger::kCapacity + 3) * 0x100;
    ledger.NoteCreated(0xFFFF00, ++now);
    const auto event = ledger.NotePresent(0xFFFF00, PresentOverlayState::kMissing, 0, ++now);
    ASSERT_TRUE(event.handoff);
    EXPECT_EQ(event.departing.swapchain, last);
    EXPECT_EQ(event.departing.lastState, PresentOverlayState::kDrawn);
}

// Session 20260925_043001: PostSL and ProcessFrameExternal each accounted the same Present,
// so the second call of a covered present read as uncovered. Both Present detours now open
// one scope per physical Present, accounting inside a scope merges, and swapchain creation
// starts a new visibility lifetime.
TEST(SwapchainPresentLedgerTest, PresentDetoursMergeAccountingIntoOnePhysicalPresent) {
    const std::string present = ReadSource("hook/common/dxgi_shared_present.cpp");
    const std::string present1 = ReadSource("hook/common/dxgi_shared_present1.cpp");
    const std::string coverage = ReadSource("hook/apis/dx12_hook_overlay_coverage.cpp");
    const std::string create = ReadSource("hook/apis/dx12_hook_swapchain_create.cpp");
    const std::string eclInstall = ReadSource("hook/apis/dx12_hook_ecl_install.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(present1.empty());
    ASSERT_FALSE(coverage.empty());

    for (const std::string* detour : {&present, &present1}) {
        const size_t begin = detour->find("DX12_BeginOverlayPresentScope(pSwapChain);");
        ASSERT_NE(begin, std::string::npos);
        EXPECT_NE(detour->find("DX12_EndOverlayPresentScope();", begin), std::string::npos);
    }
    // The scope opens after the re-entrant forward, so nested forwards merge into the outer Present.
    EXPECT_LT(present.find("if (isReentrant) {"), present.find("DX12_BeginOverlayPresentScope(pSwapChain);"));

    const size_t account = coverage.find("void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw");
    ASSERT_NE(account, std::string::npos);
    const size_t merge = coverage.find("if (scope.depth > 0) {", account);
    const size_t judge = coverage.find("AccountPhysicalPresentForOverlayCoverage(pSwapChain", account);
    ASSERT_NE(merge, std::string::npos);
    ASSERT_NE(judge, std::string::npos);
    EXPECT_LT(merge, judge);
    EXPECT_NE(coverage.find("[OVERLAY SWAPCHAIN HANDOFF]"), std::string::npos);
    EXPECT_NE(coverage.find("endedWithoutOverlay="), std::string::npos);
    EXPECT_NE(coverage.find("noPresentGapMs="), std::string::npos);

    EXPECT_NE(create.find("DX12_NoteOverlayVisibilitySwapchainCreated(*ppSC);"), std::string::npos);
    EXPECT_NE(eclInstall.find("DX12_NoteOverlayVisibilitySwapchainCreated(*ppSwapChain);"), std::string::npos);
    EXPECT_NE(eclInstall.find("DX12_NoteOverlayVisibilitySwapchainCreated(*ppSC);"), std::string::npos);
}

}  // namespace

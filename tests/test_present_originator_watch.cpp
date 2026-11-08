// When the originator's stack walk may be skipped (hook/present/present_originator_policy.h). Every session of
// 20261104 ran below a foreign chain, so each Present walked 16 frames and scanned their paths for runtimes the
// process did not have. Skipping it is only sound while no runtime can be on the stack: these tests pin that.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

#include "hook/overlay/overlay_compat.h"
#include "hook/present/present_originator_policy.h"
#include "hook/present/swapchain_probe_memo.h"

namespace {

using ce::present_originator::OriginatorWalkMemo;
using ce::present_originator::RuntimeModuleWatch;

TEST(PresentRuntimeModuleWatchTest, NothingIsKnownUntilTheScanOfMappedModulesCompletes) {
    RuntimeModuleWatch watch;
    EXPECT_FALSE(watch.OnlyApplicationCanOriginate());
    watch.NoteModule("D:\\Games\\Game\\Game.exe", false);
    EXPECT_FALSE(watch.OnlyApplicationCanOriginate());
    watch.CompleteSeed();
    EXPECT_TRUE(watch.OnlyApplicationCanOriginate());
}

TEST(PresentRuntimeModuleWatchTest, EveryRuntimeTheWalkCouldNameEndsTheSkip) {
    struct Case {
        const char* path;
        bool exportsStreamlineApi;
    };
    const Case runtimes[] = {
        {"D:\\Games\\Game\\sl.interposer.dll", false},
        {"D:\\Games\\Game\\sl.dlss_g.dll", false},
        {"C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_g_0\\1B0_E658703.dll", true},  // hashed plugin: the export names it
        {"D:\\Games\\Game\\amd_fidelityfx_dx12.dll", false},
        {"D:\\Games\\Game\\amd_fidelityfx_framegeneration_dx12.dll", false},
        {"D:\\Games\\Game\\OptiScaler\\libxess_fg.dll", false},
        {"C:\\WINDOWS\\system32\\NvPresent64.dll", false},
    };
    for (const Case& runtime : runtimes) {
        RuntimeModuleWatch watch;
        watch.CompleteSeed();
        ASSERT_TRUE(watch.OnlyApplicationCanOriginate());
        watch.NoteModule(runtime.path, runtime.exportsStreamlineApi);
        EXPECT_FALSE(watch.OnlyApplicationCanOriginate()) << runtime.path;
    }
}

TEST(PresentRuntimeModuleWatchTest, AModuleWithoutAReadableIdentityCountsAsARuntime) {
    RuntimeModuleWatch nullPath;
    nullPath.CompleteSeed();
    nullPath.NoteModule(nullptr, false);
    EXPECT_FALSE(nullPath.OnlyApplicationCanOriginate());

    RuntimeModuleWatch emptyPath;
    emptyPath.CompleteSeed();
    emptyPath.NoteModule("", false);
    EXPECT_FALSE(emptyPath.OnlyApplicationCanOriginate());
}

TEST(PresentRuntimeModuleWatchTest, OverlaysAndOrdinaryModulesKeepTheSkip) {
    RuntimeModuleWatch watch;
    watch.NoteModule("C:\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll", false);
    watch.NoteModule("C:\\WINDOWS\\system32\\dxgi.dll", false);
    watch.NoteModule("D:\\Games\\Game\\engine.dll", false);
    watch.CompleteSeed();
    EXPECT_TRUE(watch.OnlyApplicationCanOriginate());
}

TEST(OriginatorWalkMemoTest, ARuntimeInTheProcessWalksEveryPresent) {
    OriginatorWalkMemo memo;
    for (int present = 0; present < 1000; ++present) {
        EXPECT_TRUE(memo.ShouldWalk(false, 7));
        memo.NoteWalked(7);
    }
}

TEST(OriginatorWalkMemoTest, WithoutARuntimeTheFirstWalkIsKeptUntilTheModuleSetChanges) {
    OriginatorWalkMemo memo;
    EXPECT_TRUE(memo.ShouldWalk(true, 7));
    memo.NoteWalked(7);
    EXPECT_FALSE(memo.ShouldWalk(true, 7));
    EXPECT_FALSE(memo.ShouldWalk(true, 7));
    EXPECT_TRUE(memo.ShouldWalk(true, 8));
    memo.NoteWalked(8);
    EXPECT_FALSE(memo.ShouldWalk(true, 8));
}

TEST(OriginatorWalkMemoTest, ANewIdentityBankEndsTheMemoWithoutAModuleLoad) {
    OriginatorWalkMemo memo;
    memo.NoteWalked(7, 2);
    EXPECT_FALSE(memo.ShouldWalk(true, 7, 2));
    EXPECT_TRUE(memo.ShouldWalk(true, 7, 4));
}

TEST(OriginatorWalkMemoTest, ARuntimeAppearingEndsTheMemoOnTheNextPresent) {
    OriginatorWalkMemo memo;
    EXPECT_TRUE(memo.ShouldWalk(true, 7));
    memo.NoteWalked(7);
    EXPECT_FALSE(memo.ShouldWalk(true, 7));
    EXPECT_TRUE(memo.ShouldWalk(false, 7));
}

TEST(OriginatorWalkMemoTest, ThePeriodicWalkKeepsTheLoggedModuleNamesCurrent) {
    OriginatorWalkMemo memo;
    EXPECT_TRUE(memo.ShouldWalk(true, 7));
    memo.NoteWalked(7);
    int walks = 0;
    const int presents = static_cast<int>(OriginatorWalkMemo::kPresentsBetweenWalks) * 4;
    for (int present = 0; present < presents; ++present) {
        if (memo.ShouldWalk(true, 7)) {
            ++walks;
            memo.NoteWalked(7);
        }
    }
    EXPECT_EQ(walks, 4);
}

TEST(WrapperOwnedPresentTest, OnlyAWrapperPresentInAProcessWithNothingElseSkipsTheRouting) {
    using ce::present_originator::WrapperOwnedPresentNeedsNoRouting;
    EXPECT_TRUE(WrapperOwnedPresentNeedsNoRouting(true, true, false));
    EXPECT_FALSE(WrapperOwnedPresentNeedsNoRouting(false, true, false)) << "CE processes this one itself";
    EXPECT_FALSE(WrapperOwnedPresentNeedsNoRouting(true, false, false)) << "a runtime may need the route";
    EXPECT_FALSE(WrapperOwnedPresentNeedsNoRouting(true, true, true)) << "a foreign chain may need the bypass";
}

// The route out sits before the context capture it saves, and after what every Present still has to do.
TEST(WrapperOwnedPresentTest, TheDetoursLeaveBeforeTheContextCaptureAndKeepTheLimits) {
    namespace fs = std::filesystem;
    const std::string present =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present.cpp");
    const size_t scope = present.find("DX12_BeginOverlayPresentScope(pSwapChain);");
    const size_t decision = present.find("WrapperOwnedPresentNeedsNoRouting(");
    const size_t origin = present.find("NotePresentOrigin(pSwapChain, false,", decision);
    const size_t exitRoute = present.find("return ExecuteWrapperOwnedPresent(pSwapChain", decision);
    const size_t capture = present.find("PresentCallContext ctx = CapturePresentCallContext(");
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(origin, std::string::npos);
    ASSERT_NE(exitRoute, std::string::npos);
    ASSERT_NE(capture, std::string::npos);
    EXPECT_LT(scope, decision);
    EXPECT_LT(origin, exitRoute);
    EXPECT_LT(exitRoute, capture);

    const std::string routing =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "present" / "dxgi_shared_present_routing.cpp");
    const size_t common = routing.find("HRESULT ExecuteCommonPresentRouting(");
    ASSERT_NE(common, std::string::npos);
    const size_t latency = routing.find("ApplyPresentFrameLatencyOverrides(pSwapChain);", common);
    const size_t limit = routing.find("ApplyPrerenderLimit(pSwapChain", common);
    const size_t flush = routing.find("InvokeDX12FlushDeferredSignal();", common);
    const size_t postPresent = routing.find("g_SharedFpsLimiter.ApplyPostPresent();", common);
    EXPECT_NE(latency, std::string::npos);
    EXPECT_NE(limit, std::string::npos);
    EXPECT_NE(flush, std::string::npos);
    EXPECT_NE(postPresent, std::string::npos);
    const size_t wrapperRoute = routing.find("HRESULT ExecuteWrapperOwnedPresent(");
    ASSERT_NE(wrapperRoute, std::string::npos);
    EXPECT_NE(routing.find("PresentInFlightScope presentInFlight;", wrapperRoute), std::string::npos);
    EXPECT_NE(routing.find("ExecuteCommonPresentRouting(", wrapperRoute), std::string::npos);
}

TEST(SwapchainProbeMemoTest, OnlyTheSameObjectWithTheSameTableIsAlreadyProved) {
    int objectA = 0;
    int objectB = 0;
    int tableA = 0;
    int tableB = 0;
    ce::present_probe::SwapchainProbeMemo memo;
    EXPECT_FALSE(memo.HoldsObject(&objectA));
    EXPECT_FALSE(memo.Holds(&objectA, &tableA));
    memo.Remember(&objectA, &tableA);
    EXPECT_TRUE(memo.HoldsObject(&objectA));
    EXPECT_TRUE(memo.Holds(&objectA, &tableA));
    EXPECT_FALSE(memo.Holds(&objectA, &tableB)) << "a new chain at a dead one's address brings its own table";
    EXPECT_FALSE(memo.HoldsObject(&objectB));
    EXPECT_FALSE(memo.Holds(&objectB, &tableA));
    EXPECT_FALSE(memo.HoldsObject(nullptr));
    EXPECT_FALSE(memo.Holds(&objectA, nullptr));
}

// A Present route that asked the kernel twice per frame spent up to 131 us on it (20261104_210842).
TEST(SwapchainProbeMemoTest, AProbeAsksTheKernelOnlyAboutWhatItHasNotProvedYet) {
    void* tableA[2] = {};
    void* tableB[2] = {};
    void** swapchainA = tableA;
    void** swapchainB = tableA;
    const void* const addressA = static_cast<const void*>(&swapchainA);
    const void* const addressB = static_cast<const void*>(&swapchainB);
    int asked = 0;
    bool readable = true;
    const auto isReadable = [&](const void*, size_t) {
        ++asked;
        return readable;
    };
    ce::present_probe::SwapchainProbeMemo memo;

    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(addressA, sizeof(tableA), memo, isReadable), tableA);
    EXPECT_EQ(asked, 2) << "the first sight asks about the object and its table";
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(addressA, sizeof(tableA), memo, isReadable), tableA);
    EXPECT_EQ(asked, 2) << "every Present after that asks nothing";

    swapchainA = tableB;
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(addressA, sizeof(tableB), memo, isReadable), tableB);
    EXPECT_EQ(asked, 3) << "a new table at the same address is proved";

    asked = 0;
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(addressB, sizeof(tableA), memo, isReadable), tableA);
    EXPECT_EQ(asked, 2) << "another object starts over";

    ce::present_probe::SwapchainProbeMemo unreadable;
    readable = false;
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(addressA, sizeof(tableB), unreadable, isReadable), nullptr);
    EXPECT_FALSE(unreadable.HoldsObject(addressA)) << "nothing unproved is remembered";
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(nullptr, sizeof(tableB), unreadable, isReadable), nullptr);
    void** noTable = nullptr;
    readable = true;
    EXPECT_EQ(ce::present_probe::ProbeSwapchainTable(static_cast<const void*>(&noTable), sizeof(tableB), unreadable,
                                                  isReadable), nullptr);
}

}  // namespace

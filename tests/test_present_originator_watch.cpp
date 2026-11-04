// When the originator's stack walk may be skipped (hook/present/present_originator_policy.h). Every session of
// 20261104 ran below a foreign chain, so each Present walked 16 frames and scanned their paths for runtimes the
// process did not have. Skipping it is only sound while no runtime can be on the stack: these tests pin that.

#include <gtest/gtest.h>

#include "hook/overlay/overlay_compat.h"
#include "hook/present/present_originator_policy.h"

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

}  // namespace

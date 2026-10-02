#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "../hook/common/streamline_runtime_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProof;
using ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold;
using ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof;
using ce::streamline_runtime_policy::ShouldCountTitleFrameAsStartupProtectedActiveProof;
using ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof;

// Either clock alone satisfies the proof; they never add up (a GetState-polling title advances both
// once per frame, so summing would halve its protection).
TEST(StreamlineOffChurnProofTest, ActiveProofIsTheFurtherOfTheTwoClocks) {
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(0, 0), 0u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(1, 0), 1u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(1, 3), 3u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(3, 1), 3u);
    EXPECT_EQ(GetStartupProtectedOffChurnActiveProof(2, 2), 2u);
    EXPECT_FALSE(HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof(2, 2)));
}

TEST(StreamlineOffChurnProofTest, TitleFrameCountsOnlyWhileGeneratedThroughAndOncePerFrame) {
    EXPECT_TRUE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, true, true));
    // Nothing pending -> nothing to prove.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(false, true, true, true));
    // A re-marked present reuses its frame token.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, /*newTitleFrame=*/false, true, true));
    // FG not running, or PostSL not confirmed: the frame is no evidence that FG runs stably.
    EXPECT_FALSE(ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, /*streamlineFGRunning=*/false, true));
    EXPECT_FALSE(
        ShouldCountTitleFrameAsStartupProtectedActiveProof(true, true, true, /*postSLConfirmedRendering=*/false));
}

// Witcher 3 through the 2.x bridge, session 20261002_051703: the title calls SetOptions only on its
// ON/OFF edges and never polls GetState. An OFF 0.4 s after an ON was suppressed as startup churn;
// the next ON gave 1 of 3 active updates, and every later genuine OFF (the game menu) was suppressed
// and re-armed the latch, so DLSS-G stayed on in menus for the rest of the session.
TEST(StreamlineOffChurnProofTest, EdgeOnlyTitleReachesProofThroughItsFrames) {
    const uint32_t threshold = GetStartupProtectedOffChurnActiveProofUpdateThreshold();
    const bool churnObserved = true;
    const bool comebackProof = true;  // explicit SetOptions(ON) for this comeback
    const bool confirmed = true;
    const bool settling = false;

    // Before: one active update per comeback, no other clock.
    const uint32_t updatesOnly = GetStartupProtectedOffChurnActiveProof(1, 0);
    EXPECT_TRUE(ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(churnObserved, updatesOnly, comebackProof,
                                                                          confirmed, settling));

    // Now: the title's frames while DLSS-G generates satisfy the proof within a few frames.
    uint32_t frames = 0;
    for (uint32_t i = 0; i < threshold; ++i) {
        if (ShouldCountTitleFrameAsStartupProtectedActiveProof(churnObserved, true, true, confirmed)) {
            ++frames;
        }
    }
    const uint32_t proof = GetStartupProtectedOffChurnActiveProof(1, frames);
    EXPECT_TRUE(HasStartupProtectedOffChurnActiveProof(proof));
    EXPECT_FALSE(ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(churnObserved, proof, comebackProof,
                                                                           confirmed, settling));
}

// A GetState-polling title (the GTA startup-churn family) keeps the same pace: one stale OFF per frame
// resets both clocks, so neither reaches the threshold while the churn lasts.
TEST(StreamlineOffChurnProofTest, PerFrameChurnStillNeverReachesProof) {
    uint32_t updates = 0;
    uint32_t frames = 0;
    for (int frame = 0; frame < 100; ++frame) {
        ++updates;  // GetState(active)
        ++frames;   // present-start marker
        if (frame % 2 == 1) {
            updates = 0;  // stale GetState(OFF) -> MarkStartupProtectedOffChurnObserved
            frames = 0;
        }
        EXPECT_FALSE(HasStartupProtectedOffChurnActiveProof(GetStartupProtectedOffChurnActiveProof(updates, frames)));
    }
}

TEST(StreamlineOffChurnProofSourceTest, PresentStartMarkerFeedsTheProofAndEveryResetClearsBothClocks) {
    namespace fs = std::filesystem;
    const fs::path pcl = fs::current_path() / "hook" / "apis" / "streamline_hook_pcl.cpp";
    const fs::path startup = fs::current_path() / "hook" / "apis" / "streamline_hook_startup.cpp";
    ASSERT_TRUE(fs::exists(pcl));
    ASSERT_TRUE(fs::exists(startup));
    const std::string pclText = ce::test_source::ReadLogicalSource(pcl);
    const std::string startupText = ce::test_source::ReadLogicalSource(startup);

    const size_t hook = pclText.find("sl::Result Hooked_slPCLSetMarker(");
    ASSERT_NE(hook, std::string::npos);
    const size_t forward = pclText.find("original(marker, frame)", hook);
    const size_t presentStart = pclText.find("kPresentStartMarker)", forward);
    const size_t note = pclText.find("NoteStartupProtectedActiveTitleFrame(frameId)", forward);
    ASSERT_NE(forward, std::string::npos);
    ASSERT_NE(presentStart, std::string::npos);
    ASSERT_NE(note, std::string::npos);
    EXPECT_LT(presentStart, note) << "only the present-start marker is the per-frame clock";

    for (const char* fn : {"void ResetStartupProtectedOffChurnActiveProof(", "void MarkStartupProtectedOffChurnObserved("}) {
        const size_t begin = startupText.find(fn);
        ASSERT_NE(begin, std::string::npos) << fn;
        const size_t end = startupText.find("\n}\n", begin);
        ASSERT_NE(end, std::string::npos) << fn;
        const std::string body = startupText.substr(begin, end - begin);
        EXPECT_NE(body.find("streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0"), std::string::npos)
            << fn;
        EXPECT_NE(body.find("streamline_hook_g_StartupProtectedOffChurnActiveFrameCount.exchange(0"), std::string::npos)
            << fn;
    }
}

}  // namespace

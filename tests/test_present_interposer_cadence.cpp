// Regression tests for present-interposer output cadence
// (hook/common/present_interposer_cadence.h).
//
// Root cause these guard against: NVIDIA Smooth Motion status was inferred from command-list
// work populations and Present gap pairing measured on whatever present stream CE happened to
// process. The July 2026 validation runs only saw a 2x pattern because CE was processing
// NvPresent64's PRIVATE output chain — the same chain whose back buffers CE was compositing
// into, which removed the D3D12 device with DXGI_ERROR_ACCESS_DENIED in Strange Brigade DX12
// (session 20260914_102700). Once CE correctly moved onto the application's chain, that stream
// is 1x by construction and the heuristics can never fire again.
//
// The replacement is structural: the interposer presents its private chain once per frame it
// puts on screen, the application presents its proxy once per rendered frame, and the ratio IS
// the generation factor. Session 20260914_105853 shows exactly two private-chain presents per
// application present.

#include <gtest/gtest.h>

#include "../hook/common/present_interposer_cadence.h"

namespace {

using namespace ce::present_interposer;

TEST(PresentInterposerCadenceTest, TwoOutputPresentsPerApplicationPresentIsSmoothMotion2x) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(120, 240, 1000000);
    EXPECT_TRUE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 2);
    EXPECT_NEAR(verdict.applicationFps, 120.0f, 0.5f);
    EXPECT_NEAR(verdict.outputFps, 240.0f, 0.5f);
}

// The interposer is in the chain whenever the driver feature is installed. Forwarding one output
// per application frame is Smooth Motion loaded and NOT engaged, and must read as no frame
// generation rather than as a 1x generator.
TEST(PresentInterposerCadenceTest, OneToOneForwardingIsNotFrameGeneration) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(120, 120, 1000000);
    EXPECT_FALSE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 1);
    // The measured rates are still published: they are what the overlay shows as the base rate.
    EXPECT_NEAR(verdict.applicationFps, 120.0f, 0.5f);
}

// A window that straddles a present burst or a dropped output must not publish a factor.
TEST(PresentInterposerCadenceTest, ShortWindowsProduceNoVerdict) {
    const CadenceVerdict verdict = ClassifyInterposerCadence(kMinApplicationPresentsForVerdict - 1, 200, 1000000);
    EXPECT_FALSE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 1);
    EXPECT_EQ(verdict.outputFps, 0.0f);
}

TEST(PresentInterposerCadenceTest, RatioRoundsToTheNearestWholeFactorAndIsClamped) {
    // 1.5x is the lowest ratio that counts as generating, and it rounds up to 2x: a generator
    // that drops some of its output is still a generator.
    EXPECT_TRUE(IsInterposerGeneratingFrames(100, 150));
    EXPECT_FALSE(IsInterposerGeneratingFrames(100, 149));
    EXPECT_EQ(ResolveInterposerMultiplier(100, 150), 2);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 240), 2);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 260), 3);
    EXPECT_EQ(ResolveInterposerMultiplier(100, 400), 4);
    // A miscounted window must not publish an absurd factor into the overlay.
    EXPECT_EQ(ResolveInterposerMultiplier(100, 5000), kMaxInterposerMultiplier);
}

TEST(PresentInterposerCadenceTest, TrackerClosesAWindowOnTheApplicationPresentThatCrossesIt) {
    CadenceTracker tracker;
    CadenceVerdict verdict;

    // The first application present only opens the window.
    EXPECT_FALSE(tracker.NoteApplicationPresent(0, &verdict));

    // One application present, two interposer output presents, for a full second.
    for (int i = 0; i < 200; ++i) {
        tracker.NoteOutputPresent();
        tracker.NoteOutputPresent();
        const int64_t nowUs = static_cast<int64_t>(i + 1) * 8333;
        if (tracker.NoteApplicationPresent(nowUs, &verdict)) {
            EXPECT_TRUE(verdict.generating);
            EXPECT_EQ(verdict.multiplier, 2);
            return;
        }
    }
    FAIL() << "a one-second window never closed";
}

TEST(PresentInterposerCadenceTest, ResetDiscardsAPartialWindow) {
    CadenceTracker tracker;
    CadenceVerdict verdict;
    EXPECT_FALSE(tracker.NoteApplicationPresent(0, &verdict));
    for (int i = 0; i < 60; ++i) {
        tracker.NoteOutputPresent();
        tracker.NoteOutputPresent();
        EXPECT_FALSE(tracker.NoteApplicationPresent(static_cast<int64_t>(i + 1) * 8333, &verdict));
    }

    // A retired swapchain leaves counts that no longer describe anything.
    tracker.Reset();
    EXPECT_FALSE(tracker.NoteApplicationPresent(2000000, &verdict));
    // Only presents recorded after the reset may close the next window.
    for (int i = 0; i < kMinApplicationPresentsForVerdict - 2; ++i) {
        EXPECT_FALSE(tracker.NoteApplicationPresent(2000000 + static_cast<int64_t>(i + 1) * 8333, &verdict));
    }
    const bool closed = tracker.NoteApplicationPresent(3100000, &verdict);
    EXPECT_TRUE(closed);
    EXPECT_FALSE(verdict.generating) << "no output presents were recorded after the reset";
}

}  // namespace

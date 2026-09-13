#include "test_fps_limiter_shared.h"

#include <algorithm>

// Front-loaded cadence placement. The limiter's deadline decides when a frame
// is PRESENTED; it does not have to decide when the game may BUILD it.
//
// Strange Brigade DX12, session installed/captureengine/logs/20260913_124032:
// under a 90 fps cap the game built a frame in a median 1.8 ms (stddev 0.15 ms)
// and then sat in CE's present hook for a median 9.3 ms of every 11.1 ms
// period. The overlay's PC-latency chain measured that shape directly -
// anchorToPresent 20.5 ms against a 0.4 ms present-to-display - while a
// front-edge limiter in the same scene read 11.2 ms / 7.5 ms and published
// 24.2 ms against CE's 26.4 ms.

TEST(FpsLimiterPolicyTest, FrameWorkBudgetReservesTheWholePeriodUntilItIsMeasurable) {
    using ce::fps_limiter_policy::ResolveFrameWorkBudgetUs;

    // Too few samples: reserve the whole period, which is the original
    // back-edge placement expressed as a budget.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(1800, 200, 11111, 4, 16), 11111);
    // A nonsensical ceiling behaves the same way.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(-1, 200, 11111, 64, 16), 11111);
    // A ceiling that rounds to zero is a real measurement of a frame cheaper
    // than the timer margin, so the margin alone is the reservation.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(0, 200, 11111, 64, 16), 200);
    // A degenerate interval cannot produce a release target at all.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(1800, 200, 0, 64, 16), 0);
}

TEST(FpsLimiterPolicyTest, FrameWorkBudgetIsTheObservedCeilingPlusTheTimerMargin) {
    using ce::fps_limiter_policy::ResolveFrameWorkBudgetUs;

    // The measured high-water plus the adaptive timer margin, so the frame is
    // released exactly late enough to still make its deadline.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(3100, 250, 11111, 64, 16), 3350);
    // Work that no longer fits inside the period falls back to the whole
    // period rather than producing a release that is already late.
    EXPECT_EQ(ResolveFrameWorkBudgetUs(11000, 250, 11111, 64, 16), 11111);
    EXPECT_EQ(ResolveFrameWorkBudgetUs(20000, 250, 11111, 64, 16), 11111);
}

TEST(FpsLimiterPolicyTest, FrontLoadingRequiresAPeriodTheLimiterFullyOwns) {
    using ce::fps_limiter_policy::ShouldFrontLoadCadenceWait;

    EXPECT_TRUE(ShouldFrontLoadCadenceWait(/*gatedOnCadenceGrid=*/true, /*callSiteRunsPostPresentCadence=*/true,
                                           /*frameGenerationActive=*/false,
                                           /*explicitPostPresentCadencePending=*/false));
    // A duplicate-prone site can deliver more than one entry per frame, so a
    // release cannot be attributed to one present.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(false, true, false, false));
    // A site that never runs the post-present half would arm a release nothing
    // consumes.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, false, false, false));
    // Under frame generation the present stream is not CE's to re-phase, and
    // blocking after a runtime-owned present is the FFX freeze class.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, true, true, false));
    // An explicit Reflex/native post-present cadence already owns the slot.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, true, false, true));
}

TEST_F(FpsLimiterTest, UniquePresentSiteMovesTheWaitAheadOfTheFrameItPresents) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }

    const auto state = limiter.GetFrontLoadedPacingState();
    EXPECT_GT(state.releases, 0u) << "the post-present half must have run";
    EXPECT_GT(state.workSamples, 0u);
    EXPECT_GT(state.intervalUs, 0);
    EXPECT_GT(state.budgetUs, 0);
    // The budget reserves only the measured frame work, so most of the period
    // is now spent before the frame is built rather than after it.
    EXPECT_LT(state.budgetUs, state.intervalUs)
        << "a budget of a whole interval is the back-edge placement, not a front-loaded one";
    EXPECT_GE(state.budgetUs, state.workCeilingUs);
}

// The release is a latency control, never a rate one: the deadline and the
// pre-present wait own the cap, so a call site that never runs the
// post-present half must still be capped exactly.
TEST_F(FpsLimiterTest, SkippedPostPresentReleaseStillHoldsTheCap) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(true, kUniquePresentSite);

    constexpr int kFrames = 24;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < kFrames; ++i) {
        // Deliberately no ApplyPostPresent(): the armed release is dropped.
        limiter.Apply(true, kUniquePresentSite);
    }
    QueryPerformanceCounter(&end);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    const double idealMs = kFrames * 1000.0 / 120.0;
    EXPECT_GE(elapsedMs, idealMs * 0.9) << "dropping the release must never lift the cap";
    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

TEST_F(FpsLimiterTest, DuplicateProneSiteNeverArmsAFrontLoadedRelease) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        limiter.Apply(true, kDuplicateProneSite);
        limiter.ApplyPostPresent();
    }

    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

// Frame generation disqualifies the placement for the same reason it
// disqualifies the strict grid on a unique-application-present site.
TEST_F(FpsLimiterTest, FrameGenerationKeepsTheBackEdgePlacement) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();

    for (int i = 0; i < 48; ++i) {
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }
    const auto state = limiter.GetFrontLoadedPacingState();

    g_FGCompat.SetDLSSFGActive(false);

    EXPECT_EQ(state.releases, 0u);
}

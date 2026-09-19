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
                                           /*explicitPostPresentCadencePending=*/false,
                                           /*usingCaptureSync=*/false));
    // A duplicate-prone site can deliver more than one entry per frame, so a
    // release cannot be attributed to one present.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(false, true, false, false, false));
    // A site that never runs the post-present half would arm a release nothing
    // consumes.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, false, false, false, false));
    // Under frame generation the present stream is not CE's to re-phase, and
    // blocking after a runtime-owned present is the FFX freeze class.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, true, true, false, false));
    // An explicit Reflex/native post-present cadence already owns the slot.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, true, false, true, false));
    // Capture sync: a missed deadline skips whole CFR grid slots, so the
    // capture grid outranks input latency while a recording is the product.
    EXPECT_FALSE(ShouldFrontLoadCadenceWait(true, true, false, false, true));
}

TEST(FpsLimiterPolicyTest, FrontLoadHeadroomGrowsOnlyOnRealOverruns) {
    using ce::fps_limiter_policy::GrowFrontLoadHeadroomUs;

    constexpr int64_t kIntervalUs = 11111;
    // A present that missed its deadline by less than an interval is a budget
    // overrun: reserve exactly what it cost.
    EXPECT_EQ(GrowFrontLoadHeadroomUs(0, 455, kIntervalUs), 455);
    // The reservation only ever grows towards the worst overrun seen.
    EXPECT_EQ(GrowFrontLoadHeadroomUs(455, 24, kIntervalUs), 455);
    EXPECT_EQ(GrowFrontLoadHeadroomUs(455, 900, kIntervalUs), 900);
    // On-time frames leave it alone.
    EXPECT_EQ(GrowFrontLoadHeadroomUs(455, 0, kIntervalUs), 455);
    // A whole interval or more is a hitch the frame could never have been
    // started early enough for; reserving for it would park the placement at
    // the back edge forever.
    EXPECT_EQ(GrowFrontLoadHeadroomUs(455, kIntervalUs, kIntervalUs), 455);
    EXPECT_EQ(GrowFrontLoadHeadroomUs(455, 717844, kIntervalUs), 455);
}

TEST(FpsLimiterPolicyTest, FrontLoadHeadroomDecaysBackToZero) {
    using ce::fps_limiter_policy::DecayFrontLoadHeadroomUs;

    EXPECT_EQ(DecayFrontLoadHeadroomUs(0), 0);
    EXPECT_EQ(DecayFrontLoadHeadroomUs(800), 700);
    // Integer division must still reach zero rather than asymptote, so a
    // one-off overrun cannot hold the reservation for the rest of the session.
    int64_t headroom = 455;
    for (int i = 0; i < 200 && headroom > 0; ++i) {
        const int64_t next = DecayFrontLoadHeadroomUs(headroom);
        ASSERT_LT(next, headroom);
        headroom = next;
    }
    EXPECT_EQ(headroom, 0);
}

TEST_F(FpsLimiterTest, UniquePresentSiteMovesTheWaitAheadOfTheFrameItPresents) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        // A present that is scanned out promptly: the frame's GPU work finished
        // before its deadline, so the release may move in.
        limiter.ObservePresentToDisplay(400);
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
        limiter.ObservePresentToDisplay(400);
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
        limiter.ObservePresentToDisplay(400);
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }
    const auto state = limiter.GetFrontLoadedPacingState();

    g_FGCompat.SetDLSSFGActive(false);

    EXPECT_EQ(state.releases, 0u);
}

// Capture sync owns a CFR grid, not merely a frequency. Removing the game's
// slack before the deadline raises the rate of presents that miss it, and a
// missed deadline there skips whole grid slots.
TEST_F(FpsLimiterTest, CaptureSyncKeepsTheBackEdgePlacement) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(120);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        limiter.ObservePresentToDisplay(400);
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }

    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

TEST(FpsLimiterPolicyTest, GpuExcessIsMeasuredAgainstTheIrreducibleFlipLatency) {
    using ce::fps_limiter_policy::ResolveFrontLoadGpuExcessUs;

    // A present scanned out at the floor: the grid, not the GPU, still decides
    // the screen time.
    EXPECT_EQ(ResolveFrontLoadGpuExcessUs(400, 400, 250), 0);
    // Within the timer margin is still the floor.
    EXPECT_EQ(ResolveFrontLoadGpuExcessUs(600, 400, 250), 0);
    // The Strange Brigade shape: 6.8 ms against a 0.4 ms floor is 6.4 ms of GPU
    // work that ran past the deadline.
    EXPECT_EQ(ResolveFrontLoadGpuExcessUs(6800, 400, 250), 6400);
    // Missing or nonsensical inputs must never move the reservation.
    EXPECT_EQ(ResolveFrontLoadGpuExcessUs(0, 400, 250), 0);
    EXPECT_EQ(ResolveFrontLoadGpuExcessUs(6800, -1, 250), 0);
}

TEST(FpsLimiterPolicyTest, GpuHeadroomWalksBackInByABoundedProbe) {
    using ce::fps_limiter_policy::DecayFrontLoadGpuHeadroomUs;

    EXPECT_EQ(DecayFrontLoadGpuHeadroomUs(0, 250), 0);
    // One timer margin per clean window, so discovering that a lighter scene no
    // longer needs the reservation costs at most that much straddle.
    EXPECT_EQ(DecayFrontLoadGpuHeadroomUs(6400, 250), 6150);
    // It reaches zero rather than asymptoting, and never goes negative.
    EXPECT_EQ(DecayFrontLoadGpuHeadroomUs(100, 250), 0);
    EXPECT_EQ(DecayFrontLoadGpuHeadroomUs(250, 250), 0);
}

TEST(FpsLimiterPolicyTest, FrontLoadingNeedsDisplayedTransitionEvidence) {
    using ce::fps_limiter_policy::HasUsableGpuCompletionEvidence;

    EXPECT_TRUE(HasUsableGpuCompletionEvidence(/*presentToDisplaySamples=*/16, /*minimumSamples=*/16, /*floorSeeded=*/true));
    EXPECT_FALSE(HasUsableGpuCompletionEvidence(15, 16, true));
    // A floor that was never seeded at the back edge is not a floor: it would
    // measure a frame CE released too late, not the irreducible flip latency.
    EXPECT_FALSE(HasUsableGpuCompletionEvidence(64, 16, false));
}

// Without displayed-transition evidence there is no way to tell whether
// releasing the game later pushes its GPU work past the deadline, and a budget
// below the frame's whole CPU+GPU time buys no latency at all - so the back
// edge stays the default rather than a guess.
TEST_F(FpsLimiterTest, NoDisplayedTransitionEvidenceKeepsTheBackEdgePlacement) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }

    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

// Strange Brigade DX12 is GPU-bound: a 1.8 ms CPU frame in front of ~8.5 ms of
// GPU work. A CPU-sized budget released the game far too late, the GPU ran past
// the deadline, and present-to-display rose 0.4 -> 6.8 ms - which put the game's
// own variance on the screen timeline the overlay measures its percentiles
// from. The reservation must grow until the frame is finished by its deadline.
TEST_F(FpsLimiterTest, GpuWorkRunningPastTheDeadlineGrowsTheReservation) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(240);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    // State the CPU half rather than letting it be whatever wall-clock elapsed
    // between two Apply() calls. Strange Brigade's 1.8 ms CPU frame is the
    // point of the scenario, and on a loaded host the measured span is instead
    // a whole 4.17 ms interval - which saturates the budget at the back edge
    // during the "engaged" phase and leaves nothing for the second phase to
    // grow, the exact shape this test used to fail in.
    limiter.SetObservedFrameWorkOverrideUs(1800);

    // Seed the floor and engage the placement while the frame is finishing
    // early enough that the grid still decides the screen time.
    for (int i = 0; i < 96; ++i) {
        limiter.ObservePresentToDisplay(400);
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }
    const auto engaged = limiter.GetFrontLoadedPacingState();
    ASSERT_GT(engaged.releases, 0u);
    EXPECT_EQ(engaged.gpuHeadroomUs, 0);
    ASSERT_LT(engaged.budgetUs, engaged.intervalUs)
        << "the budget must still have room to grow, or the assertion below proves nothing";

    // Now the presents start waiting on GPU work that no longer fits.
    for (int i = 0; i < 192; ++i) {
        limiter.ObservePresentToDisplay(2400);
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }
    const auto grown = limiter.GetFrontLoadedPacingState();

    EXPECT_GT(grown.gpuHeadroomUs, 0) << "the reservation must cover the GPU half too";
    EXPECT_GT(grown.budgetUs, engaged.budgetUs);
    EXPECT_LE(grown.budgetUs, grown.intervalUs) << "it may saturate to the back edge, never past it";
}

// A wait shorter than the scheduler tick cannot be resolved by the kernel
// timer - it sleeps to the next tick, past the deadline. Front-loaded pacing
// made the pre-present wait hundreds of microseconds, where the measured
// overshoot went from a 37 us median on ~9 ms coarse waits to an 88 us median
// (561 us worst) on ~500 us ones. SmartWait must land those by yielding.
// Asserted structurally rather than by measuring how long the waits took. The
// claim is that SmartWait does not hand a sub-tick wait to the kernel timer,
// and which path it takes is SmartWait's own decision; how long the wait then
// lasts is the host scheduler's answer and moves with system load. The measured
// form of this test failed on an otherwise healthy tree whenever the machine
// was busy - see the sibling test below for the supra-tick half of the same
// contract.
TEST_F(FpsLimiterTest, SubTickWaitsLandWithoutTheKernelTimer) {
    limiter.ResetSmartWaitCounters();

    for (int i = 0; i < 15; ++i) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const int64_t targetUs = 400;
        const int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

        ASSERT_TRUE(limiter.SmartWait(targetTicks));

        // Never early: the deadline is the contract, and it holds under any
        // load. An overshoot bound would not.
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        EXPECT_GE(end.QuadPart, targetTicks);
    }

    EXPECT_EQ(limiter.GetSmartWaitCount(), 15u);
    EXPECT_EQ(limiter.GetKernelTimerWaitCount(), 0u)
        << "a wait shorter than the scheduler tick cannot land inside it; the timer sleeps past the deadline";
}

// The other half of the same contract: a wait with room for the timer does use
// it, rather than burning the whole interval in the yield/spin loop. Without
// this, the test above would still pass if SmartWait stopped using the kernel
// timer altogether.
TEST_F(FpsLimiterTest, SupraTickWaitsStillUseTheKernelTimer) {
    limiter.ResetSmartWaitCounters();

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const int64_t targetUs = 9000;
    const int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

    ASSERT_TRUE(limiter.SmartWait(targetTicks));

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    EXPECT_GE(end.QuadPart, targetTicks);

    EXPECT_EQ(limiter.GetSmartWaitCount(), 1u);
    EXPECT_EQ(limiter.GetKernelTimerWaitCount(), 1u);
}

#include "test_fps_limiter_shared.h"

#include <algorithm>
#include <string>

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

// The frame work the front-loaded tests simulate: the game builds each frame for
// 1 ms of virtual time after the limiter releases it. On the real clock that span
// was whatever the host scheduler did between two calls, and a loaded machine
// reported a whole interval of "work" - the budget saturated and
// UniquePresentSiteMovesTheWaitAheadOfTheFrameItPresents failed under load.
constexpr int64_t kGameWorkUs = 1000;

TEST_F(FpsLimiterTest, UniquePresentSiteMovesTheWaitAheadOfTheFrameItPresents) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        clock.Advance(TicksFromUs(kGameWorkUs));
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
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(true, kUniquePresentSite);

    constexpr int kFrames = 24;
    const int64_t elapsedTicks = VirtualTicksOf([&] {
        for (int i = 0; i < kFrames; ++i) {
            // Deliberately no ApplyPostPresent(): the armed release is dropped.
            limiter.Apply(true, kUniquePresentSite);
        }
    });

    // No time passes outside the limiter's own waits, so the cap is exact:
    // kFrames whole intervals, each within one tick of rational remainder.
    EXPECT_GE(elapsedTicks, kFrames * (FirstIntervalTicks(120) - 1))
        << "dropping the release must never lift the cap";
    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

TEST_F(FpsLimiterTest, DuplicateProneSiteNeverArmsAFrontLoadedRelease) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        clock.Advance(TicksFromUs(kGameWorkUs));
        limiter.ObservePresentToDisplay(400);
        limiter.Apply(true, kDuplicateProneSite);
        limiter.ApplyPostPresent();
    }

    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

// Frame generation disqualifies the placement for the same reason it
// disqualifies the strict grid on a unique-application-present site.
TEST_F(FpsLimiterTest, FrameGenerationKeepsTheBackEdgePlacement) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();

    for (int i = 0; i < 48; ++i) {
        clock.Advance(TicksFromUs(kGameWorkUs));
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
    UseVirtualClock();
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(120);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        clock.Advance(TicksFromUs(kGameWorkUs));
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
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    for (int i = 0; i < 48; ++i) {
        clock.Advance(TicksFromUs(kGameWorkUs));
        limiter.Apply(true, kUniquePresentSite);
        limiter.ApplyPostPresent();
    }

    EXPECT_EQ(limiter.GetFrontLoadedPacingState().releases, 0u);
}

// A wait shorter than the scheduler tick cannot be resolved by the kernel
// timer - it sleeps to the next tick, past the deadline. Front-loaded pacing
// made the pre-present wait hundreds of microseconds, where the measured
// overshoot went from a 37 us median on ~9 ms coarse waits to an 88 us median
// (561 us worst) on ~500 us ones. SmartWait must land those by yielding.
//
// The path is chosen from the time remaining when SmartWait looks, which is a
// pure function, so it is pinned there exactly - both directions, so the
// sub-tick rule cannot be satisfied by abandoning the timer altogether.
TEST(FpsLimiterPolicyTest, SmartWaitPathIsChosenByRemainingTime) {
    using ce::fps_limiter_policy::kSmartWaitSchedulerTickUs;
    using ce::fps_limiter_policy::SmartWaitArmsKernelTimer;
    constexpr int64_t kMarginUs = 250;

    EXPECT_FALSE(SmartWaitArmsKernelTimer(400, kMarginUs, false)) << "sub-tick: yield and spin";
    EXPECT_FALSE(SmartWaitArmsKernelTimer(kMarginUs + kSmartWaitSchedulerTickUs, kMarginUs, false))
        << "exactly one tick after the margin still cannot land inside the tick";
    EXPECT_TRUE(SmartWaitArmsKernelTimer(kMarginUs + kSmartWaitSchedulerTickUs + 1, kMarginUs, false));
    EXPECT_TRUE(SmartWaitArmsKernelTimer(9000, kMarginUs, false)) << "supra-tick: use the timer";
    EXPECT_FALSE(SmartWaitArmsKernelTimer(9000, kMarginUs, true)) << "no timer available: poll";
}

// The real primitive, for what holds under any load. A sub-tick request can
// only shrink while the host stalls, never grow, so it can never reach the
// timer path - and no wait may return before its deadline.
TEST_F(FpsLimiterTest, SubTickWaitsLandWithoutTheKernelTimer) {
    limiter.ResetSmartWaitCounters();

    for (int i = 0; i < 15; ++i) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const int64_t targetTicks = start.QuadPart + (400 * freq.QuadPart / 1000000);

        limiter.SmartWait(targetTicks);

        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        EXPECT_GE(end.QuadPart, targetTicks);
    }

    EXPECT_EQ(limiter.GetKernelTimerWaitCount(), 0u)
        << "a wait shorter than the scheduler tick cannot land inside it; the timer sleeps past the deadline";
}

// A real supra-tick wait still lands on or after its deadline, whichever path
// the remaining time selected by the time SmartWait looked.
TEST_F(FpsLimiterTest, SupraTickWaitsNeverReturnEarly) {
    limiter.ResetSmartWaitCounters();

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const int64_t targetTicks = start.QuadPart + (9000 * freq.QuadPart / 1000000);

    limiter.SmartWait(targetTicks);

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    EXPECT_GE(end.QuadPart, targetTicks);
    EXPECT_LE(limiter.GetKernelTimerWaitCount(), limiter.GetSmartWaitCount());
    RecordProperty("kernelTimerWaits", std::to_string(limiter.GetKernelTimerWaitCount()));
}

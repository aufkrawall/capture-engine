#include "test_fps_limiter_shared.h"
#include "test_fps_limiter_sleep_mode_recorder.h"

#include <initializer_list>

// The Reflex warmup is judged per game frame, not per Apply() evaluation. Talos 20261019_204226, pre-FSR DLSS
// 4x: every output reached the DXGI hook as a standalone Streamline Present (site kUniqueApplicationPresent),
// flip-metered into a ~2.5 ms burst per 28.6 ms game frame. Output 1 saw the frame's new Sleep (warmup), the
// 2 ms duplicate window swallowed outputs 2-3, and output 4 saw no new Sleep: "stalled", timer fallback with a
// ~25 ms CE wait on Streamline's presenter thread, fresh streak re-based. That repeated every frame for 1.6 s.
namespace {

using ce::fps_limiter_policy::IsEvaluationCoveredByGameSleep;
using ce::fps_limiter_policy::PresentSite;
using ce::fps_limiter_policy::ResolveEvaluationsPerGameFrame;

TEST(ReflexWarmupPolicyTest, EvaluationsPerGameFrameFollowTheSite) {
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kUniqueApplicationPresent, true, 4), 4u)
        << "the DXGI application-present stream carries every Streamline output";
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kRuntimeOutputPresent, true, 2), 2u);
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kDuplicateProne, true, 3), 3u);
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kFinalOutputBoundary, true, 4), 1u)
        << "output-group admission already evaluates one output per group";
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kRuntimeInputPresent, true, 4), 1u);
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kUniqueApplicationPresent, false, 4), 1u)
        << "no frame generation producing: one Present per game frame";
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(PresentSite::kUniqueApplicationPresent, true, 0), 1u);
}

TEST(ReflexWarmupPolicyTest, OneSleepCoversItsFrameAndOneFrameOfPhase) {
    EXPECT_TRUE(IsEvaluationCoveredByGameSleep(true, 0, 1));
    EXPECT_FALSE(IsEvaluationCoveredByGameSleep(false, 1, 1))
        << "one evaluation per frame: the very next evaluation without a new Sleep is a stall, as before";
    EXPECT_TRUE(IsEvaluationCoveredByGameSleep(false, 6, 4))
        << "4x: up to three outputs of the Sleep's own frame plus three before the next Sleep may land";
    EXPECT_FALSE(IsEvaluationCoveredByGameSleep(false, 7, 4)) << "a whole game frame went by without its Sleep";
    EXPECT_TRUE(IsEvaluationCoveredByGameSleep(false, 2, 2));
    EXPECT_FALSE(IsEvaluationCoveredByGameSleep(false, 3, 2));
    EXPECT_FALSE(IsEvaluationCoveredByGameSleep(false, 1, 0)) << "an unknown site counts as one per frame";
}

TEST(ReflexWarmupPolicyTest, CoverageReplacesPerEvaluationProgressInTheDecision) {
    using ce::fps_limiter_policy::ResolveReflexPacingDecision;
    EXPECT_TRUE(ResolveReflexPacingDecision(false, true, true, true, true, 1, false).useGameSleepWarmup);
    EXPECT_FALSE(ResolveReflexPacingDecision(false, true, true, true, false, 1, false).useGameSleepWarmup);
    EXPECT_FALSE(ResolveReflexPacingDecision(false, true, true, false, true, 1, false).useGameSleepWarmup)
        << "coverage never outlives the recency grace";
}

TEST(ReflexWarmupPolicyTest, StallAndResumeLinesAreMeteredPerEpisode) {
    using fps_limiter_detail::ShouldLogReflexWarmupTransition;
    EXPECT_TRUE(ShouldLogReflexWarmupTransition(0)) << "a warmup without a stall logs its resume";
    EXPECT_TRUE(ShouldLogReflexWarmupTransition(1));
    EXPECT_TRUE(ShouldLogReflexWarmupTransition(3));
    EXPECT_FALSE(ShouldLogReflexWarmupTransition(4)) << "a flapping Sleep must not log three lines a frame";
    EXPECT_FALSE(ShouldLogReflexWarmupTransition(63));
    EXPECT_TRUE(ShouldLogReflexWarmupTransition(64));
}

class ReflexWarmupPerGameFrameTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
        ResetManualRearmSetSleepModeRecorder();
        g_ReflexLimiter.TestInstallSetSleepModeForUnitTest(&TestManualRearmSetSleepMode,
                                                           reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x6789)));
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(140);
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));
        g_FGCompat.SetDLSSFGMultiplier(4);
        g_FGCompat.SetDLSSFGActive(true);
        ConfirmDLSSFGPacing();  // the game's Sleep for the first frame
    }
    void TearDown() override {
        g_FGCompat.SetDLSSFGActive(false);
        g_ReflexLimiter.Shutdown();
        FpsLimiterTest::TearDown();
    }

    void Sleep() { g_ReflexLimiter.MarkGameSleep("unit-test-frame"); }

    // One output: Apply() on the given site, then the time until the next output.
    void Output(int64_t untilNextUs, PresentSite site = PresentSite::kUniqueApplicationPresent) {
        limiter.Apply(true, site);
        clock.Advance(TicksFromUs(untilNextUs));
    }

    // Talos's flip-metered 4x burst at a 35 fps base (140 cap): outputs at +0, +1.0, +1.4, +2.5 ms of a 28.57 ms
    // frame. The game's Sleep for the frame comes first.
    void TalosBurstFrame() {
        Sleep();
        for (const int64_t untilNextUs : {1000, 400, 1100, 28571 - 2500}) {
            Output(untilNextUs);
        }
    }
};

TEST_F(ReflexWarmupPerGameFrameTest, TalosBurstHandsPacingToTheDriverWithoutACeWait) {
    const int64_t ticks = VirtualTicksOf([&] {
        for (int frame = 0; frame < 6; ++frame) {
            TalosBurstFrame();
        }
    });
    EXPECT_EQ(ticks, TicksFromUs(28571) * 6)
        << "output 4 of a warmup frame is that frame's output, not a stalled Sleep: CE must not timer-wait on "
           "Streamline's presenter thread";
    EXPECT_FALSE(limiter.IsActivelyLimiting()) << "the driver owns pacing";
}

// Evenly spaced 4x outputs at a 35 fps base: 7.142 ms apart.
constexpr int64_t kOutputUs = 7142;

TEST_F(ReflexWarmupPerGameFrameTest, SleepThatStopsMidWarmupFallsBackWithinTheFirstFrameWithoutIt) {
    // Frame 1 had its Sleep (fresh 1 of the 3 the handoff needs).
    for (int output = 0; output < 4; ++output) {
        Output(kOutputUs);
        EXPECT_FALSE(limiter.IsActivelyLimiting()) << "frame 1 output " << output + 1;
    }
    // Frame 2 has none. Its first three outputs can still precede a Sleep that lands late among them.
    for (int output = 0; output < 3; ++output) {
        Output(kOutputUs);
        EXPECT_FALSE(limiter.IsActivelyLimiting()) << "frame 2 output " << output + 1;
    }
    ASSERT_TRUE(g_ReflexLimiter.HasRecentGameSleep(84))
        << "still inside the limiter's recency grace (3 frames at 35 fps): that alone would keep the warmup";
    Output(kOutputUs);
    EXPECT_TRUE(limiter.IsActivelyLimiting())
        << "the seventh evaluation without a Sleep proves a whole game frame without one: fallback resumes";
}

TEST_F(ReflexWarmupPerGameFrameTest, NextSleepLandingAnywhereAmongTheOutputsIsNoStall) {
    // The game thread's Sleep is asynchronous to the presenter: here the Sleep for frame 1 lands right after
    // frame 0's first output, the one for frame 2 only after frame 1's last. Six outputs pass between the two.
    const int64_t ticks = VirtualTicksOf([&] {
        Output(kOutputUs);  // frame 0 output 1: fresh 1
        Sleep();            // for frame 1
        for (int output = 0; output < 3; ++output) {
            Output(kOutputUs);  // frame 0 outputs 2-4: the first one sees fresh 2
        }
        for (int output = 0; output < 4; ++output) {
            Output(kOutputUs);  // frame 1
            EXPECT_FALSE(limiter.IsActivelyLimiting()) << "frame 1 output " << output + 1;
        }
        Sleep();            // for frame 2
        Output(kOutputUs);  // frame 2 output 1: fresh 3, the driver owns pacing
    });
    EXPECT_EQ(ticks, TicksFromUs(kOutputUs) * 9) << "no evaluation of the warmup was judged a stall";
    EXPECT_FALSE(limiter.IsActivelyLimiting());
}

TEST_F(ReflexWarmupPerGameFrameTest, OneEvaluationPerFrameStillFallsBackOnTheNextFrameWithoutSleep) {
    // Output-group admission evaluates one output per frame, so nothing changes there: frame 2's admitted output
    // without a new Sleep resumes the fallback at once.
    for (int output = 0; output < 4; ++output) {
        Output(kOutputUs, PresentSite::kFinalOutputBoundary);
    }
    EXPECT_FALSE(limiter.IsActivelyLimiting());
    Output(kOutputUs, PresentSite::kFinalOutputBoundary);
    EXPECT_TRUE(limiter.IsActivelyLimiting());
}

TEST_F(ReflexWarmupPerGameFrameTest, WithoutFrameGenerationTheNextPresentWithoutSleepFallsBack) {
    g_FGCompat.SetDLSSFGActive(false);
    Output(7142);
    EXPECT_FALSE(limiter.IsActivelyLimiting());
    Output(7142);
    EXPECT_TRUE(limiter.IsActivelyLimiting()) << "one Present per game frame: the original per-evaluation rule";
}

}  // namespace

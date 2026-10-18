#include "test_fps_limiter_shared.h"

// FpsLimiter::PreWaitSubmission: a present site's recorded-but-unsubmitted GPU work (a D3D10/11
// immediate context) goes to the GPU right before the cadence blocks, so the GPU renders the frame
// during CE's wait instead of after it. BioShock Infinite at a 140 fps cap (20261018_020443) showed
// what happens without it: the frame reached the GPU only at Present, its screen time followed its
// GPU time, and the render-ahead limit waited on every other frame.
namespace {

struct SubmissionRecorder {
    const VirtualLimiterClock* clock = nullptr;
    int calls = 0;
    int64_t lastCallTick = 0;

    FpsLimiter::PreWaitSubmission Submission() {
        FpsLimiter::PreWaitSubmission submission;
        submission.context = this;
        submission.submit = [](void* context) {
            auto* recorder = static_cast<SubmissionRecorder*>(context);
            ++recorder->calls;
            recorder->lastCallTick = recorder->clock->Now();
        };
        return submission;
    }
};

class FpsLimiterPreWaitSubmissionTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
        mockShm->runtimeState.isRecording = false;
        mockShm->runtimeState.captureRequested = false;
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(140);
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        recorder.clock = &clock;
    }

    SubmissionRecorder recorder;
};

TEST_F(FpsLimiterPreWaitSubmissionTest, SubmitsOnceJustBeforeTheCadenceBlocks) {
    const int64_t start = clock.Now();
    int64_t waited = 0;
    {
        const FpsLimiter::ScopedPreWaitSubmission armed(recorder.Submission());
        waited = VirtualTicksOf([&] { limiter.Apply(true, kUniquePresentSite); });
    }
    EXPECT_EQ(waited, FirstSlotTicks(140)) << "the submission does not change the deadline";
    EXPECT_EQ(recorder.calls, 1);
    EXPECT_EQ(recorder.lastCallTick, start) << "the frame went to the GPU before the wait, not after it";
    EXPECT_EQ(limiter.GetPreWaitSubmissionCount(), 1u);

    // Every frame that waits hands its own work over, once.
    for (int frame = 0; frame < 4; ++frame) {
        const FpsLimiter::ScopedPreWaitSubmission armed(recorder.Submission());
        const int64_t entry = clock.Now();
        // The rational grid alternates whole ticks around 1/140 s.
        EXPECT_NEAR(VirtualTicksOf([&] { limiter.Apply(true, kUniquePresentSite); }), FirstIntervalTicks(140), 1);
        EXPECT_EQ(recorder.lastCallTick, entry) << "frame " << frame;
    }
    EXPECT_EQ(recorder.calls, 5);
    EXPECT_EQ(limiter.GetPreWaitSubmissionCount(), 5u);
}

TEST_F(FpsLimiterPreWaitSubmissionTest, ALateFrameHasNothingToWaitForAndSubmitsNothing) {
    limiter.Apply(true, kUniquePresentSite);
    // The game took three intervals: the deadline has passed and the cadence re-bases without a wait.
    clock.Advance(FirstIntervalTicks(140) * 3);
    {
        const FpsLimiter::ScopedPreWaitSubmission armed(recorder.Submission());
        EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(true, kUniquePresentSite); }), 0);
    }
    EXPECT_EQ(recorder.calls, 0) << "Present submits the frame right away; an extra flush buys nothing";
    EXPECT_EQ(limiter.GetPreWaitSubmissionCount(), 0u);
}

TEST_F(FpsLimiterPreWaitSubmissionTest, AnInactiveLimiterNeverSubmits) {
    mockShm->fpsLimiter.SetGeneralEnabled(false);
    const FpsLimiter::ScopedPreWaitSubmission armed(recorder.Submission());
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(true, kUniquePresentSite); }), 0);
    EXPECT_FALSE(limiter.IsActivelyLimiting());
    EXPECT_EQ(recorder.calls, 0);
}

TEST_F(FpsLimiterPreWaitSubmissionTest, OnlyTheArmedCallSubmitsAndScopesNest) {
    {
        const FpsLimiter::ScopedPreWaitSubmission armed(recorder.Submission());
        limiter.Apply(true, kUniquePresentSite);
    }
    ASSERT_EQ(recorder.calls, 1);

    // Unarmed: the next wait happens without any submission (D3D12, Vulkan, OpenGL and D3D9 sites).
    EXPECT_NEAR(VirtualTicksOf([&] { limiter.Apply(true, kUniquePresentSite); }), FirstIntervalTicks(140), 1);
    EXPECT_EQ(recorder.calls, 1);

    // A nested scope (a wrapper present inside a hooked one) restores the outer arming on exit.
    SubmissionRecorder outer;
    outer.clock = &clock;
    const FpsLimiter::ScopedPreWaitSubmission outerArmed(outer.Submission());
    {
        const FpsLimiter::ScopedPreWaitSubmission innerArmed(recorder.Submission());
        limiter.Apply(true, kUniquePresentSite);
    }
    EXPECT_EQ(recorder.calls, 2);
    EXPECT_EQ(outer.calls, 0);
    limiter.Apply(true, kUniquePresentSite);
    EXPECT_EQ(outer.calls, 1);
}

}  // namespace

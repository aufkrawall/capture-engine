// CFR app-audio backlog drain target (ce::audio::TrailingPeakHold): the drain follows the
// live lead target's peak over its own compensation window instead of the jittery raw target.

#include "test_audio_sync_utils_shared.h"

namespace {

constexpr int64_t kRate = 48000;
constexpr int64_t kWindow = kRate * 10;  // the drain's compensation window
constexpr int64_t kMs = kRate / 1000;

ce::audio::CfrAppAudioBacklogDrainDecision Drain(int64_t backlogSamples, int64_t targetSamples) {
    return ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
        /*isCfr*/ true, /*isApp*/ true, /*forceDrain*/ false, /*settled*/ true, /*startupProtected*/ false,
        /*recovery*/ false, backlogSamples, targetSamples, /*minBuffer*/ kRate / 20, kWindow,
        /*maxPitchPercent*/ 0.5, /*slack*/ kRate / 50, /*deadband*/ kRate / 100);
}

}  // namespace

// Session 20260926_030958 (Heroes of the Storm, static/loading screens): the app buffer held a
// steady ~335 ms while the live target alternated between ~330 and ~270 ms every ~0.33 s. Each
// dip pushed the excess past the 20 ms slack and switched a 0.5% speed-up on for a moment:
// 715 on/off transitions in 4 minutes.
TEST(AudioSyncUtilsTest, DrainTargetPeakHoldStopsFlappingOnAJitteryTarget) {
    ce::audio::TrailingPeakHold hold;
    int rawTransitions = 0;
    int heldTransitions = 0;
    bool rawActive = false;
    bool heldActive = false;
    int64_t clock = 0;
    for (int step = 0; step < 720; ++step, clock += kRate / 3) {
        const int64_t target = (step % 2 == 0 ? 330 : 270) * kMs;
        const int64_t backlog = 335 * kMs;
        const bool raw = Drain(backlog, target).active;
        const bool held = Drain(backlog, hold.Observe(clock, target, kWindow)).active;
        rawTransitions += raw != rawActive ? 1 : 0;
        heldTransitions += held != heldActive ? 1 : 0;
        rawActive = raw;
        heldActive = held;
    }
    EXPECT_GT(rawTransitions, 700);  // the recorded failure
    EXPECT_EQ(heldTransitions, 0);
}

TEST(AudioSyncUtilsTest, DrainTargetPeakHoldStillDrainsAPersistentBacklog) {
    ce::audio::TrailingPeakHold hold;
    int64_t clock = 0;
    bool active = false;
    // A real backlog (e.g. after a stall) well above a steady target keeps draining.
    for (int step = 0; step < 30; ++step, clock += kRate / 3) {
        active = Drain(600 * kMs, hold.Observe(clock, 330 * kMs, kWindow)).active;
    }
    EXPECT_TRUE(active);
}

TEST(AudioSyncUtilsTest, DrainTargetPeakHoldReleasesALoweredTargetWithinOneWindow) {
    ce::audio::TrailingPeakHold hold;
    int64_t clock = 0;
    for (; clock < kWindow; clock += kRate / 3) {
        hold.Observe(clock, 330 * kMs, kWindow);
    }
    // The target drops for good: the old peak holds for at most one more window.
    int64_t held = 0;
    for (const int64_t end = clock + kWindow; clock <= end; clock += kRate / 3) {
        held = hold.Observe(clock, 270 * kMs, kWindow);
    }
    EXPECT_EQ(held, 270 * kMs);
}

TEST(AudioSyncUtilsTest, DrainTargetPeakHoldRestartsOnAClockRewind) {
    ce::audio::TrailingPeakHold hold;
    EXPECT_EQ(hold.Observe(kWindow * 5, 900 * kMs, kWindow), 900 * kMs);
    // A new recording or epoch restarts the source timeline: the old peak must not carry over.
    EXPECT_EQ(hold.Observe(0, 330 * kMs, kWindow), 330 * kMs);
    hold.Reset();
    EXPECT_EQ(hold.Observe(kRate, 300 * kMs, kWindow), 300 * kMs);
}

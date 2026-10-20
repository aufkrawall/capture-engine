// The HELPER event transitions (runtime/core/helper_health.h): one READY per episode, one LOST per loss, RECOVERED
// on return, and no event for a helper that was intentionally absent. Pure data; nothing runs.
#include <gtest/gtest.h>

#include "runtime/core/helper_health.h"

namespace {
using ce::engine::HelperHealth;
using ce::engine::StepHelper;

TEST(EngineRuntimeHelperHealthTest, TheFirstReadinessIsAnnouncedAsReady) {
    const auto step = StepHelper(HelperHealth::kDown, true, true);
    EXPECT_EQ(step.health, HelperHealth::kUp);
    EXPECT_EQ(step.event, CE_HELPER_STATE_READY);
}

TEST(EngineRuntimeHelperHealthTest, AStableReadyHelperIsSilent) {
    const auto step = StepHelper(HelperHealth::kUp, true, true);
    EXPECT_EQ(step.health, HelperHealth::kUp);
    EXPECT_EQ(step.event, 0);
}

TEST(EngineRuntimeHelperHealthTest, AReadyHelperThatVanishesWhileWantedIsLostOnce) {
    const auto lost = StepHelper(HelperHealth::kUp, false, true);
    EXPECT_EQ(lost.health, HelperHealth::kLost);
    EXPECT_EQ(lost.event, CE_HELPER_STATE_LOST);
    const auto stillLost = StepHelper(lost.health, false, true);
    EXPECT_EQ(stillLost.health, HelperHealth::kLost);
    EXPECT_EQ(stillLost.event, 0) << "one LOST per loss, however many ticks it lasts";
}

TEST(EngineRuntimeHelperHealthTest, ALostHelperThatReturnsIsRecovered) {
    const auto recovered = StepHelper(HelperHealth::kLost, true, true);
    EXPECT_EQ(recovered.health, HelperHealth::kUp);
    EXPECT_EQ(recovered.event, CE_HELPER_STATE_RECOVERED);
}

TEST(EngineRuntimeHelperHealthTest, AHelperThatIsNoLongerWantedEndsItsEpisodeWithoutALoss) {
    // A media child retired after its recording: present = false, so it is not wanted.
    const auto retired = StepHelper(HelperHealth::kUp, false, false);
    EXPECT_EQ(retired.health, HelperHealth::kDown);
    EXPECT_EQ(retired.event, 0);
    // The next recording brings it back: a new episode, announced as ready rather than recovered.
    const auto nextRecording = StepHelper(retired.health, true, true);
    EXPECT_EQ(nextRecording.health, HelperHealth::kUp);
    EXPECT_EQ(nextRecording.event, CE_HELPER_STATE_READY);
}

TEST(EngineRuntimeHelperHealthTest, ALostHelperSwitchedOffIsNotRecoveredLater) {
    const auto switchedOff = StepHelper(HelperHealth::kLost, false, false);
    EXPECT_EQ(switchedOff.health, HelperHealth::kDown);
    EXPECT_EQ(switchedOff.event, 0);
    const auto back = StepHelper(switchedOff.health, true, true);
    EXPECT_EQ(back.event, CE_HELPER_STATE_READY);
}

TEST(EngineRuntimeHelperHealthTest, AHelperThatIsNeverReadyAndNotWantedIsSilent) {
    const auto step = StepHelper(HelperHealth::kDown, false, false);
    EXPECT_EQ(step.health, HelperHealth::kDown);
    EXPECT_EQ(step.event, 0);
}

TEST(EngineRuntimeHelperHealthTest, AWantedHelperThatNeverBecameReadyIsNotReportedLost) {
    // A failed start is reported by the start command itself, not as a helper loss.
    const auto step = StepHelper(HelperHealth::kDown, false, true);
    EXPECT_EQ(step.health, HelperHealth::kDown);
    EXPECT_EQ(step.event, 0);
}
}  // namespace

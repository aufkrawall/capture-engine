// The per-route summary of how the overlay graph cursor moved (hook/overlay/graph_step_audit.h), as the
// [OverlayGraph] log line reports it.

#include <gtest/gtest.h>

#include <string>

#include "hook/overlay/graph_step_audit.h"

namespace {

constexpr double kSamplePeriodUs = 6944.0;

TEST(GraphStepAuditTest, DrawsOnTheirOwnClockAreNeitherHoldsNorJumps) {
    GraphStepAudit audit;
    double position = 10.0;
    int64_t drawUs = 1'000'000;
    for (int i = 0; i < 100; ++i) {
        const int64_t interval = i % 2 ? 6000 : 7000;
        drawUs += interval;
        position += interval / kSamplePeriodUs;
        audit.Observe(GraphOutputKind::kApplicationDraw, 0, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
    }
    EXPECT_EQ(audit.application().draws, 100u);
    EXPECT_EQ(audit.application().holds, 0u);
    EXPECT_EQ(audit.application().jumps, 0u);
    EXPECT_EQ(audit.output().draws, 0u);
}

// GTA's menu (session 20261112_154352): one draw at the same position, the next two slots on.
TEST(GraphStepAuditTest, AHoldAndItsCatchUpAreCounted) {
    GraphStepAudit audit;
    double position = 10.0;
    int64_t drawUs = 1'000'000;
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
    drawUs += 6944;
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
    drawUs += 6944;
    position += 2.0;
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
    EXPECT_EQ(audit.application().holds, 1u);
    EXPECT_EQ(audit.application().jumps, 1u);
    EXPECT_NEAR(audit.application().maxStep, 2.0, 1e-9);
}

// Runtime outputs of one burst arrive within microseconds but are displayed one refresh apart; a repeat of the same
// output must not move.
TEST(GraphStepAuditTest, RuntimeOutputsAreJudgedPerDisplayInterval) {
    GraphStepAudit audit;
    double position = 20.0;
    const int64_t drawUs = 2'000'000;
    for (uint64_t id = 1; id <= 4; ++id) {
        audit.Observe(GraphOutputKind::kDistinctOutput, id, position, drawUs + static_cast<int64_t>(id), 3472.0,
                      3472.0, false);
        audit.Observe(GraphOutputKind::kDistinctOutput, id, position, drawUs + static_cast<int64_t>(id), 3472.0,
                      3472.0, false);
        position += 1.0;
    }
    EXPECT_EQ(audit.output().draws, 8u);
    EXPECT_EQ(audit.output().holds, 0u);
    EXPECT_EQ(audit.output().jumps, 0u);
    EXPECT_EQ(audit.application().draws, 0u);
}

// An FG switch hands the cursor from runtime outputs to application draws and back (session 20261113_181813). Each
// route's first draw after the other drew is a handoff, not a step from the route's own last draw seconds earlier.
TEST(GraphStepAuditTest, ARouteResumingAfterTheOtherIsNotJudgedAgainstItsStalePosition) {
    GraphStepAudit audit;
    double position = 100.0;
    int64_t drawUs = 1'000'000;
    for (uint64_t id = 1; id <= 3; ++id) {
        audit.Observe(GraphOutputKind::kDistinctOutput, id, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
        position += 1.0;
        drawUs += 6944;
    }
    for (int i = 0; i < 500; ++i) {
        audit.Observe(GraphOutputKind::kApplicationDraw, 0, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
        position += 1.0;
        drawUs += 6944;
    }
    audit.Observe(GraphOutputKind::kDistinctOutput, 4, position, drawUs, kSamplePeriodUs, kSamplePeriodUs, false);
    audit.Observe(GraphOutputKind::kDistinctOutput, 5, position + 1.0, drawUs + 6944, kSamplePeriodUs,
                  kSamplePeriodUs, false);
    EXPECT_EQ(audit.output().jumps, 0u);
    EXPECT_NEAR(audit.output().maxStep, 1.0, 1e-9);
    EXPECT_EQ(audit.output().judged, 3u);  // two before the handoff, one after
    EXPECT_EQ(audit.application().judged, 499u);
}

TEST(GraphStepAuditTest, PresentationTimingExpectsOneSlotPerDrawAndRestartForgetsPositions) {
    GraphStepAudit audit;
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, 5.0, 100, 0.0, 0.0, true);
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, 6.0, 200, 0.0, 0.0, true);
    audit.Restart();
    audit.Observe(GraphOutputKind::kApplicationDraw, 0, 0.0, 300, 0.0, 0.0, true);
    EXPECT_EQ(audit.application().judged, 1u);
    EXPECT_EQ(audit.application().holds, 0u);
    char line[192] = {};
    audit.Format(line, sizeof(line));
    EXPECT_EQ(std::string(line), "app=3/0/0/0.00/1.00 out=0/0/0/0.00/0.00");
    audit.ResetCounters();
    EXPECT_EQ(audit.application().draws, 0u);
}

}  // namespace

#include <gtest/gtest.h>

#include "../common/pseudo_overlay_focus_grace.h"

namespace pofg = ce::pseudo_overlay;

// Helper: simulate a sequence of ticks by feeding the previous decision's graceActive
// back into the next call. Returns the final decision.
static pofg::FocusGraceDecision RunTick(pofg::FocusGraceDecision prev, uint64_t nowMs,
                                         uint64_t lastTick, uint32_t lastPid, uint32_t currPid,
                                         bool hadTarget, bool currTarget, uint32_t graceMs,
                                         bool recChanged) {
    return pofg::ComputeFocusGraceDecision(nowMs, lastTick, lastPid, currPid, hadTarget,
                                            currTarget, prev.graceActive, graceMs, recChanged);
}

TEST(PseudoOverlayFocusGraceTest, GraceDisabledNeverSuppresses) {
    // grace_ms == 0 -> never suppress, even on first focus detection or transition.
    auto d = pofg::ComputeFocusGraceDecision(/*now*/ 1000, /*lastTick*/ 0, /*lastPid*/ 0,
                                              /*currPid*/ 42, /*hadTarget*/ false,
                                              /*currTarget*/ true, /*prevActive*/ false,
                                              /*graceMs*/ 0, /*recChanged*/ false);
    EXPECT_FALSE(d.suppressVisibleOverlay);
    EXPECT_FALSE(d.graceActive);
    EXPECT_FALSE(d.justStartedGrace);
    EXPECT_FALSE(d.justEndedGrace);
    EXPECT_EQ(d.graceMs, 0u);
}

TEST(PseudoOverlayFocusGraceTest, FirstDetectionSuppressesUntilGrace) {
    constexpr uint32_t kGraceMs = 2000;
    // First ever detection at t=500 with the whitelisted PID 1234.
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 500, /*lastTick*/ 0, /*lastPid*/ 0,
                                              /*currPid*/ 1234, /*hadTarget*/ false,
                                              /*currTarget*/ true, /*prevActive*/ false,
                                              kGraceMs, false);
    EXPECT_TRUE(d0.justStartedGrace);
    EXPECT_TRUE(d0.suppressVisibleOverlay);

    // Same PID, still in grace at t=1000. Caller remembers d0.graceActive==true? No
    // (d0.graceActive is false on the transition tick). So pass prevActive=false.
    auto d1 = pofg::ComputeFocusGraceDecision(/*now*/ 1000, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 1234, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ false, kGraceMs,
                                              false);
    EXPECT_FALSE(d1.justStartedGrace);
    EXPECT_TRUE(d1.suppressVisibleOverlay);
    EXPECT_TRUE(d1.graceActive);

    // Next tick: now prevActive=true (we are in grace). 1500-500=1000 < 2000 -> still
    // in grace, still suppressing.
    auto d2 = pofg::ComputeFocusGraceDecision(/*now*/ 1500, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 1234, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ true, kGraceMs,
                                              false);
    EXPECT_TRUE(d2.suppressVisibleOverlay);
    EXPECT_TRUE(d2.graceActive);
    EXPECT_FALSE(d2.justStartedGrace);
    EXPECT_FALSE(d2.justEndedGrace);

    // Edge: t=2500 = 500 + 2000 -> elapsed == graceMs -> render. prevActive was true.
    auto d3 = pofg::ComputeFocusGraceDecision(/*now*/ 2500, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 1234, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ true, kGraceMs,
                                              false);
    EXPECT_FALSE(d3.suppressVisibleOverlay);
    EXPECT_TRUE(d3.justEndedGrace);
    EXPECT_FALSE(d3.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, MidSessionPidChangeReseatsGrace) {
    constexpr uint32_t kGraceMs = 2000;
    // Already tracking PID 1234 from t=500; focus stays on it through t=3000.
    // Now a different whitelisted PID 5678 acquires focus at t=4000.
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 4000, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 5678, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ false, kGraceMs,
                                              false);
    EXPECT_TRUE(d0.justStartedGrace);
    // Mid-session PID change: the transition tick renders (so the overlay repositions
    // onto the new monitor) but the *next* tick at t=4500 is in grace.
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_TRUE(d0.graceActive);

    // t=4500: 500ms into the new grace window -> suppress.
    auto d1 = RunTick(d0, 4500, 4000, 5678, 5678, true, true, kGraceMs, false);
    EXPECT_FALSE(d1.justStartedGrace);
    EXPECT_TRUE(d1.suppressVisibleOverlay);
    EXPECT_TRUE(d1.graceActive);

    // t=6000: 2000ms in -> render, prevActive was true so justEndedGrace.
    auto d2 = RunTick(d1, 6000, 4000, 5678, 5678, true, true, kGraceMs, false);
    EXPECT_FALSE(d2.suppressVisibleOverlay);
    EXPECT_TRUE(d2.justEndedGrace);
    EXPECT_FALSE(d2.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, FocusLostClearsGrace) {
    constexpr uint32_t kGraceMs = 2000;
    // After focus is lost, the helper must not suppress and must not consider itself
    // in grace. This is the path when the user Alt+Tabs AWAY from the game.
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 1000, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 0, /*hadTarget*/ true,
                                              /*currTarget*/ false, /*prevActive*/ false,
                                              kGraceMs, false);
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_FALSE(d0.graceActive);
    EXPECT_FALSE(d0.justStartedGrace);
    EXPECT_FALSE(d0.justEndedGrace);

    // Re-acquiring after focus loss is a fresh transition -> suppress.
    auto d1 = pofg::ComputeFocusGraceDecision(/*now*/ 3000, /*lastTick*/ 0, /*lastPid*/ 0,
                                              /*currPid*/ 1234, /*hadTarget*/ false,
                                              /*currTarget*/ true, /*prevActive*/ false, kGraceMs,
                                              false);
    EXPECT_TRUE(d1.suppressVisibleOverlay);
    EXPECT_TRUE(d1.justStartedGrace);
}

TEST(PseudoOverlayFocusGraceTest, RecordingStateChangeAbortsGrace) {
    constexpr uint32_t kGraceMs = 2000;
    // Whitelisted PID focused, 800ms into grace (still suppressing).
    // User toggles recording mid-grace -> helper must commit immediately.
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 1300, /*lastTick*/ 500, /*lastPid*/ 1234,
                                              /*currPid*/ 1234, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ true, kGraceMs,
                                              /*recChanged*/ true);
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_TRUE(d0.justEndedGrace);  // we were in grace and just committed
    EXPECT_FALSE(d0.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, RecordingChangeOutOfGraceDoesNotReportJustEnded) {
    constexpr uint32_t kGraceMs = 2000;
    // Long-stable recording, no grace active. A recording toggle outside grace should
    // not be reported as a "grace ended" event.
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 60000, /*lastTick*/ 500, /*lastPid*/ 42,
                                              /*currPid*/ 42, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ false, kGraceMs,
                                              /*recChanged*/ true);
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_FALSE(d0.justEndedGrace);
    EXPECT_FALSE(d0.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, GraceClampedToMaxTenSeconds) {
    // Larger configured grace is clamped to 10s for safety. 15000 -> 10000.
    auto d = pofg::ComputeFocusGraceDecision(/*now*/ 1000, /*lastTick*/ 500, /*lastPid*/ 42,
                                             /*currPid*/ 42, /*hadTarget*/ true,
                                             /*currTarget*/ true, /*prevActive*/ false,
                                             /*graceMs*/ 15000, false);
    EXPECT_EQ(d.graceMs, 10000u);
}

TEST(PseudoOverlayFocusGraceTest, GraceExpiredAfterEdgeRendersAndDoesNotResuppress) {
    constexpr uint32_t kGraceMs = 2000;
    // t=2501: just past grace -> render, mark ended. prevActive is true (we were
    // in grace last tick).
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 2501, /*lastTick*/ 500, /*lastPid*/ 42,
                                              /*currPid*/ 42, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ true, kGraceMs,
                                              false);
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_TRUE(d0.justEndedGrace);

    // t=5000: still past grace, same PID, no transition -> render, do NOT mark ended
    // again. The "ended" flag is for the single tick where the grace boundary is
    // crossed so the caller can log it once.
    auto d1 = RunTick(d0, 5000, 500, 42, 42, true, true, kGraceMs, false);
    EXPECT_FALSE(d1.suppressVisibleOverlay);
    EXPECT_FALSE(d1.justEndedGrace);
    EXPECT_FALSE(d1.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, ConvenienceWrapperMatchesDecisionField) {
    constexpr uint32_t kGraceMs = 2000;
    const bool wrapperRes = pofg::ShouldSuppressPseudoOverlayForFocusGrace(
        1000, 500, 42, 42, true, true, true, kGraceMs, false);
    const auto decision = pofg::ComputeFocusGraceDecision(1000, 500, 42, 42, true, true, true,
                                                         kGraceMs, false);
    EXPECT_EQ(wrapperRes, decision.suppressVisibleOverlay);
}

TEST(PseudoOverlayFocusGraceTest, NoTransitionNoGraceStillRenders) {
    // Steady state: same PID focused for a long time, no transition, grace is
    // technically still active if lastAcquireTick is recent, but once it has elapsed
    // the helper must say "not suppressing, not ended". This is the common case for
    // long recording sessions.
    constexpr uint32_t kGraceMs = 2000;
    auto d = pofg::ComputeFocusGraceDecision(/*now*/ 60000, /*lastTick*/ 500, /*lastPid*/ 42,
                                             /*currPid*/ 42, /*hadTarget*/ true,
                                             /*currTarget*/ true, /*prevActive*/ false, kGraceMs,
                                             false);
    EXPECT_FALSE(d.suppressVisibleOverlay);
    EXPECT_FALSE(d.justStartedGrace);
    EXPECT_FALSE(d.justEndedGrace);
    EXPECT_FALSE(d.graceActive);
}

TEST(PseudoOverlayFocusGraceTest, GraceBoundaryReachedExactlyMarksEnded) {
    // Boundary: elapsed == graceMs. We consider grace "elapsed" so the overlay renders,
    // and justEndedGrace is true on this exact tick.
    constexpr uint32_t kGraceMs = 2000;
    auto d0 = pofg::ComputeFocusGraceDecision(/*now*/ 2500, /*lastTick*/ 500, /*lastPid*/ 42,
                                              /*currPid*/ 42, /*hadTarget*/ true,
                                              /*currTarget*/ true, /*prevActive*/ true, kGraceMs,
                                              false);
    EXPECT_FALSE(d0.suppressVisibleOverlay);
    EXPECT_TRUE(d0.justEndedGrace);
}

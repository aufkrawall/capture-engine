#include <gtest/gtest.h>

#include "common/graphics/post_process_route_policy.h"

using namespace ce::post_process_route;

namespace {

NormalRouteInputs Skipping(SkipCause cause) {
    NormalRouteInputs in;
    in.skipOverlayDraw = true;
    in.cause = cause;
    return in;
}

}  // namespace

TEST(PostProcessRoutePolicy, NormalRouteRunsOnlyWhereTheRoutingAllowsCeGpuWork) {
    EXPECT_TRUE(NormalRouteMayPostProcess(NormalRouteInputs{}));
    for (SkipCause cause : {SkipCause::None, SkipCause::FocusLossHold, SkipCause::FgTransitionCooldown,
                            SkipCause::PostSlWarmup, SkipCause::PostSlRecent})
        EXPECT_FALSE(NormalRouteMayPostProcess(Skipping(cause))) << static_cast<int>(cause);
}

TEST(PostProcessRoutePolicy, ToggleOnKeepsTheFilterWhereTheOverlayItselfStaysOnTheGameQueue) {
    NormalRouteInputs in = Skipping(SkipCause::FgTransitionCooldown);
    in.preSlDrawKeptThroughToggleOn = true;
    EXPECT_TRUE(NormalRouteMayPostProcess(in));
    // A swapchain that cannot be presented takes no CE work, kept overlay or not.
    in.cause = SkipCause::FocusLossHold;
    EXPECT_FALSE(NormalRouteMayPostProcess(in));
}

TEST(PostProcessRoutePolicy, RuntimeOwnedRoutesNeverTakeTheSeparateGpuWork) {
    NormalRouteInputs in;
    in.separateGpuWorkBlocked = true;
    EXPECT_FALSE(NormalRouteMayPostProcess(in));
    in.skipOverlayDraw = true;
    in.preSlDrawKeptThroughToggleOn = true;
    EXPECT_FALSE(NormalRouteMayPostProcess(in));
    EXPECT_EQ(ClassifyNormalRouteSkip(in), Outcome::CoveredByRuntimeRoute);
}

TEST(PostProcessRoutePolicy, SkippedFramesAreClassifiedByCause) {
    EXPECT_EQ(ClassifyNormalRouteSkip(Skipping(SkipCause::PostSlRecent)), Outcome::CoveredByPostSl);
    EXPECT_EQ(ClassifyNormalRouteSkip(Skipping(SkipCause::PostSlWarmup)), Outcome::CoveredByPostSl);
    EXPECT_EQ(ClassifyNormalRouteSkip(Skipping(SkipCause::FocusLossHold)), Outcome::SkippedFocusLossHold);
    EXPECT_EQ(ClassifyNormalRouteSkip(Skipping(SkipCause::FgTransitionCooldown)), Outcome::SkippedFgTransition);
    EXPECT_EQ(ClassifyNormalRouteSkip(Skipping(SkipCause::None)), Outcome::SkippedUnclassified);
    EXPECT_FALSE(IsGap(Outcome::CoveredByPostSl));
    EXPECT_TRUE(IsGap(Outcome::SkippedFgTransition));
    EXPECT_TRUE(IsGap(Outcome::SkippedUnclassified));
}

TEST(PostProcessRoutePolicy, WithoutTheOverlayOnlyAQuiescentPlainStateRuns) {
    EXPECT_TRUE(MayPostProcessWithoutOverlay(OverlayUnavailableInputs{}));
    bool OverlayUnavailableInputs::*blockers[] = {
        &OverlayUnavailableInputs::focusLossHold,        &OverlayUnavailableInputs::deviceLost,
        &OverlayUnavailableInputs::insideExecuteCommandLists, &OverlayUnavailableInputs::fgTransitionCooldown,
        &OverlayUnavailableInputs::streamlineOffGrace,   &OverlayUnavailableInputs::streamlineFgRunning,
        &OverlayUnavailableInputs::fgActive,             &OverlayUnavailableInputs::runtimeOwnsSwapchain,
        &OverlayUnavailableInputs::postSlRouteActive,    &OverlayUnavailableInputs::postSlKeepAlive};
    for (size_t index = 0; index < sizeof(blockers) / sizeof(blockers[0]); ++index) {
        OverlayUnavailableInputs in;
        in.*blockers[index] = true;
        EXPECT_FALSE(MayPostProcessWithoutOverlay(in)) << "blocker " << index;
    }
}

TEST(PostProcessRoutePolicy, PassResultsMapToOutcomes) {
    EXPECT_EQ(FromPassResult(PassResult::Applied, false), Outcome::Applied);
    EXPECT_EQ(FromPassResult(PassResult::Applied, true), Outcome::AppliedOverlayUnavailable);
    EXPECT_EQ(FromPassResult(PassResult::AlreadyApplied, false), Outcome::AlreadyApplied);
    EXPECT_EQ(FromPassResult(PassResult::Failed, false), Outcome::Failed);
    EXPECT_EQ(FromPassResult(PassResult::Unavailable, true), Outcome::Failed);
    EXPECT_TRUE(IsGap(Outcome::Failed));
    EXPECT_FALSE(IsGap(Outcome::AlreadyApplied));
}

TEST(PostProcessFrameLedger, CorrectedFramesNeverLog) {
    FrameLedger ledger;
    for (int frame = 0; frame < 20000; ++frame) {
        const auto report = ledger.Note(frame % 3 == 0 ? Outcome::CoveredByPostSl : Outcome::Applied);
        EXPECT_FALSE(report.logRunEnd);
        EXPECT_FALSE(report.logHeartbeat);
        EXPECT_FALSE(report.logSummary);
    }
    EXPECT_EQ(ledger.GapFrames(), 0u);
    EXPECT_EQ(ledger.GapRuns(), 0u);
    EXPECT_EQ(ledger.TotalFrames(), 20000u);
}

TEST(PostProcessFrameLedger, ARunIsReportedOnceWhenItEndsWithItsLengthAndReason) {
    FrameLedger ledger;
    ledger.Note(Outcome::Applied);
    for (int frame = 0; frame < 7; ++frame) {
        const auto report = ledger.Note(Outcome::SkippedFgTransition);
        EXPECT_FALSE(report.logRunEnd);
        EXPECT_EQ(report.runFrames, static_cast<uint64_t>(frame + 1));
    }
    const auto ended = ledger.Note(Outcome::Applied);
    EXPECT_TRUE(ended.logRunEnd);
    EXPECT_EQ(ended.endedOutcome, Outcome::SkippedFgTransition);
    EXPECT_EQ(ended.endedFrames, 7u);
    EXPECT_EQ(ended.endedRunIndex, 1u);
    EXPECT_EQ(ledger.GapFrames(), 7u);
    EXPECT_EQ(ledger.GapRuns(), 1u);
    EXPECT_FALSE(ledger.Note(Outcome::Applied).logRunEnd);
}

TEST(PostProcessFrameLedger, ACoveredFrameEndsAGapRunBecauseTheFrameWasCorrected) {
    FrameLedger ledger;
    ledger.Note(Outcome::SkippedFgTransition);
    ledger.Note(Outcome::SkippedFgTransition);
    const auto ended = ledger.Note(Outcome::CoveredByPostSl);
    EXPECT_TRUE(ended.logRunEnd);
    EXPECT_EQ(ended.endedFrames, 2u);
}

TEST(PostProcessFrameLedger, ChangingTheReasonEndsTheRunAndStartsAnother) {
    FrameLedger ledger;
    ledger.Note(Outcome::SkippedFocusLossHold);
    const auto switched = ledger.Note(Outcome::SkippedFgTransition);
    EXPECT_TRUE(switched.logRunEnd);
    EXPECT_EQ(switched.endedOutcome, Outcome::SkippedFocusLossHold);
    EXPECT_EQ(switched.endedFrames, 1u);
    EXPECT_EQ(switched.runIndex, 2u);
    EXPECT_EQ(switched.runFrames, 1u);
}

TEST(PostProcessFrameLedger, OnlyTheFirstRunsThenEverySixtyFourthAreLogged) {
    FrameLedger ledger;
    uint32_t logged = 0;
    for (uint32_t run = 1; run <= 200; ++run) {
        ledger.Note(Outcome::SkippedUnclassified);
        const auto report = ledger.Note(Outcome::Applied);
        if (report.logRunEnd) {
            ++logged;
            EXPECT_TRUE(run <= FrameLedger::kLoggedRunBurst || run % FrameLedger::kLoggedRunStride == 0) << run;
        }
    }
    EXPECT_EQ(logged, FrameLedger::kLoggedRunBurst + 200u / FrameLedger::kLoggedRunStride -
                          FrameLedger::kLoggedRunBurst / FrameLedger::kLoggedRunStride);
}

TEST(PostProcessFrameLedger, AnEndlessRunReportsProgressInsteadOfStayingSilent) {
    FrameLedger ledger;
    uint32_t heartbeats = 0;
    for (uint64_t frame = 1; frame <= FrameLedger::kHeartbeatFrames * 3; ++frame) {
        const auto report = ledger.Note(Outcome::SkippedOverlayUnavailable);
        if (report.logHeartbeat) {
            ++heartbeats;
            EXPECT_EQ(report.runFrames % FrameLedger::kHeartbeatFrames, 0u);
        }
    }
    EXPECT_EQ(heartbeats, 3u);
}

TEST(PostProcessFrameLedger, TheSummaryAppearsOnlyWhileGapsKeepAppearing) {
    FrameLedger ledger;
    uint32_t summaries = 0;
    for (uint64_t frame = 1; frame <= FrameLedger::kSummaryFrames * 2; ++frame)
        summaries += ledger.Note(Outcome::Applied).logSummary ? 1 : 0;
    EXPECT_EQ(summaries, 0u);
    ledger.Note(Outcome::SkippedFocusLossHold);
    for (uint64_t frame = 1; frame <= FrameLedger::kSummaryFrames; ++frame)
        summaries += ledger.Note(Outcome::Applied).logSummary ? 1 : 0;
    EXPECT_EQ(summaries, 1u);
    for (uint64_t frame = 1; frame <= FrameLedger::kSummaryFrames; ++frame)
        summaries += ledger.Note(Outcome::Applied).logSummary ? 1 : 0;
    EXPECT_EQ(summaries, 1u);
}

TEST(PostProcessRoutePolicy, EveryRuntimeRouteHasADistinctName) {
    const RuntimeRoute routes[] = {RuntimeRoute::PostSl, RuntimeRoute::FsrCallback, RuntimeRoute::FsrOverlayOutput};
    EXPECT_EQ(sizeof(routes) / sizeof(routes[0]), kRuntimeRouteCount);
    for (size_t first = 0; first < kRuntimeRouteCount; ++first) {
        EXPECT_LT(static_cast<size_t>(routes[first]), kRuntimeRouteCount);
        for (size_t second = first + 1; second < kRuntimeRouteCount; ++second)
            EXPECT_STRNE(RouteName(routes[first]), RouteName(routes[second]));
    }
}

TEST(PostProcessFrameLedger, EveryOutcomeIsCountedExactlyOnce) {
    FrameLedger ledger;
    for (size_t outcome = 0; outcome < kOutcomeCount; ++outcome)
        ledger.Note(static_cast<Outcome>(outcome));
    for (size_t outcome = 0; outcome < kOutcomeCount; ++outcome)
        EXPECT_EQ(ledger.OutcomeCount(static_cast<Outcome>(outcome)), 1u) << Name(static_cast<Outcome>(outcome));
    EXPECT_EQ(ledger.TotalFrames(), kOutcomeCount);
    EXPECT_EQ(ledger.CorrectedFrames() + ledger.CoveredFrames() + ledger.GapFrames(), kOutcomeCount);
}

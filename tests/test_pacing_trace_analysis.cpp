#include <gtest/gtest.h>
#include "../hook/common/pacing_trace_analysis.h"

namespace {
using namespace ce::pacing_trace;
Event Boundary(Kind kind, int64_t time, uint64_t id, uint32_t stage, uint64_t elapsed = 0,
               uint32_t thread = 1, uint64_t epoch = 1) {
    return {time, epoch, id, 42, elapsed, 0, 0, thread, stage, kind};
}

TEST(PacingTraceAnalysisTest, PairsNestedStagesWithoutAddingTheirInclusiveDurations) {
    const std::vector<Event> events{
        Boundary(Kind::PresentBegin, 100, 1, 0),
        Boundary(Kind::PresentForward, 103, 1, 0),
        Boundary(Kind::PresentBegin, 120, 2, 2),
        Boundary(Kind::PresentBegin, 130, 3, 4),
        Boundary(Kind::PresentEnd, 150, 3, 4, 20),
        Boundary(Kind::PresentEnd, 160, 2, 2, 40),
        Boundary(Kind::PresentEnd, 200, 1, 0, 100)};
    const auto result = Analyze(events);
    EXPECT_EQ(result.matchedPresents, 3u);
    EXPECT_EQ(result.proxyPrework.p95Us, 3u);
    EXPECT_EQ(result.proxyRuntime.p95Us, 97u);
    EXPECT_EQ(result.detour.p95Us, 40u);
    EXPECT_EQ(result.forwarding.p95Us, 20u);
    EXPECT_EQ(result.unmatchedBegins, 0u);
    EXPECT_EQ(result.unmatchedEnds, 0u);
}

TEST(PacingTraceAnalysisTest, ThreadIdentitySeparatesNestedLocalIdsAndEpochChangesExcludeOldCalls) {
    const std::vector<Event> events{
        Boundary(Kind::PresentBegin, 10, 1, 2, 0, 1, 1),
        Boundary(Kind::PresentBegin, 20, 1, 2, 0, 1, 2),
        Boundary(Kind::PresentBegin, 30, 1, 4, 0, 2, 2),
        Boundary(Kind::PresentEnd, 40, 1, 4, 10, 2, 2),
        Boundary(Kind::PresentEnd, 50, 1, 2, 30, 1, 2)};
    const auto result = Analyze(events);
    EXPECT_EQ(result.epoch, 2u);
    EXPECT_EQ(result.matchedPresents, 2u);
    EXPECT_EQ(result.unmatchedBegins, 0u);
    EXPECT_EQ(result.detour.meanUs, 30u);
    EXPECT_EQ(result.forwarding.meanUs, 10u);
}

TEST(PacingTraceAnalysisTest, TruncatedWindowsReportMissingPairsRatherThanZeroDuration) {
    const std::vector<Event> events{
        Boundary(Kind::PresentBegin, 10, 1, 2),
        Boundary(Kind::PresentBegin, 70, 2, 2),
        Boundary(Kind::PresentEnd, 100, 1, 2, 90)};
    const auto result = Analyze(events, 50);
    EXPECT_EQ(result.windowUs, 50);
    EXPECT_EQ(result.unmatchedBegins, 1u);
    EXPECT_EQ(result.unmatchedEnds, 1u);
    EXPECT_EQ(result.detour.samples, 0u);
}

TEST(PacingTraceAnalysisTest, RejectsMismatchedStageObjectDurationAndDuplicateForward) {
    for (int corruption = 0; corruption < 4; ++corruption) {
        std::vector<Event> events{
            Boundary(Kind::PresentBegin, 10, 1, 0),
            Boundary(Kind::PresentForward, 15, 1, 0),
            Boundary(Kind::PresentEnd, 20, 1, 0, 10)};
        if (corruption == 0) events.back().flags = 1;
        if (corruption == 1) events.back().object = 43;
        if (corruption == 2) events.back().a = 11;
        if (corruption == 3) events.insert(events.begin() + 2, events[1]);
        const auto result = Analyze(events);
        EXPECT_EQ(result.invalidPresents, 1u);
        EXPECT_EQ(result.matchedPresents, 0u);
        EXPECT_EQ(result.proxyRuntime.samples, 0u);
    }
}

TEST(PacingTraceAnalysisTest, MarkerBoundsRequireMatchingBufferSlotGenerationAndEpoch) {
    const Event marker{10, 1, 0, 99, 7, 2, 42, 1, 0, Kind::Marker};
    Event observed{50, 1, 0, 42, 7, 7, 2, 1, 1, Kind::MarkerObserved};
    // The marker precedes the reporting window, but still establishes the bound.
    auto result = Analyze({marker, observed}, 20);
    EXPECT_EQ(result.latestComplete, 1u);
    EXPECT_EQ(result.completedMarkerAge.meanUs, 40u);
    observed.a = 6;
    result = Analyze({marker, observed});
    EXPECT_EQ(result.latestPending, 1u);
    EXPECT_EQ(result.pendingMarkerAge.meanUs, 40u);
    observed.b = 8;
    result = Analyze({marker, observed});
    EXPECT_EQ(result.unmatchedMarkers, 1u);
    EXPECT_EQ(result.pendingMarkerAge.samples, 0u);
    observed.b = 7;
    observed.epoch = 2;
    EXPECT_EQ(Analyze({marker, observed}).unmatchedMarkers, 1u);
    observed.epoch = 1;
    observed.flags = 0; // Old reuse checks are not latest-marker observations.
    EXPECT_EQ(Analyze({marker, observed}).latestPending, 0u);
}

TEST(PacingTraceAnalysisTest, EmptyInputAndQuantilesAreExplicit) {
    EXPECT_EQ(Analyze({}).windowUs, 0);
    EXPECT_EQ(Summarize({}).samples, 0u);
    const auto metric = Summarize({1, 3, 2, 100});
    EXPECT_EQ(metric.samples, 4u);
    EXPECT_EQ(metric.meanUs, 26u);
    EXPECT_EQ(metric.p95Us, 100u); // Nearest-rank quantile, no interpolation.
    EXPECT_EQ(metric.maxUs, 100u);
}

TEST(PacingTraceAnalysisTest, EqualTimestampCallsRetainTheirRecordedOrder) {
    const std::vector<Event> events{
        Boundary(Kind::PresentBegin, 10, 1, 0),
        Boundary(Kind::PresentForward, 10, 1, 0),
        Boundary(Kind::PresentEnd, 10, 1, 0, 0)};
    const auto result = Analyze(events);
    EXPECT_EQ(result.matchedPresents, 1u);
    EXPECT_EQ(result.proxyPrework.samples, 1u);
    EXPECT_EQ(result.proxyRuntime.samples, 1u);
    EXPECT_EQ(result.proxyRuntime.maxUs, 0u);
}
}  // namespace

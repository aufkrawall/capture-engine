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

namespace {
Event Callback(Kind kind, int64_t time, uint32_t generated, uint32_t thread = 1) {
    return {time, 1, 7, 0, 0, 0, 0, thread, generated, kind};
}
Event Display(int64_t screen, int64_t presentStart, uint32_t resolved = 1, uint32_t thread = 9) {
    return {screen, 1, 0, 0, static_cast<uint64_t>(presentStart), 0, 0, thread, resolved, Kind::DisplayPair};
}
// One presented frame: the callback that produced it, then its runtime Present.
void PushFrame(std::vector<Event>& events, int64_t callbackBegin, int64_t callbackEnd, int64_t presentBegin,
               int64_t screen, uint32_t generated) {
    events.push_back(Callback(Kind::CallbackBegin, callbackBegin, generated));
    events.push_back(Callback(Kind::CallbackEnd, callbackEnd, generated));
    events.push_back(Boundary(Kind::PresentBegin, presentBegin, static_cast<uint64_t>(presentBegin), 2));
    events.push_back(Boundary(Kind::PresentEnd, presentBegin + 300, static_cast<uint64_t>(presentBegin), 2, 300));
    events.push_back(Display(screen, presentBegin + 120));
}
}  // namespace

TEST(PacingTraceAnalysisTest, DecomposesScreenTimeAgainstTheProducingCallbackPerFrameType) {
    std::vector<Event> events;
    // Generated frames are held 3 ms after the callback and reach the screen
    // 2 ms after Present; application frames are held 10 ms and land in 1.5 ms.
    for (int i = 0; i < 4; ++i) {
        const int64_t base = 100'000 + i * 20'000;
        PushFrame(events, base, base + 100, base + 3'100, base + 5'100, 1);
        PushFrame(events, base + 3'400, base + 3'500, base + 13'500, base + 15'000, 0);
    }
    const auto result = Analyze(events);
    EXPECT_EQ(result.displayPairs, 8u);
    EXPECT_EQ(result.displayPairsMatched, 8u);
    EXPECT_EQ(result.displayPairsUnresolved, 0u);
    EXPECT_EQ(result.generated.pacerWait.samples, 4u);
    EXPECT_EQ(result.generated.pacerWait.meanUs, 3'000u);
    EXPECT_EQ(result.generated.presentToDisplay.meanUs, 1'880u);
    EXPECT_EQ(result.generated.callbackToDisplay.meanUs, 5'000u);
    EXPECT_EQ(result.application.pacerWait.samples, 4u);
    EXPECT_EQ(result.application.pacerWait.meanUs, 10'000u);
    EXPECT_EQ(result.application.presentToDisplay.meanUs, 1'380u);
    EXPECT_EQ(result.application.callbackToDisplay.meanUs, 11'500u);
}

TEST(PacingTraceAnalysisTest, UnresolvedAndUnassociatedDisplayPairsAreCountedNotGuessed) {
    std::vector<Event> events;
    PushFrame(events, 100'000, 100'100, 103'100, 105'100, 1);
    // An unresolved screen time carries no trustworthy interval, and a pair with
    // no Present within tolerance must not bind to the nearest unrelated frame.
    events.push_back(Display(126'000, 125'880, 0));
    events.push_back(Display(200'000, 199'880));
    const auto result = Analyze(events);
    EXPECT_EQ(result.displayPairs, 3u);
    EXPECT_EQ(result.displayPairsUnresolved, 1u);
    EXPECT_EQ(result.displayPairsMatched, 1u);
    EXPECT_EQ(result.generated.callbackToDisplay.samples, 1u);
    EXPECT_EQ(result.application.callbackToDisplay.samples, 0u);
}

TEST(PacingTraceAnalysisTest, ACallbackWithoutItsOwnRuntimePresentContributesNothing) {
    std::vector<Event> events;
    // The proxy stage is the game's Present, not the runtime's: a callback
    // followed only by a proxy Present must not be associated with a screen time.
    events.push_back(Callback(Kind::CallbackBegin, 100'000, 1));
    events.push_back(Callback(Kind::CallbackEnd, 100'100, 1));
    events.push_back(Boundary(Kind::PresentBegin, 103'100, 1, 0));
    events.push_back(Boundary(Kind::PresentEnd, 103'400, 1, 0, 300));
    events.push_back(Display(105'100, 103'220));
    const auto result = Analyze(events);
    EXPECT_EQ(result.displayPairs, 1u);
    EXPECT_EQ(result.displayPairsMatched, 0u);
    EXPECT_EQ(result.generated.callbackToDisplay.samples, 0u);
}

TEST(PacingTraceAnalysisTest, GpuSpansSeparateWhenCeCommandsRanFromHowLongTheyTook) {
    std::vector<Event> events;
    for (int i = 0; i < 3; ++i) {
        const int64_t callbackEnd = 100'000 + i * 20'000;
        // Generated frames: CE's commands start 2 ms after the callback and run
        // 300 us. Application frames start 4 ms after and run 400 us.
        Event generated{callbackEnd + 2'000, 1, 0, 0, static_cast<uint64_t>(callbackEnd + 2'000),
                        static_cast<uint64_t>(callbackEnd + 2'300), static_cast<uint64_t>(callbackEnd), 5, 1,
                        Kind::GpuSpan};
        Event application{callbackEnd + 14'000, 1, 0, 0, static_cast<uint64_t>(callbackEnd + 14'000),
                          static_cast<uint64_t>(callbackEnd + 14'400), static_cast<uint64_t>(callbackEnd + 10'000), 5,
                          0, Kind::GpuSpan};
        events.push_back(generated);
        events.push_back(application);
    }
    const auto result = Analyze(events);
    EXPECT_EQ(result.generated.gpuStartDelay.samples, 3u);
    EXPECT_EQ(result.generated.gpuStartDelay.meanUs, 2'000u);
    EXPECT_EQ(result.generated.gpuDuration.meanUs, 300u);
    EXPECT_EQ(result.application.gpuStartDelay.meanUs, 4'000u);
    EXPECT_EQ(result.application.gpuDuration.meanUs, 400u);
}

TEST(PacingTraceAnalysisTest, UncalibratedOrReorderedGpuSpansAreDroppedNotAveragedIn) {
    std::vector<Event> events;
    // No callback anchor (calibration not settled), and an end before its begin.
    events.push_back(Event{100'000, 1, 0, 0, 102'000, 102'300, 0, 5, 1, Kind::GpuSpan});
    events.push_back(Event{120'000, 1, 0, 0, 122'000, 121'000, 120'000, 5, 1, Kind::GpuSpan});
    events.push_back(Event{140'000, 1, 0, 0, 142'000, 142'500, 140'000, 5, 1, Kind::GpuSpan});
    const auto result = Analyze(events);
    EXPECT_EQ(result.generated.gpuDuration.samples, 1u);
    EXPECT_EQ(result.generated.gpuDuration.meanUs, 500u);
    EXPECT_EQ(result.generated.gpuStartDelay.meanUs, 2'000u);
}

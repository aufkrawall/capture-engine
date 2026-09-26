#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "../hook/common/present_stage_cost.h"

// Stage-level attribution of CE's own time inside DetourPresent. GTA V Enhanced
// with FSR FG (sessions 20260925_233000 / 20260925_235838) showed ~80 us of CE
// time per Present on AMD's presenter thread (detour ~350 us minus forwarded
// Present ~270 us) with no way to say where it went. These tests pin the
// accounting model the next hardware run relies on: disjoint stages that sum to
// the detour, the forwarded Present kept out of CE's share, and percentiles that
// count presents which never entered a stage.

namespace {
using namespace ce::present_stage_cost;

struct FakeLapBackend {
    static inline bool enabled = true;
    static inline int64_t now = 0;
    static inline unsigned clockReads = 0;
    static inline ThreadRole defaultRole = ThreadRole::kOther;
    static inline std::vector<std::pair<ThreadRole, StageNs>> commits;

    static bool Enabled() { return enabled; }
    static int64_t Now() {
        ++clockReads;
        return now;
    }
    static uint64_t ToNs(int64_t ticks) { return ticks > 0 ? static_cast<uint64_t>(ticks) : 0; }
    static ThreadRole DefaultRole() { return defaultRole; }
    static void Commit(ThreadRole role, const StageNs& stageNs) { commits.emplace_back(role, stageNs); }
    static void Reset(bool enable = true) {
        enabled = enable;
        now = 1000;
        clockReads = 0;
        defaultRole = ThreadRole::kOther;
        commits.clear();
    }
};
using Recorder = DetourRecorderT<FakeLapBackend>;
using Scope = StageScopeT<FakeLapBackend>;
void Enter(Stage stage) { EnterStageT<FakeLapBackend>(stage); }
void Advance(int64_t ticks) { FakeLapBackend::now += ticks; }
uint64_t At(const StageNs& stageNs, Stage stage) { return stageNs[StageIndex(stage)]; }

uint64_t Sum(const StageNs& stageNs) {
    uint64_t total = 0;
    for (const uint64_t ns : stageNs) {
        total += ns;
    }
    return total;
}
}  // namespace

TEST(PresentStageCostTest, ScopesOutsideARecordedDetourReadNoClock) {
    FakeLapBackend::Reset();
    {
        Scope overlay(Stage::kOverlay);
        Enter(Stage::kEntry);
        Scope forward(Stage::kForward);
    }
    EXPECT_EQ(FakeLapBackend::clockReads, 0u);
    EXPECT_TRUE(FakeLapBackend::commits.empty());
}

TEST(PresentStageCostTest, DisabledBackendRecordsNothing) {
    FakeLapBackend::Reset(false);
    {
        Recorder recorder;
        Enter(Stage::kEntry);
        Scope forward(Stage::kForward);
        Advance(50);
    }
    EXPECT_EQ(FakeLapBackend::clockReads, 0u);
    EXPECT_TRUE(FakeLapBackend::commits.empty());
}

TEST(PresentStageCostTest, StagesAreDisjointAndSumExactlyToTheDetour) {
    FakeLapBackend::Reset();
    const int64_t begin = FakeLapBackend::now;
    {
        Recorder recorder;
        Advance(5);  // before the first named stage
        Enter(Stage::kEntry);
        Advance(10);
        Enter(Stage::kContext);
        Advance(20);
        {
            Scope overlay(Stage::kOverlay);
            Advance(30);
            {
                Scope forward(Stage::kForward);
                Advance(100);
            }
            Advance(7);  // back in the overlay region
        }
        Advance(3);  // back in context
        Enter(Stage::kPostPresent);
        Advance(4);
    }
    ASSERT_EQ(FakeLapBackend::commits.size(), 1u);
    const StageNs& ns = FakeLapBackend::commits[0].second;
    EXPECT_EQ(At(ns, Stage::kUnattributed), 5u);
    EXPECT_EQ(At(ns, Stage::kEntry), 10u);
    EXPECT_EQ(At(ns, Stage::kContext), 23u);
    EXPECT_EQ(At(ns, Stage::kOverlay), 37u);
    EXPECT_EQ(At(ns, Stage::kForward), 100u);
    EXPECT_EQ(At(ns, Stage::kPostPresent), 4u);
    EXPECT_EQ(Sum(ns), static_cast<uint64_t>(FakeLapBackend::now - begin));
}

// Streamline's route forwards through SL, which calls back down into
// DetourPresent. That nested detour's work is CE's, but once it returns the
// clock must be back on the runtime's forward, or the rest of the runtime's
// Present would be billed to whatever stage the nested detour ended in.
TEST(PresentStageCostTest, ADetourNestedInTheForwardHandsTheClockBackToTheForward) {
    FakeLapBackend::Reset();
    {
        Recorder outer;
        Enter(Stage::kCorePolicy);
        Advance(1);
        {
            Scope forward(Stage::kForward);
            Advance(10);
            {
                Recorder nested;
                nested.SetRole(ThreadRole::kGame);  // only the outermost detour names the role
                Enter(Stage::kEntry);
                Advance(4);
                Enter(Stage::kPostPresent);
                Advance(2);
            }
            Advance(10);
        }
        Enter(Stage::kPostPresent);
        Advance(1);
    }
    ASSERT_EQ(FakeLapBackend::commits.size(), 1u);
    EXPECT_EQ(FakeLapBackend::commits[0].first, ThreadRole::kOther);
    const StageNs& ns = FakeLapBackend::commits[0].second;
    EXPECT_EQ(At(ns, Stage::kCorePolicy), 1u);
    EXPECT_EQ(At(ns, Stage::kForward), 20u);
    EXPECT_EQ(At(ns, Stage::kEntry), 4u);
    EXPECT_EQ(At(ns, Stage::kPostPresent), 3u);
}

TEST(PresentStageCostTest, ReenteringTheCurrentStageReadsNoClock) {
    FakeLapBackend::Reset();
    {
        Recorder recorder;
        Scope forward(Stage::kForward);
        const unsigned readsInForward = FakeLapBackend::clockReads;
        {
            Scope forwardAgain(Stage::kForward);  // CallOriginalPresent inside ForwardPresentThrough
            Enter(Stage::kForward);
        }
        EXPECT_EQ(FakeLapBackend::clockReads, readsInForward);
    }
}

TEST(PresentStageCostTest, TheRoleComesFromTheContextWhenKnownAndTheThreadOtherwise) {
    FakeLapBackend::Reset();
    FakeLapBackend::defaultRole = ThreadRole::kGame;
    { Recorder earlyReturn; }
    {
        Recorder classified;
        classified.SetRole(ThreadRole::kRuntimePresenter);
    }
    ASSERT_EQ(FakeLapBackend::commits.size(), 2u);
    EXPECT_EQ(FakeLapBackend::commits[0].first, ThreadRole::kGame);
    EXPECT_EQ(FakeLapBackend::commits[1].first, ThreadRole::kRuntimePresenter);
}

TEST(PresentStageCostTest, ClassifiesThePresentThread) {
    // The learned game thread wins even when a Streamline module forwards its present.
    EXPECT_EQ(ClassifyThreadRole(0x10, 0x10, true), ThreadRole::kGame);
    // AMD's presenter thread: not the game's, carries an FG signal.
    EXPECT_EQ(ClassifyThreadRole(0x10, 0x20, true), ThreadRole::kRuntimePresenter);
    EXPECT_EQ(ClassifyThreadRole(0, 0x20, true), ThreadRole::kRuntimePresenter);
    // Not learned yet (or an API that never learns it): the game's own present.
    EXPECT_EQ(ClassifyThreadRole(0, 0x20, false), ThreadRole::kGame);
    // A second thread without any FG signal is neither.
    EXPECT_EQ(ClassifyThreadRole(0x10, 0x20, false), ThreadRole::kOther);
}

TEST(PresentStageCostHistogramTest, EveryValueLandsInABucketThatBoundsIt) {
    for (uint64_t value = 0; value < 200'000; value += (value < 4096 ? 1 : 97)) {
        const size_t bucket = LogHistogram::BucketOf(value);
        ASSERT_LT(bucket, LogHistogram::kBuckets);
        EXPECT_GE(LogHistogram::UpperBound(bucket), value) << value;
        if (bucket > 0) {
            EXPECT_LT(LogHistogram::UpperBound(bucket - 1), value) << value;
        }
    }
}

TEST(PresentStageCostHistogramTest, BucketsStayWithinOneEighthOfTheirValue) {
    for (size_t bucket = LogHistogram::kSubBuckets; bucket + 1 < LogHistogram::kBuckets; ++bucket) {
        const uint64_t upper = LogHistogram::UpperBound(bucket);
        const uint64_t width = upper - LogHistogram::UpperBound(bucket - 1);
        EXPECT_LE(width * 8, upper + 1) << bucket;
        EXPECT_GT(LogHistogram::UpperBound(bucket + 1), upper);
    }
}

TEST(PresentStageCostHistogramTest, ValuesBeyondTheRangeClampIntoTheLastBucket) {
    EXPECT_EQ(LogHistogram::BucketOf(uint64_t{1} << 40), LogHistogram::kBuckets - 1);
    EXPECT_EQ(LogHistogram::BucketOf(UINT64_MAX), LogHistogram::kBuckets - 1);
}

TEST(PresentStageCostStatsTest, P95FindsTheSlowTailAndIsCappedAtTheMaximum) {
    StageStats stats;
    for (int i = 0; i < 90; ++i) {
        stats.Observe(1'000);
    }
    for (int i = 0; i < 10; ++i) {
        stats.Observe(50'000);
    }
    const StageSummary summary = stats.Drain(100);
    EXPECT_EQ(summary.samples, 100u);
    EXPECT_EQ(summary.maxNs, 50'000u);
    EXPECT_EQ(summary.p95Ns, 50'000u);  // bucket upper bound, capped at the observed maximum
    EXPECT_DOUBLE_EQ(summary.MeanUs(), (90.0 * 1.0 + 10.0 * 50.0) / 100.0);
}

TEST(PresentStageCostStatsTest, PresentsThatSkippedTheStageCountAsZeros) {
    StageStats rare;
    for (int i = 0; i < 4; ++i) {
        rare.Observe(80'000);
    }
    const StageSummary rareSummary = rare.Drain(100);
    EXPECT_EQ(rareSummary.p95Ns, 0u);  // 96 of 100 presents spent nothing here
    EXPECT_EQ(rareSummary.maxNs, 80'000u);
    EXPECT_DOUBLE_EQ(rareSummary.MeanUs(), 4.0 * 80.0 / 100.0);

    StageStats frequent;
    for (int i = 0; i < 10; ++i) {
        frequent.Observe(2'000);
    }
    const StageSummary frequentSummary = frequent.Drain(100);  // rank 95 falls inside the samples
    EXPECT_GE(frequentSummary.p95Ns, 1'800u);
    EXPECT_LE(frequentSummary.p95Ns, 2'000u);
}

TEST(PresentStageCostStatsTest, DrainStartsANewWindow) {
    StageStats stats;
    stats.Observe(5'000);
    EXPECT_EQ(stats.Drain(1).samples, 1u);
    const StageSummary empty = stats.Drain(0);
    EXPECT_EQ(empty.samples, 0u);
    EXPECT_EQ(empty.sumNs, 0u);
    EXPECT_EQ(empty.maxNs, 0u);
    EXPECT_EQ(empty.p95Ns, 0u);
    EXPECT_DOUBLE_EQ(empty.MeanUs(), 0.0);
}

// The quantity the GTA evidence needs: per role, the stage means (forward
// excluded) add up to CE's own mean, and own + forward is the detour.
TEST(PresentStageCostAggregatorTest, StageMeansSumToOwnAndForwardStaysOutOfIt) {
    Aggregator aggregator;
    StageNs first{};
    first[StageIndex(Stage::kContext)] = 20'000;
    first[StageIndex(Stage::kFsrTopmost)] = 50'000;
    first[StageIndex(Stage::kPostPresent)] = 10'000;
    first[StageIndex(Stage::kForward)] = 270'000;
    StageNs second{};
    second[StageIndex(Stage::kContext)] = 30'000;
    second[StageIndex(Stage::kUnattributed)] = 6'000;
    second[StageIndex(Stage::kForward)] = 260'000;
    aggregator.Commit(ThreadRole::kRuntimePresenter, first);
    aggregator.Commit(ThreadRole::kRuntimePresenter, second);
    StageNs game{};
    game[StageIndex(Stage::kOverlay)] = 900'000;
    aggregator.Commit(ThreadRole::kGame, game);

    const RoleSummary runtime = aggregator.Drain(ThreadRole::kRuntimePresenter);
    EXPECT_EQ(runtime.calls, 2u);
    double stageMeanSum = 0.0;
    for (size_t stage = 0; stage < kStageCount; ++stage) {
        if (stage != StageIndex(Stage::kForward)) {
            stageMeanSum += runtime.stages[stage].MeanUs();
        }
    }
    EXPECT_DOUBLE_EQ(runtime.own.MeanUs(), 58.0);  // (80 + 36) / 2
    EXPECT_DOUBLE_EQ(stageMeanSum, runtime.own.MeanUs());
    EXPECT_DOUBLE_EQ(runtime.stages[StageIndex(Stage::kForward)].MeanUs(), 265.0);
    EXPECT_DOUBLE_EQ(runtime.detour.MeanUs(), 323.0);
    EXPECT_EQ(runtime.own.maxNs, 80'000u);
    EXPECT_EQ(runtime.stages[StageIndex(Stage::kFsrTopmost)].samples, 1u);
    EXPECT_EQ(runtime.stages[StageIndex(Stage::kOverlay)].samples, 0u);  // the game's overlay stays the game's

    const RoleSummary gameSummary = aggregator.Drain(ThreadRole::kGame);
    EXPECT_EQ(gameSummary.calls, 1u);
    EXPECT_DOUBLE_EQ(gameSummary.own.MeanUs(), 900.0);
    EXPECT_EQ(aggregator.Drain(ThreadRole::kOther).calls, 0u);
    EXPECT_EQ(aggregator.Drain(ThreadRole::kRuntimePresenter).calls, 0u);
}

TEST(PresentStageCostReportTest, TheLineNamesTheRoleTotalsAndEveryStage) {
    Aggregator aggregator;
    StageNs ns{};
    ns[StageIndex(Stage::kFsrTopmost)] = 60'000;
    ns[StageIndex(Stage::kForward)] = 270'000;
    aggregator.Commit(ThreadRole::kRuntimePresenter, ns);
    const RoleSummary summary = aggregator.Drain(ThreadRole::kRuntimePresenter);

    char line[1024];
    const int written = FormatRoleSummary(summary, 10'000, line, sizeof(line));
    const std::string text(line);
    EXPECT_EQ(static_cast<size_t>(written), text.size());
    EXPECT_EQ(text.rfind("[PRESENT STAGE COST] role=runtime_presenter window=10000ms calls=1 ", 0), 0u) << text;
    EXPECT_NE(text.find("own=60.0/60.0/60.0"), std::string::npos) << text;
    EXPECT_NE(text.find("detour=330.0/"), std::string::npos) << text;
    EXPECT_NE(text.find("forward=270.0/"), std::string::npos) << text;
    EXPECT_NE(text.find(" fsr_topmost=60.0/60.0/60.0(n=1)"), std::string::npos) << text;
    for (size_t stage = 0; stage < kStageCount; ++stage) {
        if (stage == StageIndex(Stage::kForward)) {
            continue;
        }
        EXPECT_NE(text.find(std::string(" ") + StageName(static_cast<Stage>(stage)) + "="), std::string::npos)
            << StageName(static_cast<Stage>(stage));
    }
}

TEST(PresentStageCostReportTest, ASmallBufferIsTruncatedNotOverrun) {
    Aggregator aggregator;
    aggregator.Commit(ThreadRole::kGame, StageNs{});
    const RoleSummary summary = aggregator.Drain(ThreadRole::kGame);
    char line[48];
    line[sizeof(line) - 1] = 'x';
    FormatRoleSummary(summary, 10'000, line, sizeof(line) - 1);
    EXPECT_EQ(line[sizeof(line) - 1], 'x');
    EXPECT_EQ(std::string(line).size(), sizeof(line) - 2);
}

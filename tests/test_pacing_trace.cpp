#include <gtest/gtest.h>
#include <thread>
#include "../hook/common/pacing_trace.h"
#include "source_fragment_reader.h"
#include <filesystem>

using namespace ce::pacing_trace;

namespace {
ce::pacing_health::ChannelStats Stats(bool bad) {
    ce::pacing_health::ChannelStats s;
    s.samples = 180; s.medianUs = 11000; s.maxUs = 18000;
    s.stddevUs = bad ? 2400 : 700; s.latePermille = bad ? 300 : 50;
    return s;
}
}
TEST(PacingTraceTest, RequiresThreeStableWindowsAndRearmsOnlyAfterRecovery) {
    EpisodeDetector d;
    EXPECT_FALSE(d.Observe(1, false, Stats(true), Stats(false)));
    EXPECT_FALSE(d.Observe(1, true, Stats(true), Stats(false)));
    EXPECT_FALSE(d.Observe(1, true, Stats(true), Stats(false)));
    EXPECT_TRUE(d.Observe(1, true, Stats(true), Stats(false)));
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(d.Observe(1, true, Stats(true), Stats(false)));
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(d.Observe(2, true, Stats(true), Stats(false)));
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(d.Observe(2, true, Stats(false), Stats(false)));
    EXPECT_FALSE(d.Observe(2, true, Stats(true), Stats(false)));
    EXPECT_FALSE(d.Observe(2, true, Stats(true), Stats(false)));
    EXPECT_TRUE(d.Observe(2, true, Stats(true), Stats(false)));
}
TEST(PacingTraceTest, LoadingStallsTransitionsMissingSamplesAndCadenceChangesBreakStreak) {
    for (int reason = 0; reason < 5; ++reason) {
        EpisodeDetector d;
        auto display = Stats(true), present = Stats(false);
        EXPECT_FALSE(d.Observe(1, true, display, present));
        EXPECT_FALSE(d.Observe(1, true, display, present));
        if (reason == 0) display.maxUs = 200000;
        if (reason == 1) present.samples = 5;
        if (reason == 2) present.medianUs = 20000;
        EXPECT_FALSE(d.Observe(reason == 3 ? 2 : 1, reason != 4, display, present));
        EXPECT_FALSE(d.Observe(1, true, Stats(true), Stats(false)));
    }
}
TEST(PacingTraceTest, MixedJitterDoesNotTrigger) {
    EpisodeDetector d;
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(d.Observe(1, true, Stats(true), Stats(true)));
}
TEST(PacingTraceTest, RingIsBoundedAndSessionMinimumExcludesOldSamples) {
    Ring<8> ring;
    for (unsigned i = 0; i < 20; ++i) { Event e; e.id = i; ring.Push(e); }
    const auto all = ring.Snapshot();
    ASSERT_EQ(all.size(), 8u);
    EXPECT_EQ(all.front().id, 12u);
    const auto session = ring.Snapshot(17);
    ASSERT_EQ(session.size(), 3u);
    EXPECT_EQ(session.front().id, 17u);
}
TEST(PacingTraceTest, ConcurrentReadersNeverSeeTornRecords) {
    Ring<64> ring;
    std::atomic<bool> done{false};
    std::thread writer([&]() {
        for (unsigned i = 1; i < 10000; ++i) { Event e; e.id = i; e.a = i * 3; ring.Push(e); }
        done.store(true);
    });
    do { for (const auto& e : ring.Snapshot()) EXPECT_EQ(e.a, e.id * 3); } while (!done.load());
    writer.join();
}

TEST(PacingTraceTest, MultipleProducersKeepRecordsCoherent) {
    Ring<128> ring;
    auto produce = [&](unsigned offset) {
        for (unsigned i = 1; i <= 5000; ++i) { Event e; e.id = offset + i; e.b = e.id * 7; ring.Push(e); }
    };
    std::thread first(produce, 0), second(produce, 10000);
    first.join(); second.join();
    EXPECT_EQ(ring.Total(), 10000u);
    for (const auto& e : ring.Snapshot()) EXPECT_EQ(e.b, e.id * 7);
}

TEST(PacingTraceTest, TraceUsesExistingProgressAndSavesOnlyOnServiceThread) {
    const auto root = std::filesystem::current_path();
    const auto upload = ce::test_source::ReadLogicalSource(root / "hook/common/custom_overlay_dx12_inline_upload.cpp");
    EXPECT_NE(upload.find("const uint32_t observed = inlineCompletions[index]"), std::string::npos);
    EXPECT_NE(upload.find("Kind::MarkerObserved"), std::string::npos);
    const auto trace = ce::test_source::ReadLogicalSource(root / "hook/common/pacing_trace.cpp");
    EXPECT_EQ(trace.find("->GetCompletedValue("), std::string::npos);
    EXPECT_EQ(trace.find("->Signal("), std::string::npos);
    EXPECT_EQ(trace.find("Sleep("), std::string::npos);
    EXPECT_NE(trace.find("saves >= 6"), std::string::npos);
    const auto loop = ce::test_source::ReadLogicalSource(root / "hook/main_hookthread.cpp");
    EXPECT_NE(loop.find("ce::pacing_trace::Service()"), std::string::npos);
}

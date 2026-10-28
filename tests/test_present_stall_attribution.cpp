#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "hook/present/present_stall_attribution.h"

namespace {

using ce::present_stall::Attribute;
using ce::present_stall::DownCall;
using ce::present_stall::kSlowDownCallUs;
using ce::present_stall::Window;

CaptureCopyProgress Progress(uint64_t submitted, uint64_t completed, uint32_t framesCaptured, bool held = false) {
    CaptureCopyProgress progress;
    progress.valid = true;
    progress.submitted = submitted;
    progress.completed = completed;
    progress.framesCaptured = framesCaptured;
    progress.signalHeld = held;
    return progress;
}

DownCall Call(uint64_t durationUs, CaptureCopyProgress entry, CaptureCopyProgress exit) {
    DownCall call;
    call.durationUs = durationUs;
    call.entry = entry;
    call.exit = exit;
    return call;
}

TEST(PresentStallAttributionTest, WithoutALiveCaptureNothingIsAttributed) {
    const auto attribution = Attribute(Call(5000, CaptureCopyProgress{}, CaptureCopyProgress{}), 0);
    EXPECT_FALSE(attribution.captured);
    EXPECT_EQ(attribution.copiesInFlightAtEntry, 0u);
    EXPECT_FALSE(attribution.copiesRetiredDuringCall);
    EXPECT_FALSE(attribution.signalHeld);
}

// The two readings the HotS stall needs told apart: a GPU still owing earlier
// copies, and a call that blocked while no copy retired.
TEST(PresentStallAttributionTest, CopyFenceProgressSaysWhetherTheGpuWasBehind) {
    const auto behind = Attribute(Call(4000, Progress(12, 9, 12), Progress(12, 11, 12)), 11);
    EXPECT_TRUE(behind.captured) << "a copy was submitted since the previous down-call";
    EXPECT_EQ(behind.copiesInFlightAtEntry, 3u);
    EXPECT_TRUE(behind.copiesRetiredDuringCall) << "the call ended once copies retired";

    const auto caughtUp = Attribute(Call(4000, Progress(12, 12, 12), Progress(12, 12, 12)), 12);
    EXPECT_FALSE(caughtUp.captured) << "no copy since the previous down-call";
    EXPECT_EQ(caughtUp.copiesInFlightAtEntry, 0u);
    EXPECT_FALSE(caughtUp.copiesRetiredDuringCall) << "a wait the copy fence cannot explain";

    const auto held = Attribute(Call(300, Progress(13, 12, 13, true), Progress(13, 12, 13, true)), 12);
    EXPECT_TRUE(held.signalHeld);
    EXPECT_EQ(held.copiesInFlightAtEntry, 1u) << "a held Signal counts as a copy in flight";
}

TEST(PresentStallAttributionTest, TheWindowSplitsCapturedFromUncapturedDownCalls) {
    Window window;
    EXPECT_EQ(window.Observe(Call(250, Progress(1, 1, 1), Progress(1, 1, 1))), 0u) << "fast calls get no ordinal";
    EXPECT_EQ(window.Observe(Call(kSlowDownCallUs + 1500, Progress(2, 1, 2, true), Progress(2, 1, 2, true))), 1u);
    EXPECT_EQ(window.Observe(Call(kSlowDownCallUs, Progress(3, 1, 3), Progress(3, 2, 3))), 2u);
    EXPECT_EQ(window.Observe(Call(kSlowDownCallUs + 10, Progress(3, 3, 3), Progress(3, 3, 3))), 1u)
        << "uncaptured slow calls count their own ordinals";
    EXPECT_EQ(window.Observe(Call(9000, Progress(9, 3, 4), Progress(9, 3, 4))), 3u);

    const auto summary = window.Drain();
    EXPECT_EQ(summary.calls, 5u);
    EXPECT_EQ(summary.captured, 4u);
    EXPECT_EQ(summary.slow, 4u);
    EXPECT_EQ(summary.slowCaptured, 3u);
    EXPECT_EQ(summary.slowInFlightAtEntry[0], 1u);
    EXPECT_EQ(summary.slowInFlightAtEntry[1], 1u);
    EXPECT_EQ(summary.slowInFlightAtEntry[2], 1u);
    EXPECT_EQ(summary.slowInFlightAtEntry[3], 1u) << "6 copies in flight fall into 3+";
    EXPECT_EQ(summary.slowRetiredDuringCall, 1u);
    EXPECT_EQ(summary.slowSignalHeld, 1u);
    EXPECT_EQ(summary.capturedForward.calls, 4u);
    EXPECT_EQ(summary.capturedForward.maxNs, 9000u * 1000u);
    EXPECT_EQ(summary.uncapturedForward.calls, 1u);
    EXPECT_EQ(summary.uncapturedForward.maxNs, (kSlowDownCallUs + 10) * 1000u);

    const auto empty = window.Drain();
    EXPECT_EQ(empty.calls, 0u);
    EXPECT_EQ(empty.slow, 0u);
    EXPECT_EQ(window.Observe(Call(kSlowDownCallUs, Progress(10, 9, 5), Progress(10, 9, 5))), 4u)
        << "ordinals pace the individual lines over the process, not per window";
}

TEST(PresentStallAttributionTest, SlowCallsAreLoggedInABurstThenSparsely) {
    using ce::present_stall::ShouldLogSlowCall;
    EXPECT_FALSE(ShouldLogSlowCall(0));
    for (uint64_t ordinal = 1; ordinal <= 8; ++ordinal) {
        EXPECT_TRUE(ShouldLogSlowCall(ordinal)) << ordinal;
    }
    EXPECT_FALSE(ShouldLogSlowCall(9));
    EXPECT_FALSE(ShouldLogSlowCall(255));
    EXPECT_TRUE(ShouldLogSlowCall(256));
    EXPECT_TRUE(ShouldLogSlowCall(512));
}

TEST(PresentStallAttributionTest, LinesCarryTheAttributionAndSurviveTruncation) {
    const DownCall call = Call(3480, Progress(40, 38, 40, true), Progress(40, 39, 40, true));
    const auto attribution = Attribute(call, 39);
    char line[384];
    const int written = ce::present_stall::FormatSlowCall(call, attribution, 3, line, sizeof(line));
    const std::string slow(line);
    EXPECT_EQ(written, static_cast<int>(slow.size()));
    EXPECT_NE(slow.find("[PRESENT STALL] slow down-call #3 3.48ms captured=1"), std::string::npos) << slow;
    EXPECT_NE(slow.find("completed=38->39 inFlightAtEntry=2 retiredDuringCall=1"), std::string::npos) << slow;
    EXPECT_NE(slow.find("signal=held capture=live"), std::string::npos) << slow;

    Window window;
    window.Observe(call);
    char summaryLine[640];
    ce::present_stall::FormatSummary(window.Drain(), 10'000, summaryLine, sizeof(summaryLine));
    const std::string summary(summaryLine);
    EXPECT_NE(summary.find("window=10000ms calls=1 captured=1 slow(>=2ms)=1 captured=1"), std::string::npos)
        << summary;
    EXPECT_NE(summary.find("copiesInFlightAtEntry(0/1/2/3+)=0/0/1/0 retiredDuringCall=1 heldSignal=1"),
              std::string::npos)
        << summary;

    char tiny[16];
    const int truncated = ce::present_stall::FormatSummary(window.Drain(), 10'000, tiny, sizeof(tiny));
    EXPECT_EQ(truncated, static_cast<int>(sizeof(tiny) - 1));
    EXPECT_EQ(std::strlen(tiny), sizeof(tiny) - 1);
}

}  // namespace

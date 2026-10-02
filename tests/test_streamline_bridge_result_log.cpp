#include <gtest/gtest.h>

#include <vector>

#include "hook/streamline/streamline_bridge_result_log.h"

// Session 20261002_043740: `slEvaluateFeature returned sl::Result=38` was logged once and whether
// DLSS kept failing until the game crashed nine seconds later could not be told from the log.
namespace {

namespace bridge = ce::streamline_bridge;
using bridge::ResultLogEvent;

constexpr int kOk = 0;
constexpr int kMissingInputParameter = 20;
constexpr int kInvalidState = 38;

TEST(StreamlineBridgeResultLog, SuccessAloneLogsNothing) {
    bridge::ResultTracker tracker;
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(tracker.Observe(kOk).event, ResultLogEvent::kNone);
    }
}

TEST(StreamlineBridgeResultLog, LogsFailureRunAtPowersOfTwo) {
    bridge::ResultTracker tracker;
    std::vector<uint32_t> logged;
    for (uint32_t i = 0; i < 20; ++i) {
        const auto decision = tracker.Observe(kInvalidState);
        if (decision.event == ResultLogEvent::kFailure) {
            logged.push_back(decision.failures);
        }
    }
    EXPECT_EQ(logged, (std::vector<uint32_t>{1, 2, 4, 8, 16}));
}

TEST(StreamlineBridgeResultLog, LogsRecoveryWithTheRunLengthOnce) {
    bridge::ResultTracker tracker;
    for (int i = 0; i < 5; ++i) {
        tracker.Observe(kInvalidState);
    }
    const auto recovered = tracker.Observe(kOk);
    EXPECT_EQ(recovered.event, ResultLogEvent::kRecovered);
    EXPECT_EQ(recovered.failures, 5u);
    EXPECT_EQ(recovered.total, 5u);
    EXPECT_EQ(tracker.Observe(kOk).event, ResultLogEvent::kNone);
}

TEST(StreamlineBridgeResultLog, ANewFailingResultIsLoggedMidRun) {
    bridge::ResultTracker tracker;
    EXPECT_EQ(tracker.Observe(kInvalidState).event, ResultLogEvent::kFailure);  // 1
    EXPECT_EQ(tracker.Observe(kInvalidState).event, ResultLogEvent::kFailure);  // 2
    EXPECT_EQ(tracker.Observe(kInvalidState).event, ResultLogEvent::kNone);     // 3
    EXPECT_EQ(tracker.Observe(kMissingInputParameter).event, ResultLogEvent::kFailure);
}

TEST(StreamlineBridgeResultLog, ANewRunStartsCountingAgainButKeepsTheTotal) {
    bridge::ResultTracker tracker;
    for (int i = 0; i < 3; ++i) {
        tracker.Observe(kInvalidState);
    }
    tracker.Observe(kOk);
    const auto again = tracker.Observe(kInvalidState);
    EXPECT_EQ(again.event, ResultLogEvent::kFailure);
    EXPECT_EQ(again.failures, 1u);
    EXPECT_EQ(again.total, 4u);
}

TEST(StreamlineBridgeResultLog, NamesTheResultTheRuntimeReturned) {
    EXPECT_STREQ(bridge::ResultCodeName(kInvalidState), "eErrorInvalidState");
    EXPECT_STREQ(bridge::ResultCodeName(15), "eErrorNGXFailed");
    EXPECT_STREQ(bridge::ResultCodeName(kMissingInputParameter), "eErrorMissingInputParameter");
    EXPECT_STREQ(bridge::ResultCodeName(39), "eWarnOutOfVRAM");
    EXPECT_STREQ(bridge::ResultCodeName(40), "unknown");
    EXPECT_STREQ(bridge::ResultCodeName(-1), "unknown");
}

}  // namespace

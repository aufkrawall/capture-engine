#pragma once

// Helpers shared by the FG flow scenarios.

#include <gtest/gtest.h>

#include <string>

#include "tests/flow/flow_host.h"

namespace ce::flow {

inline std::string CurrentTestName() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::string(info->test_suite_name()) + "." + info->name();
}

// The inject overlay's contract: on every presented frame, drawn once. CE must also account every physical
// present: one its ledger never saw would show no overlay without counting as uncovered.
inline void ExpectEveryPresentCoveredOnce(const FlowGame& game) {
    const CEFlowOverlayCoverage coverage = game.Coverage();
    EXPECT_EQ(coverage.presents, game.PhysicalPresents()) << "logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.uncovered, 0u) << "of " << coverage.presents << " presents; longest uncovered streak "
                                      << coverage.longestUncoveredStreak << "; logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.doubleDraws, 0u) << "logs: " << game.LogDirectory();
}

// What the overlay shows: "<type> <multiplier>x" or off.
inline void ExpectPublished(const FlowGame& game, int type, int multiplier, const char* where) {
    const CEFlowPublishedFG published = game.PublishedFG();
    if (multiplier < 2) {
        EXPECT_LT(published.multiplier, 2) << where << "; logs: " << game.LogDirectory();
        return;
    }
    EXPECT_EQ(published.type, type) << where << "; logs: " << game.LogDirectory();
    EXPECT_EQ(published.multiplier, multiplier) << where << "; logs: " << game.LogDirectory();
}

}  // namespace ce::flow

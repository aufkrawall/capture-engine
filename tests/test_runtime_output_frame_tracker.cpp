#include <gtest/gtest.h>

#include "tests/flow/runtime_output_frame_tracker.h"

namespace {

using Tracker = ce::flow::RuntimeOutputFrameTracker;
using Observation = Tracker::Observation;

TEST(RuntimeOutputFrameTrackerTest, ReusedAddressStartsANewLifetimeOffset) {
    int presenter = 0;
    Tracker tracker;
    EXPECT_EQ(tracker.Observe(&presenter, 1, 0), Observation::kFirstInLifetime);
    EXPECT_EQ(tracker.Observe(&presenter, 1, 0), Observation::kConsistent);
    EXPECT_EQ(tracker.Observe(&presenter, 2, 900), Observation::kFirstInLifetime);
    EXPECT_EQ(tracker.Offset(), 900);
    EXPECT_EQ(tracker.Observe(&presenter, 2, 900), Observation::kConsistent);
}

TEST(RuntimeOutputFrameTrackerTest, OffsetChangesWithinALifetimeRemainFailures) {
    int presenter = 0;
    Tracker tracker;
    ASSERT_EQ(tracker.Observe(&presenter, 1, -12), Observation::kFirstInLifetime);
    EXPECT_EQ(tracker.Observe(&presenter, 1, -11), Observation::kMismatch);
    EXPECT_EQ(tracker.Offset(), -12);
    EXPECT_EQ(tracker.Observe(&presenter, 1, -11), Observation::kMismatch);
    EXPECT_EQ(tracker.Observe(&presenter, 1, -12), Observation::kConsistent);
}

TEST(RuntimeOutputFrameTrackerTest, DifferentPresenterStartsANewOffset) {
    int first = 0;
    int second = 0;
    Tracker tracker;
    EXPECT_EQ(tracker.Observe(&first, 1, 0), Observation::kFirstInLifetime);
    EXPECT_EQ(tracker.Observe(&second, 1, 40), Observation::kFirstInLifetime);
    EXPECT_EQ(tracker.Observe(&second, 1, 40), Observation::kConsistent);
}

}  // namespace

#include "../captureengine/display_timing_correlation.h"
#include <gtest/gtest.h>
namespace { DisplayLayerPresentKey Key(uint64_t id) { return {3, 2, id}; } DisplayPendingFrameTypeFlip Payload(int64_t t, uint8_t type) { return {t, t, type}; } }

TEST(DisplayTimingCorrelationTest, PayloadBeforeMpoAndMpoBeforePayloadBothCorrelate) {
    DisplayTimingCorrelation c;
    c.ObservePayload(Key(1), Payload(100, 50));
    EXPECT_EQ(c.pendingPayloads().size(), 1u);
    c.Associate(Key(1), {7, 11, 100, 90});
    EXPECT_TRUE(c.pendingPayloads().empty());
    auto first = c.TakePayloads();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].presentStartTimestamp, 90);
    c.Associate(Key(2), {7, 12, 101, 91});
    EXPECT_EQ(c.ObservePayload(Key(2), Payload(102, 100)), DisplayTimingCorrelation::PayloadResult::Correlated);
    auto second = c.TakePayloads();
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].presentStartTimestamp, 91);
}

TEST(DisplayTimingCorrelationTest, ReversePresentIdsAndMismatchStayIsolated) {
    DisplayTimingCorrelation c;
    c.ObservePayload(Key(20), Payload(202, 50)); c.ObservePayload(Key(19), Payload(201, 100));
    c.Associate(Key(19), {7, 19, 201}); c.Associate(Key(20), {7, 20, 202});
    auto q = c.TakePayloads(); ASSERT_EQ(q.size(), 2u); EXPECT_EQ(q[0].associationId, 19u); EXPECT_EQ(q[1].associationId, 20u);
    EXPECT_EQ(c.ObservePayload({3, 9, 19}, Payload(203, 1)), DisplayTimingCorrelation::PayloadResult::Pending);
}

TEST(DisplayTimingCorrelationTest, DuplicateDoesNotOverwriteTimestampOrExtendRetention) {
    DisplayTimingCorrelation c; c.Associate(Key(1), {7, 1, 100});
    EXPECT_EQ(c.ObservePayload(Key(1), Payload(100, 1)), DisplayTimingCorrelation::PayloadResult::Correlated);
    EXPECT_EQ(c.ObservePayload(Key(1), Payload(999, 50)), DisplayTimingCorrelation::PayloadResult::Duplicate);
    ASSERT_EQ(c.states().at(1).timestamp, 100);
}

TEST(DisplayTimingCorrelationTest, FallbackPolicyGeneratedAllowedNonGeneratedSuppressed) {
    DisplayTimingCorrelation c; std::vector<PendingTimestamp> q; uint64_t order = 1;
    q.push_back({7, 1, 100, DisplayCompletionKind::Sync, order++}); EXPECT_TRUE(c.ShouldPublish(q.back())); c.CommitFallback(q.back());
    EXPECT_EQ(c.QueuePayload(7, 1, 101, 101, 50, q, order), DisplayTimingCorrelation::PayloadResult::Late);
    c.Associate(Key(2), {7, 2, 200});
    EXPECT_EQ(c.QueuePayload(7, 2, 201, 201, 1, q, order), DisplayTimingCorrelation::PayloadResult::Correlated);
    q.push_back({7, 2, 202, DisplayCompletionKind::Sync, order++});
    EXPECT_FALSE(c.ShouldPublish(q.back()));
}

TEST(DisplayTimingCorrelationTest, TombstonesRetainUntilPrunedThenAllowNewAssociation) {
    DisplayTimingCorrelation c; c.Associate(Key(3), {7, 3, 100}); std::vector<PendingTimestamp> q; uint64_t order = 1;
    q.push_back({7, 3, 100, DisplayCompletionKind::Sync, order++}); c.CommitFallback(q.back());
    EXPECT_EQ(c.QueuePayload(7, 3, 102, 102, 1, q, order), DisplayTimingCorrelation::PayloadResult::Late);
    c.Prune(101); EXPECT_TRUE(c.states().empty()); c.Associate(Key(3), {7, 4, 300});
    EXPECT_EQ(c.ObservePayload(Key(3), Payload(301, 50)), DisplayTimingCorrelation::PayloadResult::Correlated);
}

TEST(DisplayTimingCorrelationTest, CompletionFallbackRetainsAssociatedPresentStart) {
    DisplayTimingCorrelation c;
    std::vector<PendingTimestamp> q;
    uint64_t order = 1;

    c.QueueFallback(7, 4, 300, DisplayCompletionKind::Immediate, q, order, 250);

    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].presentStartTimestamp, 250);
}

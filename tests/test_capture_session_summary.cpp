#include <gtest/gtest.h>

#include "captureengine/media/media_encoder_session_summary.h"
#include <memory>

#include "common/capture/detached_live_state.h"

using ce::capture_summary::CaptureSessionSummary;
using ce::capture_summary::DuplicateReason;

TEST(CaptureSessionSummaryTest, KeepsRepeatReasonsAfterSharedPublicationIsReset) {
    CaptureSessionSummary summary;
    auto live = std::make_unique<SharedMemoryLayout>();
    EXPECT_EQ(summary.RecordDuplicateReason(false, false, false, false), DuplicateReason::kSource);
    live->runtimeState.duplicateFramesNoSource.store(1);
    EXPECT_EQ(summary.RecordDuplicateReason(false, false, true, false), DuplicateReason::kTimer);
    live->runtimeState.duplicateFramesTimerRebase.store(1);

    // Stop retargets shared publication to a private copy with fresh diagnostic counters.
    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::detached_live_state::CopyForDetachedSession(*live, *detached);
    EXPECT_EQ(detached->runtimeState.duplicateFramesNoSource.load(), 0u);
    EXPECT_EQ(detached->runtimeState.duplicateFramesTimerRebase.load(), 0u);
    EXPECT_EQ(summary.RecordDuplicateReason(false, false, false, false), DuplicateReason::kSource);
    EXPECT_EQ(summary.duplicateNoSourceTicks, 2u);
    EXPECT_EQ(summary.duplicateTimerTicks, 1u);
    EXPECT_EQ(summary.duplicateDeferredTicks, 0u);
    EXPECT_EQ(summary.duplicateDrainTicks, 0u);
}

TEST(CaptureSessionSummaryTest, PreservesReasonPriorityAndSeparatesCapacityRepeats) {
    CaptureSessionSummary summary;
    EXPECT_EQ(summary.RecordDuplicateReason(true, true, true, true), DuplicateReason::kDrain);
    EXPECT_EQ(summary.RecordDuplicateReason(false, true, true, true), DuplicateReason::kDeferred);
    EXPECT_EQ(summary.RecordDuplicateReason(false, false, true, true), DuplicateReason::kTimer);
    EXPECT_EQ(summary.RecordDuplicateReason(false, false, false, true), DuplicateReason::kCapacity);
    EXPECT_EQ(summary.duplicateDrainTicks, 1u);
    EXPECT_EQ(summary.duplicateDeferredTicks, 1u);
    EXPECT_EQ(summary.duplicateTimerTicks, 1u);
    EXPECT_EQ(summary.duplicateNoSourceTicks, 0u);
}

TEST(CaptureSessionSummaryTest, CountsBeyondPublicationWidthAndResetsOnlyForTheNextSession) {
    CaptureSessionSummary summary;
    summary.duplicateNoSourceTicks = UINT32_MAX;
    summary.RecordDuplicateReason(false, false, false, false);
    EXPECT_EQ(summary.duplicateNoSourceTicks, uint64_t{UINT32_MAX} + 1);
    summary.Reset();
    EXPECT_EQ(summary.duplicateNoSourceTicks, 0u);
    summary.RecordDuplicateReason(false, true, false, false);
    EXPECT_EQ(summary.duplicateDeferredTicks, 1u);
    EXPECT_EQ(summary.duplicateNoSourceTicks, 0u);
}

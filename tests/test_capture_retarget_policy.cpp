#include <gtest/gtest.h>

#include "../common/capture_retarget_policy.h"

namespace {

using namespace ce::capture_retarget;  // NOLINT(google-build-using-namespace) - test-local policy vocabulary

constexpr uint32_t kDegraded = ce::capture_policy::kRecordingHealthFlagVideoDegraded;

}  // namespace

// Regression: a window-target (or explicit-selector) recording whose source
// died mid-recording used to fall through the kAuto chain to the foreground
// monitor or primary - silently recording whatever the user was doing after
// the game exited. Only auto-origin targets may re-resolve automatically.

TEST(CaptureRetargetPolicyTest, SourceLossRetargetNeverReachesAutoForNonAutoTargets) {
    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kWindowTarget, true), RetargetSelector::kPinnedMonitorId);
    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kExplicitMonitor, true), RetargetSelector::kPinnedMonitorId);
    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kAutoMonitor, true), RetargetSelector::kPinnedMonitorId);

    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kWindowTarget, false), RetargetSelector::kStopRecording);
    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kExplicitMonitor, false), RetargetSelector::kStopRecording);
    EXPECT_EQ(SelectSourceLossRetarget(TargetOrigin::kAutoMonitor, false), RetargetSelector::kAutoMonitor);
}

TEST(CaptureRetargetPolicyTest, TargetOriginNamesAreStableForLogs) {
    EXPECT_STREQ(TargetOriginName(TargetOrigin::kWindowTarget), "window-target");
    EXPECT_STREQ(TargetOriginName(TargetOrigin::kExplicitMonitor), "explicit-monitor");
    EXPECT_STREQ(TargetOriginName(TargetOrigin::kAutoMonitor), "auto-monitor");
}

// Regression: a failed in-recording retarget whose rollback restart also
// failed only logged and left a frozen-frame recording running to the end.
// The terminal action is now defined: stop through the normal path (keeping
// the committed prefix) and latch degraded truth.

TEST(CaptureRetargetPolicyTest, FailedRollbackStopsTheRecordingDegraded) {
    EXPECT_EQ(SelectSourceLossRecovery(true, false, false), SourceLossRecovery::kStopDegraded);
    EXPECT_EQ(SelectSourceLossRecovery(true, true, false), SourceLossRecovery::kStopDegraded);
}

TEST(CaptureRetargetPolicyTest, RestoredSourceContinuesWithHonestDegradedTruth) {
    EXPECT_EQ(SelectSourceLossRecovery(true, true, true), SourceLossRecovery::kContinueDegraded);
    EXPECT_EQ(SelectSourceLossRecovery(true, false, true), SourceLossRecovery::kContinue);
    EXPECT_EQ(SelectSourceLossRecovery(false, true, true), SourceLossRecovery::kNone);
    EXPECT_EQ(SelectSourceLossRecovery(false, true, false), SourceLossRecovery::kNone);
}

// Regression: source-loss truth must survive later recording-health
// publications (the encoder thread overwrites its registers wholesale), or
// the manifest and completion notification claim a clean save.

TEST(CaptureRetargetPolicyTest, LatchedSourceLossSurvivesLaterHealthPublications) {
    ResetSourceLossHealth();
    std::atomic<uint32_t> sessionFlags{0};
    std::atomic<uint32_t> sharedFlags{0};

    RecordSourceLossHealth(sessionFlags, &sharedFlags);
    EXPECT_NE(sessionFlags.load() & kDegraded, 0u);
    EXPECT_NE(sharedFlags.load() & kDegraded, 0u);

    // A later publication replaces the registers with encoder-side state that
    // knows nothing about the loss; folding the latch back in keeps the truth.
    const uint32_t replaced = 0;
    EXPECT_NE(WithLatchedSourceLossHealth(replaced) & kDegraded, 0u);

    sessionFlags.store(0, std::memory_order_relaxed);
    sharedFlags.store(0, std::memory_order_relaxed);
    PublishLatchedSourceLossHealth(sessionFlags, &sharedFlags);
    EXPECT_NE(sessionFlags.load() & kDegraded, 0u);
    EXPECT_NE(sharedFlags.load() & kDegraded, 0u);

    ResetSourceLossHealth();
    EXPECT_EQ(WithLatchedSourceLossHealth(replaced) & kDegraded, 0u);
}

TEST(CaptureRetargetPolicyTest, FreshRecordingStartsWithoutSourceLossTruth) {
    ResetSourceLossHealth();
    std::atomic<uint32_t> sessionFlags{0};
    RecordSourceLossHealth(sessionFlags, nullptr);
    ResetSourceLossHealth();
    std::atomic<uint32_t> fresh{0};
    PublishLatchedSourceLossHealth(fresh, nullptr);
    EXPECT_EQ(fresh.load(), 0u);
    EXPECT_EQ(WithLatchedSourceLossHealth(0), 0u);
}

// Regression: after an HDR/mode change, frames kept the stale HDR contract for
// up to the 2 s recheck throttle. A delivered texture family crossing the HDR
// boundary must request the recheck immediately.

TEST(CaptureRetargetPolicyTest, FormatFamilyCrossingHdrBoundaryContradictsTheContract) {
    // HDR contract expects FP16 (scRGB); anything else crossed the boundary.
    EXPECT_TRUE(SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kEightBit, SourceFormatFamily::kFloat16,
                                                        true));
    EXPECT_TRUE(SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kTenBit, SourceFormatFamily::kFloat16, true));
    EXPECT_FALSE(
        SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kFloat16, SourceFormatFamily::kFloat16, true));

    // SDR contract contradicts only FP16 outside the SDR 10-bit FP16 pool
    // fallback (expected family FP16), which is a legitimate SDR shape.
    EXPECT_TRUE(SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kFloat16, SourceFormatFamily::kEightBit,
                                                        false));
    EXPECT_FALSE(
        SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kFloat16, SourceFormatFamily::kFloat16, false));
    EXPECT_FALSE(
        SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kEightBit, SourceFormatFamily::kEightBit, false));
    EXPECT_FALSE(
        SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kTenBit, SourceFormatFamily::kEightBit, false));

    // Unknown formats never trigger a reset storm.
    EXPECT_FALSE(SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kOther, SourceFormatFamily::kFloat16, true));
    EXPECT_FALSE(SourceFormatFamilyContradictsHdrContract(SourceFormatFamily::kEightBit, SourceFormatFamily::kOther, true));
}

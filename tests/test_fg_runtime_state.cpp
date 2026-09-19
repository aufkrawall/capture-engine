#include <gtest/gtest.h>

#include "../hook/common/fg_detection.h"
#include "../hook/common/fg_runtime_state.h"

#include <filesystem>

#include "../common/log_meter.h"
#include "source_fragment_reader.h"

namespace {

using ce::fg_runtime::DetectionSnapshot;
using ce::fg_runtime::RuntimeMode;

TEST(FGRuntimeStateTest, ClassifiesOffWithoutSupport) {
    DetectionSnapshot snapshot;
    EXPECT_EQ(RuntimeMode::kOff, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, ClassifiesNvidiaSmoothMotionSeparately) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.nvPresentLoaded = true;
    snapshot.nvidiaSmoothMotionDetected = true;
    EXPECT_EQ(RuntimeMode::kNvidiaSmoothMotion, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, ClassifiesNvidiaSmoothMotionOverIdleStreamlineSupport) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.nvPresentLoaded = true;
    snapshot.streamlineLoaded = true;
    snapshot.nvidiaSmoothMotionDetected = true;
    EXPECT_EQ(RuntimeMode::kNvidiaSmoothMotion, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, StreamlineWithoutFGStaysSeparate) {
    DetectionSnapshot snapshot;
    snapshot.streamlineLoaded = true;
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, StreamlineSignalWithoutSupportStillStaysSeparate) {
    DetectionSnapshot snapshot;
    snapshot.streamlineFGSignaled = true;
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, DlssRequiresMultiplierConfirmation) {
    DetectionSnapshot snapshot;
    snapshot.streamlineLoaded = true;
    snapshot.dlssFGApiActive = true;
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));

    snapshot.dlssFGMultiplier = 2;
    EXPECT_EQ(RuntimeMode::kDLSSFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, FsrOverridesTransientDlssApiState) {
    DetectionSnapshot snapshot;
    snapshot.streamlineLoaded = true;
    snapshot.dlssFGApiActive = true;
    snapshot.dlssFGMultiplier = 2;
    snapshot.fsrFGApiActive = true;
    EXPECT_EQ(RuntimeMode::kFSRFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, HeuristicFsrAllowedWhileStreamlineModuleIsPresentButFGIsNotSignaled) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    snapshot.streamlineLoaded = true;
    EXPECT_EQ(RuntimeMode::kFSRFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, HeuristicFsrBlockedWhileStreamlineFGIsSignaled) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    snapshot.streamlineLoaded = true;
    snapshot.streamlineFGSignaled = true;
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, HeuristicFsrBlockedByConfirmedDlssFG) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    snapshot.streamlineLoaded = true;
    snapshot.dlssFGApiActive = true;
    snapshot.dlssFGMultiplier = 2;
    EXPECT_EQ(RuntimeMode::kDLSSFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, HeuristicFsrAllowedWithoutStreamline) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    EXPECT_EQ(RuntimeMode::kFSRFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, NvPresentWithoutConfirmedSmoothMotionStaysOff) {
    DetectionSnapshot snapshot;
    snapshot.nvPresentLoaded = true;
    EXPECT_EQ(RuntimeMode::kOff, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, NvPresentDetectionWakesPatternAnalysisWithoutClaimingFrameGeneration) {
    FGCompatibility detector;
    ASSERT_TRUE(detector.IsDormant());

    detector.MarkNvPresentLoaded();

    EXPECT_FALSE(detector.IsDormant());
    EXPECT_TRUE(detector.IsNvPresentLoaded());
    EXPECT_FALSE(detector.IsFGActive());
    EXPECT_EQ(RuntimeMode::kOff, detector.GetRuntimeMode());
}

TEST(FGRuntimeStateTest, NvPresentDetectionPreservesStreamlineWithoutFrameGeneration) {
    FGCompatibility detector;
    detector.SetStreamlineSupportPresent(true);

    detector.MarkNvPresentLoaded();

    EXPECT_FALSE(detector.IsFGActive());
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, detector.GetRuntimeMode());
}

TEST(FGRuntimeStateTest, SmoothMotionPatternEligibilityExcludesCompetingFrameGeneration) {
    DetectionSnapshot snapshot;
    snapshot.nvPresentLoaded = true;
    snapshot.streamlineLoaded = true;
    EXPECT_TRUE(ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(snapshot));

    snapshot.streamlineFGSignaled = true;
    EXPECT_FALSE(ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(snapshot));
    snapshot.streamlineFGSignaled = false;

    snapshot.dlssFGApiActive = true;
    EXPECT_FALSE(ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(snapshot));
    snapshot.dlssFGApiActive = false;

    snapshot.fsrFGApiActive = true;
    EXPECT_FALSE(ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(snapshot));
    snapshot.fsrFGApiActive = false;

    snapshot.heuristicFSRFGActive = true;
    EXPECT_FALSE(ce::fg_runtime::CanEvaluateNvidiaSmoothMotionPattern(snapshot));
}

TEST(FGRuntimeStateTest, SmoothMotionTwoPopulationInferenceMatchesObservedDriverCadence) {
    EXPECT_TRUE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 56, 31));
    EXPECT_TRUE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 120, 65));

    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(false, 56, 31));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 28, 1));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 56, 56));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 30, 25));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotion2xPopulation(true, 60, 20));
}

TEST(FGRuntimeStateTest, SmoothMotionPairedPresentCadenceMatchesDriverPacedCallbacks) {
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(600, 13800));
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(14480, 458));
    EXPECT_FALSE(ce::fg_runtime::HasContrastingPresentGaps(7000, 7100));
    EXPECT_FALSE(ce::fg_runtime::HasContrastingPresentGaps(0, 13800));

    EXPECT_TRUE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(true, 119, 118));
    EXPECT_TRUE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(true, 20, 16));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(false, 119, 118));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(true, 19, 18));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(true, 20, 15));
    EXPECT_FALSE(ce::fg_runtime::HasNvidiaSmoothMotionPairedPresentCadence(true, 120, 2));
}

TEST(FGRuntimeStateTest, SmoothMotionPresentCadenceIsIndependentOfAbsoluteFrameRateAndCap) {
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(250, 3500));
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(500, 7000));
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(1000, 14000));
    EXPECT_TRUE(ce::fg_runtime::HasContrastingPresentGaps(2000, 28000));

    EXPECT_FALSE(ce::fg_runtime::HasContrastingPresentGaps(4000, 8000));
    EXPECT_FALSE(ce::fg_runtime::HasContrastingPresentGaps(7000, 7000));
}

TEST(FGRuntimeStateTest, ConfirmedSmoothMotionLatchesAcrossPacingChangesButNotCompetingFG) {
    DetectionSnapshot snapshot;
    snapshot.nvPresentLoaded = true;
    snapshot.nvidiaSmoothMotionDetected = true;
    EXPECT_TRUE(ce::fg_runtime::ShouldRetainConfirmedNvidiaSmoothMotion(snapshot));

    snapshot.streamlineFGSignaled = true;
    EXPECT_FALSE(ce::fg_runtime::ShouldRetainConfirmedNvidiaSmoothMotion(snapshot));
    snapshot.streamlineFGSignaled = false;

    snapshot.fsrFGApiActive = true;
    EXPECT_FALSE(ce::fg_runtime::ShouldRetainConfirmedNvidiaSmoothMotion(snapshot));
    snapshot.fsrFGApiActive = false;

    snapshot.nvidiaSmoothMotionDetected = false;
    EXPECT_FALSE(ce::fg_runtime::ShouldRetainConfirmedNvidiaSmoothMotion(snapshot));
}

TEST(FGRuntimeStateTest, FsrApiWinsWithoutStreamlineSupport) {
    DetectionSnapshot snapshot;
    snapshot.fsrFGApiActive = true;
    EXPECT_EQ(RuntimeMode::kFSRFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, FsrApiOverridesStreamlineWithoutConfirmedDlssMultiplier) {
    DetectionSnapshot snapshot;
    snapshot.streamlineLoaded = true;
    snapshot.fsrFGApiActive = true;
    EXPECT_EQ(RuntimeMode::kFSRFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
}

TEST(FGRuntimeStateTest, HeuristicFsrRuntimeModeIsConsideredFsrForRoutingGuards) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    snapshot.streamlineLoaded = true;
    EXPECT_TRUE(ce::fg_runtime::RuntimeModeUsesFSR(ce::fg_runtime::ClassifyRuntimeMode(snapshot)));
}

TEST(FGRuntimeStateTest, RuntimeModeHelpersMatchClassification) {
    EXPECT_FALSE(ce::fg_runtime::IsRuntimeFGActive(RuntimeMode::kOff));
    EXPECT_TRUE(ce::fg_runtime::IsRuntimeFGActive(RuntimeMode::kDLSSFG));
    EXPECT_TRUE(ce::fg_runtime::IsRuntimeFGActive(RuntimeMode::kFSRFG));
    EXPECT_TRUE(ce::fg_runtime::IsRuntimeFGActive(RuntimeMode::kNvidiaSmoothMotion));
    EXPECT_FALSE(ce::fg_runtime::IsActualGeneratedFrameMode(RuntimeMode::kOff));
    EXPECT_TRUE(ce::fg_runtime::IsActualGeneratedFrameMode(RuntimeMode::kDLSSFG));
    EXPECT_TRUE(ce::fg_runtime::IsActualGeneratedFrameMode(RuntimeMode::kFSRFG));
    EXPECT_FALSE(ce::fg_runtime::IsActualGeneratedFrameMode(RuntimeMode::kNvidiaSmoothMotion));
    EXPECT_TRUE(ce::fg_runtime::RuntimeModeUsesStreamline(RuntimeMode::kStreamlineNoFG));
    EXPECT_TRUE(ce::fg_runtime::RuntimeModeUsesStreamline(RuntimeMode::kDLSSFG));
    EXPECT_FALSE(ce::fg_runtime::RuntimeModeUsesStreamline(RuntimeMode::kFSRFG));
    EXPECT_TRUE(ce::fg_runtime::RuntimeModeUsesFSR(RuntimeMode::kFSRFG));
    EXPECT_STREQ("NVIDIA_SM", ce::fg_runtime::GetRuntimeModeName(RuntimeMode::kNvidiaSmoothMotion));
}

// Dynamic multi-frame generation changes the DLSS-G cadence per frame, so the
// multiplier transition log stopped being a rare event: session
// `20260919_223111` recorded 2743 of those lines in five minutes, 26% of the
// whole hook log. The transition still has to be visible - an activation, or a
// factor stuck at the wrong value, shows up in the first few - so the meter is
// burst-then-heartbeat rather than a blanket suppression.
TEST(FGRuntimeStateTest, DlssFGMultiplierTransitionLogIsMetered) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "hook" / "common" / "fg_detection.cpp";
    ASSERT_TRUE(fs::exists(source));
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    const size_t setter = text.find("void FGCompatibility::SetDLSSFGMultiplier");
    ASSERT_NE(setter, std::string::npos);
    const size_t nextFn = text.find("void FGCompatibility::SetFSRFGMultiplier", setter);
    ASSERT_NE(nextFn, std::string::npos);
    const std::string body = text.substr(setter, nextFn - setter);

    EXPECT_NE(body.find("ce::log_meter::ShouldLogCadence"), std::string::npos)
        << "the DLSS FG multiplier transition must stay metered under dynamic MFG";
    // The heartbeat is only useful if it carries the range it stands for.
    EXPECT_NE(body.find("observed %dx..%dx"), std::string::npos);
}

TEST(FGRuntimeStateTest, DlssFGMultiplierMeterKeepsTheActivationBurstVisible) {
    // Mirrors kDLSSFGMultiplierLogBurst / kDLSSFGMultiplierLogStride.
    constexpr uint32_t kBurst = 8;
    constexpr uint32_t kStride = 512;

    for (uint32_t i = 1; i <= kBurst; ++i) {
        EXPECT_TRUE(ce::log_meter::ShouldLogCadence(i, kBurst, kStride)) << "burst entry " << i;
    }
    EXPECT_FALSE(ce::log_meter::ShouldLogCadence(kBurst + 1, kBurst, kStride));
    EXPECT_TRUE(ce::log_meter::ShouldLogCadence(kStride, kBurst, kStride));
    EXPECT_FALSE(ce::log_meter::ShouldLogCadence(kStride + 1, kBurst, kStride));

    // The session that motivated the meter: 2743 changes would have become 8
    // burst lines plus 5 heartbeats instead of 2743.
    uint32_t logged = 0;
    for (uint32_t i = 1; i <= 2743; ++i) {
        if (ce::log_meter::ShouldLogCadence(i, kBurst, kStride))
            ++logged;
    }
    EXPECT_EQ(logged, kBurst + 5);
}

}  // namespace

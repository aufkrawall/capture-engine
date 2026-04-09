#include <gtest/gtest.h>

#include "../hook/common/fg_runtime_state.h"

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

TEST(FGRuntimeStateTest, HeuristicFsrBlockedWhileStreamlineIsPresent) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.heuristicFSRFGActive = true;
    snapshot.streamlineLoaded = true;
    EXPECT_EQ(RuntimeMode::kStreamlineNoFG, ce::fg_runtime::ClassifyRuntimeMode(snapshot));
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

}  // namespace

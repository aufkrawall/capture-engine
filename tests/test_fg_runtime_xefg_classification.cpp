// Intel XeSS frame generation in CE's runtime model (hook/fg/fg_runtime_state.h, fg_detection, the overlay's
// FG status): its own swapchain presents every displayed frame, so a presenter-owned XeFG with generation on is
// what the status shows, ahead of any API signal - a Streamline DLSS-G signal beside it is a front.

#include <gtest/gtest.h>

#include "hook/fg/fg_detection.h"
#include "hook/fg/fg_runtime_state.h"
#include "hook/metrics/performance_metrics.h"
#include "hook/overlay/overlay_fg_metric_policy.h"
#include "hook/overlay/overlay_metrics_publisher.h"

namespace {

using ce::fg_runtime::ClassifyRuntimeMode;
using ce::fg_runtime::DetectionSnapshot;
using ce::fg_runtime::RuntimeMode;

DetectionSnapshot XeFGPresenting(bool enabled) {
    DetectionSnapshot snapshot;
    snapshot.dormant = false;
    snapshot.xefgPresenterActive = true;
    snapshot.xefgEnabled = enabled;
    return snapshot;
}

TEST(XeFGRuntimeClassificationTest, APresenterOwnedGeneratorOutranksEveryApiSignal) {
    DetectionSnapshot snapshot = XeFGPresenting(true);
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kXeFG);

    // The OptiScaler session: a DLSS-G signal from the game's slDLSSGSetOptions(on), multiplier 2.
    snapshot.streamlineLoaded = true;
    snapshot.streamlineFGSignaled = true;
    snapshot.dlssFGApiActive = true;
    snapshot.dlssFGMultiplier = 2;
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kXeFG);

    snapshot.fsrFGApiActive = true;
    snapshot.heuristicFSRFGActive = true;
    snapshot.nvPresentLoaded = true;
    snapshot.nvidiaSmoothMotionDetected = true;
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kXeFG);
}

TEST(XeFGRuntimeClassificationTest, TheOrderBelowThePresenterIsUnchanged) {
    DetectionSnapshot snapshot = XeFGPresenting(false);  // XeFG presents with generation off
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kOff);
    snapshot.streamlineLoaded = true;
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kStreamlineNoFG);
    snapshot.dlssFGApiActive = true;
    snapshot.dlssFGMultiplier = 2;
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kDLSSFG);
    snapshot.fsrFGApiActive = true;
    EXPECT_EQ(ClassifyRuntimeMode(snapshot), RuntimeMode::kFSRFG);

    DetectionSnapshot noPresenter;
    noPresenter.dormant = false;
    noPresenter.xefgEnabled = true;  // SetEnabled(1) without a registered native swapchain
    EXPECT_EQ(ClassifyRuntimeMode(noPresenter), RuntimeMode::kOff);
}

TEST(XeFGRuntimeClassificationTest, XeFGIsAnActiveFGModeWithItsOwnNamesAndStatusType) {
    EXPECT_TRUE(ce::fg_runtime::IsRuntimeFGActive(RuntimeMode::kXeFG));
    EXPECT_FALSE(ce::fg_runtime::RuntimeModeUsesStreamline(RuntimeMode::kXeFG));
    EXPECT_FALSE(ce::fg_runtime::RuntimeModeUsesFSR(RuntimeMode::kXeFG));
    EXPECT_STREQ(ce::fg_runtime::GetRuntimeModeName(RuntimeMode::kXeFG), "XeFG");
    EXPECT_EQ(ce::overlay_metrics::ResolveFGMetricType(true, RuntimeMode::kXeFG),
              ce::overlay_metrics::FGMetricType::kXeFG);
    EXPECT_EQ(ce::overlay_metrics::ResolveFGMetricType(false, RuntimeMode::kXeFG),
              ce::overlay_metrics::FGMetricType::kNone);
}

TEST(XeFGRuntimeClassificationTest, TheMultiplierIsTheConfiguredOneUntilTheCadenceMeasuresAnother) {
    FGCompatibility compat;
    compat.SetXeFGState(true, true, 1);
    EXPECT_EQ(compat.GetRuntimeMode(), RuntimeMode::kXeFG);
    EXPECT_EQ(compat.GetActiveFGType(), FGCompatibility::FGType::XeFG);
    EXPECT_STREQ(compat.GetFGTypeName(FGCompatibility::FGType::XeFG), "XeFG");
    EXPECT_EQ(compat.GetFGMultiplier(), 2);

    compat.SetXeFGState(true, true, 2);  // SetNumInterpolatedFrames(2)
    EXPECT_EQ(compat.GetFGMultiplier(), 3);

    // The measured output/application ratio wins when they disagree.
    ce::present_interposer::CadenceVerdict verdict;
    verdict.generating = true;
    verdict.multiplier = 2;
    verdict.outputFps = 144.0f;
    verdict.applicationFps = 72.0f;
    compat.NoteXeFGCadence(verdict);
    EXPECT_EQ(compat.GetFGMultiplier(), 2);
    EXPECT_FLOAT_EQ(compat.GetOutputFPS(), 144.0f);
    EXPECT_FLOAT_EQ(compat.GetBaseFPS(), 72.0f);

    // Generation off: no measured factor survives, and nothing is published as FG.
    compat.SetXeFGState(true, false, 2);
    EXPECT_EQ(compat.GetRuntimeMode(), RuntimeMode::kOff);
    EXPECT_FALSE(compat.IsFGActive());
    compat.SetXeFGState(true, true, 2);
    EXPECT_EQ(compat.GetFGMultiplier(), 3);
}

// Talos's menu (20261104_183540): generation stays enabled while XeFG queues one frame per Present. The fallback to
// the configured factor read that as "XeFG 2x".
TEST(XeFGRuntimeClassificationTest, MeasuredAsNotGeneratingItReadsLikeGenerationOff) {
    FGCompatibility compat;
    compat.SetXeFGState(true, true, 1);

    ce::present_interposer::CadenceVerdict notGenerating;
    notGenerating.decided = true;
    notGenerating.outputFps = 144.0f;
    notGenerating.applicationFps = 144.0f;
    compat.NoteXeFGCadence(notGenerating);
    EXPECT_NE(compat.GetRuntimeMode(), RuntimeMode::kXeFG);
    EXPECT_FALSE(compat.IsFGActive());

    // The next undecided window (the game is back, or below 20 fps) restores what the game asked for at once.
    compat.NoteXeFGCadence(ce::present_interposer::CadenceVerdict{});
    EXPECT_EQ(compat.GetRuntimeMode(), RuntimeMode::kXeFG);
    EXPECT_EQ(compat.GetFGMultiplier(), 2);

    compat.NoteXeFGCadence(notGenerating);
    ce::present_interposer::CadenceVerdict generating;
    generating.decided = true;
    generating.generating = true;
    generating.multiplier = 2;
    compat.NoteXeFGCadence(generating);
    EXPECT_EQ(compat.GetRuntimeMode(), RuntimeMode::kXeFG);

    // Generation off and on again starts from what the game configured, not from an old measurement.
    compat.NoteXeFGCadence(notGenerating);
    compat.SetXeFGState(true, false, 1);
    EXPECT_FALSE(compat.IsFGActive());
    compat.NoteXeFGCadence(notGenerating);  // a window closing as generation went off
    compat.SetXeFGState(true, true, 1);
    EXPECT_EQ(compat.GetRuntimeMode(), RuntimeMode::kXeFG);
}

// A generating XeFG read as off for exactly 300 frames after Streamline's FG had gone (Talos, 20261104_183540).
TEST(XeFGRuntimeClassificationTest, TheGraceAfterStreamlineBelievesOnlyARuntimeStatingItsOwnState) {
    using ce::fg_runtime::SLOffGraceSuppressesFG;
    EXPECT_FALSE(SLOffGraceSuppressesFG(RuntimeMode::kXeFG, false));
    EXPECT_FALSE(SLOffGraceSuppressesFG(RuntimeMode::kFSRFG, true));
    EXPECT_TRUE(SLOffGraceSuppressesFG(RuntimeMode::kFSRFG, false)) << "heuristic FSR FG, from elevated ECL counts";
    EXPECT_TRUE(SLOffGraceSuppressesFG(RuntimeMode::kNvidiaSmoothMotion, false)) << "the departing cached multiplier";
    EXPECT_TRUE(SLOffGraceSuppressesFG(RuntimeMode::kDLSSFG, false));
}

TEST(XeFGRuntimeClassificationTest, TheOverlayShowsXeFGWithItsMultiplier) {
    PerformanceMetrics metrics;
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, {
                                                               .effectiveFGActive = true,
                                                               .runtimeMode = RuntimeMode::kXeFG,
                                                               .outputFPS = 144.0f,
                                                               .baseFPS = 72.0f,
                                                               .multiplier = 2,
                                                               .publicationSource = "test",
                                                           });
    EXPECT_EQ(metrics.GetFGType(), 4);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "XeFG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
    EXPECT_TRUE(metrics.IsFGActive());
}

}  // namespace

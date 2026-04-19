#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"
#include "../hook/common/fg_session_state.h"
#include "../hook/common/overlay_metrics_publisher.h"
#include "../hook/common/performance_metrics.h"

namespace {

const char* LabelForMetricType(int metricType) {
    switch (metricType) {
        case 1:
            return "DLSS FG";
        case 2:
            return "FSR FG";
        case 3:
            return "NVIDIA SM";
        default:
            return "FG";
    }
}

void Publish(PerformanceMetrics& metrics, bool active, ce::fg_runtime::RuntimeMode runtimeMode, int multiplier,
             float outputFps = 120.0f, float baseFps = 60.0f) {
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics,
                                                 {
                                                     .effectiveFGActive = active,
                                                     .runtimeMode = runtimeMode,
                                                     .outputFPS = outputFps,
                                                     .baseFPS = baseFps,
                                                     .multiplier = multiplier,
                                                     .publicationSource = "test_overlay_fg_status_publication",
                                                 });
}

void PublishPlanner(PerformanceMetrics& metrics, bool active, ce::fg_runtime::RuntimeMode runtimeMode, int multiplier,
                    float outputFps = 120.0f, float baseFps = 60.0f) {
    ce::fg_session::FGActionPlan plan;
    plan.publishFGActive = active;
    plan.publishRuntimeMode = runtimeMode;
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, plan, outputFps, baseFps, multiplier,
                                                 "test_overlay_fg_status_publication_planner");
}

}  // namespace

TEST(OverlayFGStatusPublicationTest, PublishesNoFGAsBaseline) {
    PerformanceMetrics metrics;

    Publish(metrics, false, ce::fg_runtime::RuntimeMode::kOff, 1, 999.0f, 999.0f);

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 0.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 0.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), LabelForMetricType(0));
}

TEST(OverlayFGStatusPublicationTest, PublishesFSRAndDlssMultipliersConsistently) {
    PerformanceMetrics metrics;

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kFSRFG, 2, 144.0f, 72.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), LabelForMetricType(ce::dx12_overlay_policy::ResolveOverlayFGMetricType(
                                               true, ce::fg_runtime::RuntimeMode::kFSRFG)));
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 72.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 144.0f);

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 4, 240.0f, 60.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), LabelForMetricType(ce::dx12_overlay_policy::ResolveOverlayFGMetricType(
                                               true, ce::fg_runtime::RuntimeMode::kDLSSFG)));
    EXPECT_EQ(metrics.GetFGMultiplier(), 4);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 60.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 240.0f);
}

TEST(OverlayFGStatusPublicationTest, PublishesNvidiaSmoothMotionSeparately) {
    PerformanceMetrics metrics;

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion, 2, 200.0f, 100.0f);

    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), LabelForMetricType(ce::dx12_overlay_policy::ResolveOverlayFGMetricType(
                                               true, ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion)));
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
}

TEST(OverlayFGStatusPublicationTest, SwitchingBetweenFGModesUpdatesVisibleStatusImmediately) {
    PerformanceMetrics metrics;

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kFSRFG, 2, 144.0f, 72.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FSR FG");

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 3, 180.0f, 60.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 3);

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kFSRFG, 2, 144.0f, 72.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FSR FG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
}

TEST(OverlayFGStatusPublicationTest, TransitionBackToOffClearsPublishedStatus) {
    PerformanceMetrics metrics;

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 120.0f, 60.0f);
    ASSERT_TRUE(metrics.IsFGActive());

    Publish(metrics, false, ce::fg_runtime::RuntimeMode::kOff, 1);

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
}

TEST(OverlayFGStatusPublicationTest, PlannerDrivenPublicationUsesPlannerRuntimeAndActiveState) {
    PerformanceMetrics metrics;

    PublishPlanner(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 3, 180.0f, 60.0f);
    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 3);

    PublishPlanner(metrics, false, ce::fg_runtime::RuntimeMode::kOff, 1, 0.0f, 0.0f);
    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
}

#include <gtest/gtest.h>

#include "hook/d3d12/dx12_overlay_policy.h"
#include "hook/fg/fg_detection.h"
#include "hook/fg/fg_session_state.h"
#include "hook/overlay/overlay_fg_publication_log.h"
#include "hook/overlay/overlay_metrics_publisher.h"
#include "hook/metrics/performance_metrics.h"
#include "source_fragment_reader.h"

void TestStubSetPreferredOverlayFGPublicationState(bool valid, bool active, ce::fg_runtime::RuntimeMode runtimeMode);
void TestStubResetPreferredOverlayFGPublicationState();

namespace {

struct PreferredOverlayPublicationStateGuard {
    ~PreferredOverlayPublicationStateGuard() {
        TestStubResetPreferredOverlayFGPublicationState();
    }
};

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

// W3 (session 20261106_195839) spent 41% of its hook log's bytes on this line, one per factor change.
TEST(FGPublicationLogGateTest, FactorChangesAreMeteredAndStateChangesAlwaysLog) {
    using ce::overlay_metrics::FGPublicationLogGate;
    constexpr int kDLSSFG = static_cast<int>(ce::fg_runtime::RuntimeMode::kDLSSFG);
    constexpr int kFSRFG = static_cast<int>(ce::fg_runtime::RuntimeMode::kFSRFG);
    FGPublicationLogGate gate;
    EXPECT_TRUE(gate.Observe(true, kDLSSFG, 1, 2).log) << "the first publication";
    EXPECT_FALSE(gate.Observe(true, kDLSSFG, 1, 2).log) << "nothing changed";

    constexpr uint32_t kFlips = 2048;
    uint32_t logged = 0;
    uint32_t lastCount = 0;
    for (uint32_t flip = 1; flip <= kFlips; ++flip) {
        const auto verdict = gate.Observe(true, kDLSSFG, 1, flip % 2 == 0 ? 2 : 4);
        EXPECT_EQ(verdict.factorChanges, flip);
        if (verdict.log) {
            ++logged;
            lastCount = verdict.factorChanges;
        }
    }
    EXPECT_EQ(logged, FGPublicationLogGate::kFactorChangeBurst + kFlips / FGPublicationLogGate::kFactorChangeStride);
    EXPECT_EQ(lastCount, kFlips) << "the heartbeat carries the count";

    const auto otherRuntime = gate.Observe(true, kFSRFG, 2, 2);
    EXPECT_TRUE(otherRuntime.log);
    EXPECT_EQ(otherRuntime.factorChanges, 0u);
    EXPECT_TRUE(gate.Observe(false, kFSRFG, 0, 1).log) << "off always shows";
    EXPECT_TRUE(gate.Observe(true, kDLSSFG, 1, 4).log) << "on always shows";
    EXPECT_EQ(gate.Observe(true, kDLSSFG, 1, 2).factorChanges, 1u) << "a new state starts a new count";
}

TEST(OverlayFGStatusPublicationTest, PublishesNoFGAsBaseline) {
    PerformanceMetrics metrics;

    Publish(metrics, false, ce::fg_runtime::RuntimeMode::kOff, 1, 999.0f, 999.0f);

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 0.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 0.0f);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), LabelForMetricType(0));
}

// A menu with DLSS-G enabled and generating nothing: "DLSS FG 2x" with base and output both at 144 (20261104_221303).
TEST(OverlayFGStatusPublicationTest, DLSSGEnabledButIdleReadsOffUntilItGeneratesAgain) {
    using ce::fg_runtime::ApplicationFrameSource;
    using ce::fg_runtime::OutputRatioWindow;
    const auto runFrames = [](uint32_t frames, uint32_t outputsPerFrame) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            for (uint32_t output = 0; output < outputsPerFrame; ++output) {
                g_FGCompat.NoteRuntimeOutput();
            }
            g_FGCompat.NoteApplicationFrame(ApplicationFrameSource::kPresentHook);
        }
    };
    const auto resetIdleProofs = [] {
        g_FGCompat.SetDLSSGHealthIdle(false);
        const bool active = g_FGCompat.IsDLSSFGApiActive();
        g_FGCompat.SetDLSSFGActive(!active);
        g_FGCompat.SetDLSSFGActive(active);
    };
    PerformanceMetrics metrics;
    resetIdleProofs();

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 72.0f);
    EXPECT_TRUE(metrics.IsFGActive());

    g_FGCompat.SetDLSSGHealthIdle(true);
    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 72.0f);
    EXPECT_TRUE(metrics.IsFGActive()) << "the health report alone is what flip-metered generation looks like";

    runFrames(OutputRatioWindow::kWindowFrames, 1);
    ASSERT_TRUE(g_FGCompat.IsDLSSGGenerationIdle());
    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 144.0f);
    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 0.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 0.0f);

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kFSRFG, 2, 144.0f, 72.0f);
    EXPECT_TRUE(metrics.IsFGActive()) << "the DLSS-G proofs say nothing about another runtime";

    runFrames(OutputRatioWindow::kWindowFrames / 4 + 1, 2);
    ASSERT_FALSE(g_FGCompat.IsDLSSGGenerationIdle());
    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 72.0f);
    EXPECT_TRUE(metrics.IsFGActive()) << "generation resumed";
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
    resetIdleProofs();
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

TEST(OverlayFGStatusPublicationTest, InactiveStreamlineNoFGPublishesSameBaselineAsOff) {
    PerformanceMetrics metrics;

    Publish(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 120.0f, 60.0f);
    ASSERT_TRUE(metrics.IsFGActive());

    Publish(metrics, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, 1, 200.0f, 100.0f);

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 0.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 0.0f);
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

TEST(OverlayFGStatusPublicationTest, PlannerDrivenPublicationPrefersVisibleStateOverStalePlan) {
    PreferredOverlayPublicationStateGuard guard;
    PerformanceMetrics metrics;

    PublishPlanner(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 72.0f);
    ASSERT_TRUE(metrics.IsFGActive());
    ASSERT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");

    TestStubSetPreferredOverlayFGPublicationState(true, true, ce::fg_runtime::RuntimeMode::kFSRFG);
    PublishPlanner(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 144.0f, 72.0f);
    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FSR FG");

    TestStubSetPreferredOverlayFGPublicationState(true, false, ce::fg_runtime::RuntimeMode::kOff);
    PublishPlanner(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 3, 180.0f, 60.0f);
    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
}

TEST(OverlayFGStatusPublicationTest, PlannerDrivenPublicationLetsNewerPlannerOffBeatStaleVisibleOnState) {
    PreferredOverlayPublicationStateGuard guard;
    PerformanceMetrics metrics;

    TestStubSetPreferredOverlayFGPublicationState(true, true, ce::fg_runtime::RuntimeMode::kDLSSFG);
    PublishPlanner(metrics, true, ce::fg_runtime::RuntimeMode::kDLSSFG, 2, 120.0f, 60.0f);
    ASSERT_TRUE(metrics.IsFGActive());
    ASSERT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");

    PublishPlanner(metrics, false, ce::fg_runtime::RuntimeMode::kOff, 1, 0.0f, 0.0f);
    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
}

TEST(OverlayFGStatusPublicationSourceTest, DirectX11AndVulkanPublishDetectedStatusBeforeOverlayRendering) {
    const auto projectRoot = std::filesystem::current_path();
    const std::string dx11Source =
        ce::test_source::ReadLogicalSource(projectRoot / "hook" / "d3d11" / "dx11_hook.cpp");
    const std::string vulkanSource =
        ce::test_source::ReadLogicalSource(projectRoot / "hook" / "vulkan_layer" / "layer_overlay.cpp");
    const std::string vulkanPresentSource =
        ce::test_source::ReadLogicalSource(projectRoot / "hook" / "vulkan_layer" / "vulkan_layer.cpp");
    const std::string buildSource = ce::test_source::ReadLogicalSource(projectRoot / "build.py");

    const size_t dx11ProcessFrame =
        dx11Source.find("void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {");
    const size_t dx11Publish = dx11Source.find("PublishDetectedOverlayFGMetrics", dx11ProcessFrame);
    const size_t dx11Overlay = dx11Source.find("ProcessDX11FrameWithOverlayOrdering", dx11ProcessFrame);
    ASSERT_NE(dx11ProcessFrame, std::string::npos);
    const size_t dx11RecordPresent =
        dx11Source.find("RecordPresentForNvidiaSmoothMotion", dx11ProcessFrame);
    ASSERT_NE(dx11Publish, std::string::npos);
    ASSERT_NE(dx11Overlay, std::string::npos);
    ASSERT_NE(dx11RecordPresent, std::string::npos);
    EXPECT_LT(dx11RecordPresent, dx11Publish);
    EXPECT_LT(dx11Publish, dx11Overlay);

    const size_t vulkanPresent =
        vulkanPresentSource.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR");
    const size_t vulkanRecordPresent =
        vulkanPresentSource.find("RecordPresentForNvidiaSmoothMotion", vulkanPresent);
    const size_t vulkanMetricsStart = vulkanPresentSource.find("// Performance metrics for this frame", vulkanPresent);
    ASSERT_NE(vulkanPresent, std::string::npos);
    ASSERT_NE(vulkanRecordPresent, std::string::npos);
    ASSERT_NE(vulkanMetricsStart, std::string::npos);
    EXPECT_LT(vulkanRecordPresent, vulkanMetricsStart);

    const size_t vulkanUpdate = vulkanSource.find("state.metrics->Update(");
    const size_t vulkanPublish = vulkanSource.find("PublishDetectedOverlayFGMetrics", vulkanUpdate);
    const size_t vulkanFenceWait = vulkanSource.find("fp_vkWaitForFences", vulkanUpdate);
    ASSERT_NE(vulkanUpdate, std::string::npos);
    ASSERT_NE(vulkanPublish, std::string::npos);
    ASSERT_NE(vulkanFenceWait, std::string::npos);
    EXPECT_LT(vulkanPublish, vulkanFenceWait);

    EXPECT_NE(buildSource.find("\"overlay_metrics_publisher.cpp\""), std::string::npos);
}

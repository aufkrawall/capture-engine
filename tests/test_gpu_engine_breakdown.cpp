#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "captureengine/app/gpu_engine_breakdown.h"
#include "source_fragment_reader.h"

namespace policy = scan_host::metrics_policy;

namespace {

constexpr uint64_t kThreeD0 = 1;
constexpr uint64_t kThreeD1 = 2;
constexpr uint64_t kEncode = 3;
constexpr uint64_t kCopy = 4;

policy::GpuEngineTypedSample Sample(uint64_t key, policy::GpuEngineType type, uint32_t pid, double value) {
    return {key, type, pid, value};
}

}  // namespace

TEST(GpuEngineBreakdownTest, ClassifiesWindowsEngineInstanceNames) {
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_0_engtype_3D"),
              policy::GpuEngineType::ThreeD);
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_1_engtype_Compute_0"),
              policy::GpuEngineType::Compute);
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_2_engtype_Copy"),
              policy::GpuEngineType::Copy);
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_8_engtype_VideoEncode"),
              policy::GpuEngineType::VideoEncode);
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_7_engtype_VideoDecode"),
              policy::GpuEngineType::VideoDecode);
    EXPECT_EQ(policy::ClassifyGpuEngineType("pid_4321_luid_0x00000000_0x0000BCF3_phys_0_eng_9_engtype_OFA"),
              policy::GpuEngineType::Other);
    EXPECT_EQ(policy::ClassifyGpuEngineType("no engine marker"), policy::GpuEngineType::Other);
}

TEST(GpuEngineBreakdownTest, AddsProcessesWithinAnEngineAndSplitsTheBusiest3dEngine) {
    // Game 70% + CE 20% time-share 3D engine 0; engine 1 is quieter; NVENC 85%.
    const policy::GpuEngineBreakdown breakdown = policy::ResolveGpuEngineBreakdown(
        {Sample(kThreeD0, policy::GpuEngineType::ThreeD, 100, 70.0),
         Sample(kThreeD0, policy::GpuEngineType::ThreeD, 200, 20.0),
         Sample(kThreeD1, policy::GpuEngineType::ThreeD, 100, 95.0 - 90.0),
         Sample(kEncode, policy::GpuEngineType::VideoEncode, 200, 85.0),
         Sample(kCopy, policy::GpuEngineType::Copy, 200, 3.0)},
        100);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::ThreeD)], 90.0);
    EXPECT_DOUBLE_EQ(breakdown.threeDTarget, 70.0);
    EXPECT_DOUBLE_EQ(breakdown.threeDOthers, 20.0);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)], 85.0);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::Copy)], 3.0);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::Compute)], 0.0);
}

TEST(GpuEngineBreakdownTest, ClampsOversubscribedAndInvalidValues) {
    const policy::GpuEngineBreakdown breakdown = policy::ResolveGpuEngineBreakdown(
        {Sample(kThreeD0, policy::GpuEngineType::ThreeD, 100, 80.0),
         Sample(kThreeD0, policy::GpuEngineType::ThreeD, 200, 40.0),
         Sample(kEncode, policy::GpuEngineType::VideoEncode, 200, -5.0)},
        100);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::ThreeD)], 100.0);
    EXPECT_DOUBLE_EQ(breakdown.threeDTarget, 80.0);
    EXPECT_DOUBLE_EQ(breakdown.threeDOthers, 20.0);
    EXPECT_DOUBLE_EQ(breakdown.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)], 0.0);
}

TEST(GpuEngineBreakdownTest, WindowReportsAveragesAndPeaksOnlyWhileAnEncoderRan) {
    policy::GpuEngineWindow window;
    policy::GpuEngineBreakdown first;
    first.busiest[static_cast<size_t>(policy::GpuEngineType::ThreeD)] = 96.0;
    first.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 60.0;
    first.threeDTarget = 90.0;
    first.threeDOthers = 6.0;
    policy::GpuEngineBreakdown second = first;
    second.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 80.0;
    window.Add(first, 1000);
    window.Add(second, 2000);
    EXPECT_FALSE(window.Due(1000 + policy::kGpuEngineWindowMs - 1));
    EXPECT_TRUE(window.Due(1000 + policy::kGpuEngineWindowMs));
    EXPECT_TRUE(window.EncoderActive());
    const std::string line = window.TakeLine(4321);
    EXPECT_NE(line.find("[Metrics] GPU engines polls=2"), std::string::npos) << line;
    EXPECT_NE(line.find("3d=96/96% (target pid 4321=90/90% others=6/6%)"), std::string::npos) << line;
    EXPECT_NE(line.find("videoEncode=70/80%"), std::string::npos) << line;
    EXPECT_EQ(window.Count(), 0u);

    policy::GpuEngineWindow idle;
    idle.Add(policy::GpuEngineBreakdown(), 0);
    EXPECT_FALSE(idle.EncoderActive());
}

TEST(GpuEngineBreakdownTest, HostMetricsLogsTheWindowOnlyWhenAnEncoderRan) {
    const std::string source = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" / "app" /
                                                         "host_metrics.cpp");
    ASSERT_FALSE(source.empty());
    const size_t observe = source.find("ObserveGpuEngineBreakdown(g_HostMetrics, gpuValues, adapter.adapterLuid");
    const size_t active = source.find("const bool encoderActive = state.engineWindow.EncoderActive();");
    const size_t take = source.find("state.engineWindow.TakeLine(targetPid)");
    const size_t log = source.find("if (encoderActive)", take);
    EXPECT_NE(observe, std::string::npos);
    ASSERT_NE(active, std::string::npos);
    EXPECT_LT(active, take);
    EXPECT_LT(take, log);
    // Video engines stay out of the overlay's GPU load.
    EXPECT_NE(source.find("if (!value.videoEngine)"), std::string::npos);
}

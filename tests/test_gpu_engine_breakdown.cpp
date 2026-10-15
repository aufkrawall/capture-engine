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

TEST(GpuEngineBreakdownTest, WindowReportsAveragesPeaksAndSpanOfEncodingPolls) {
    policy::GpuEngineWindow window;
    policy::GpuEngineBreakdown first;
    first.busiest[static_cast<size_t>(policy::GpuEngineType::ThreeD)] = 96.0;
    first.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 60.0;
    first.busiest[static_cast<size_t>(policy::GpuEngineType::Other)] = 12.0;
    first.threeDTarget = 90.0;
    first.threeDOthers = 6.0;
    policy::GpuEngineBreakdown second = first;
    second.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 80.0;
    EXPECT_TRUE(window.Empty());
    window.Add(first, 1000);
    window.Add(second, 2000);
    EXPECT_FALSE(window.Due(1000 + policy::kGpuEngineWindowMs - 1));
    EXPECT_TRUE(window.Due(1000 + policy::kGpuEngineWindowMs));
    const std::string line = window.TakeLine(4321, "window");
    EXPECT_NE(line.find("[Metrics] GPU engines polls=2 spanMs=1000 end=window"), std::string::npos) << line;
    EXPECT_NE(line.find("3d=96/96% (target pid 4321=90/90% others=6/6%)"), std::string::npos) << line;
    EXPECT_NE(line.find("videoEncode=70/80%"), std::string::npos) << line;
    EXPECT_NE(line.find("other=12/12%"), std::string::npos) << line;
    EXPECT_TRUE(window.Empty());
    EXPECT_EQ(window.Count(), 0u);
}

TEST(GpuEngineBreakdownTest, OnlyPollsWithABusyVideoEncodeEngineAreEncodingPolls) {
    policy::GpuEngineBreakdown idle;
    idle.busiest[static_cast<size_t>(policy::GpuEngineType::ThreeD)] = 99.0;
    EXPECT_FALSE(policy::IsEncodingPoll(idle));
    policy::GpuEngineBreakdown encoding = idle;
    encoding.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 1.0;
    EXPECT_TRUE(policy::IsEncodingPoll(encoding));
    // A residual the line would print as 0% is not encoding.
    policy::GpuEngineBreakdown residual = idle;
    residual.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 0.3;
    EXPECT_FALSE(policy::IsEncodingPoll(residual));
    residual.busiest[static_cast<size_t>(policy::GpuEngineType::VideoEncode)] = 0.5;
    EXPECT_FALSE(policy::IsEncodingPoll(residual));
}

TEST(GpuEngineBreakdownTest, HostMetricsFeedsOnlyEncodingPollsAndFlushesTheTail) {
    const std::string source = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" / "app" /
                                                         "host_metrics.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("ObserveGpuEngineBreakdown(g_HostMetrics, gpuValues, adapter.adapterLuid"),
              std::string::npos);
    const size_t change = source.find("TakeLine(state.engineWindowPid, \"targetChange\")");
    const size_t gate = source.find("if (!metrics_policy::IsEncodingPoll(breakdown))");
    const size_t idle = source.find("TakeLine(targetPid, \"encoderIdle\")");
    const size_t add = source.find("state.engineWindow.Add(breakdown, nowMs)");
    const size_t due = source.find("TakeLine(targetPid, \"window\")");
    ASSERT_NE(change, std::string::npos);
    ASSERT_NE(gate, std::string::npos);
    // An idle poll flushes the tail and never enters the averages.
    EXPECT_LT(change, gate);
    EXPECT_LT(gate, idle);
    EXPECT_LT(idle, add);
    EXPECT_LT(add, due);
    // Video engines stay out of the overlay's GPU load.
    EXPECT_NE(source.find("if (!value.videoEngine)"), std::string::npos);
}

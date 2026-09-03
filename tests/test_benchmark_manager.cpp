#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>

#include "benchmark_html_report.h"
#include "benchmark_manager.h"

TEST(BenchmarkStatsTest, EmptyVectorReturnsZeroes) {
    std::vector<float> empty;
    BenchmarkStats stats = BenchmarkManager::CalculateStats(empty);

    EXPECT_FLOAT_EQ(stats.avgFps, 0.0f);
    EXPECT_FLOAT_EQ(stats.maxFps, 0.0f);
    EXPECT_FLOAT_EQ(stats.minFps, 0.0f);
    EXPECT_FLOAT_EQ(stats.onePercentLowFps, 0.0f);
    EXPECT_FLOAT_EQ(stats.zeroPointOnePercentLowFps, 0.0f);
}

TEST(BenchmarkStatsTest, UniformFrameTimesComputeExactFps) {
    // 100 frames at 16.666666 ms = 60 FPS
    std::vector<float> frames(100, 16.666666f);
    BenchmarkStats stats = BenchmarkManager::CalculateStats(frames);

    EXPECT_NEAR(stats.avgFps, 60.0f, 0.1f);
    EXPECT_NEAR(stats.maxFps, 60.0f, 0.1f);
    EXPECT_NEAR(stats.minFps, 60.0f, 0.1f);
    EXPECT_NEAR(stats.onePercentLowFps, 60.0f, 0.1f);
    EXPECT_NEAR(stats.zeroPointOnePercentLowFps, 60.0f, 0.1f);
}

TEST(BenchmarkStatsTest, PercentileCalculationsReflectStutters) {
    // 99 frames at 10.0 ms (100 FPS), 1 frame at 100.0 ms (10 FPS)
    std::vector<float> frames(99, 10.0f);
    frames.push_back(100.0f);

    BenchmarkStats stats = BenchmarkManager::CalculateStats(frames);

    // Max FPS is 1000 / 10 = 100
    EXPECT_NEAR(stats.maxFps, 100.0f, 0.1f);
    // Min FPS is 1000 / 100 = 10
    EXPECT_NEAR(stats.minFps, 10.0f, 0.1f);
    // 1% worst of 100 frames is exactly 1 frame (the 100.0 ms frame) -> 10 FPS
    EXPECT_NEAR(stats.onePercentLowFps, 10.0f, 0.1f);
    EXPECT_NEAR(stats.zeroPointOnePercentLowFps, 10.0f, 0.1f);
    // Average FPS: 100 frames in (99 * 10 + 100) = 1090 ms -> 100000 / 1090 = ~91.74 FPS
    EXPECT_NEAR(stats.avgFps, 91.74f, 0.5f);
}

TEST(BenchmarkSensorSummaryTest, ComputesAvgMaxMinProperly) {
    std::vector<BenchmarkFrameRecord> records;
    BenchmarkFrameRecord r1 = {};
    r1.cpuUsage = 20.0f;
    r1.validMask |= kBenchSensorCpuUsage;
    records.push_back(r1);

    BenchmarkFrameRecord r2 = {};
    r2.cpuUsage = 80.0f;
    r2.validMask |= kBenchSensorCpuUsage;
    records.push_back(r2);

    BenchmarkFrameRecord r3 = {};
    r3.cpuUsage = 50.0f;
    r3.validMask |= kBenchSensorCpuUsage;
    records.push_back(r3);

    BenchmarkSensorSummary summary;
    BenchmarkManager::ComputeSensorSummary(records, summary, kBenchSensorCpuUsage, &BenchmarkFrameRecord::cpuUsage);

    EXPECT_TRUE(summary.valid);
    EXPECT_FLOAT_EQ(summary.avg, 50.0f);
    EXPECT_FLOAT_EQ(summary.max, 80.0f);
    EXPECT_FLOAT_EQ(summary.min, 20.0f);
}

TEST(BenchmarkHtmlReportTest, GeneratesSelfContainedInteractiveReport) {
    BenchmarkResults results;
    results.profileName = "TestGame";
    results.executableName = "TestGame.exe";
    results.cpuName = "Test CPU 16-Core";
    results.gpuName = "Test GPU 16GB";
    results.timestampStr = "2026-09-04 12:00:00";
    results.durationSeconds = 10.0f;
    results.totalFrames = 600;
    results.ramTotalGb = 32.0f;
    results.vramTotalGb = 16.0f;

    results.presentationStats.avgFps = 60.0f;
    results.presentationStats.maxFps = 62.0f;
    results.presentationStats.minFps = 45.0f;
    results.presentationStats.onePercentLowFps = 52.0f;
    results.presentationStats.zeroPointOnePercentLowFps = 48.0f;

    results.displayStats.avgFps = 59.8f;
    results.displayStats.maxFps = 61.5f;
    results.displayStats.minFps = 44.0f;
    results.displayStats.onePercentLowFps = 51.0f;
    results.displayStats.zeroPointOnePercentLowFps = 47.0f;

    results.cpuUsage = {42.0f, 65.0f, 25.0f, true};
    results.gpuUsage = {95.0f, 99.0f, 80.0f, true};

    // Add samples
    for (int i = 0; i < 60; ++i) {
        BenchmarkFrameRecord r = {};
        r.timeSeconds = static_cast<float>(i) * 0.16f;
        r.presentationFrameTimeMs = 16.6f;
        r.displayFrameTimeMs = 16.7f;
        r.presentationFps = 60.2f;
        r.displayFps = 59.8f;
        r.cpuUsage = 40.0f + static_cast<float>(i % 10);
        r.gpuUsage = 90.0f + static_cast<float>(i % 10);
        r.validMask = kBenchSensorCpuUsage | kBenchSensorGpuUsage;
        results.records.push_back(r);
    }

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "ce_test_benchmarks";
    std::filesystem::create_directories(tempDir);

    std::string htmlPath = SaveBenchmarkHtmlReport(results, tempDir.string());
    ASSERT_FALSE(htmlPath.empty());
    EXPECT_TRUE(std::filesystem::exists(htmlPath));

    // Read and verify HTML content
    std::ifstream file(htmlPath);
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Verify key requirements from user prompt:
    EXPECT_NE(content.find("TestGame Benchmark Report"), std::string::npos);
    EXPECT_NE(content.find("Classic Frame Start (Presentation)"), std::string::npos);
    EXPECT_NE(content.find("msBetweenDisplayChange (Display Cadence)"), std::string::npos);
    EXPECT_NE(content.find("Average FPS"), std::string::npos);
    EXPECT_NE(content.find("1% Low FPS"), std::string::npos);
    EXPECT_NE(content.find("0.1% Low FPS"), std::string::npos);
    EXPECT_NE(content.find("Max FPS"), std::string::npos);
    EXPECT_NE(content.find("chartCanvas"), std::string::npos);
    EXPECT_NE(content.find("hudTooltip"), std::string::npos);
    EXPECT_NE(content.find("CPU Total Load"), std::string::npos);
    EXPECT_NE(content.find("GPU Usage"), std::string::npos);

    file.close();
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

TEST(BenchmarkManagerTest, StateTransitionsFollowHotkeyCycle) {
    BenchmarkManager& mgr = BenchmarkManager::Get();
    BenchmarkConfig cfg;
    cfg.startDelaySeconds = 0;  // Immediate recording
    cfg.durationSeconds = 0;    // Manual stop
    mgr.Init(cfg, "TestGame");

    // Initial state
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Idle);

    // Press hotkey -> Recording
    mgr.Toggle();
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Recording);
    EXPECT_TRUE(mgr.IsActiveOrShowingResults());

    // Press hotkey again -> Stops recording & shows results (or idle if 0 frames)
    mgr.Toggle();
    // Since 0 frames were recorded in unit test, it returns to Idle cleanly
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Idle);
}

TEST(BenchmarkManagerTest, RunningStatsComputedDuringRecording) {
    BenchmarkManager& mgr = BenchmarkManager::Get();
    BenchmarkConfig cfg;
    cfg.startDelaySeconds = 0;
    cfg.durationSeconds = 0;
    mgr.Init(cfg, "TestGame");

    mgr.Toggle();
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Recording);

    SystemMetrics metrics = {};
    for (int i = 0; i < 20; ++i) {
        mgr.OnFrame(i * 16666, 16.666f, 16.666f, metrics, nullptr);
    }

    BenchmarkStats running = mgr.GetRunningStats(false);
    EXPECT_NEAR(running.avgFps, 60.0f, 1.0f);

    mgr.Toggle();
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Results);

    BenchmarkStats finalStats = mgr.GetRunningStats(false);
    EXPECT_NEAR(finalStats.avgFps, 60.0f, 1.0f);

    mgr.Toggle();
    EXPECT_EQ(mgr.GetState(), BenchmarkState::Idle);
}

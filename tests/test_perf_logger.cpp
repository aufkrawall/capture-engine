#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "hook/metrics/perf_logger.h"
#include "scoped_test_directory.h"

namespace {
struct ShutdownLogger {
    ~ShutdownLogger() {
        PerfLogger::Get().StopService();
        PerfLogger::Get().Shutdown();
    }
};

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
}  // namespace

TEST(PerfLoggerTest, PerfMetricsCsvFlushPolicyKeepsEarlyAndPeriodicFramesDurable) {
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(1));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(8));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(32));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(64));

    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(9));
    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(31));
    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(63));
}

TEST(PerfLoggerTest, ForceRebindFinalizesOldCsvAndStartsFreshSequence) {
    namespace fs = std::filesystem;
    ce::test_support::ScopedTestDirectory scratch("ce_perf_rebind_test");
    const fs::path& dir = scratch.path();
    const fs::path firstCsv = dir / "perf_metrics_first.csv";
    const fs::path secondCsv = dir / "perf_metrics_second.csv";

    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();  // Finalize any state a previous test may have left open.
    ShutdownLogger shutdown;

    FrameMetrics metrics{};
    metrics.qpcUs = PerfLogger::GetQpcUs();

    logger.Init(firstCsv.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);

    // A plain Init while a file is open must keep the original session file.
    logger.Init(secondCsv.string().c_str());
    EXPECT_TRUE(logger.IsEnabled());
    EXPECT_TRUE(fs::exists(firstCsv));
    EXPECT_FALSE(fs::exists(secondCsv));

    // Resident-hook reactivation force-rebinds: finalize the old CSV and open
    // the new session path with a fresh frame sequence.
    logger.Init(secondCsv.string().c_str(), true);
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);
    logger.Shutdown();

    EXPECT_TRUE(fs::exists(secondCsv));
    std::ifstream first(firstCsv);
    std::ifstream second(secondCsv);
    std::stringstream firstText;
    std::stringstream secondText;
    firstText << first.rdbuf();
    secondText << second.rdbuf();

    // Both files carry the header and restart their row numbering at 1.
    EXPECT_NE(firstText.str().find("frame,qpc_us,total_us"), std::string::npos);
    EXPECT_NE(firstText.str().find("\n1,"), std::string::npos);
    EXPECT_NE(secondText.str().find("frame,qpc_us,total_us"), std::string::npos);
    EXPECT_NE(secondText.str().find("\n1,"), std::string::npos);

    first.close();
    second.close();
}

TEST(PerfLoggerTest, ShutdownThenInitStartsANewFilesFrameSequence) {
    ce::test_support::ScopedTestDirectory scratch("ce_perf_reopen_test");
    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();
    ShutdownLogger shutdown;
    const auto first = scratch.path() / "first.csv";
    const auto second = scratch.path() / "second.csv";
    FrameMetrics metrics{};
    metrics.qpcUs = PerfLogger::GetQpcUs();
    logger.Init(first.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);
    logger.LogFrame(metrics);
    logger.Shutdown();
    logger.Init(second.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);
    logger.Shutdown();
    std::ifstream file(second);
    std::stringstream contents;
    contents << file.rdbuf();
    EXPECT_NE(contents.str().find("\n1,"), std::string::npos);
    EXPECT_EQ(contents.str().find("\n3,"), std::string::npos);
}

// Formatting a row on the game's thread cost it a few microseconds a frame (Borderlands 2,
// 20261103_194318). Once the hook thread services the queue, LogFrame only queues.
TEST(PerfLoggerTest, ServicedRowsWaitForTheHookThreadAndKeepTheirOrder) {
    ce::test_support::ScopedTestDirectory scratch("ce_perf_service_test");
    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();
    ShutdownLogger shutdown;
    const auto csv = scratch.path() / "serviced.csv";
    logger.Init(csv.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());

    logger.Service();
    FrameMetrics metrics{};
    for (int i = 1; i <= 3; ++i) {
        metrics.qpcUs = 1'000'000 + i * 1000;
        metrics.totalUs = 100 + i;
        logger.LogFrame(metrics);
    }
    EXPECT_EQ(logger.GetFrameCount(), 0u) << "the presenting thread wrote rows itself";
    logger.Service();
    EXPECT_EQ(logger.GetFrameCount(), 3u);

    // Without a servicer LogFrame writes again, after anything still queued.
    logger.Service();
    metrics.qpcUs = 1'000'000 + 4 * 1000;
    metrics.totalUs = 104;
    logger.LogFrame(metrics);
    logger.StopService();
    EXPECT_EQ(logger.GetFrameCount(), 4u);
    metrics.qpcUs = 1'000'000 + 5 * 1000;
    metrics.totalUs = 105;
    logger.LogFrame(metrics);
    EXPECT_EQ(logger.GetFrameCount(), 5u);
    logger.Shutdown();

    const std::string text = ReadAll(csv);
    size_t previous = 0;
    for (int i = 1; i <= 5; ++i) {
        const std::string row = "\n" + std::to_string(i) + "," + std::to_string(1'000'000 + i * 1000) + "," +
                                std::to_string(100 + i) + ",";
        const size_t at = text.find(row);
        ASSERT_NE(at, std::string::npos) << row;
        EXPECT_GT(at, previous) << row;
        previous = at;
    }
}

TEST(PerfLoggerTest, AFullQueueIsWrittenByThePresentingThread) {
    ce::test_support::ScopedTestDirectory scratch("ce_perf_full_queue_test");
    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();
    ShutdownLogger shutdown;
    const auto csv = scratch.path() / "full.csv";
    logger.Init(csv.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());
    logger.Service();
    FrameMetrics metrics{};
    metrics.qpcUs = PerfLogger::GetQpcUs();
    for (size_t i = 1; i < PerfLogger::kQueuedRows; ++i)
        logger.LogFrame(metrics);
    EXPECT_EQ(logger.GetFrameCount(), 0u);
    logger.LogFrame(metrics);
    EXPECT_EQ(logger.GetFrameCount(), PerfLogger::kQueuedRows) << "a stalled hook thread must not hold rows forever";
}

TEST(PerfLoggerTest, TerminationAndShutdownWriteTheQueuedRows) {
    ce::test_support::ScopedTestDirectory scratch("ce_perf_queue_exit_test");
    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();
    ShutdownLogger shutdown;
    const auto terminated = scratch.path() / "terminated.csv";
    const auto shutDown = scratch.path() / "shutdown.csv";
    FrameMetrics metrics{};
    metrics.qpcUs = PerfLogger::GetQpcUs();

    logger.Init(terminated.string().c_str());
    logger.Service();
    logger.LogFrame(metrics);
    logger.LogFrame(metrics);
    logger.FlushForTermination("test");
    EXPECT_FALSE(logger.IsEnabled());
    EXPECT_NE(ReadAll(terminated).find("\n2,"), std::string::npos);

    logger.Init(shutDown.string().c_str());
    logger.LogFrame(metrics);
    logger.Shutdown();
    EXPECT_NE(ReadAll(shutDown).find("\n1,"), std::string::npos);
}

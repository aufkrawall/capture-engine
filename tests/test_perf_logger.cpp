#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "hook/metrics/perf_logger.h"
#include "scoped_test_directory.h"

namespace {
struct ShutdownLogger {
    ~ShutdownLogger() { PerfLogger::Get().Shutdown(); }
};
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

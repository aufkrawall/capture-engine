#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "hook/metrics/perf_logger.h"
#include "scoped_test_directory.h"

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
    struct ShutdownLogger {
        ~ShutdownLogger() { PerfLogger::Get().Shutdown(); }
    } shutdown;

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

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "../captureengine/logger_service_policy.h"

#include "source_fragment_reader.h"

TEST(LoggerServicePolicyTest, SessionDiscoveryPathOverridesExecutableLogsFallback) {
    char discoveryPath[64] = {};
    std::strcpy(discoveryPath, "C:\\captureengine\\logs\\20260717_133007");

    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(discoveryPath, sizeof(discoveryPath),
                                                                "C:\\captureengine\\logs"),
              "C:\\captureengine\\logs\\20260717_133007");
}

TEST(LoggerServicePolicyTest, MissingOrUnterminatedDiscoveryPathUsesFallback) {
    char emptyPath[8] = {};
    char unterminatedPath[8];
    std::memset(unterminatedPath, 'x', sizeof(unterminatedPath));

    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(emptyPath, sizeof(emptyPath), "fallback"),
              "fallback");
    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(unterminatedPath, sizeof(unterminatedPath),
                                                                "fallback"),
              "fallback");
}

TEST(LoggerServicePolicyTest, LogFilenameCannotEscapeSessionDirectory) {
    EXPECT_TRUE(logger_service_policy::IsSafeLogFilename("hook_debug.log"));
    EXPECT_TRUE(logger_service_policy::IsSafeLogFilename("vulkan-layer.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("..\\outside.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("C:outside.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("../outside.log"));
}

// A full ring means the producer has already begun diverting into a fallback
// that drops on lock contention, so it must be drained again immediately rather
// than after a nap - sleeping there guarantees the next burst overflows too.
// Session `20260921_183446` overflowed 1091 times across two UE5 titles.
TEST(LoggerServicePolicyTest, SaturatedLogRingIsDrainedAgainImmediately) {
    EXPECT_EQ(logger_service_policy::SelectLogDrainWaitMs(true, true, true), 0u);
    EXPECT_EQ(logger_service_policy::SelectLogDrainWaitMs(true, false, false), 0u);
}

TEST(LoggerServicePolicyTest, UnsaturatedLogDrainKeepsItsPreviousPacing) {
    EXPECT_EQ(logger_service_policy::SelectLogDrainWaitMs(false, true, true), 100u);
    EXPECT_EQ(logger_service_policy::SelectLogDrainWaitMs(false, false, true), 250u);
    EXPECT_EQ(logger_service_policy::SelectLogDrainWaitMs(false, false, false), 1000u);
}

// The drop paths must stay accounted for: a gap in the per-process sequence
// numbers has to have a line in the log explaining it.
TEST(LoggerServicePolicySourceTest, EveryDroppedLogLineIsCounted) {
    const std::string hookCommon =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook/common/hook_common.cpp");
    ASSERT_FALSE(hookCommon.empty());

    const size_t tryLock = hookCommon.find("if (!lock.try_lock())");
    ASSERT_NE(tryLock, std::string::npos);
    const size_t count = hookCommon.find("s_DroppedFileLogs.fetch_add", tryLock);
    const size_t give_up = hookCommon.find("return;", tryLock);
    ASSERT_NE(count, std::string::npos);
    ASSERT_NE(give_up, std::string::npos);
    EXPECT_LT(count, give_up) << "the contended file-log path must count what it gives up on";
    EXPECT_NE(hookCommon.find("s_DroppedFileLogs.exchange(0"), std::string::npos)
        << "the counted drops must be reported by the next writer";

    const std::string service =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine/logger_service.cpp");
    ASSERT_FALSE(service.empty());
    EXPECT_NE(service.find("logs.overflowCount.load"), std::string::npos)
        << "overflowCount existed from the start and nothing ever read it";
    EXPECT_NE(service.find("SelectLogDrainWaitMs"), std::string::npos);
}

// A game that exits through TerminateProcess never runs CRT static
// destructors, and PerfLogger::Shutdown is reachable only from one. Talos and
// RoboCop in session `20260921_183446` both left perf_metrics_*.csv cut off
// mid-row for exactly that reason, so the termination hooks finalize it.
TEST(LoggerServicePolicySourceTest, PerfCsvIsFinalizedOnHardProcessExit) {
    const std::string hooks =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook/main_fatal_hooks.cpp");
    ASSERT_FALSE(hooks.empty());

    for (const char* exitPath : {"TerminateProcess", "ExitProcess", "RtlExitUserProcess", "NtTerminateProcess"}) {
        const std::string expected = std::string("FinalizeSessionArtifactsBeforeTermination(\"") + exitPath + "\"";
        EXPECT_NE(hooks.find(expected), std::string::npos) << exitPath << " must finalize CE's session artifacts";
    }

    // The dump path deliberately returns early for an ordinary exit code, so the
    // flush must not be nested inside it.
    const size_t flush = hooks.find("void FinalizeSessionArtifactsBeforeTermination(");
    ASSERT_NE(flush, std::string::npos);
    EXPECT_NE(hooks.find("PerfLogger::Get().FlushForTermination", flush), std::string::npos);

    const std::string perf =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook/common/perf_logger.cpp");
    ASSERT_FALSE(perf.empty());
    const size_t impl = perf.find("void PerfLogger::FlushForTermination(");
    ASSERT_NE(impl, std::string::npos);
    // Never block an exit path: other threads are still live and may hold it.
    EXPECT_NE(perf.find("std::try_to_lock", impl), std::string::npos);
    EXPECT_NE(perf.find("fflush(file_)", impl), std::string::npos);
}

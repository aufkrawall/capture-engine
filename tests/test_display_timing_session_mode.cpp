#include <gtest/gtest.h>
#include "captureengine/display_timing/display_timing_etw.h"
#include "tests/source_fragment_reader.h"
#include <filesystem>

TEST(DisplayTimingSessionModeTest, OtherRealtimeCollectorsCannotSuppressOurEvents) {
    // Windows can return success from startup/enablement yet suppress our writes when another
    // session rejects them. The live baseline/independent/baseline probe reproduces this loss.
    auto properties = display_timing_etw::MakeProperties(L"CE_Test_DisplayTiming");
    EXPECT_NE(properties.Get()->LogFileMode & EVENT_TRACE_INDEPENDENT_SESSION_MODE, 0u);
    EXPECT_NE(properties.Get()->LogFileMode & EVENT_TRACE_REAL_TIME_MODE, 0u);
}

TEST(DisplayTimingSessionModeTest, RealtimeSessionUsesMillisecondFlushTimer) {
    auto properties = display_timing_etw::MakeProperties(L"CE_Test_DisplayTiming");
    EXPECT_NE(properties.Get()->LogFileMode & EVENT_TRACE_USE_MS_FLUSH_TIMER, 0u);
    EXPECT_EQ(properties.Get()->FlushTimer, 1u);
}

TEST(DisplayTimingSessionModeTest, ServiceAndLocalSessionCreationUseTheSameProperties) {
    const auto root = std::filesystem::current_path();
    for (const char* path : {"elevationservice/service_trace.cpp",
                              "captureengine/display_timing/display_timing_session_reclaim.cpp"}) {
        const auto source = ce::test_source::ReadFile(root / path);
        ASSERT_FALSE(source.empty()) << path;
        EXPECT_NE(source.find("MakeProperties("), std::string::npos) << path;
        EXPECT_NE(source.find("StartTraceW("), std::string::npos) << path;
        // A second hand-built mode would diverge from the shared session contract.
        EXPECT_EQ(source.find("LogFileMode ="), std::string::npos) << path;
    }
}

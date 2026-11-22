#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

#include "mediaengine/engine/av_log_open_capture.h"
#include "mediaengine/engine/mediaengine.h"

namespace {

std::mutex g_logMutex;
std::vector<std::string> g_emittedLogs;

void CollectMediaLog(const char* message) {
    if (!message) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_emittedLogs.emplace_back(message);
}

class AvLogOpenCaptureTest : public ::testing::Test {
protected:
    int savedAvLogLevel = AV_LOG_INFO;

    void SetUp() override {
        savedAvLogLevel = av_log_get_level();
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_emittedLogs.clear();
        }
        MediaEngine_SetLogCallback(CollectMediaLog);
        av_log_set_level(AV_LOG_INFO);
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_emittedLogs.clear();
        }
    }

    void TearDown() override {
        av_log_set_level(savedAvLogLevel);
        av_log_set_callback(av_log_default_callback);
        MediaEngine_SetLogCallback(nullptr);
    }

    static std::vector<std::string> SnapshotEmittedLogs() {
        std::lock_guard<std::mutex> lock(g_logMutex);
        return g_emittedLogs;
    }
};

TEST_F(AvLogOpenCaptureTest, CapturesAvLogDebugLinesWithoutEmittingToNormalLog) {
    {
        ce::media::ScopedAvOpenDebugCapture capture;
        av_log(nullptr, AV_LOG_DEBUG, "AMFEncoderAV1Impl: PreAnalysis unsupported for P010\n");
        av_log(nullptr, AV_LOG_INFO, "AMF via D3D11.\n");

        ASSERT_EQ(capture.Lines().size(), 1u);
        EXPECT_NE(capture.Lines().front().find("PreAnalysis unsupported for P010"), std::string::npos);
        EXPECT_EQ(capture.DroppedCount(), 0u);
    }

    const std::vector<std::string> logs = SnapshotEmittedLogs();
    bool sawInfo = false;
    bool sawDebug = false;
    for (const std::string& line : logs) {
        if (line.find("AMF via D3D11.") != std::string::npos) {
            sawInfo = true;
        }
        if (line.find("PreAnalysis unsupported for P010") != std::string::npos) {
            sawDebug = true;
        }
    }
    EXPECT_TRUE(sawInfo);
    EXPECT_FALSE(sawDebug);
}

TEST_F(AvLogOpenCaptureTest, KeepsNewestLinesCountsDroppedEntriesAndTruncatesTail) {
    ce::media::ScopedAvOpenDebugCapture capture(3);
    for (int i = 1; i <= 5; ++i) {
        av_log(nullptr, AV_LOG_DEBUG, "amf-line-%d\n", i);
    }

    ASSERT_EQ(capture.Lines().size(), 3u);
    EXPECT_EQ(capture.DroppedCount(), 2u);
    EXPECT_EQ(capture.Lines()[0], "amf-line-3");
    EXPECT_EQ(capture.Lines()[1], "amf-line-4");
    EXPECT_EQ(capture.Lines()[2], "amf-line-5");
    EXPECT_EQ(capture.LastLineTruncated(160), "amf-line-5");
    EXPECT_EQ(capture.LastLineTruncated(8), "amf-l...");
}

TEST_F(AvLogOpenCaptureTest, RedactsStreamEndpointsAndIsolatesOtherThreadsAndNestedScopes) {
    ce::media::ScopedAvOpenDebugCapture outer(8);
    av_log(nullptr, AV_LOG_DEBUG, "endpoint rtmp://live.example/app/secret_stream_key\n");

    std::thread otherThread([] { av_log(nullptr, AV_LOG_DEBUG, "from-other-thread\n"); });
    otherThread.join();

    {
        ce::media::ScopedAvOpenDebugCapture inner(8);
        av_log(nullptr, AV_LOG_DEBUG, "inner-only-line\n");
        ASSERT_EQ(inner.Lines().size(), 1u);
        EXPECT_EQ(inner.Lines().front(), "inner-only-line");
    }

    av_log(nullptr, AV_LOG_DEBUG, "outer-after-inner\n");

    ASSERT_EQ(outer.Lines().size(), 2u);
    EXPECT_EQ(outer.Lines()[0].find("secret_stream_key"), std::string::npos);
    EXPECT_EQ(outer.Lines()[1], "outer-after-inner");
}

}  // namespace

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadVideoEncoderSource() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "mediaengine" / "video_encoder.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(VideoEncoderSourceTest, RealTimeGpuPathsNeverUseSleepRetries) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("Sleep(2)"), std::string::npos);
    EXPECT_NE(source.find("if (FAILED(hr) && hasSharedAlt)"), std::string::npos);
    EXPECT_NE(source.find("dupTexAlt.addressof()"), std::string::npos);
    EXPECT_EQ(source.find("Sleep(1)"), std::string::npos);
    EXPECT_NE(source.find("lastFrameDeferred.store(true"), std::string::npos);
    EXPECT_NE(source.find("Frame-ring publication uses release/acquire ordering"), std::string::npos);
}

TEST(VideoEncoderSourceTest, PostMuxProbeNeverOutlivesMediaEngineCode) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("std::thread probeThread"), std::string::npos);
    EXPECT_EQ(source.find("probeFuture.wait_for"), std::string::npos);
    EXPECT_NE(source.find("probeCtx->interrupt_callback.callback = PostMuxProbeInterrupt"), std::string::npos);
    EXPECT_NE(source.find("kPostMuxProbeMaxPackets"), std::string::npos);
    EXPECT_NE(source.find("Run on the already-owned writer/finalizer thread"), std::string::npos);
}

TEST(VideoEncoderSourceTest, CaptureSourceTransitionDropsEveryRepeatCacheLayer) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t reset = source.find("void VideoEncoder::ResetRepeatFrameCache()");
    ASSERT_NE(reset, std::string::npos);
    const size_t end = source.find("bool VideoEncoder::WasLastFrameDeferred", reset);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(reset, end - reset);
    EXPECT_NE(body.find("repeatFrameTexture->Release()"), std::string::npos);
    EXPECT_NE(body.find("InvalidateRepeatSourceFrameTexture()"), std::string::npos);
    EXPECT_NE(body.find("InvalidateRepeatPacketCache()"), std::string::npos);
}

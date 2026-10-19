#include <gtest/gtest.h>

#include <string>

#include "test_config_shared.h"
#include "video_encoder_test_access.h"

// VideoEncoder::Init runs again for every recording's first frame and on a
// format-mode change; it used to allocate new format/codec contexts without
// releasing the previous ones.

namespace {

VideoConfig DefaultVideoConfig() {
    const std::string path = MakeTestPath("video_encoder_reinit.ini");
    WriteTextFile(path, "[Video]\n");
    AppConfig config;
    LoadConfig(path, config);
    DeleteFileA(path.c_str());
    return config.video;
}

// A buffer whose release is counted, so a test sees when FFmpeg frees the
// context holding it.
AVBufferRef* CountedBuffer(int* releases) {
    constexpr size_t kSize = 16;
    auto* data = static_cast<uint8_t*>(av_mallocz(kSize));
    if (!data)
        return nullptr;
    AVBufferRef* buffer = av_buffer_create(
        data, kSize,
        [](void* opaque, uint8_t* bytes) {
            ++*static_cast<int*>(opaque);
            av_free(bytes);
        },
        releases, 0);
    if (!buffer)
        av_free(data);
    return buffer;
}

TEST(VideoEncoderReinitTest, FirstFrameReinitializationReleasesThePreviousContextsOnce) {
    const VideoConfig config = DefaultVideoConfig();
    int codecReleases = 0;
    int formatReleases = 0;
    {
        VideoEncoder encoder;
        ASSERT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
        AVCodecContext* oldCodec = VideoEncoderTestAccess::CodecContext(encoder);
        AVFormatContext* oldFormat = VideoEncoderTestAccess::FormatContext(encoder);
        ASSERT_NE(oldCodec, nullptr);
        ASSERT_NE(oldFormat, nullptr);
        // avcodec_free_context releases hw_device_ctx; avformat_free_context
        // releases each stream's attached picture.
        oldCodec->hw_device_ctx = CountedBuffer(&codecReleases);
        ASSERT_NE(oldCodec->hw_device_ctx, nullptr);
        // A stream left by an open that failed after creating it.
        AVStream* oldStream = avformat_new_stream(oldFormat, nullptr);
        ASSERT_NE(oldStream, nullptr);
        oldStream->attached_pic.buf = CountedBuffer(&formatReleases);
        ASSERT_NE(oldStream->attached_pic.buf, nullptr);
        oldStream->attached_pic.data = oldStream->attached_pic.buf->data;
        oldStream->attached_pic.size = static_cast<int>(oldStream->attached_pic.buf->size);
        VideoEncoderTestAccess::SetStream(encoder, oldStream);

        ASSERT_TRUE(VideoEncoderTestAccess::InitializeFirstInjectFrame(encoder, 64, 32));
        EXPECT_EQ(codecReleases, 1);
        EXPECT_EQ(formatReleases, 1);
        EXPECT_EQ(VideoEncoderTestAccess::Stream(encoder), nullptr);
        EXPECT_NE(VideoEncoderTestAccess::CodecContext(encoder), nullptr);
        EXPECT_NE(VideoEncoderTestAccess::FormatContext(encoder), nullptr);
    }
    // The destructor frees only the replacements: nothing is released twice.
    EXPECT_EQ(codecReleases, 1);
    EXPECT_EQ(formatReleases, 1);
}

TEST(VideoEncoderReinitTest, InitRefusesAnOpenEncoderAndKeepsItsContexts) {
    const VideoConfig config = DefaultVideoConfig();
    VideoEncoder encoder;
    ASSERT_TRUE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
    AVCodecContext* codec = VideoEncoderTestAccess::CodecContext(encoder);
    AVFormatContext* format = VideoEncoderTestAccess::FormatContext(encoder);

    VideoEncoderTestAccess::SetInitDone(encoder, true);  // as after a real codec open
    EXPECT_FALSE(encoder.Init(config, 0, 0, config.fps, [](AVPacket*) {}));
    EXPECT_EQ(VideoEncoderTestAccess::CodecContext(encoder), codec);
    EXPECT_EQ(VideoEncoderTestAccess::FormatContext(encoder), format);
    VideoEncoderTestAccess::SetInitDone(encoder, false);
}

}  // namespace

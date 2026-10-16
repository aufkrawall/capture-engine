#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "mediaengine/video/encoder_input_retention.h"
#include "source_fragment_reader.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

// FFmpeg's Media Foundation wrapper unreferences the input frame right after
// handing it to the asynchronous MFT, so the pool could recycle a texture the
// encoder has not read yet. CE holds the frame until its packet returns.

namespace {

using ce::media::EncoderInputRetention;

AVFrame* MakeFrame(int64_t pts) {
    AVFrame* frame = av_frame_alloc();
    frame->format = 0;  // AV_PIX_FMT_YUV420P
    frame->width = 16;
    frame->height = 16;
    frame->pts = pts;
    if (av_frame_get_buffer(frame, 0) < 0)
        av_frame_free(&frame);
    return frame;
}

TEST(EncoderInputRetentionTest, OnlyMediaFoundationReleasesItsInputOnSubmit) {
    EXPECT_TRUE(ce::media::EncoderReleasesInputOnSubmit("h264_mf"));
    EXPECT_TRUE(ce::media::EncoderReleasesInputOnSubmit("hevc_mf"));
    EXPECT_TRUE(ce::media::EncoderReleasesInputOnSubmit("av1_mf"));
    EXPECT_FALSE(ce::media::EncoderReleasesInputOnSubmit("av1_nvenc"));
    EXPECT_FALSE(ce::media::EncoderReleasesInputOnSubmit("hevc_amf"));
    EXPECT_FALSE(ce::media::EncoderReleasesInputOnSubmit("h264_qsv"));
    EXPECT_FALSE(ce::media::EncoderReleasesInputOnSubmit(nullptr));
}

TEST(EncoderInputRetentionTest, FrameStaysReferencedUntilItsPacketReturns) {
    AVFrame* frame = MakeFrame(7);
    ASSERT_NE(frame, nullptr);
    EncoderInputRetention retention;
    EXPECT_EQ(retention.Retain(frame), 0u);
    // The submitter drops its own reference, as the encoder wrapper does.
    AVBufferRef* buffer = av_buffer_ref(frame->buf[0]);
    av_frame_free(&frame);
    EXPECT_EQ(av_buffer_get_ref_count(buffer), 2);  // ours + the retention's

    EXPECT_FALSE(retention.ReleaseForPacket(8));  // another frame's packet
    EXPECT_EQ(av_buffer_get_ref_count(buffer), 2);
    EXPECT_TRUE(retention.ReleaseForPacket(7));
    EXPECT_EQ(av_buffer_get_ref_count(buffer), 1);
    EXPECT_EQ(retention.Size(), 0u);
    av_buffer_unref(&buffer);
}

TEST(EncoderInputRetentionTest, ReorderedPacketsReleaseTheirOwnFrames) {
    EncoderInputRetention retention;
    for (int64_t pts = 0; pts < 3; ++pts) {
        AVFrame* frame = MakeFrame(pts);
        ASSERT_NE(frame, nullptr);
        retention.Retain(frame);
        av_frame_free(&frame);
    }
    // B-frames: packets arrive in decode order 0, 2, 1.
    EXPECT_TRUE(retention.ReleaseForPacket(0));
    EXPECT_TRUE(retention.ReleaseForPacket(2));
    EXPECT_EQ(retention.Size(), 1u);
    EXPECT_TRUE(retention.ReleaseForPacket(1));
    EXPECT_EQ(retention.Size(), 0u);
}

TEST(EncoderInputRetentionTest, UnmatchedFramesAreBoundedAndReleasedAtTeardown) {
    EncoderInputRetention retention;
    size_t forced = 0;
    for (int64_t pts = 0; pts < static_cast<int64_t>(EncoderInputRetention::kMaxRetainedFrames) + 3; ++pts) {
        AVFrame* frame = MakeFrame(pts);
        ASSERT_NE(frame, nullptr);
        forced += retention.Retain(frame);
        av_frame_free(&frame);
    }
    EXPECT_EQ(forced, 3u);
    EXPECT_EQ(retention.Size(), EncoderInputRetention::kMaxRetainedFrames);
    EXPECT_FALSE(retention.ReleaseForPacket(0));  // the oldest went first
    EXPECT_EQ(retention.ReleaseAll(), EncoderInputRetention::kMaxRetainedFrames);
    EXPECT_EQ(retention.Size(), 0u);
}

TEST(EncoderInputRetentionTest, EncoderRetainsOnSubmitAndReleasesOnPacket) {
    const std::string send = ce::test_source::ReadFile(std::filesystem::current_path() / "mediaengine" / "video" /
                                                       "video_encoder_send.cpp");
    const std::string gate = ce::test_source::ReadFile(std::filesystem::current_path() / "mediaengine" / "video" /
                                                       "video_encoder_submit_gate.cpp");
    ASSERT_FALSE(send.empty());
    ASSERT_FALSE(gate.empty());
    const size_t submit = send.find("avcodec_send_frame(codecCtx, frame)");
    const size_t retain = send.find("RetainEncoderInputFrame(frame)", submit);
    const size_t receive = send.find("avcodec_receive_packet(codecCtx, pkt)");
    const size_t release = send.find("encoderInputRetention.ReleaseForPacket(packetKey)", receive);
    ASSERT_NE(submit, std::string::npos);
    EXPECT_NE(retain, std::string::npos);
    EXPECT_NE(release, std::string::npos);
    EXPECT_NE(send.find("EncoderReleasesInputOnSubmit(savedConfig.encoder.c_str())"), std::string::npos);
    const size_t releaseContexts = gate.find("void VideoEncoder::ReleaseEncoderFramesContexts()");
    ASSERT_NE(releaseContexts, std::string::npos);
    EXPECT_NE(gate.find("encoderInputRetention.ReleaseAll()", releaseContexts), std::string::npos);
}

}  // namespace

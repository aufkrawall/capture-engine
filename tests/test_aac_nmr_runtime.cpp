// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {
struct CodecDelete {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct FrameDelete {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketDelete {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
}  // namespace

TEST(AacNmrRuntimeTest, RateModesKeepLegalPacketsAndContinuousSampleTimeAcrossShortWindowTransitions) {
    constexpr int frameCount = 96;
    constexpr int samplesPerFrame = 1024;
    for (const int channels : {1, 2}) {
        for (const int rate : {48000, 96000}) {
            for (const int mode : {0, 1, 2, 3}) {  // CBR, ABR, constant-quality VBR, strict CBR
                SCOPED_TRACE(::testing::Message() << "channels=" << channels << " rate=" << rate << " mode=" << mode);
                const AVCodec* codec = avcodec_find_encoder_by_name("aac");
                ASSERT_NE(codec, nullptr);
                std::unique_ptr<AVCodecContext, CodecDelete> context(avcodec_alloc_context3(codec));
                ASSERT_NE(context, nullptr);
                context->sample_rate = rate;
                context->sample_fmt = AV_SAMPLE_FMT_FLTP;
                context->time_base = {1, rate};
                context->bit_rate = 160000 * channels;
                if (mode == 3)
                    context->bit_rate_tolerance = 0;
                av_channel_layout_default(&context->ch_layout, channels);
                if (mode == 2) {
                    context->flags |= AV_CODEC_FLAG_QSCALE;
                    context->global_quality = 6 * FF_QP2LAMBDA;
                }
                AVDictionary* options = nullptr;
                ASSERT_GE(av_dict_set(&options, "aac_coder", "nmr", 0), 0);
                ASSERT_GE(av_dict_set(&options, "aac_nmr_speed", "0", 0), 0);
                ASSERT_GE(av_dict_set(&options, "aac_rc", mode == 1 ? "abr" : "cbr", 0), 0);
                const int opened = avcodec_open2(context.get(), codec, &options);
                av_dict_free(&options);
                ASSERT_GE(opened, 0);

                std::unique_ptr<AVFrame, FrameDelete> frame(av_frame_alloc());
                std::unique_ptr<AVPacket, PacketDelete> packet(av_packet_alloc());
                ASSERT_NE(frame, nullptr);
                ASSERT_NE(packet, nullptr);
                frame->format = context->sample_fmt;
                frame->nb_samples = samplesPerFrame;
                frame->sample_rate = rate;
                ASSERT_GE(av_channel_layout_copy(&frame->ch_layout, &context->ch_layout), 0);
                ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);
                int64_t packetSamples = 0;
                int64_t packetEnd = 0;
                int packets = 0;
                bool reachedEof = false;
                const auto drain = [&] {
                    for (;;) {
                        const int received = avcodec_receive_packet(context.get(), packet.get());
                        if (received == AVERROR(EAGAIN))
                            return true;
                        if (received == AVERROR_EOF) {
                            reachedEof = true;
                            return true;
                        }
                        if (received < 0)
                            return false;
                        EXPECT_GT(packet->size, 0);
                        EXPECT_LE(packet->size * 8, 6144 * channels);
                        EXPECT_EQ(packet->duration, samplesPerFrame);
                        if (packets != 0)
                            EXPECT_EQ(packet->pts, packetEnd);
                        packetEnd = packet->pts + packet->duration;
                        packetSamples += packet->duration;
                        ++packets;
                        av_packet_unref(packet.get());
                    }
                };
                uint32_t random = 0x12345678u;
                for (int index = 0; index < frameCount; ++index) {
                    ASSERT_GE(av_frame_make_writable(frame.get()), 0);
                    for (int channel = 0; channel < channels; ++channel) {
                        auto* plane = reinterpret_cast<float*>(frame->data[channel]);
                        for (int sample = 0; sample < samplesPerFrame; ++sample) {
                            random ^= random << 13;
                            random ^= random >> 17;
                            random ^= random << 5;
                            const float noise = static_cast<float>(random >> 8) / 8388608.0f - 1.0f;
                            const float gain = index % 19 == 0 ? 0.0f : ((sample / 64 + index) % 2 ? 0.03f : 0.9f);
                            plane[sample] = noise * gain;
                        }
                    }
                    frame->pts = static_cast<int64_t>(index) * samplesPerFrame;
                    ASSERT_GE(avcodec_send_frame(context.get(), frame.get()), 0);
                    ASSERT_TRUE(drain());
                }
                ASSERT_GE(avcodec_send_frame(context.get(), nullptr), 0);
                ASSERT_TRUE(drain());
                EXPECT_TRUE(reachedEof);
                EXPECT_EQ(packetEnd, static_cast<int64_t>(frameCount) * samplesPerFrame);
                EXPECT_EQ(packetSamples, static_cast<int64_t>(frameCount) * samplesPerFrame + context->initial_padding);
            }
        }
    }
}

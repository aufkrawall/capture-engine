// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
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

// The codec reports its legality repairs only through av_log. Capture them
// for one encode and give libav its default sink back afterwards.
class AacDiagnostics {
public:
    AacDiagnostics() {
        Lines().clear();
        av_log_set_callback(Capture);
    }
    ~AacDiagnostics() { av_log_set_callback(av_log_default_callback); }
    AacDiagnostics(const AacDiagnostics&) = delete;
    AacDiagnostics& operator=(const AacDiagnostics&) = delete;

    static int Count(const char* prefix) {
        std::lock_guard lock(Mutex());
        return static_cast<int>(std::count_if(Lines().begin(), Lines().end(), [&](const std::string& line) {
            return line.find(prefix) != std::string::npos;
        }));
    }

private:
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static std::vector<std::string>& Lines() {
        static std::vector<std::string> lines;
        return lines;
    }
    [[gnu::format(printf, 3, 0)]] static void Capture(void*, int level, const char* fmt, va_list args) {
        if (level > AV_LOG_WARNING || !fmt || std::string(fmt).rfind("AAC NMR ", 0) != 0)
            return;
        char line[512];
        std::vsnprintf(line, sizeof(line), fmt, args);
        std::lock_guard lock(Mutex());
        Lines().emplace_back(line);
    }
};

struct EncodeConfig {
    int channels = 2;
    int sampleRate = 48000;
    int64_t bitRate = 320000;
    int mode = 0;  // 0 CBR, 1 ABR, 2 constant-quality VBR, 3 strict CBR
    int frames = 96;
};

struct EncodeResult {
    int packets = 0;
    int maxPacketBits = 0;
    int64_t packetSamples = 0;
    int64_t packetEnd = 0;
    int initialPadding = 0;
    bool reachedEof = false;
    int64_t decodedSamples = 0;
    float decodedPeak = 0.0f;
};

// Dense transients between gated noise and silence: the content class whose
// short-window frames exceeded the frame budget.
void FillTransientNoise(AVFrame& frame, int channels, int index, uint32_t& random) {
    for (int channel = 0; channel < channels; ++channel) {
        auto* plane = reinterpret_cast<float*>(frame.data[channel]);
        for (int sample = 0; sample < frame.nb_samples; ++sample) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            const float noise = static_cast<float>(random >> 8) / 8388608.0f - 1.0f;
            const float gain = index % 19 == 0 ? 0.0f : ((sample / 64 + index) % 2 ? 0.03f : 0.9f);
            plane[sample] = noise * gain;
        }
    }
}

// Encodes, checks every packet for legality and continuous sample time, and
// decodes the stream to prove the decoder accepts every frame.
void Encode(const EncodeConfig& config, EncodeResult& result) {
    constexpr int samplesPerFrame = 1024;
    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    ASSERT_NE(codec, nullptr);
    std::unique_ptr<AVCodecContext, CodecDelete> context(avcodec_alloc_context3(codec));
    ASSERT_NE(context, nullptr);
    context->sample_rate = config.sampleRate;
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context->time_base = {1, config.sampleRate};
    context->bit_rate = config.bitRate;
    if (config.mode == 3)
        context->bit_rate_tolerance = 0;
    av_channel_layout_default(&context->ch_layout, config.channels);
    if (config.mode == 2) {
        context->flags |= AV_CODEC_FLAG_QSCALE;
        context->global_quality = 6 * FF_QP2LAMBDA;
    }
    AVDictionary* options = nullptr;
    ASSERT_GE(av_dict_set(&options, "aac_coder", "nmr", 0), 0);
    ASSERT_GE(av_dict_set(&options, "aac_nmr_speed", "0", 0), 0);
    ASSERT_GE(av_dict_set(&options, "aac_rc", config.mode == 1 ? "abr" : "cbr", 0), 0);
    const int opened = avcodec_open2(context.get(), codec, &options);
    av_dict_free(&options);
    ASSERT_GE(opened, 0);
    result.initialPadding = context->initial_padding;

    const AVCodec* decoderCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    ASSERT_NE(decoderCodec, nullptr);
    std::unique_ptr<AVCodecContext, CodecDelete> decoder(avcodec_alloc_context3(decoderCodec));
    ASSERT_NE(decoder, nullptr);
    decoder->sample_rate = config.sampleRate;
    ASSERT_GE(av_channel_layout_copy(&decoder->ch_layout, &context->ch_layout), 0);
    ASSERT_GT(context->extradata_size, 0);
    decoder->extradata = static_cast<uint8_t*>(av_mallocz(context->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
    ASSERT_NE(decoder->extradata, nullptr);
    std::copy_n(context->extradata, context->extradata_size, decoder->extradata);
    decoder->extradata_size = context->extradata_size;
    ASSERT_GE(avcodec_open2(decoder.get(), decoderCodec, nullptr), 0);

    std::unique_ptr<AVFrame, FrameDelete> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDelete> decoded(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDelete> packet(av_packet_alloc());
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(decoded, nullptr);
    ASSERT_NE(packet, nullptr);
    frame->format = context->sample_fmt;
    frame->nb_samples = samplesPerFrame;
    frame->sample_rate = config.sampleRate;
    ASSERT_GE(av_channel_layout_copy(&frame->ch_layout, &context->ch_layout), 0);
    ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);

    const auto decode = [&] {
        EXPECT_GE(avcodec_send_packet(decoder.get(), packet.get()), 0);
        while (avcodec_receive_frame(decoder.get(), decoded.get()) >= 0) {
            result.decodedSamples += decoded->nb_samples;
            for (int channel = 0; channel < decoded->ch_layout.nb_channels; ++channel) {
                const auto* plane = reinterpret_cast<const float*>(decoded->extended_data[channel]);
                for (int sample = 0; sample < decoded->nb_samples; ++sample)
                    result.decodedPeak = std::max(result.decodedPeak, std::fabs(plane[sample]));
            }
            av_frame_unref(decoded.get());
        }
    };
    const auto drain = [&] {
        for (;;) {
            const int received = avcodec_receive_packet(context.get(), packet.get());
            if (received == AVERROR(EAGAIN))
                return true;
            if (received == AVERROR_EOF) {
                result.reachedEof = true;
                return true;
            }
            if (received < 0)
                return false;
            EXPECT_GT(packet->size, 0);
            const int packetBits = packet->size * 8;
            EXPECT_LE(packetBits, 6144 * config.channels);
            result.maxPacketBits = std::max(result.maxPacketBits, packetBits);
            EXPECT_EQ(packet->duration, samplesPerFrame);
            if (result.packets != 0)
                EXPECT_EQ(packet->pts, result.packetEnd);
            result.packetEnd = packet->pts + packet->duration;
            result.packetSamples += packet->duration;
            ++result.packets;
            decode();
            av_packet_unref(packet.get());
        }
    };
    uint32_t random = 0x12345678u;
    for (int index = 0; index < config.frames; ++index) {
        ASSERT_GE(av_frame_make_writable(frame.get()), 0);
        FillTransientNoise(*frame, config.channels, index, random);
        frame->pts = static_cast<int64_t>(index) * samplesPerFrame;
        ASSERT_GE(avcodec_send_frame(context.get(), frame.get()), 0);
        ASSERT_TRUE(drain());
    }
    ASSERT_GE(avcodec_send_frame(context.get(), nullptr), 0);
    ASSERT_TRUE(drain());
}

void ExpectCompleteTimeline(const EncodeConfig& config, const EncodeResult& result) {
    EXPECT_TRUE(result.reachedEof);
    EXPECT_EQ(result.packetEnd, static_cast<int64_t>(config.frames) * 1024);
    EXPECT_EQ(result.packetSamples, static_cast<int64_t>(config.frames) * 1024 + result.initialPadding);
    // The decoder drops the encoder's priming; every coded frame must decode.
    EXPECT_EQ(result.decodedSamples, result.packetSamples - result.initialPadding);
}
}  // namespace

TEST(AacNmrRuntimeTest, RateModesKeepLegalPacketsAndContinuousSampleTimeAcrossShortWindowTransitions) {
    for (const int channels : {1, 2}) {
        for (const int rate : {48000, 96000}) {
            for (const int mode : {0, 1, 2, 3}) {
                SCOPED_TRACE(::testing::Message() << "channels=" << channels << " rate=" << rate << " mode=" << mode);
                AacDiagnostics diagnostics;
                const EncodeConfig config{channels, rate, 160000 * channels, mode, 96};
                EncodeResult result;
                Encode(config, result);
                if (HasFatalFailure())
                    return;
                ExpectCompleteTimeline(config, result);
                EXPECT_GT(result.decodedPeak, 0.0f);
                if (mode == 3)
                    EXPECT_LE(result.maxPacketBits, config.bitRate * 1024 / rate);
                // Real content must always be repaired, never silenced.
                EXPECT_EQ(AacDiagnostics::Count("AAC NMR emitted a silent frame"), 0);
                EXPECT_EQ(AacDiagnostics::Count("removal_events="), 0);
            }
        }
    }
}

// At the format's highest rate every frame plans close to the legality cap,
// so the solver's bisection regularly lands just above it. The real codec must
// take the repair path and still emit legal, decodable, gap-free frames.
TEST(AacNmrRuntimeTest, NearCapCbrTakesTheLegalityRepairAndStaysLegal) {
    AacDiagnostics diagnostics;
    const EncodeConfig config{1, 48000, 288000, 0, 160};
    EncodeResult result;
    Encode(config, result);
    if (HasFatalFailure())
        return;
    ExpectCompleteTimeline(config, result);
    EXPECT_GT(AacDiagnostics::Count("AAC NMR budget repair"), 0) << "the real repair path did not run";
    EXPECT_EQ(AacDiagnostics::Count("AAC NMR emitted a silent frame"), 0);
    // Metered (powers of two) per class; recorded so a run shows which steps ran.
    RecordProperty("logged_refits", AacDiagnostics::Count("refit_events="));
    RecordProperty("logged_reopens", AacDiagnostics::Count("reopened=1"));
    RecordProperty("logged_removals", AacDiagnostics::Count("removal_events="));
}

// A rate below the minimum AAC syntax overhead cannot fit even silence. The
// fallback must use only mandatory syntax and preserve samples and timestamps.
TEST(AacNmrRuntimeTest, UnreachableStrictBoundEndsInSilentFramesInsteadOfAHang) {
    AacDiagnostics diagnostics;
    const EncodeConfig config{1, 48000, 1000, 3, 6};
    EncodeResult result;
    Encode(config, result);
    if (HasFatalFailure())
        return;
    ExpectCompleteTimeline(config, result);
    EXPECT_LE(result.maxPacketBits, 40) << "mono silence should contain only mandatory AAC syntax";
    EXPECT_EQ(result.decodedPeak, 0.0f);
    int expectedReports = 0;
    for (int count = result.packets; count > 0; count /= 2)
        ++expectedReports;
    EXPECT_EQ(AacDiagnostics::Count("AAC NMR emitted a silent frame"), expectedReports);
}

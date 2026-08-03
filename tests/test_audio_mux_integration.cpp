#include <gtest/gtest.h>

#include "audio_encoder.h"
#include "matroska_timing.h"

#include <mmreg.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

struct TemporaryMka {
    TemporaryMka() {
        wchar_t directory[MAX_PATH]{};
        wchar_t temporary[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"cea", 0, temporary)) {
            DeleteFileW(temporary);
            path = std::filesystem::path(temporary).replace_extension(L".mka");
        }
    }
    ~TemporaryMka() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
    std::filesystem::path path;
};

struct MuxTrack {
    std::string codec;
    std::unique_ptr<AudioEncoder> encoder;
    AVStream* stream = nullptr;
    AVFormatContext* format = nullptr;
    bool writeFailed = false;
};

std::vector<float> MakeDeterministicStereoSignal(int samples, int sampleRate) {
    std::vector<float> signal(static_cast<size_t>(samples) * 2u);
    uint32_t noiseState = 0x12345678u;
    for (int sample = 0; sample < samples; ++sample) {
        noiseState = noiseState * 1664525u + 1013904223u;
        const float noise = (static_cast<float>((noiseState >> 8) & 0xffffu) / 32767.5f) - 1.0f;
        const double time = static_cast<double>(sample) / sampleRate;
        const float value =
            static_cast<float>(0.22 * std::sin(2.0 * 3.141592653589793 * 437.0 * time) +
                               0.11 * std::sin(2.0 * 3.141592653589793 * 1231.0 * time) + 0.025 * noise);
        signal[static_cast<size_t>(sample) * 2] = value;
        signal[static_cast<size_t>(sample) * 2 + 1] = value;
    }
    return signal;
}

struct DecodedTrack {
    int64_t samples = 0;
    std::vector<float> mono;
    std::string failure;
};

bool DecodeFailure(DecodedTrack& decoded, const char* stage, int error) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    if (error < 0) {
        av_strerror(error, detail, sizeof(detail));
    }
    decoded.failure = stage;
    if (detail[0]) {
        decoded.failure += ": ";
        decoded.failure += detail;
    }
    return false;
}

bool AppendDecodedFrames(AVCodecContext* decoder, SwrContext* resampler, AVFrame* frame, DecodedTrack& decoded) {
    while (true) {
        const int receiveResult = avcodec_receive_frame(decoder, frame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            return true;
        }
        if (receiveResult < 0) {
            return DecodeFailure(decoded, "avcodec_receive_frame", receiveResult);
        }
        decoded.samples += frame->nb_samples;
        const int outputCapacity =
            static_cast<int>(av_rescale_rnd(swr_get_delay(resampler, decoder->sample_rate) + frame->nb_samples, 48000,
                                            decoder->sample_rate, AV_ROUND_UP));
        std::vector<float> converted(static_cast<size_t>(std::max(outputCapacity, 0)));
        uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
        const uint8_t* const* input = const_cast<const uint8_t* const*>(frame->extended_data);
        const int outputSamples = swr_convert(resampler, output, outputCapacity, input, frame->nb_samples);
        if (outputSamples < 0) {
            return DecodeFailure(decoded, "swr_convert", outputSamples);
        }
        decoded.mono.insert(decoded.mono.end(), converted.begin(), converted.begin() + outputSamples);
        av_frame_unref(frame);
    }
}

bool DecodeAudioStream(const std::filesystem::path& path, int audioOrdinal, DecodedTrack& decoded) {
    AVFormatContext* input = nullptr;
    const int openResult = avformat_open_input(&input, path.string().c_str(), nullptr, nullptr);
    if (openResult < 0) {
        return DecodeFailure(decoded, "avformat_open_input", openResult);
    }
    const int streamInfoResult = avformat_find_stream_info(input, nullptr);
    bool ok = streamInfoResult >= 0;
    if (!ok) {
        DecodeFailure(decoded, "avformat_find_stream_info", streamInfoResult);
    }
    int selectedStream = -1;
    int ordinal = 0;
    for (unsigned int index = 0; ok && index < input->nb_streams; ++index) {
        if (input->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        if (ordinal++ == audioOrdinal) {
            selectedStream = static_cast<int>(index);
            break;
        }
    }
    if (ok && selectedStream < 0) {
        ok = DecodeFailure(decoded, "audio stream ordinal not found", 0);
    }
    const AVCodec* codec = ok ? avcodec_find_decoder(input->streams[selectedStream]->codecpar->codec_id) : nullptr;
    AVCodecContext* decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (ok && !codec) {
        decoded.failure = "avcodec_find_decoder: id=" +
                          std::to_string(static_cast<int>(input->streams[selectedStream]->codecpar->codec_id)) +
                          " name=" + avcodec_get_name(input->streams[selectedStream]->codecpar->codec_id);
        ok = false;
    } else if (ok && !decoder) {
        ok = DecodeFailure(decoded, "avcodec_alloc_context3", AVERROR(ENOMEM));
    }
    if (ok) {
        const int parametersResult = avcodec_parameters_to_context(decoder, input->streams[selectedStream]->codecpar);
        ok = parametersResult >= 0 || DecodeFailure(decoded, "avcodec_parameters_to_context", parametersResult);
    }
    if (ok) {
        const int decoderOpenResult = avcodec_open2(decoder, codec, nullptr);
        ok = decoderOpenResult >= 0 || DecodeFailure(decoded, "avcodec_open2", decoderOpenResult);
    }

    SwrContext* resampler = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (ok) {
        const int allocateResamplerResult =
            swr_alloc_set_opts2(&resampler, &mono, AV_SAMPLE_FMT_FLT, 48000, &decoder->ch_layout, decoder->sample_fmt,
                                decoder->sample_rate, 0, nullptr);
        ok = allocateResamplerResult >= 0 || DecodeFailure(decoded, "swr_alloc_set_opts2", allocateResamplerResult);
    }
    if (ok) {
        const int resamplerInitResult = swr_init(resampler);
        ok = resamplerInitResult >= 0 || DecodeFailure(decoded, "swr_init", resamplerInitResult);
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (ok && (!packet || !frame)) {
        ok = DecodeFailure(decoded, "packet/frame allocation", AVERROR(ENOMEM));
    }
    while (ok && av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == selectedStream) {
            const int sendResult = avcodec_send_packet(decoder, packet);
            ok = sendResult >= 0 || DecodeFailure(decoded, "avcodec_send_packet", sendResult);
            if (ok) {
                ok = AppendDecodedFrames(decoder, resampler, frame, decoded);
            }
        }
        av_packet_unref(packet);
    }
    if (ok) {
        const int drainResult = avcodec_send_packet(decoder, nullptr);
        ok = drainResult >= 0 || DecodeFailure(decoded, "decoder drain send", drainResult);
        if (ok) {
            ok = AppendDecodedFrames(decoder, resampler, frame, decoded);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resampler);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    return ok;
}

std::pair<double, int> BestCorrelation(const std::vector<float>& reference, const std::vector<float>& candidate,
                                       int maxLag) {
    double bestCorrelation = -1.0;
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        const size_t referenceStart = lag < 0 ? static_cast<size_t>(-lag) : 0;
        const size_t candidateStart = lag > 0 ? static_cast<size_t>(lag) : 0;
        const size_t count = std::min(reference.size() - std::min(referenceStart, reference.size()),
                                      candidate.size() - std::min(candidateStart, candidate.size()));
        if (count < 256) {
            continue;
        }
        double referenceMean = 0.0;
        double candidateMean = 0.0;
        for (size_t index = 0; index < count; ++index) {
            referenceMean += reference[referenceStart + index];
            candidateMean += candidate[candidateStart + index];
        }
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        referenceMean /= count;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        candidateMean /= count;
        double cross = 0.0;
        double referenceEnergy = 0.0;
        double candidateEnergy = 0.0;
        for (size_t index = 0; index < count; ++index) {
            const double left = reference[referenceStart + index] - referenceMean;
            const double right = candidate[candidateStart + index] - candidateMean;
            cross += left * right;
            referenceEnergy += left * left;
            candidateEnergy += right * right;
        }
        const double denominator = std::sqrt(referenceEnergy * candidateEnergy);
        const double correlation = denominator > 0.0 ? cross / denominator : 0.0;
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    return {bestCorrelation, bestLag};
}

}  // namespace

TEST(AudioMuxIntegrationTest, FiveCodecsDecodeToTheSameExactEndpointThroughMatroska) {
    TemporaryMka file;
    ASSERT_FALSE(file.path.empty());
    AVFormatContext* format = nullptr;
    ASSERT_GE(avformat_alloc_output_context2(&format, nullptr, "matroska", file.path.string().c_str()), 0);
    ASSERT_NE(format, nullptr);
    ASSERT_TRUE(ce::media::RequireMicrosecondMatroskaTimestampPrecision(format));

    const char* codecs[] = {"aac", "alac", "flac", "opus", "pcm"};
    std::vector<std::unique_ptr<MuxTrack>> tracks;
    for (const char* codec : codecs) {
        auto track = std::make_unique<MuxTrack>();
        track->codec = codec;
        track->encoder = std::make_unique<AudioEncoder>();
        track->format = format;
        MuxTrack* stableTrack = track.get();
        AudioConfig config;
        config.codec = codec;
        config.bitrate = 192;
        config.sampleRate = "48000";
        config.bitDepth = "24";
        config.outputChannels = 2;
        config.outputChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        ASSERT_TRUE(track->encoder->Init(config, [stableTrack](AVPacket* packet) {
            AVPacket* copy = av_packet_clone(packet);
            if (!copy || !stableTrack->stream) {
                stableTrack->writeFailed = true;
                av_packet_free(&copy);
                return;
            }
            av_packet_rescale_ts(copy, AVRational{1, stableTrack->stream->codecpar->sample_rate},
                                 stableTrack->stream->time_base);
            copy->stream_index = stableTrack->stream->index;
            if (av_interleaved_write_frame(stableTrack->format, copy) < 0) {
                stableTrack->writeFailed = true;
            }
            av_packet_free(&copy);
        })) << codec;
        track->stream = avformat_new_stream(format, nullptr);
        ASSERT_NE(track->stream, nullptr);
        ASSERT_GE(avcodec_parameters_from_context(track->stream->codecpar, track->encoder->GetCodecContext()), 0);
        track->stream->time_base = {1, track->encoder->GetCodecContext()->sample_rate};
        track->encoder->SetStreamIndex(track->stream->index);
        tracks.push_back(std::move(track));
    }

    ASSERT_GE(avio_open(&format->pb, file.path.string().c_str(), AVIO_FLAG_WRITE), 0);
    ASSERT_GE(avformat_write_header(format, nullptr), 0);
    constexpr int kTargetSamples = 4800;
    const std::vector<float> source = MakeDeterministicStereoSignal(kTargetSamples, 48000);
    uint64_t generation = 1;
    for (auto& track : tracks) {
        ASSERT_TRUE(track->encoder->ResetForRecordingStart(0, generation++));
        const auto result = track->encoder->EncodeSamples(reinterpret_cast<const uint8_t*>(source.data()),
                                                          static_cast<int>(source.size() * sizeof(float)), 2, 48000, 32,
                                                          32, 8, true, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0);
        ASSERT_FALSE(result.failed) << track->codec;
        track->encoder->SetRecordingEndUs(100000);
        track->encoder->Stop();
        EXPECT_FALSE(track->writeFailed) << track->codec;
        EXPECT_FALSE(track->encoder->GetFinalizationReport().protocolError) << track->codec;
    }
    ASSERT_GE(av_write_trailer(format), 0);
    avio_closep(&format->pb);
    avformat_free_context(format);

    std::vector<DecodedTrack> decoded(tracks.size());
    for (size_t index = 0; index < decoded.size(); ++index) {
        ASSERT_TRUE(DecodeAudioStream(file.path, static_cast<int>(index), decoded[index]))
            << codecs[index] << ": " << decoded[index].failure;
        EXPECT_EQ(decoded[index].samples, kTargetSamples) << codecs[index];
        EXPECT_EQ(decoded[index].mono.size(), static_cast<size_t>(kTargetSamples)) << codecs[index];
    }
    const DecodedTrack& reference = decoded.back();
    for (size_t index = 0; index + 1 < decoded.size(); ++index) {
        const auto [correlation, lag] = BestCorrelation(reference.mono, decoded[index].mono, 48);
        EXPECT_GE(correlation, 0.85) << codecs[index];
        EXPECT_LE(std::abs(lag), 48) << codecs[index];
    }
}

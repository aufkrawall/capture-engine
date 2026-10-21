#include <gtest/gtest.h>

#include "mediaengine/mux/crash_safe_muxing.h"
#include "mediaengine/mux/matroska_timing.h"
#include "mediaengine/mux/recording_recovery.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
}

// The recording recovery repairs what a crash leaves: a staging file with header and closed clusters (or fragments),
// no index, and tracks that end at different times (libcengine 11, part 3). The fixtures are written the way a crash
// leaves them: packets muxed with the crash-safe options, then the file closed without a trailer, so FFmpeg's open
// cluster or fragment is lost exactly as it is when the process dies.
namespace {
using ce::media::RecoveryOutcome;
using ce::media::RecoveryReport;

constexpr int kSampleRate = 48000;

class RecordingRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     (L"recording_recovery_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                      std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }
    std::filesystem::path Staging(const wchar_t* extension) const {
        return directory_ / (std::wstring(L"capture_stage_1700000000000_p1_s1.") + extension);
    }
    std::filesystem::path directory_;
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct CodecDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

std::string Utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

struct TrackSpec {
    const char* encoder;
    double seconds;
};

struct EncodedTrack {
    std::vector<PacketPtr> packets;  // in the encoder's time base
    AVRational timeBase{1, 1};       // the encoder's; the muxer may pick another at write_header
    AVStream* stream = nullptr;
};

// Encodes `seconds` of silence (audio) or gray frames (video) and keeps the packets.
bool EncodeTrack(AVFormatContext* format, const TrackSpec& spec, EncodedTrack* out, std::string* skipReason) {
    const AVCodec* codec = avcodec_find_encoder_by_name(spec.encoder);
    if (!codec) {
        *skipReason = std::string(spec.encoder) + " is not in this FFmpeg build";
        return false;
    }
    std::unique_ptr<AVCodecContext, CodecDeleter> context(avcodec_alloc_context3(codec));
    const bool video = codec->type == AVMEDIA_TYPE_VIDEO;
    int frameSamples = 0;
    if (video) {
        context->width = 64;
        context->height = 64;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, 30};
        context->framerate = AVRational{30, 1};
        context->gop_size = 30;
        av_opt_set(context->priv_data, "preset", "12", 0);
    } else {
        context->sample_rate = kSampleRate;
        av_channel_layout_default(&context->ch_layout, 2);
        const void* formats = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(context.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count) >= 0 &&
            formats && count > 0)
            context->sample_fmt = static_cast<const AVSampleFormat*>(formats)[0];
        else
            context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->time_base = AVRational{1, kSampleRate};
    }
    if (format->oformat->flags & AVFMT_GLOBALHEADER)
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        *skipReason = std::string(spec.encoder) + " cannot open";
        return false;
    }
    AVStream* stream = avformat_new_stream(format, nullptr);
    avcodec_parameters_from_context(stream->codecpar, context.get());
    stream->time_base = context->time_base;
    out->stream = stream;
    out->timeBase = context->time_base;
    if (!video)
        frameSamples = context->frame_size > 0 ? context->frame_size : 1024;
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    const int64_t total = video ? static_cast<int64_t>(spec.seconds * 30.0)
                                : static_cast<int64_t>(spec.seconds * kSampleRate) / frameSamples;
    PacketPtr packet(av_packet_alloc());
    auto drain = [&]() {
        while (avcodec_receive_packet(context.get(), packet.get()) >= 0) {
            packet->stream_index = stream->index;
            out->packets.emplace_back(av_packet_clone(packet.get()));
            av_packet_unref(packet.get());
        }
    };
    for (int64_t i = 0; i < total; ++i) {
        av_frame_unref(frame.get());
        if (video) {
            frame->format = context->pix_fmt;
            frame->width = context->width;
            frame->height = context->height;
            frame->pts = i;
        } else {
            frame->format = context->sample_fmt;
            av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
            frame->sample_rate = kSampleRate;
            frame->nb_samples = frameSamples;
            frame->pts = i * frameSamples;
        }
        if (av_frame_get_buffer(frame.get(), 0) < 0)
            return false;
        if (video) {
            for (int plane = 0; plane < 3; ++plane)
                memset(frame->data[plane], 128, static_cast<size_t>(frame->linesize[plane]) * (plane ? 32 : 64));
        } else {
            av_samples_set_silence(frame->extended_data, 0, frameSamples, frame->ch_layout.nb_channels,
                                   static_cast<AVSampleFormat>(frame->format));
        }
        if (avcodec_send_frame(context.get(), frame.get()) < 0)
            return false;
        drain();
    }
    avcodec_send_frame(context.get(), nullptr);
    drain();
    return !out->packets.empty();
}

// Writes the tracks interleaved with the crash-safe options, then closes without a trailer (a crash).
bool WriteCrashedRecording(const std::filesystem::path& path, const char* muxer, bool fragmentedMp4,
                           const std::vector<TrackSpec>& specs, std::string* skipReason) {
    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, muxer, Utf8(path).c_str()) < 0)
        return false;
    std::vector<EncodedTrack> tracks(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
        if (!EncodeTrack(format, specs[i], &tracks[i], skipReason)) {
            avformat_free_context(format);
            return false;
        }
    }
    ce::media::RequireMicrosecondMatroskaTimestampPrecision(format);
    ce::media::ApplyCrashSafeMuxing(format, fragmentedMp4);
    if (avio_open(&format->pb, Utf8(path).c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(format, nullptr) < 0) {
        avformat_free_context(format);
        return false;
    }
    // Interleave by time like a recording does, in the time bases the muxer chose.
    std::vector<std::pair<int64_t, AVPacket*>> order;
    for (EncodedTrack& track : tracks) {
        for (PacketPtr& packet : track.packets) {
            av_packet_rescale_ts(packet.get(), track.timeBase, track.stream->time_base);
            order.emplace_back(av_rescale_q(packet->dts, track.stream->time_base, AVRational{1, 1000000}),
                               packet.get());
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& entry : order)
        av_write_frame(format, entry.second);
    avio_flush(format->pb);
    avio_closep(&format->pb);  // no trailer: the open cluster/fragment is lost, as in a crash
    avformat_free_context(format);
    return true;
}

struct TrackEnd {
    bool video = false;
    int64_t endUs = 0;       // video, and audio in microseconds
    int64_t endSamples = 0;  // audio: the last sample a decoder outputs
    std::string last;        // the last packet, for failure messages
};

// Where every track of `path` ends, with each audio packet's end trim applied (what a decoder outputs).
std::vector<TrackEnd> TrackEnds(const std::filesystem::path& path, int64_t* durationUs, bool* seekable) {
    AVFormatContext* input = nullptr;
    std::vector<TrackEnd> ends;
    if (avformat_open_input(&input, Utf8(path).c_str(), nullptr, nullptr) < 0)
        return ends;
    avformat_find_stream_info(input, nullptr);
    *durationUs = input->duration;
    ends.resize(input->nb_streams);
    for (unsigned i = 0; i < input->nb_streams; ++i)
        ends[i].video = input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
    PacketPtr packet(av_packet_alloc());
    while (av_read_frame(input, packet.get()) >= 0) {
        const AVStream* stream = input->streams[packet->stream_index];
        const int64_t start = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        TrackEnd& end = ends[packet->stream_index];
        if (end.video) {
            int64_t duration = packet->duration;
            if (duration <= 0 && stream->avg_frame_rate.num > 0)
                duration = av_rescale_q(1, av_inv_q(stream->avg_frame_rate), stream->time_base);
            end.endUs = std::max(end.endUs, av_rescale_q(start + duration, stream->time_base, AVRational{1, 1000000}));
        } else {
            const int64_t startSamples = av_rescale_q(start, stream->time_base, AVRational{1, kSampleRate});
            int64_t samples = av_rescale_q(packet->duration, stream->time_base, AVRational{1, kSampleRate});
            if (stream->codecpar->block_align > 0 && stream->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE)
                samples = packet->size / stream->codecpar->block_align;
            size_t size = 0;
            const uint8_t* skip = av_packet_get_side_data(packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &size);
            if (skip && size >= 10)
                samples -= AV_RL32(skip + 4);
            end.endSamples = std::max(end.endSamples, startSamples + samples);
            end.endUs = av_rescale(end.endSamples, 1000000, kSampleRate);
            end.last = "start=" + std::to_string(startSamples) + " duration=" + std::to_string(packet->duration) +
                       " samples=" + std::to_string(samples) + " skip=" + std::to_string(skip && size >= 10 ? AV_RL32(skip + 4) : 0);
        }
        av_packet_unref(packet.get());
    }
    *seekable = av_seek_frame(input, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0;
    avformat_close_input(&input);
    return ends;
}

void ExpectEveryTrackEndsAt(const std::vector<TrackEnd>& ends, int64_t endUs) {
    const int64_t endSamples = av_rescale(endUs, kSampleRate, 1000000);
    for (size_t i = 0; i < ends.size(); ++i) {
        SCOPED_TRACE("stream " + std::to_string(i) + " " + ends[i].last + " endSamples=" +
                     std::to_string(ends[i].endSamples) + " expected=" + std::to_string(endSamples));
        if (ends[i].video) {
            EXPECT_EQ(ends[i].endUs, endUs);
        } else {
            // To the sample: the common end converted to samples rounds within one sample.
            EXPECT_LE(std::llabs(ends[i].endSamples - endSamples), 1);
        }
    }
}

TEST_F(RecordingRecoveryTest, AnInterruptedMkvIsRepairedWithAnIndexAndEqualTrackEnds) {
    const std::filesystem::path staging = Staging(L"mkv");
    std::string skip;
    // Tracks of different lengths: what a crash leaves after FFmpeg's interleaving and the lost open cluster.
    if (!WriteCrashedRecording(staging, "matroska", false,
                               {{"libsvtav1", 3.0}, {"aac", 3.4}, {"flac", 2.7}, {"pcm_s16le", 3.2}}, &skip)) {
        if (!skip.empty())
            GTEST_SKIP() << skip;
        FAIL() << "fixture";
    }
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    ASSERT_EQ(report.outcome, RecoveryOutcome::kRecovered) << report.detail;
    EXPECT_FALSE(std::filesystem::exists(staging)) << "the repaired staging file is removed";
    ASSERT_TRUE(std::filesystem::exists(report.output));
    EXPECT_EQ(report.output.filename().wstring().rfind(L"capture_recovered_", 0), 0u);
    EXPECT_EQ(report.streams, 4u);
    // The fixture keeps more than a second of every track and its tracks end apart: the cut really trimmed.
    EXPECT_GT(report.endUs, 1000000);
    EXPECT_GT(report.longestTailUs, 0);
    int64_t durationUs = 0;
    bool seekable = false;
    const std::vector<TrackEnd> ends = TrackEnds(report.output, &durationUs, &seekable);
    ASSERT_EQ(ends.size(), 4u);
    ExpectEveryTrackEndsAt(ends, report.endUs);
    EXPECT_GT(durationUs, 0) << "the repaired file states its duration";
    EXPECT_TRUE(seekable) << "the repaired file has an index";
}

TEST_F(RecordingRecoveryTest, AnInterruptedAudioOnlyMkaEndsEveryTrackTogether) {
    const std::filesystem::path staging = Staging(L"mka");
    std::string skip;
    if (!WriteCrashedRecording(staging, "matroska", false, {{"libopus", 2.6}, {"pcm_s16le", 2.9}, {"aac", 2.2}},
                               &skip)) {
        if (!skip.empty())
            GTEST_SKIP() << skip;
        FAIL() << "fixture";
    }
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    ASSERT_EQ(report.outcome, RecoveryOutcome::kRecovered) << report.detail;
    int64_t durationUs = 0;
    bool seekable = false;
    const std::vector<TrackEnd> ends = TrackEnds(report.output, &durationUs, &seekable);
    ASSERT_EQ(ends.size(), 3u);
    ExpectEveryTrackEndsAt(ends, report.endUs);
}

TEST_F(RecordingRecoveryTest, AnInterruptedFragmentedMp4BecomesARegularMp4WithEqualTrackEnds) {
    const std::filesystem::path staging = Staging(L"mp4");
    std::string skip;
    if (!WriteCrashedRecording(staging, "mp4", true, {{"libsvtav1", 3.0}, {"aac", 3.3}}, &skip)) {
        if (!skip.empty())
            GTEST_SKIP() << skip;
        FAIL() << "fixture";
    }
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    ASSERT_EQ(report.outcome, RecoveryOutcome::kRecovered) << report.detail;
    EXPECT_EQ(report.output.extension(), L".mp4");
    int64_t durationUs = 0;
    bool seekable = false;
    const std::vector<TrackEnd> ends = TrackEnds(report.output, &durationUs, &seekable);
    ASSERT_EQ(ends.size(), 2u);
    EXPECT_EQ(ends[0].endUs, report.endUs);
    EXPECT_GT(durationUs, 0);
}

TEST_F(RecordingRecoveryTest, AnEmptyStagingFileIsRemoved) {
    const std::filesystem::path staging = Staging(L"mkv");
    std::ofstream(staging).close();
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    EXPECT_EQ(report.outcome, RecoveryOutcome::kDiscardedEmpty);
    EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST_F(RecordingRecoveryTest, UnreadableBytesAreKeptUnderAnIncompleteName) {
    const std::filesystem::path staging = Staging(L"mkv");
    {
        std::ofstream out(staging, std::ios::binary);
        const std::string garbage(4096, 'x');
        out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    ASSERT_EQ(report.outcome, RecoveryOutcome::kKeptIncomplete);
    EXPECT_FALSE(std::filesystem::exists(staging));
    ASSERT_TRUE(std::filesystem::exists(report.output));
    EXPECT_EQ(report.output.filename().wstring().rfind(L"capture_incomplete_", 0), 0u);
    EXPECT_EQ(std::filesystem::file_size(report.output), 4096u) << "no user data is deleted";
}

TEST_F(RecordingRecoveryTest, AFileStillHeldByItsWriterIsLeftAlone) {
    const std::filesystem::path staging = Staging(L"mkv");
    std::ofstream(staging, std::ios::binary) << "data";
    // The writer's reservation: read access, sharing read and write, as ReservedCaptureOutput holds it.
    HANDLE reservation = CreateFileW(staging.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(reservation, INVALID_HANDLE_VALUE);
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging);
    CloseHandle(reservation);
    EXPECT_EQ(report.outcome, RecoveryOutcome::kWriterAlive);
    EXPECT_TRUE(std::filesystem::exists(staging));
}

TEST_F(RecordingRecoveryTest, AMissingFileIsReportedMissing) {
    EXPECT_EQ(ce::media::RecoverInterruptedRecording(Staging(L"mkv")).outcome, RecoveryOutcome::kMissing);
}

TEST_F(RecordingRecoveryTest, ACanceledRunLeavesTheStagingFileUntouched) {
    const std::filesystem::path staging = Staging(L"mka");
    std::string skip;
    if (!WriteCrashedRecording(staging, "matroska", false, {{"pcm_s16le", 2.5}}, &skip)) {
        if (!skip.empty())
            GTEST_SKIP() << skip;
        FAIL() << "fixture";
    }
    const uintmax_t size = std::filesystem::file_size(staging);
    volatile long cancel = 1;
    const RecoveryReport report = ce::media::RecoverInterruptedRecording(staging, &cancel);
    EXPECT_EQ(report.outcome, RecoveryOutcome::kFailed);
    EXPECT_TRUE(std::filesystem::exists(staging));
    EXPECT_EQ(std::filesystem::file_size(staging), size);
    EXPECT_FALSE(ce::media::RecoveryIsTerminal(report.outcome)) << "a canceled file stays in the journal";
}

TEST_F(RecordingRecoveryTest, TheCrashSafeOptionsAreAcceptedByTheMuxers) {
    for (const char* muxer : {"matroska", "mp4", "mov"}) {
        SCOPED_TRACE(muxer);
        AVFormatContext* format = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&format, nullptr, muxer, "unused"), 0);
        EXPECT_TRUE(ce::media::ApplyCrashSafeMuxing(format, true));
        avformat_free_context(format);
    }
}

}  // namespace

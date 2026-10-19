#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "av1_test_bitstream.h"
#include "mediaengine/mux/cfr_packet_duration.h"
#include "mediaengine/mux/matroska_timing.h"
#include "mediaengine/video/av1_show_existing.h"
#include "source_fragment_reader.h"

// A packet that cannot be re-shown (NVENC chose a keyframe nobody asked for)
// holds the show-existing slots committed behind it by extending its duration.
// Both video writers capped every packet at two slots, so a hold of more than
// one slot lost the rest; as the recording's last packet the video ended
// before the CFR timeline and the audio. The chain below is production code
// from the ledger to the muxer; only the sink glue is pinned by source.

namespace {

using ce::media::Av1HeldPacketDuration;
using ce::media::Av1PacketPlan;
using ce::media::Av1RepeatReencodeReason;
using ce::media::Av1ShowExistingConfig;
using ce::media::Av1ShowExistingRepeats;
using ce::mux::CfrVideoPacketDurationCeilingSlots;
using ce::mux::ClampCfrVideoPacketDuration;

constexpr AVRational kCodecTimeBase{1, 60};

std::vector<uint8_t> Unit(bool key) {
    av1_test::Frame frame;
    frame.type = key ? 0 : 1;
    frame.refresh = key ? 0xff : 0x01;
    return av1_test::TemporalUnit({}, frame, key);
}

struct WrittenPacket {
    int64_t pts = 0;
    int64_t duration = 0;  // codec time base, as the session hands it to the sink
    int64_t heldSlots = 0;
    bool key = false;
};

// Key at 0, inter 1..3 (armed), inter 4 submitted, repeats 5..8 committed
// behind it; NVENC returns 4 as a keyframe. Encoder durations are 0, as the
// finalize flush passes them.
std::vector<WrittenPacket> TailWithHeldRepeats(int64_t* holdSlots) {
    Av1ShowExistingConfig config;
    config.enabled = true;
    config.disabledReason = "";
    config.armAfterInterFrames = 2;
    config.maxPendingShowExisting = 4;
    Av1ShowExistingRepeats ledger;
    ledger.Reset(config);
    std::vector<WrittenPacket> written;
    for (int64_t pts = 0; pts < 4; ++pts) {
        ledger.OnFrameSubmitted(pts, pts == 0);
        const auto unit = Unit(pts == 0);
        const Av1PacketPlan plan = ledger.OnEncoderPacket(pts, unit.data(), unit.size());
        written.push_back({pts, Av1HeldPacketDuration(0, plan.holdSlots), plan.holdSlots, pts == 0});
    }
    EXPECT_TRUE(ledger.Armed());
    ledger.OnFrameSubmitted(4, false);
    Av1PacketPlan commit;
    Av1RepeatReencodeReason reason{};
    for (int64_t pts = 5; pts <= 8; ++pts)
        EXPECT_TRUE(ledger.TryCommitShowExisting(pts, false, false, &commit, &reason)) << pts;
    const auto unexpectedKey = Unit(true);
    const Av1PacketPlan tail = ledger.OnEncoderPacket(4, unexpectedKey.data(), unexpectedKey.size());
    EXPECT_STREQ(tail.violation, "unexpected_keyframe");
    EXPECT_TRUE(tail.after.empty());
    *holdSlots = tail.holdSlots;
    written.push_back({4, Av1HeldPacketDuration(0, tail.holdSlots), tail.holdSlots, true});
    return written;
}

// Writes the packets' timing the way the finalize flush does (rescale, then the shared
// duration clamp) and returns the container's duration in microseconds, or
// the failing step in `error`.
int64_t MuxedDurationUs(const char* format, const std::filesystem::path& path,
                        const std::vector<WrittenPacket>& packets, std::string* error) {
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, format, path.string().c_str()) < 0 || !fmt) {
        *error = "no muxer";
        return -1;
    }
    EXPECT_TRUE(ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmt));
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    // The muxers' tail duration does not depend on the codec; MJPEG payloads are
    // not parsed, unlike AV1's av1C, which needs a complete sequence header.
    stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    stream->codecpar->width = 64;
    stream->codecpar->height = 64;
    stream->time_base = kCodecTimeBase;
    int ret = avio_open(&fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE);
    if (ret < 0)
        *error = "open " + std::to_string(ret);
    if (error->empty()) {
        ret = avformat_write_header(fmt, nullptr);
        if (ret < 0)
            *error = "header " + std::to_string(ret);
    }
    AVPacket* packet = av_packet_alloc();
    for (const WrittenPacket& in : packets) {
        if (!error->empty())
            break;
        av_new_packet(packet, 16);
        std::memset(packet->data, 0, 16);
        packet->pts = packet->dts = in.pts;
        packet->duration = in.duration;
        packet->flags = AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(packet, kCodecTimeBase, stream->time_base);
        if (packet->duration <= 0)
            packet->duration = av_rescale_q(1, kCodecTimeBase, stream->time_base);
        packet->duration =
            ClampCfrVideoPacketDuration(packet->duration, in.heldSlots, kCodecTimeBase, stream->time_base);
        ret = av_interleaved_write_frame(fmt, packet);
        if (ret < 0)
            *error = "write pts " + std::to_string(in.pts) + ": " + std::to_string(ret);
    }
    if (error->empty()) {
        ret = av_write_trailer(fmt);
        if (ret < 0)
            *error = "trailer " + std::to_string(ret);
    }
    av_packet_free(&packet);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    if (!error->empty())
        return -1;

    AVFormatContext* in = nullptr;
    ret = avformat_open_input(&in, path.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        *error = "reopen " + std::to_string(ret);
        return -1;
    }
    avformat_find_stream_info(in, nullptr);
    const int64_t durationUs = in->duration;
    avformat_close_input(&in);
    return durationUs;
}

}  // namespace

TEST(Av1ShowExistingHoldMuxTest, TheCeilingAdmitsExactlyTheHeldSlots) {
    EXPECT_EQ(CfrVideoPacketDurationCeilingSlots(0), 2);
    EXPECT_EQ(CfrVideoPacketDurationCeilingSlots(-3), 2);
    EXPECT_EQ(CfrVideoPacketDurationCeilingSlots(4), 6);
    const AVRational ms{1, 1000};
    // Unheld packets keep the two-slot guard against bogus codec durations.
    EXPECT_EQ(ClampCfrVideoPacketDuration(500, 0, kCodecTimeBase, ms), 33);
    EXPECT_EQ(ClampCfrVideoPacketDuration(17, 0, kCodecTimeBase, ms), 17);
    // Five slots (own + four held) at 60 fps in milliseconds: 83, not 33.
    EXPECT_EQ(ClampCfrVideoPacketDuration(av_rescale_q(5, kCodecTimeBase, ms), 4, kCodecTimeBase, ms), 83);
    // A time base coarser than the slot still allows two ticks.
    EXPECT_EQ(ClampCfrVideoPacketDuration(9, 0, kCodecTimeBase, AVRational{1, 1}), 2);
}

TEST(Av1ShowExistingHoldMuxTest, HeldDurationCoversTheOwnSlotWhateverTheEncoderReported) {
    EXPECT_EQ(Av1HeldPacketDuration(0, 0), 0);  // unheld packets are left alone
    EXPECT_EQ(Av1HeldPacketDuration(1, 0), 1);
    EXPECT_EQ(Av1HeldPacketDuration(0, 4), 5);  // finalize flush: NVENC duration 0
    EXPECT_EQ(Av1HeldPacketDuration(1, 4), 5);  // drain path: duration set to 1
}

TEST(Av1ShowExistingHoldMuxTest, AHeldTailKeepsTheVideoOnTheCfrTimelineInEveryContainer) {
    int64_t holdSlots = 0;
    const std::vector<WrittenPacket> packets = TailWithHeldRepeats(&holdSlots);
    ASSERT_EQ(holdSlots, 4);
    ASSERT_EQ(packets.back().duration, 5);
    // Slots 0..8 accepted: 9 slots at 60 fps.
    const int64_t expectedUs = av_rescale_q(9, kCodecTimeBase, AVRational{1, 1000000});
    for (const char* format : {"matroska", "mp4"}) {
        SCOPED_TRACE(format);
        const auto path = std::filesystem::temp_directory_path() /
                          ("ce_av1_hold_" + std::to_string(GetCurrentProcessId()) + "." + format);
        std::string error;
        const int64_t durationUs = MuxedDurationUs(format, path, packets, &error);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ASSERT_GT(durationUs, 0) << error;
        // The two-slot cap ended the file at slot 6 (100 ms instead of 150 ms).
        EXPECT_NEAR(static_cast<double>(durationUs), static_cast<double>(expectedUs), 1000.0);
    }
}

TEST(Av1ShowExistingHoldMuxTest, BothVideoWritersClampThroughTheSessionsHold) {
    const auto video = std::filesystem::current_path() / "mediaengine" / "video";
    for (const char* file : {"video_encoder_write.cpp", "video_encoder_lifecycle.cpp"}) {
        SCOPED_TRACE(file);
        const std::string source = ce::test_source::ReadLogicalSource(video / file);
        ASSERT_FALSE(source.empty());
        EXPECT_NE(source.find("ce::mux::ClampCfrVideoPacketDuration("), std::string::npos);
        EXPECT_NE(source.find("av1Repeats.HeldSlotsInSink()"), std::string::npos);
        EXPECT_EQ(source.find("av_rescale_q(2, codecCtx->time_base"), std::string::npos);
    }
    const std::string session = ce::test_source::ReadLogicalSource(video / "av1_show_existing_session.cpp");
    const size_t write = session.find("void Av1ShowExistingSession::WritePacket");
    ASSERT_NE(write, std::string::npos);
    const size_t held = session.find("heldSlotsInSink_ = std::max<int64_t>(0, plan.holdSlots);", write);
    const size_t sink = held == std::string::npos ? held : session.find("sink(packet);", held);
    const size_t cleared = session.find("heldSlotsInSink_ = 0;", write);
    ASSERT_NE(held, std::string::npos);
    ASSERT_NE(sink, std::string::npos);
    ASSERT_NE(cleared, std::string::npos);
    EXPECT_LT(held, sink);
    EXPECT_LT(sink, cleared);
    EXPECT_NE(session.find("Av1HeldPacketDuration(packet->duration, plan.holdSlots)", write), std::string::npos);
}

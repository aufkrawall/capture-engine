#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "av1_slot_encode.h"
#include "mediaengine/video/av1_obu.h"

extern "C" {
#include <libavformat/avformat.h>
}

// av1_nvenc + Av1ShowExistingSession through the muxers CE writes: Matroska
// and MP4 for recordings, FLV for RTMP live streams. Every slot must come back
// as one packet at its own PTS, keyframe flags only on keyframes, and every
// repeated slot must decode to exactly the picture of the slot before it.

namespace {

using namespace av1_nvenc_test;

std::filesystem::path TempPath(const char* extension) {
    return std::filesystem::temp_directory_path() /
           ("ce_av1_show_existing_" + std::to_string(GetCurrentProcessId()) + extension);
}

struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

std::string Mux(const SlotRun& run, const SlotEncodeOptions& o, const char* format, const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, format, path.c_str()) < 0)
        return std::string("no muxer ") + format;
    AVStream* stream = avformat_new_stream(fmt, nullptr);
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_AV1;
    stream->codecpar->width = o.width;
    stream->codecpar->height = o.height;
    stream->codecpar->extradata =
        static_cast<uint8_t*>(av_mallocz(run.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(stream->codecpar->extradata, run.extradata.data(), run.extradata.size());
    stream->codecpar->extradata_size = static_cast<int>(run.extradata.size());
    stream->time_base = run.timeBase;
    std::string error;
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(fmt, nullptr) < 0)
        error = "header";
    AVPacket* packet = av_packet_alloc();
    for (size_t slot = 0; error.empty() && slot < run.units.size(); ++slot) {
        av_new_packet(packet, static_cast<int>(run.units[slot].size()));
        std::memcpy(packet->data, run.units[slot].data(), run.units[slot].size());
        packet->pts = packet->dts = run.pts[slot];
        packet->duration = 1;
        packet->flags = run.keys[slot] ? AV_PKT_FLAG_KEY : 0;
        packet->stream_index = 0;
        av_packet_rescale_ts(packet, run.timeBase, stream->time_base);
        if (av_interleaved_write_frame(fmt, packet) < 0)
            error = "write at slot " + std::to_string(slot);
    }
    if (error.empty() && av_write_trailer(fmt) < 0)
        error = "trailer";
    av_packet_free(&packet);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return error;
}

struct Demuxed {
    std::vector<std::vector<uint8_t>> units;
    std::vector<int64_t> slots;
    std::vector<bool> keys;
};

Demuxed DemuxContainer(const std::string& path, AVRational slotBase) {
    Demuxed out;
    AVFormatContext* fmt = nullptr;
    EXPECT_GE(avformat_open_input(&fmt, path.c_str(), nullptr, nullptr), 0);
    if (!fmt)
        return out;
    avformat_find_stream_info(fmt, nullptr);
    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(fmt, packet) >= 0) {
        out.units.emplace_back(packet->data, packet->data + packet->size);
        out.slots.push_back(av_rescale_q(packet->pts, fmt->streams[packet->stream_index]->time_base, slotBase));
        out.keys.push_back((packet->flags & AV_PKT_FLAG_KEY) != 0);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    return out;
}

// The bundled FFmpeg has no FLV demuxer; enhanced-RTMP AV1 tags are simple
// enough to read directly: flags byte (ex-header, frame type, packet type),
// FourCC, then the temporal unit for coded frames.
Demuxed DemuxFlv(const std::string& path, int fps) {
    Demuxed out;
    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t offset = 9 + 4;
    while (offset + 11 <= bytes.size()) {
        const uint8_t type = bytes[offset];
        const size_t size = (size_t{bytes[offset + 1]} << 16) | (size_t{bytes[offset + 2]} << 8) | bytes[offset + 3];
        const uint32_t ms = (uint32_t{bytes[offset + 7]} << 24) | (uint32_t{bytes[offset + 4]} << 16) |
                            (uint32_t{bytes[offset + 5]} << 8) | bytes[offset + 6];
        const uint8_t* data = bytes.data() + offset + 11;
        if (offset + 11 + size > bytes.size())
            break;
        if (type == 9 && size > 5 && (data[0] & 0x80) && (data[0] & 0x0f) == 1 &&
            std::memcmp(data + 1, "av01", 4) == 0) {
            out.units.emplace_back(data + 5, data + size);
            out.slots.push_back(static_cast<int64_t>(std::llround(ms * fps / 1000.0)));
            out.keys.push_back(((data[0] >> 4) & 0x07) == 1);
        }
        offset += 11 + size + 4;
    }
    return out;
}

TEST(Av1ShowExistingRoundTripTest, EveryContainerCarriesEverySlotAndDecodesRepeatsExactly) {
    SlotEncodeOptions o;
    o.keyframeIntervalSeconds = 1;
    std::vector<int> content;  // fresh runs, repeat runs, a keyframe slot inside a repeat run
    for (int slot = 0, picture = 0; slot < 300; ++slot) {
        const int phase = slot % 50;
        if (phase < 20 || phase % 3 == 0)
            ++picture;
        content.push_back(picture);
    }
    for (bool live : {false, true}) {
        SCOPED_TRACE(live);
        o.live = live;
        SlotRun run;
        const std::string why = EncodeSlots(content, true, o, &run);
        if (!why.empty())
            GTEST_SKIP() << why;
        ASSERT_EQ(run.units.size(), content.size());
        ASSERT_LT(run.encoderFrames, content.size()) << "no slot was shown without an encode";

        struct Target {
            const char* format;
            const char* extension;
        };
        for (const Target& target : {Target{"matroska", ".mkv"}, Target{"mp4", ".mp4"}, Target{"flv", ".flv"}}) {
            SCOPED_TRACE(target.format);
            TempFile file{TempPath(target.extension)};
            ASSERT_EQ(Mux(run, o, target.format, file.path.string()), "");
            const Demuxed back = std::string(target.format) == "flv" ? DemuxFlv(file.path.string(), o.fps)
                                                                     : DemuxContainer(file.path.string(), run.timeBase);
            ASSERT_EQ(back.units.size(), content.size());
            for (size_t slot = 0; slot < content.size(); ++slot) {
                ASSERT_EQ(back.slots[slot], static_cast<int64_t>(slot));
                ASSERT_EQ(back.keys[slot], run.keys[slot]) << "slot " << slot;
            }
            std::vector<std::vector<uint8_t>> lumas;
            std::string error;
            ASSERT_TRUE(DecodeLuma(back.units, o.width, o.height, &lumas, &error)) << error;
            ASSERT_EQ(lumas.size(), content.size());
            // Show-existing slots re-show the decoded frame bit for bit; repeats
            // that still re-encode (after a keyframe, at the keyframe cap or the
            // ledger bound) refine the picture as they always did.
            size_t shownRepeats = 0;
            ce::media::av1::SequenceHeader sequence;
            for (size_t slot = 0; slot < content.size(); ++slot) {
                ce::media::av1::TemporalUnitInfo info;
                ASSERT_EQ(
                    ce::media::av1::ParseTemporalUnit(run.units[slot].data(), run.units[slot].size(), &sequence, &info)
                        .status,
                    ce::media::av1::ParseStatus::Ok);
                if (!info.frame.showExistingFrame)
                    continue;
                ASSERT_GT(slot, 0u);
                ASSERT_EQ(content[slot], content[slot - 1]) << "slot " << slot << " is not a repeat";
                ASSERT_EQ(lumas[slot], lumas[slot - 1]) << "show-existing slot " << slot << " changed the picture";
                ++shownRepeats;
            }
            EXPECT_GT(shownRepeats, 50u);
        }
    }
}

}  // namespace

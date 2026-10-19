#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "av1_nvenc_test_session.h"
#include "av1_slot_encode.h"
#include "mediaengine/video/av1_obu.h"
#include "mediaengine/video/av1_show_existing.h"

// Real av1_nvenc on the local GPU (skips without one): the header contract the
// show-existing ledger relies on, the rate-control effect of skipped slots, and
// an encode -> show-existing -> libaom decode round trip.

namespace {

using namespace av1_nvenc_test;
namespace av1 = ce::media::av1;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 120;
constexpr const char* kOptions = "preset=p6:tune=hq:rc=vbr:b=6M:maxrate=12M:forced-idr=1:s12m_tc=0";

#define OPEN_OR_SKIP(encoder, options)                                          \
    do {                                                                        \
        const std::string why = (encoder).Open(kWidth, kHeight, kFps, options); \
        if (!why.empty())                                                       \
            GTEST_SKIP() << why;                                                \
    } while (0)

std::string RefreshPattern(const std::vector<uint8_t>& flags) {
    std::string out;
    char buffer[8];
    for (uint8_t value : flags) {
        std::snprintf(buffer, sizeof(buffer), "%02x ", value);
        out += buffer;
    }
    return out;
}

TEST(Av1ShowExistingNvencTest, InterFramesRefreshASlotAndKeysComeOnlyWhereCeForcesThem) {
    Encoder encoder;
    OPEN_OR_SKIP(encoder, kOptions);
    av1::SequenceHeader sequence;
    av1::TemporalUnitInfo info;
    AVCodecContext* context = encoder.Context();
    if (context->extradata_size > 0) {
        ASSERT_EQ(
            av1::ParseTemporalUnit(context->extradata, static_cast<size_t>(context->extradata_size), &sequence, &info)
                .status,
            av1::ParseStatus::Ok);
    }
    constexpr int kFrames = 150;
    for (int pts = 0; pts < kFrames; ++pts)
        ASSERT_TRUE(encoder.Send(pts, pts, pts == 0 || pts == 100));
    encoder.Flush();
    const auto packets = encoder.Take();
    ASSERT_EQ(packets.size(), static_cast<size_t>(kFrames));
    std::vector<uint8_t> refresh;
    for (const auto& packet : packets) {
        SCOPED_TRACE(packet.pts);
        const av1::ParseResult parsed =
            av1::ParseTemporalUnit(packet.data.data(), packet.data.size(), &sequence, &info);
        ASSERT_EQ(parsed.status, av1::ParseStatus::Ok) << parsed.reason;
        ASSERT_EQ(info.frameHeaders, 1);
        ASSERT_FALSE(info.extensionSeen);
        ASSERT_FALSE(info.frame.showExistingFrame);
        ASSERT_TRUE(info.frame.showFrame);
        const bool key = info.frame.frameType == av1::FrameType::Key;
        EXPECT_EQ(key, packet.pts == 0 || packet.pts == 100);
        EXPECT_EQ(packet.key, key);
        if (!key) {
            EXPECT_EQ(info.frame.frameType, av1::FrameType::Inter);
            EXPECT_NE(info.frame.refreshFrameFlags, 0);
        }
        refresh.push_back(info.frame.refreshFrameFlags);
    }
    EXPECT_EQ(av1::CanWriteShowExisting(sequence).status, av1::ParseStatus::Ok)
        << av1::CanWriteShowExisting(sequence).reason;
    std::printf(
        "[NVENC AV1] sequence: timing=%d equalInterval=%d decoderModel=%d frameIds=%d orderHintBits=%u "
        "screenContent=%u operatingPoints=%u\n[NVENC AV1] refresh (first 12): %s\n",
        sequence.timingInfoPresent, sequence.equalPictureInterval, sequence.decoderModelInfoPresent,
        sequence.frameIdNumbersPresent, sequence.orderHintBits, sequence.forceScreenContentTools,
        sequence.operatingPointCount,
        RefreshPattern(std::vector<uint8_t>(refresh.begin(), refresh.begin() + 12)).c_str());
}

TEST(Av1ShowExistingNvencTest, ReportsWhatLookaheadInsertsAtASceneCut) {
    Encoder encoder;
    OPEN_OR_SKIP(encoder, (std::string(kOptions) + ":rc-lookahead=20").c_str());
    av1::SequenceHeader sequence;
    av1::TemporalUnitInfo info;
    for (int pts = 0; pts < 120; ++pts)
        ASSERT_TRUE(encoder.Send(pts, pts, pts == 0, pts >= 60));
    encoder.Flush();
    int unforcedKeys = 0;
    int intraOnly = 0;
    for (const auto& packet : encoder.Take()) {
        ASSERT_EQ(av1::ParseTemporalUnit(packet.data.data(), packet.data.size(), &sequence, &info).status,
                  av1::ParseStatus::Ok);
        if (packet.pts != 0 && info.frame.frameType == av1::FrameType::Key)
            ++unforcedKeys;
        if (info.frame.frameType == av1::FrameType::IntraOnly)
            ++intraOnly;
        if (packet.pts >= 58 && packet.pts <= 62)
            std::printf("[NVENC AV1] lookahead cut pts=%lld type=%d show=%d refresh=%02x bytes=%zu\n",
                        static_cast<long long>(packet.pts), static_cast<int>(info.frame.frameType),
                        info.frame.showFrame, info.frame.refreshFrameFlags, packet.data.size());
    }
    std::printf("[NVENC AV1] lookahead: unforcedKeys=%d intraOnly=%d\n", unforcedKeys, intraOnly);
}

// FFmpeg reconfigures on any bitrate change; upstream forces an IDR each
// time, patch 0005 lets CE change the rate without one.
TEST(Av1ShowExistingNvencTest, BitrateChangesNeedNoKeyframeWithTheIdrFreeOption) {
    for (bool idrFree : {false, true}) {
        SCOPED_TRACE(idrFree);
        Encoder encoder;
        OPEN_OR_SKIP(encoder, (std::string(kOptions) + (idrFree ? ":idr_free_bitrate_change=1" : "")).c_str());
        AVCodecContext* context = encoder.Context();
        int64_t supported = 0;
        ASSERT_GE(av_opt_get_int(context->priv_data, "dyn_bitrate_supported", 0, &supported), 0)
            << "bundled FFmpeg lacks tools/patches/ffmpeg/0005";
        ASSERT_EQ(supported, 1) << "GPU reports no dynamic bitrate change";
        for (int pts = 0; pts < 120; ++pts) {
            if (pts == 60) {
                context->bit_rate *= 4;
                context->rc_max_rate *= 4;
            }
            ASSERT_TRUE(encoder.Send(pts, pts, pts == 0));
        }
        encoder.Flush();
        size_t before = 0;
        size_t after = 0;
        int keysAfterStart = 0;
        for (const auto& packet : encoder.Take()) {
            if (packet.pts > 0 && packet.key)
                ++keysAfterStart;
            (packet.pts < 60 ? before : after) += packet.data.size();
        }
        int64_t reconfigs = 0;
        int64_t failures = 0;
        av_opt_get_int(context->priv_data, "bitrate_reconfigs", 0, &reconfigs);
        av_opt_get_int(context->priv_data, "bitrate_reconfig_failures", 0, &failures);
        EXPECT_EQ(reconfigs, 1);
        EXPECT_EQ(failures, 0);
        EXPECT_EQ(keysAfterStart, idrFree ? 0 : 1);
        EXPECT_GT(after, before * 2) << "the new rate did not take effect";
    }
}

struct Quality {
    double uniquePsnr = 0.0;
    double displayedPsnr = 0.0;
    double minDisplayedPsnr = 99.0;
};

// Decodes every slot with libaom and scores it against the slot's picture.
Quality Score(const SlotRun& run, const std::vector<int>& content, const SlotEncodeOptions& o) {
    Quality quality;
    std::vector<std::vector<uint8_t>> lumas;
    std::string error;
    EXPECT_TRUE(DecodeLuma(run.units, o.width, o.height, &lumas, &error)) << error;
    EXPECT_EQ(lumas.size(), content.size());
    if (lumas.size() != content.size())
        return quality;
    AVFrame* source = av_frame_alloc();
    source->format = AV_PIX_FMT_NV12;
    source->width = o.width;
    source->height = o.height;
    av_frame_get_buffer(source, 0);
    int unique = 0;
    for (size_t slot = 0; slot < content.size(); ++slot) {
        FillFrame(source, content[slot]);
        const double psnr = LumaPsnr(lumas[slot], source);
        quality.displayedPsnr += psnr;
        quality.minDisplayedPsnr = std::min(quality.minDisplayedPsnr, psnr);
        if (slot == 0 || content[slot] != content[slot - 1]) {
            quality.uniquePsnr += psnr;
            ++unique;
        }
    }
    av_frame_free(&source);
    quality.displayedPsnr /= static_cast<double>(content.size());
    quality.uniquePsnr /= unique;
    return quality;
}

std::vector<int> UniqueEvery(int slots, int every) {
    std::vector<int> content;
    content.reserve(static_cast<size_t>(std::max(slots, 0)));
    for (int slot = 0; slot < slots; ++slot)
        content.push_back(slot / every);
    return content;
}

// The user's condition for show-existing repeats: no quality loss anywhere.
// Same slots, same pictures, today's re-encoded repeats against the session.
TEST(Av1ShowExistingNvencTest, ShowExistingRepeatsLoseNoQualityAgainstReencoding) {
    SlotEncodeOptions o;
    std::vector<std::pair<const char*, std::vector<int>>> patterns;
    patterns.emplace_back("unique every 2nd", UniqueEvery(720, 2));
    patterns.emplace_back("unique every 10th", UniqueEvery(720, 10));
    std::vector<int> varying;
    int picture = 0;
    for (const auto& [length, every] :
         std::vector<std::pair<int, int>>{{90, 1}, {150, 3}, {60, 1}, {150, 6}, {90, 2}, {60, 1}, {120, 4}}) {
        for (int slot = 0; slot < length; ++slot)
            varying.push_back(picture + slot / every);
        picture += length / every + 1;
    }
    patterns.emplace_back("varying cadence", varying);
    std::vector<int> jitter;  // a ~100 fps source on the 120 fps grid, irregular
    // NOLINTNEXTLINE(bugprone-random-generator-seed) - a fixed seed keeps the test deterministic
    std::mt19937 random(3);
    for (int slot = 0, current = 0; slot < 720; ++slot) {
        if (slot == 0 || random() % 6 != 0)
            ++current;
        jitter.push_back(current);
    }
    patterns.emplace_back("irregular ~100 fps", jitter);
    patterns.emplace_back("all fresh", UniqueEvery(240, 1));

    for (const auto& [name, content] : patterns) {
        SCOPED_TRACE(name);
        SlotRun reencoded;
        const std::string why = EncodeSlots(content, false, o, &reencoded);
        if (!why.empty())
            GTEST_SKIP() << why;
        ASSERT_EQ(reencoded.units.size(), content.size());
        const Quality before = Score(reencoded, content, o);
        // Live streams hold the bitrate cap; cadence changes are where it binds.
        const bool cadenceChanges = std::string(name) == "varying cadence" || std::string(name) == "irregular ~100 fps";
        for (bool live : {false, true}) {
            if (live && !cadenceChanges)
                continue;
            SCOPED_TRACE(live ? "live" : "file");
            SlotEncodeOptions mode = o;
            mode.live = live;
            SlotRun shown;
            ASSERT_TRUE(EncodeSlots(content, true, mode, &shown).empty());
            ASSERT_EQ(shown.units.size(), content.size());
            // NVENC keeps counting its GOP in encoded frames; repeats may
            // stretch a GOP in output time up to twice the keyframe interval.
            const size_t keyInterval = static_cast<size_t>(o.fps) * static_cast<size_t>(o.keyframeIntervalSeconds);
            size_t lastKey = 0;
            ASSERT_TRUE(shown.keys[0]);
            for (size_t slot = 0; slot < content.size(); ++slot) {
                ASSERT_EQ(shown.pts[slot], static_cast<int64_t>(slot));
                if (shown.keys[slot])
                    lastKey = slot;
                ASSERT_LT(slot - lastKey, 2 * keyInterval) << "slot " << slot;
            }
            EXPECT_EQ(shown.reconfigFailures, 0);
            const Quality after = Score(shown, content, o);
            const double seconds = static_cast<double>(content.size()) / o.fps;
            const double targetBytes = static_cast<double>(o.bitRate) * seconds / 8.0;
            std::printf(
                "[NVENC AV1] %s%s: reencoded %zu frames %zu B unique %.2f dB shown %.2f dB min %.2f | "
                "show-existing %zu frames %zu B unique %.2f dB shown %.2f dB min %.2f (reconfigs %lld)\n",
                name, live ? " (live)" : "", reencoded.encoderFrames, reencoded.bytes, before.uniquePsnr,
                before.displayedPsnr, before.minDisplayedPsnr, shown.encoderFrames, shown.bytes, after.uniquePsnr,
                after.displayedPsnr, after.minDisplayedPsnr, static_cast<long long>(shown.reconfigs));
            EXPECT_GE(after.uniquePsnr, before.uniquePsnr - 0.1);
            EXPECT_GE(after.displayedPsnr, before.displayedPsnr - 0.1);
            EXPECT_GE(after.minDisplayedPsnr, before.minDisplayedPsnr - 1.0);
            // The configured bitrate is the ceiling of what compensation spends.
            EXPECT_LE(static_cast<double>(shown.bytes),
                      std::max(targetBytes, static_cast<double>(reencoded.bytes)) * 1.15);
        }
    }
}

}  // namespace

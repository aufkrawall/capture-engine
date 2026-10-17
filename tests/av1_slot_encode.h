#pragma once

// Encodes a CFR slot sequence with av1_nvenc the way VideoEncoder does: either
// every slot re-encoded (today's repeats) or through Av1ShowExistingSession
// (CE keyframes, show-existing repeats, bitrate compensation). Used by the
// show-existing quality proof and the muxed round trip.

#include <cstdint>
#include <string>
#include <vector>

#include "av1_nvenc_test_session.h"
#include "mediaengine/video/av1_show_existing_session.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace av1_nvenc_test {

struct SlotEncodeOptions {
    int width = 640;
    int height = 360;
    int fps = 120;
    int keyframeIntervalSeconds = 2;
    int64_t bitRate = 1'500'000;
    int64_t maxRate = 3'000'000;
    bool live = false;  // live stream: pipeline-sized ledger bound, capped bitrate bursts
};

struct SlotRun {
    std::vector<std::vector<uint8_t>> units;  // one temporal unit per slot, in order
    std::vector<int64_t> pts;
    std::vector<bool> keys;
    size_t bytes = 0;
    size_t encoderFrames = 0;
    int64_t reconfigs = 0;
    int64_t reconfigFailures = 0;
    std::vector<uint8_t> extradata;
    AVRational timeBase{1, 120};
};

// `content[slot]` selects the picture; a slot equal to the previous one is a
// repeat. Returns an empty string or why the encoder is unavailable.
inline std::string EncodeSlots(const std::vector<int>& content, bool showExisting, const SlotEncodeOptions& o,
                               SlotRun* out) {
    const AVCodec* codec = avcodec_find_encoder_by_name("av1_nvenc");
    if (!codec)
        return "av1_nvenc not built";
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = o.width;
    ctx->height = o.height;
    ctx->time_base = {1, o.fps};
    ctx->framerate = {o.fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->max_b_frames = 0;
    ctx->gop_size = o.fps * o.keyframeIntervalSeconds;
    ctx->bit_rate = o.bitRate;
    ctx->rc_max_rate = o.maxRate;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* dict = nullptr;
    av_dict_parse_string(&dict, "preset=p6:tune=hq:rc=vbr:s12m_tc=0:rc-lookahead=0", "=", ":", 0);
    ce::media::Av1ShowExistingSession session;
    if (showExisting) {
        ce::media::Av1ShowExistingInputs inputs;
        inputs.encoder = codec->name;
        inputs.fps = o.fps;
        inputs.keyframeIntervalSeconds = o.keyframeIntervalSeconds;
        inputs.liveOutput = o.live;
        session.Configure(inputs, codec, &dict);
    }
    const int opened = avcodec_open2(ctx, codec, &dict);
    av_dict_free(&dict);
    if (opened < 0) {
        avcodec_free_context(&ctx);
        return "av1_nvenc unavailable on this GPU";
    }
    int64_t delay = 0;
    av_opt_get_int(ctx->priv_data, "delay", 0, &delay);
    if (showExisting)
        session.Start(ctx, static_cast<uint32_t>(delay > 0 ? delay : 1));
    out->extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    out->timeBase = ctx->time_base;

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_NV12;
    frame->width = o.width;
    frame->height = o.height;
    av_frame_get_buffer(frame, 0);
    AVPacket* packet = av_packet_alloc();
    const auto sink = [&](AVPacket* written) {
        out->units.emplace_back(written->data, written->data + written->size);
        out->pts.push_back(written->pts);
        out->keys.push_back((written->flags & AV_PKT_FLAG_KEY) != 0);
        out->bytes += static_cast<size_t>(written->size);
    };
    const auto drain = [&]() {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            if (showExisting)
                session.WritePacket(packet, sink);
            else
                sink(packet);
            av_packet_unref(packet);
        }
    };
    std::string error;
    for (size_t slot = 0; slot < content.size() && error.empty(); ++slot) {
        const bool repeat = slot > 0 && content[slot] == content[slot - 1];
        if (repeat && showExisting && session.TryRepeat(ctx, static_cast<int64_t>(slot), false, sink))
            continue;
        av_frame_make_writable(frame);
        FillFrame(frame, content[slot]);
        frame->pts = static_cast<int64_t>(slot);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        if (showExisting)
            session.PrepareSubmit(ctx, frame);
        if (avcodec_send_frame(ctx, frame) < 0)
            error = "send_frame failed at slot " + std::to_string(slot);
        ++out->encoderFrames;
        if (showExisting)
            session.NoteRepeatCacheHoldsLastSubmit();
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_opt_get_int(ctx->priv_data, "bitrate_reconfigs", 0, &out->reconfigs);
    av_opt_get_int(ctx->priv_data, "bitrate_reconfig_failures", 0, &out->reconfigFailures);
    session.Stop();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return error;
}

}  // namespace av1_nvenc_test

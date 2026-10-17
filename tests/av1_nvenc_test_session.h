#pragma once

// Small av1_nvenc (system-memory NV12 input) and libaom-av1 decode helpers for
// the show-existing hardware tests. Content is deterministic and textured
// enough that VBR is bit-starved, so rate-control effects are visible.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

namespace av1_nvenc_test {

struct EncodedPacket {
    int64_t pts = 0;
    bool key = false;
    std::vector<uint8_t> data;
};

inline uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// `content` selects the picture; `cut` switches to an unrelated scene.
inline void FillFrame(AVFrame* frame, int content, bool cut = false) {
    const int shift = content * 3;
    const uint32_t scene = cut ? 0x9e3779b9u : 0;
    for (int y = 0; y < frame->height; ++y) {
        uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x) {
            const int gradient = cut ? (255 - ((x + y) >> 2)) : ((x + shift) * 3 + y * 2) & 255;
            const uint32_t texture = Hash(static_cast<uint32_t>((x + shift) >> 1) * 7919u ^
                                          static_cast<uint32_t>(y >> 1) * 104729u ^ scene) &
                                     63;
            row[x] = static_cast<uint8_t>(std::min(255, gradient / 2 + 40 + static_cast<int>(texture)));
        }
    }
    for (int y = 0; y < frame->height / 2; ++y) {
        uint8_t* row = frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1];
        for (int x = 0; x < frame->width / 2; ++x) {
            row[2 * x] = static_cast<uint8_t>(128 + ((x + content) & 31) - 16);
            row[2 * x + 1] = static_cast<uint8_t>(cut ? 90 : 128 + ((y - content) & 15) - 8);
        }
    }
}

class Encoder {
public:
    ~Encoder() {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&codec_);
    }
    // Empty string on success, otherwise why the encoder is unavailable.
    std::string Open(int width, int height, int fps, const char* options) {
        const AVCodec* encoder = avcodec_find_encoder_by_name("av1_nvenc");
        if (!encoder)
            return "av1_nvenc not built";
        codec_ = avcodec_alloc_context3(encoder);
        codec_->width = width;
        codec_->height = height;
        codec_->time_base = {1, fps};
        codec_->framerate = {fps, 1};
        codec_->pix_fmt = AV_PIX_FMT_NV12;
        codec_->max_b_frames = 0;
        codec_->gop_size = 0x7fffffff;
        AVDictionary* dict = nullptr;
        av_dict_parse_string(&dict, options, "=", ":", 0);
        const int opened = avcodec_open2(codec_, encoder, &dict);
        av_dict_free(&dict);
        if (opened < 0)
            return "av1_nvenc unavailable on this GPU";
        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_NV12;
        frame_->width = width;
        frame_->height = height;
        av_frame_get_buffer(frame_, 0);
        packet_ = av_packet_alloc();
        return {};
    }
    AVCodecContext* Context() const {
        return codec_;
    }
    bool Send(int64_t pts, int content, bool forceKey, bool cut = false) {
        av_frame_make_writable(frame_);
        FillFrame(frame_, content, cut);
        frame_->pts = pts;
        frame_->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(codec_, frame_) < 0)
            return false;
        Drain();
        return true;
    }
    void Flush() {
        avcodec_send_frame(codec_, nullptr);
        Drain();
    }
    // Packets returned since the last call, in output order.
    std::vector<EncodedPacket> Take() {
        return std::move(packets_);
    }

private:
    void Drain() {
        while (avcodec_receive_packet(codec_, packet_) == 0) {
            EncodedPacket out;
            out.pts = packet_->pts;
            out.key = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
            out.data.assign(packet_->data, packet_->data + packet_->size);
            packets_.push_back(std::move(out));
            av_packet_unref(packet_);
        }
    }
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::vector<EncodedPacket> packets_;
};

// Decodes temporal units in order with libaom-av1; returns the luma planes.
inline bool DecodeLuma(const std::vector<std::vector<uint8_t>>& units, int width, int height,
                       std::vector<std::vector<uint8_t>>* lumas, std::string* error) {
    const AVCodec* decoder = avcodec_find_decoder_by_name("libaom-av1");
    if (!decoder) {
        *error = "libaom-av1 decoder not built";
        return false;
    }
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = avcodec_open2(codec, decoder, nullptr) >= 0;
    auto receive = [&]() {
        while (avcodec_receive_frame(codec, frame) == 0) {
            std::vector<uint8_t> luma(static_cast<size_t>(width) * height);
            for (int y = 0; y < height; ++y)
                std::memcpy(luma.data() + static_cast<size_t>(y) * width,
                            frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0], width);
            lumas->push_back(std::move(luma));
            av_frame_unref(frame);
        }
    };
    for (size_t i = 0; ok && i < units.size(); ++i) {
        av_new_packet(packet, static_cast<int>(units[i].size()));
        std::memcpy(packet->data, units[i].data(), units[i].size());
        packet->pts = static_cast<int64_t>(i);
        ok = avcodec_send_packet(codec, packet) >= 0;
        if (!ok)
            *error = "send_packet failed at unit " + std::to_string(i);
        av_packet_unref(packet);
        receive();
    }
    if (ok) {
        avcodec_send_packet(codec, nullptr);
        receive();
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    return ok;
}

inline double LumaPsnr(const std::vector<uint8_t>& a, const AVFrame* source) {
    double squared = 0.0;
    for (int y = 0; y < source->height; ++y) {
        const uint8_t* row = source->data[0] + static_cast<ptrdiff_t>(y) * source->linesize[0];
        for (int x = 0; x < source->width; ++x) {
            const double diff = static_cast<double>(a[static_cast<size_t>(y) * source->width + x]) - row[x];
            squared += diff * diff;
        }
    }
    const double mse = squared / (static_cast<double>(source->width) * source->height);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace av1_nvenc_test

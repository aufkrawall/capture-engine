#pragma once

#include "audio_encoder.h"

#include <mmreg.h>

#include "../common/strict_integer_parse.h"

#include "audio_sync_utils.h"

#include "audio_time_utils.h"  // For ce::audio::ParseSampleRateOr

#include "mediaengine.h"       // For DLL_Log

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <string>

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

int ResolveAudioBitDepth(const AudioConfig& config, int fallback) {
    if (config.bitDepth.empty() || config.bitDepth == "default") {
        return fallback;
    }
    int32_t depth = 0;
    if (ce::TryParseInt32(config.bitDepth, depth) && (depth == 16 || depth == 24 || depth == 32)) {
        return depth;
    }
    DLL_Log("[AudioEncoder] Unsupported bit_depth=%s, using %d", config.bitDepth.c_str(), fallback);
    return fallback;
}

uint32_t DefaultChannelMask(int channels) {
    switch (channels) {
        case 1:
            return SPEAKER_FRONT_CENTER;
        case 2:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 3:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
        case 4:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 5:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT |
                   SPEAKER_BACK_RIGHT;
        case 6:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 7:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_BACK_CENTER;
        case 8:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
        default:
            return 0;
    }
}

template <typename T>
struct CodecConfigList {
    const T* values = nullptr;
    int count = 0;
};

template <typename T>
CodecConfigList<T> GetCodecConfigs(const AVCodec* codec, AVCodecConfig config, const char* diagnosticName) {
    CodecConfigList<T> result;
    if (!codec) {
        return result;
    }
    const void* values = nullptr;
    if (avcodec_get_supported_config(nullptr, codec, config, 0, &values, &result.count) < 0) {
        DLL_Log("[AudioEncoder] Failed to query codec %s capabilities for %s; treating as unrestricted", codec->name,
                diagnosticName);
        result.count = 0;
        return result;
    }
    result.values = static_cast<const T*>(values);
    return result;
}

bool CodecSupportsSampleFormat(const AVCodec* codec, AVSampleFormat fmt) {
    const auto formats = GetCodecConfigs<AVSampleFormat>(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, "sample formats");
    if (!formats.values) {
        return true;
    }
    for (int i = 0; i < formats.count; ++i) {
        if (formats.values[i] == fmt) {
            return true;
        }
    }
    return false;
}

// True if the codec advertises no channel-layout restriction (ch_layouts == null)
// or the requested layout is in its supported list. Some lossless codecs accept
// only a fixed set of layouts: ALAC supports 7.1(wide) but NOT plain 7.1, so a
// 7.1 (side) endpoint fed verbatim makes avcodec_open2 fail with EINVAL.
bool CodecSupportsChannelLayout(const AVCodec* codec, const AVChannelLayout* layout) {
    const auto layouts = GetCodecConfigs<AVChannelLayout>(codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, "channel layouts");
    if (!layouts.values || !layout) {
        return true;  // No restriction advertised by the codec.
    }
    for (int i = 0; i < layouts.count; ++i) {
        if (av_channel_layout_compare(&layouts.values[i], layout) == 0) {
            return true;
        }
    }
    return false;
}

// Picks a codec-supported channel layout to use when the requested one is
// rejected. Preference order: a supported layout with the SAME channel count
// (lets every channel's samples be preserved - only mismatched position labels
// change, e.g. 7.1 stored in ALAC's 7.1-wide slots), else stereo, else the
// codec's first supported layout. Returns false if the codec lists no layout at
// all (caller keeps the requested layout and lets avcodec_open2 decide).
bool PickSupportedChannelLayout(const AVCodec* codec, int desiredChannels, AVChannelLayout* out) {
    const auto layouts = GetCodecConfigs<AVChannelLayout>(codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, "channel layouts");
    if (!layouts.values || !out) {
        return false;
    }
    const AVChannelLayout* firstSupported = nullptr;
    const AVChannelLayout* stereo = nullptr;
    const AVChannelLayout* sameCount = nullptr;
    for (int i = 0; i < layouts.count; ++i) {
        const AVChannelLayout* p = &layouts.values[i];
        if (!firstSupported) {
            firstSupported = p;
        }
        if (p->nb_channels == 2 && !stereo) {
            stereo = p;
        }
        if (p->nb_channels == desiredChannels && !sameCount) {
            sameCount = p;
        }
    }
    const AVChannelLayout* chosen = sameCount ? sameCount : (stereo ? stereo : firstSupported);
    if (!chosen) {
        return false;
    }
    av_channel_layout_copy(out, chosen);
    return true;
}

struct AudioCodecPolicy {
    std::string requestedName;
    std::string encoderName;
    int bitDepth = 24;
    bool allowShortFinalFrame = true;
    bool scaleLossyBitrate = false;
};

AudioCodecPolicy ResolveCodecPolicy(const AudioConfig& config) {
    AudioCodecPolicy policy;
    policy.requestedName = config.codec.empty() ? "aac" : config.codec;
    policy.encoderName = policy.requestedName;
    const std::string normalized = ToLowerAscii(policy.requestedName);

    if (normalized == "pcm") {
        policy.bitDepth = ResolveAudioBitDepth(config, 24);
        if (policy.bitDepth == 16) {
            policy.encoderName = "pcm_s16le";
        } else if (policy.bitDepth == 32) {
            policy.encoderName = "pcm_f32le";
        } else {
            policy.encoderName = "pcm_s24le";
            policy.bitDepth = 24;
        }
    } else if (normalized == "opus") {
        policy.encoderName = "libopus";
        policy.bitDepth = 0;
        policy.allowShortFinalFrame = false;
        policy.scaleLossyBitrate = true;
    } else if (normalized == "aac") {
        policy.bitDepth = 0;
        policy.allowShortFinalFrame = false;
        policy.scaleLossyBitrate = true;
    } else if (normalized == "alac" || normalized == "flac") {
        policy.bitDepth = ResolveAudioBitDepth(config, 24);
    }

    return policy;
}

int ScaleLossyBitrateKbps(int configuredKbps, int channels) {
    if (configuredKbps <= 0) {
        return configuredKbps;
    }
    if (channels >= 8) {
        return configuredKbps * 4;
    }
    if (channels >= 6) {
        return configuredKbps * 3;
    }
    return configuredKbps;
}

}  // namespace

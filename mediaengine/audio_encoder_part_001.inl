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

AudioEncoder::AudioEncoder()
    : codecCtx(nullptr),
      resampler(nullptr),
      frame(nullptr),
      samplesCount(0),
      streamIndex(-1),
      initDone(false),
      audioFifo(nullptr),
      savedCodecId(AV_CODEC_ID_NONE),
      firstTimestamp(-1),
      recordingStartUs(-1),
      recordingEndUs(0) {
    currentInputFormat = {};
}

AudioEncoder::~AudioEncoder() {
    try {
    Stop();
    ReleaseCodecResources();
    } catch (...) {
        DLL_Log("[AudioEncoder] Suppressed exception during destruction");
    }
}

void AudioEncoder::ReleaseCodecResources() {
    const bool hadResources = resampler || audioFifo || codecCtx || frame;
    resampler.reset();
    if (audioFifo) {
        av_audio_fifo_free(audioFifo);
        audioFifo = nullptr;
    }
    if (codecCtx) {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }
    if (frame) {
        av_frame_free(&frame);
        frame = nullptr;
    }
    initDone = false;
    if (hadResources) {
        DLL_Log("[AudioResource] Destroyed per-recording codec/FIFO/frame/resampler: encoder=%s",
                runtimeContract.encoderName.empty() ? "<unresolved>" : runtimeContract.encoderName.c_str());
    }
}

bool AudioEncoder::Init(const AudioConfig& config, std::function<void(AVPacket*)> packetCallback) {
    if (initDone) {
        DLL_Log("[AudioResource] ERROR: Init called on an active codec context; Stop must drain and destroy it first");
        return false;
    }
    // Clear any resources left by a previous failed initialization. Successful recording
    // contexts reach this point only after Stop() drained them to EOF and destroyed them.
    ReleaseCodecResources();
    onPacket = std::move(packetCallback);

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

    const AudioCodecPolicy policy = ResolveCodecPolicy(config);
    std::string codecName = policy.encoderName;
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName.c_str());
    if (!codec) {
        DLL_Log("[AudioEncoder] Codec not found: requested=%s resolved=%s", policy.requestedName.c_str(),
                codecName.c_str());
        return false;
    }

    outputChannels = std::clamp(config.downmix ? 2 : (config.outputChannels > 0 ? config.outputChannels : 2), 1, 8);
    outputChannelMask = config.downmix ? DefaultChannelMask(2)
                                       : (config.outputChannelMask != 0 ? config.outputChannelMask
                                                                        : DefaultChannelMask(outputChannels));
    allowShortFinalFrame = policy.allowShortFinalFrame;

    DLL_Log("[AudioEncoder] Using codec: requested=%s resolved=%s (id=%d) channels=%d mask=0x%x downmix=%d",
            policy.requestedName.c_str(), codecName.c_str(), codec->id, outputChannels, outputChannelMask,
            config.downmix ? 1 : 0);

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[AudioEncoder] Failed to allocate codec context");
        return false;
    }

    // Lossless/PCM policies honor bit_depth. Lossy codecs encode from the float mix path.
    if (codec->id == AV_CODEC_ID_ALAC) {
        codecCtx->sample_fmt = (policy.bitDepth == 16 && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16P))
                                   ? AV_SAMPLE_FMT_S16P
                                   : AV_SAMPLE_FMT_S32P;
    } else if (codec->id == AV_CODEC_ID_FLAC) {
        codecCtx->sample_fmt = (policy.bitDepth == 16 && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16))
                                   ? AV_SAMPLE_FMT_S16
                                   : AV_SAMPLE_FMT_S32;
    } else if (codecName == "pcm_s16le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    } else if (codecName == "pcm_s24le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S32)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S32;
    } else if (codecName == "pcm_f32le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_FLT)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_FLT;
    } else if (const auto formats =
                   GetCodecConfigs<AVSampleFormat>(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, "sample formats");
               formats.values && formats.count > 0) {
        // iterating to find best supported format
        enum AVSampleFormat best = AV_SAMPLE_FMT_NONE;

        // Preference list: Float Planar > Float > S16 Planar > S16 > S32 Planar
        enum AVSampleFormat preferences[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,  AV_SAMPLE_FMT_S16P,
                                             AV_SAMPLE_FMT_S16,  AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32};

        for (auto pref : preferences) {
            for (int i = 0; i < formats.count; ++i) {
                if (formats.values[i] == pref) {
                    best = pref;
                    break;
                }
            }
            if (best != AV_SAMPLE_FMT_NONE)
                break;
        }

        if (best == AV_SAMPLE_FMT_NONE) {
            // Fallback to first
            best = formats.values[0];
        }

        codecCtx->sample_fmt = best;
        DLL_Log("[AudioEncoder] Selected sample format: %d (%s)", codecCtx->sample_fmt,
                av_get_sample_fmt_name(codecCtx->sample_fmt));
    } else {
        // No sample formats listed - likely PCM or permissive encoder
        // Default to S16 for compatibility, or FLT if modern
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
        DLL_Log("[AudioEncoder] No sample formats listed, defaulting to S16");
    }

    int configuredBitrate = config.bitrate;
    if (policy.scaleLossyBitrate) {
        configuredBitrate = ScaleLossyBitrateKbps(configuredBitrate, outputChannels);
    }
    if (configuredBitrate > 0) {
        codecCtx->bit_rate = configuredBitrate * 1000;
        if (policy.scaleLossyBitrate && configuredBitrate != config.bitrate) {
            DLL_Log("[AudioEncoder] Scaled lossy bitrate for %d channels: %d -> %d Kbps", outputChannels,
                    config.bitrate, configuredBitrate);
        }
    }

    // Parse sample rate - "default" means use 48000, otherwise parse as int.
    // ParseSampleRateOr never throws on malformed config (would crash encoder init).
    int sampleRate = ce::audio::ParseSampleRateOr(config.sampleRate, 48000);
    if (codec->id == AV_CODEC_ID_OPUS && sampleRate != 48000) {
        DLL_Log("[AudioEncoder] Opus requires 48000Hz output, adjusting sample_rate from %d to 48000", sampleRate);
        sampleRate = 48000;
    }
    codecCtx->sample_rate = sampleRate;

    // Check if sample rate is supported
    const auto sampleRates = GetCodecConfigs<int>(codec, AV_CODEC_CONFIG_SAMPLE_RATE, "sample rates");
    if (sampleRates.values && sampleRates.count > 0) {
        int best_rate = 0;
        for (int i = 0; i < sampleRates.count; i++) {
            if (sampleRates.values[i] == codecCtx->sample_rate) {
                best_rate = codecCtx->sample_rate;
                break;
            }
            if (!best_rate ||
                abs(sampleRates.values[i] - codecCtx->sample_rate) < abs(best_rate - codecCtx->sample_rate)) {
                best_rate = sampleRates.values[i];
            }
        }
        if (best_rate != codecCtx->sample_rate) {
            DLL_Log("[AudioEncoder] Adjusting sample rate from %d to %d", codecCtx->sample_rate, best_rate);
            codecCtx->sample_rate = best_rate;
        }
    }

    AVChannelLayout chLayout{};
    if (outputChannelMask != 0 && av_channel_layout_from_mask(&chLayout, outputChannelMask) < 0) {
        DLL_Log("[AudioEncoder] Unknown output channel mask 0x%x; using default layout for %d channels",
                outputChannelMask, outputChannels);
        av_channel_layout_default(&chLayout, outputChannels);
        outputChannelMask = 0;
    } else if (outputChannelMask == 0) {
        av_channel_layout_default(&chLayout, outputChannels);
    }
    if (chLayout.nb_channels > 0 && chLayout.nb_channels != outputChannels) {
        DLL_Log("[AudioEncoder] Channel mask 0x%x resolved to %d channels, overriding configured %d", outputChannelMask,
                chLayout.nb_channels, outputChannels);
        outputChannels = chLayout.nb_channels;
    }

    // Validate the resolved layout against the codec's supported layouts. Some
    // codecs accept only a fixed set (ALAC supports 7.1(wide) but NOT plain 7.1),
    // so a surround endpoint fed verbatim makes avcodec_open2 fail with EINVAL and
    // drops ALL audio for the track (observed: an 8ch/7.1 default endpoint silently
    // produced a recording with no audio). Remap to a usable layout instead so
    // audio is always recorded.
    if (!CodecSupportsChannelLayout(codec, &chLayout)) {
        char reqDesc[128] = {0};
        av_channel_layout_describe(&chLayout, reqDesc, sizeof(reqDesc));
        AVChannelLayout remapped{};
        if (PickSupportedChannelLayout(codec, chLayout.nb_channels, &remapped)) {
            char newDesc[128] = {0};
            av_channel_layout_describe(&remapped, newDesc, sizeof(newDesc));
            const bool sameCount = remapped.nb_channels == chLayout.nb_channels;
            // Same channel count -> relabel only: keep outputChannelMask (the
            // resampler's output layout) at the original so every channel's samples
            // pass through unchanged; the encoder simply tags them with the
            // codec-supported layout. Different count -> downmix, and retarget the
            // resampler (outputChannelMask/outputChannels) to the new layout.
            DLL_Log("[AudioEncoder] Codec %s rejects channel layout '%s'; %s to '%s' (%d ch) so audio is preserved",
                    codecName.c_str(), reqDesc, sameCount ? "relabeling" : "downmixing", newDesc, remapped.nb_channels);
            av_channel_layout_uninit(&chLayout);
            av_channel_layout_copy(&chLayout, &remapped);
            if (!sameCount) {
                outputChannels = chLayout.nb_channels;
                outputChannelMask = (chLayout.order == AV_CHANNEL_ORDER_NATIVE) ? static_cast<uint32_t>(chLayout.u.mask)
                                                                                : DefaultChannelMask(outputChannels);
            }
            av_channel_layout_uninit(&remapped);
        } else {
            DLL_Log("[AudioEncoder] WARNING: codec %s lists no usable channel layout for '%s'; open may fail",
                    codecName.c_str(), reqDesc);
        }
    }

    av_channel_layout_copy(&codecCtx->ch_layout, &chLayout);
    av_channel_layout_uninit(&chLayout);

    DLL_Log("[AudioEncoder] Opening codec: %s sample_rate=%d sample_fmt=%d channels=%d mask=0x%x bit_depth=%d",
            codecName.c_str(), codecCtx->sample_rate, codecCtx->sample_fmt, codecCtx->ch_layout.nb_channels,
            outputChannelMask, policy.bitDepth);

    // ALAC: must set bits_per_raw_sample BEFORE avcodec_open2.
    // The encoder reads this during init to write the magic cookie (extradata).
    // Setting it after open causes a mismatch: extradata says 16-bit, stream
    // header says 32-bit, which makes decoders produce silence or white noise.
    if (codec->id == AV_CODEC_ID_ALAC || codec->id == AV_CODEC_ID_FLAC) {
        codecCtx->bits_per_raw_sample = policy.bitDepth == 16 ? 16 : (policy.bitDepth == 32 ? 32 : 24);
    }

    // Allow experimental codecs (like Opus in some builds)
    codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    AVDictionary* codecOptions = nullptr;
    if (codec->id == AV_CODEC_ID_OPUS && codecName == "libopus") {
        av_dict_set(&codecOptions, "application", "audio", 0);
        av_dict_set(&codecOptions, "frame_duration", "20", 0);
        av_dict_set(&codecOptions, "dtx", "0", 0);
    } else if (codec->id == AV_CODEC_ID_AAC && codecName == "aac") {
        // Current FFmpeg master exposes its new noise-to-mask ratio trellis
        // coder through the native "aac" encoder. Select its slowest/best
        // quality mode explicitly so a future default change cannot silently
        // downgrade recordings.
        av_dict_set(&codecOptions, "aac_coder", "nmr", 0);
        av_dict_set(&codecOptions, "aac_nmr_speed", "0", 0);
    }

    int ret = avcodec_open2(codecCtx, codec, &codecOptions);
    const AVDictionaryEntry* unusedCodecOption = av_dict_get(codecOptions, "", nullptr, AV_DICT_IGNORE_SUFFIX);
    if (ret >= 0 && unusedCodecOption) {
        DLL_Log("[AudioEncoder] ERROR: codec %s rejected runtime option %s=%s", codecName.c_str(),
                unusedCodecOption->key, unusedCodecOption->value);
        ret = AVERROR_OPTION_NOT_FOUND;
    }
    av_dict_free(&codecOptions);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[AudioEncoder] Failed to open codec: %d (%s)", ret, errbuf);
        avcodec_free_context(&codecCtx);
        return false;
    }

    if (codecCtx->sample_rate > 0) {
        codecCtx->time_base = {1, codecCtx->sample_rate};
    }

    bool nativeAacNmr = false;
    int64_t nativeAacNmrSpeed = -1;
    if (codec->id == AV_CODEC_ID_AAC && codecName == "aac") {
        int64_t selectedCoder = -1;
        const AVOption* nmrOption = av_opt_find(codecCtx->priv_data, "nmr", "coder", 0, 0);
        if (!nmrOption || av_opt_get_int(codecCtx->priv_data, "aac_coder", 0, &selectedCoder) < 0 ||
            av_opt_get_int(codecCtx->priv_data, "aac_nmr_speed", 0, &nativeAacNmrSpeed) < 0 ||
            selectedCoder != nmrOption->default_val.i64 || nativeAacNmrSpeed != 0) {
            DLL_Log(
                "[AudioEncoder] ERROR: native AAC did not retain required NMR best-quality contract: "
                "coder=%lld expected=%lld speed=%lld",
                static_cast<long long>(selectedCoder),
                static_cast<long long>(nmrOption ? nmrOption->default_val.i64 : -1),
                static_cast<long long>(nativeAacNmrSpeed));
            avcodec_free_context(&codecCtx);
            return false;
        }
        nativeAacNmr = true;
    }

    // Save codec ID and name for recreation in Stop()
    savedCodecId = codec->id;
    savedCodecName = codecName;

    const bool supportsVariableFrame = (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0;
    const bool supportsSmallLastFrame = (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
    const bool isPcm = codecName.rfind("pcm_", 0) == 0;
    runtimeContract = {};
    runtimeContract.encoderName = codecName;
    runtimeContract.codecId = codec->id;
    runtimeContract.sampleFormat = codecCtx->sample_fmt;
    runtimeContract.sampleRate = codecCtx->sample_rate;
    runtimeContract.channels = codecCtx->ch_layout.nb_channels;
    runtimeContract.channelMask = outputChannelMask;
    runtimeContract.rawBitDepth = codecCtx->bits_per_raw_sample;
    runtimeContract.frameSize = codecCtx->frame_size;
    runtimeContract.capabilities = codec->capabilities;
    runtimeContract.initialPadding = codecCtx->initial_padding;
    runtimeContract.finalFramePolicy = isPcm
                                           ? FinalFramePolicy::BlockAlignedPcm
                                           : (allowShortFinalFrame && (supportsVariableFrame || supportsSmallLastFrame)
                                                  ? FinalFramePolicy::ExactShortFrame
                                                  : FinalFramePolicy::PadAndSignalDiscard);
    runtimeContract.requiresMatroskaCodecDelay = codec->id == AV_CODEC_ID_OPUS || codec->id == AV_CODEC_ID_AAC;
    runtimeContract.requiresMatroskaDiscardPadding =
        runtimeContract.finalFramePolicy == FinalFramePolicy::PadAndSignalDiscard;
    runtimeContract.nativeAacNmr = nativeAacNmr;
    runtimeContract.nativeAacNmrSpeed = static_cast<int>(nativeAacNmrSpeed);
    runtimeContract.valid = true;

    if (codec->id == AV_CODEC_ID_OPUS &&
        (codecName != "libopus" || codecCtx->sample_rate != 48000 || codecCtx->frame_size != 960)) {
        DLL_Log(
            "[AudioEncoder] ERROR: invalid Opus runtime contract: encoder=%s rate=%d frame=%d "
            "(required libopus/48000/960)",
            codecName.c_str(), codecCtx->sample_rate, codecCtx->frame_size);
        avcodec_free_context(&codecCtx);
        runtimeContract.valid = false;
        return false;
    }

    DLL_Log(
        "[AudioCodecContract] encoder=%s id=%d fmt=%s rate=%d channels=%d mask=0x%x rawBits=%d frame=%d "
        "caps=0x%x initialPadding=%d finalPolicy=%d codecDelay=%d discardPadding=%d aacNmr=%d aacNmrSpeed=%d",
        runtimeContract.encoderName.c_str(), runtimeContract.codecId,
        av_get_sample_fmt_name(runtimeContract.sampleFormat), runtimeContract.sampleRate, runtimeContract.channels,
        runtimeContract.channelMask, runtimeContract.rawBitDepth, runtimeContract.frameSize,
        runtimeContract.capabilities, runtimeContract.initialPadding,
        static_cast<int>(runtimeContract.finalFramePolicy), runtimeContract.requiresMatroskaCodecDelay ? 1 : 0,
        runtimeContract.requiresMatroskaDiscardPadding ? 1 : 0, runtimeContract.nativeAacNmr ? 1 : 0,
        runtimeContract.nativeAacNmrSpeed);

    // Create audio FIFO buffer - size based on 2 seconds worth of audio
    // This scales with sample rate (e.g., 96kHz gets larger buffer than 48kHz)
    fifoCapacity = codecCtx->sample_rate * 2;  // 2 seconds buffer
    audioFifo = av_audio_fifo_alloc(codecCtx->sample_fmt, codecCtx->ch_layout.nb_channels, fifoCapacity);
    if (!audioFifo) {
        DLL_Log("[AudioEncoder] Failed to allocate audio FIFO");
        avcodec_free_context(&codecCtx);
        return false;
    }
    DLL_Log("[AudioEncoder] FIFO allocated: %d samples (%.2f sec at %dHz)", fifoCapacity,
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            (float)fifoCapacity / codecCtx->sample_rate, codecCtx->sample_rate);

    // Alloc frame for encoding
    frame = av_frame_alloc();
    if (!frame) {
        DLL_Log("[AudioEncoder] Failed to allocate frame");
        av_audio_fifo_free(audioFifo);
        audioFifo = nullptr;
        avcodec_free_context(&codecCtx);
        return false;
    }

    // Set frame parameters
    // For ALAC and variable frame size codecs, frame_size might be 0
    int frame_size = codecCtx->frame_size;
    if (frame_size == 0) {
        frame_size = 4096;  // Default frame size for variable codecs
    }

    frame->nb_samples = frame_size;
    frame->format = codecCtx->sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout);
    frame->sample_rate = codecCtx->sample_rate;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[AudioEncoder] Failed to allocate frame buffer: %s", errbuf);
        av_frame_free(&frame);
        av_audio_fifo_free(audioFifo);
        audioFifo = nullptr;
        avcodec_free_context(&codecCtx);
        return false;
    }

    initDone = true;
    savedConfig = config;       // Save for potential reinit
    lastPacketTimestampMs = 0;  // Initialize timestamp tracker
    finalizationReport = {};
    finalizationReport.primingSamples = codecCtx->initial_padding;
    totalAcceptedSamples = 0;
    DLL_Log("[AudioEncoder] Initialization complete");
    return true;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
}

// gptreport.md Section 5.1: Buffer audio packets until streamIndex is available
void AudioEncoder::SetStreamIndex(int index) {
    if (streamIndex == index)
        return;

    // Always log to trace the issue
    DLL_Log("[AudioEnc] SetStreamIndex called: current=%d new=%d pending=%zu", streamIndex, index,
            pendingPackets.size());

    int oldIndex = streamIndex;
    streamIndex = index;

    // Flush any buffered packets now that we have a valid stream
    if (oldIndex < 0 && streamIndex >= 0 && !pendingPackets.empty()) {
        DLL_Log("[AudioEnc] Flushing %zu buffered packets to stream %d", pendingPackets.size(), streamIndex);
        for (AVPacket* pkt : pendingPackets) {
            pkt->stream_index = streamIndex;
            if (onPacket) {
                onPacket(pkt);
            }
            av_packet_free(&pkt);
        }
        pendingPackets.clear();
    }
}

bool AudioEncoder::ResetForRecordingStart(int64_t startUs, uint64_t generation) {
    if (generation == 0 || generation <= recordingResetGeneration) {
        return generation != 0;
    }

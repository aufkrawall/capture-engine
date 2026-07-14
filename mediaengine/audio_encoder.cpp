#include "audio_encoder.h"
#include <mmreg.h>
#include "audio_sync_utils.h"
#include "audio_time_utils.h"  // For ce::audio::ParseSampleRateOr
#include "../common/strict_integer_parse.h"
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
    Stop();
    ReleaseCodecResources();
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
    onPacket = packetCallback;

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
    runtimeContract.finalFramePolicy =
        isPcm ? FinalFramePolicy::BlockAlignedPcm
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

    const int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
    DLL_Log(
        "[AudioEnc] Timeline reset generation=%llu: startUs=%lld oldSamples=%lld "
        "oldResampled=%lld fifo=%d",
        static_cast<unsigned long long>(generation), static_cast<long long>(startUs),
        static_cast<long long>(samplesCount), static_cast<long long>(resampledSamplesTotal), fifoSize);

    recordingStartUs = startUs;
    firstTimestamp = -1;
    samplesCount = 0;
    resampledSamplesTotal = 0;
    totalAcceptedSamples = 0;
    lastPacketTimestampMs = 0;
    finalizationReport = {};
    finalizationReport.primingSamples = runtimeContract.initialPadding;
    lastInputTimestamp = -1;
    if (audioFifo) {
        av_audio_fifo_reset(audioFifo);
    }
    if (resampler) {
        resampler->Reset();
    }
    recordingResetGeneration = generation;
    return true;
}

void AudioEncoder::ApplyPacketDuration(AVPacket* pkt) {
    if (!pkt) {
        return;
    }

    ++finalizationReport.packetCount;
    finalizationReport.packetBytes += static_cast<uint64_t>(std::max(pkt->size, 0));
    size_t newExtradataSize = 0;
    const uint8_t* newExtradata = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &newExtradataSize);
    if (pkt->duration <= 0 && pkt->size == 0 && newExtradata && newExtradataSize > 0) {
        ++finalizationReport.controlPacketCount;
        DLL_Log(
            "[AudioEncoder] Encoder EOF control packet: encoder=%s stream=%d pts=%lld newExtradata=%zu bytes",
            runtimeContract.encoderName.c_str(), streamIndex, static_cast<long long>(pkt->pts), newExtradataSize);
        return;
    }
    if (pkt->duration <= 0) {
        ++finalizationReport.durationlessPacketCount;
        finalizationReport.protocolError = true;
        DLL_Log(
            "[AudioEncoder] ERROR: encoder emitted unexplained durationless packet: encoder=%s stream=%d "
            "pts=%lld dts=%lld packet=%llu",
            runtimeContract.encoderName.c_str(), streamIndex, static_cast<long long>(pkt->pts),
            static_cast<long long>(pkt->dts), static_cast<unsigned long long>(finalizationReport.packetCount));
        return;
    }
    if (pkt->pts != AV_NOPTS_VALUE && pkt->pts <= INT64_MAX - pkt->duration) {
        finalizationReport.packetEndpointSamples =
            std::max(finalizationReport.packetEndpointSamples, pkt->pts + pkt->duration);
    }
    if (runtimeContract.finalFramePolicy == FinalFramePolicy::BlockAlignedPcm) {
        int bytesPerChannel = 0;
        if (runtimeContract.encoderName == "pcm_s16le") {
            bytesPerChannel = 2;
        } else if (runtimeContract.encoderName == "pcm_s24le") {
            bytesPerChannel = 3;
        } else if (runtimeContract.encoderName == "pcm_f32le") {
            bytesPerChannel = 4;
        }
        const int blockAlign = bytesPerChannel * runtimeContract.channels;
        if (blockAlign <= 0 || pkt->size < 0 || pkt->size % blockAlign != 0 ||
            pkt->duration != pkt->size / blockAlign) {
            finalizationReport.protocolError = true;
            DLL_Log(
                "[AudioEncoder] ERROR: PCM packet violates block alignment: encoder=%s stream=%d bytes=%d "
                "blockAlign=%d duration=%lld",
                runtimeContract.encoderName.c_str(), streamIndex, pkt->size, blockAlign,
                static_cast<long long>(pkt->duration));
        }
    }
}

AudioEncoder::EncodeResult AudioEncoder::EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate,
                                                       int bitsPerSample, int validBitsPerSample, int blockAlign,
                                                       bool isFloat, int64_t timestamp) {
    return EncodeSamples(data, sizeBytes, channels, sampleRate, bitsPerSample, validBitsPerSample, blockAlign, isFloat,
                         0, timestamp);
}

AudioEncoder::EncodeResult AudioEncoder::EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate,
                                                       int bitsPerSample, int validBitsPerSample, int blockAlign,
                                                       bool isFloat, uint32_t channelMask, int64_t timestamp) {
    EncodeResult result;
    int64_t submittedBefore = samplesCount;
    // If encoder was invalidated (reopen failed in Stop), try to reinit
    if (!initDone && !savedConfig.codec.empty()) {
        DLL_Log("[AudioEnc] Attempting reinit after previous failure");
        if (!Init(savedConfig, onPacket)) {
            DLL_Log("[AudioEnc] Reinit failed, cannot encode");
            result.failed = true;
            return result;
        }
    }

    if (!initDone || !codecCtx || !data || sizeBytes <= 0) {
        result.failed = true;
        return result;
    }

    submittedBefore = samplesCount;

    // CRITICAL: Discard audio samples that arrive before first video frame
    // This ensures 0ms A/V sync - audio should not be encoded until video starts
    if (recordingStartUs < 0) {
        // Recording start not set yet (waiting for first video frame)
        // Silently discard this audio data
        return result;
    }

    // CRITICAL: Discard audio samples that arrive after last video frame
    // This ensures audio track ends exactly when video ends
    // Check against recordingEndUs (using microsecond precision)
    int64_t timestampUs = timestamp * 1000;
    if (recordingEndUs > 0 && timestampUs > recordingEndUs) {
        // Recording has ended, discard this audio data
        return result;
    }

    // Build input format descriptor
    AudioResampler::InputFormat inputFmt;
    inputFmt.channels = channels;
    inputFmt.sampleRate = sampleRate;
    inputFmt.bitsPerSample = bitsPerSample;
    inputFmt.validBitsPerSample = validBitsPerSample;
    inputFmt.isFloat = isFloat;
    inputFmt.blockAlign = blockAlign;
    inputFmt.channelMask = channelMask;

    // Initialize or reinitialize resampler if format changed
    bool needsInit = !resampler || !resampler->IsReady();
    if (!needsInit) {
        // Check if format changed
        needsInit =
            (currentInputFormat.channels != inputFmt.channels || currentInputFormat.sampleRate != inputFmt.sampleRate ||
             currentInputFormat.bitsPerSample != inputFmt.bitsPerSample ||
             currentInputFormat.validBitsPerSample != inputFmt.validBitsPerSample ||
             currentInputFormat.isFloat != inputFmt.isFloat || currentInputFormat.channelMask != inputFmt.channelMask);
    }

    if (needsInit) {
        if (!resampler) {
            resampler = std::make_unique<AudioResampler>();
        }

        AudioResampler::OutputFormat outputFmt;
        outputFmt.channels = codecCtx->ch_layout.nb_channels;
        outputFmt.sampleRate = codecCtx->sample_rate;
        outputFmt.sampleFmt = codecCtx->sample_fmt;
        outputFmt.channelMask = outputChannelMask;

        if (!resampler->Init(inputFmt, outputFmt)) {
            DLL_Log("[AudioEnc] Failed to init resampler");
            result.failed = true;
            return result;
        }

        currentInputFormat = inputFmt;
        DLL_Log("[AudioEnc] Resampler initialized: %dHz %dch mask=0x%x %s%d -> %dHz %dch mask=0x%x fmt=%d", sampleRate,
                channels, channelMask, isFloat ? "float" : "int", bitsPerSample, codecCtx->sample_rate,
                outputFmt.channels, outputFmt.channelMask, (int)outputFmt.sampleFmt);
    }

    // Resample using AudioResampler
    uint8_t** resampledData = nullptr;
    int convertedSamples = 0;

    if (!resampler->Process(data, sizeBytes, &resampledData, &convertedSamples)) {
        DLL_Log("[AudioEnc] Resample failed");
        result.failed = true;
        return result;
    }

    if (convertedSamples <= 0) {
        AudioResampler::FreeOutputBuffer(resampledData);
        return result;
    }

    // NOTE: Fade-in is applied upstream in PullAndEncodeAudio (mediaengine.cpp)
    // before samples reach this encoder. Applying a second fade here would
    // produce a squared fade curve and an overly long startup artifact.

    // SAFETY: Check FIFO size BEFORE writing to prevent overflow
    // Dropping NEWEST samples maintains timeline continuity (avoids temporal jumps)
    // whereas draining OLDEST samples causes A/V desync and clicks
    if (codecCtx->sample_rate <= 0) {
        DLL_Log("[AudioEnc] ERROR: sample_rate=%d, codec context invalid, skipping encode", codecCtx->sample_rate);
        AudioResampler::FreeOutputBuffer(resampledData);
        result.failed = true;
        return result;
    }
    // This call may be the first one for a track that was held behind a
    // packetless co-mixed source.  Accept the complete batch and let
    // av_audio_fifo_write grow/drain the FIFO instead of truncating every such
    // track at the old five-second ceiling.
    int currentFifoSize = av_audio_fifo_size(audioFifo);
    const int MAX_FIFO_SAMPLES = std::max(codecCtx->sample_rate * 5, currentFifoSize + std::max(convertedSamples, 0));
    const int CROSSFADE_SAMPLES = codecCtx->sample_rate / 50;  // 20ms - smoother overflow handling
    int samplesToWrite = convertedSamples;
    bool applyingFadeOut = false;

    if (recordingEndUs > 0 && recordingStartUs >= 0 && recordingEndUs >= recordingStartUs) {
        const int64_t durationUs = recordingEndUs - recordingStartUs;
        const int64_t maxSamples = ce::audio::ComputeDurationUsToSamples(durationUs, codecCtx->sample_rate);
        const int64_t allowedSamples =
            ce::audio::ComputeAudioSamplesAllowedBeforeEnd(maxSamples, samplesCount, currentFifoSize);
        if (allowedSamples <= 0) {
            static int endDropLogCount = 0;
            if (endDropLogCount++ < 5) {
                DLL_Log(
                    "[AudioEnc] End boundary reached: dropping %d samples before FIFO write "
                    "(encoded=%lld fifo=%d max=%lld)",
                    convertedSamples, (long long)samplesCount, currentFifoSize, (long long)maxSamples);
            }
            AudioResampler::FreeOutputBuffer(resampledData);
            return result;
        }
        if (samplesToWrite > allowedSamples) {
            DLL_Log(
                "[AudioEnc] Clamping audio write at recording end: write=%d -> %lld "
                "(encoded=%lld fifo=%d max=%lld)",
                samplesToWrite, (long long)allowedSamples, (long long)samplesCount, currentFifoSize,
                (long long)maxSamples);
            samplesToWrite = static_cast<int>(std::min<int64_t>(allowedSamples, INT_MAX));
        }
    }

    if (currentFifoSize + samplesToWrite > MAX_FIFO_SAMPLES) {
        samplesToWrite = MAX_FIFO_SAMPLES - currentFifoSize;
        if (samplesToWrite < 0)
            samplesToWrite = 0;

        if (!wasDroppingSamples && samplesToWrite > 0) {
            applyingFadeOut = true;
        }

        if (!wasDroppingSamples) {
            DLL_Log(
                "[AudioEnc] FIFO NEAR OVERFLOW: size=%d + new=%d > max=%d. "
                "Writing %d samples, dropping %d newest (maintains timeline)",
                currentFifoSize, convertedSamples, MAX_FIFO_SAMPLES, samplesToWrite, convertedSamples - samplesToWrite);
            // Enhanced FIFO stats logging
            DLL_Log("[AudioEnc] FIFO stats: droppedTotal=%llu, fifoLogCounter=%d, samplesCount=%lld, streamIdx=%d",
                    (unsigned long long)totalDroppedSamples, fifoLogCounter, (long long)samplesCount, streamIndex);
        }

        wasDroppingSamples = true;
        totalDroppedSamples += convertedSamples - samplesToWrite;

        if (samplesToWrite == 0) {
            AudioResampler::FreeOutputBuffer(resampledData);
            result.failed = true;
            return result;
        }
    } else if (wasDroppingSamples) {
        wasDroppingSamples = false;
        DLL_Log("[AudioEnc] FIFO recovered after dropping %lld total samples, applying fade-in",
                (long long)totalDroppedSamples);
        totalDroppedSamples = 0;
    }

    if (applyingFadeOut && samplesToWrite > 0 && samplesToWrite < convertedSamples) {
        int fadeStart = std::max(0, samplesToWrite - CROSSFADE_SAMPLES);
        int channels = codecCtx->ch_layout.nb_channels;
        int numPlanes = av_sample_fmt_is_planar(codecCtx->sample_fmt) ? channels : 1;

        for (int p = 0; p < numPlanes; p++) {
            if (codecCtx->sample_fmt == AV_SAMPLE_FMT_FLT || codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                float* fData = (float*)resampledData[p];
                for (int i = fadeStart; i < samplesToWrite; i++) {
                    float fadePos = (float)(samplesToWrite - 1 - i) / CROSSFADE_SAMPLES;
                    float gain = fadePos < 1.0f ? fadePos : 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            fData[i * channels + c] *= gain;
                    } else {
                        fData[i] *= gain;
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S16 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                int16_t* sData = (int16_t*)resampledData[p];
                for (int i = fadeStart; i < samplesToWrite; i++) {
                    float fadePos = (float)(samplesToWrite - 1 - i) / CROSSFADE_SAMPLES;
                    float gain = fadePos < 1.0f ? fadePos : 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
                    } else {
                        sData[i] = (int16_t)(sData[i] * gain);
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                int32_t* sData = (int32_t*)resampledData[p];
                for (int i = fadeStart; i < samplesToWrite; i++) {
                    float fadePos = (float)(samplesToWrite - 1 - i) / CROSSFADE_SAMPLES;
                    float gain = fadePos < 1.0f ? fadePos : 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);
                    } else {
                        sData[i] = (int32_t)(sData[i] * gain);
                    }
                }
            }
        }
    }

    if (wasDroppingSamples && !applyingFadeOut && samplesToWrite > 0) {
        int fadeEnd = std::min(samplesToWrite, CROSSFADE_SAMPLES);
        int channels = codecCtx->ch_layout.nb_channels;
        int numPlanes = av_sample_fmt_is_planar(codecCtx->sample_fmt) ? channels : 1;

        for (int p = 0; p < numPlanes; p++) {
            if (codecCtx->sample_fmt == AV_SAMPLE_FMT_FLT || codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                float* fData = (float*)resampledData[p];
                for (int i = 0; i < fadeEnd; i++) {
                    float gain = (float)(i + 1) / CROSSFADE_SAMPLES;
                    if (gain > 1.0f)
                        gain = 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            fData[i * channels + c] *= gain;
                    } else {
                        fData[i] *= gain;
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S16 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                int16_t* sData = (int16_t*)resampledData[p];
                for (int i = 0; i < fadeEnd; i++) {
                    float gain = (float)(i + 1) / CROSSFADE_SAMPLES;
                    if (gain > 1.0f)
                        gain = 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
                    } else {
                        sData[i] = (int16_t)(sData[i] * gain);
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                int32_t* sData = (int32_t*)resampledData[p];
                for (int i = 0; i < fadeEnd; i++) {
                    float gain = (float)(i + 1) / CROSSFADE_SAMPLES;
                    if (gain > 1.0f)
                        gain = 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);
                    } else {
                        sData[i] = (int32_t)(sData[i] * gain);
                    }
                }
            }
        }
    }

    int ret = av_audio_fifo_write(audioFifo, (void**)resampledData, samplesToWrite);

    if (ret < samplesToWrite) {
        DLL_Log("[AudioEnc] Failed to write to audio FIFO: wrote %d of %d", ret, samplesToWrite);
        result.failed = true;
    }
    result.acceptedSamples = std::max(ret, 0);
    resampledSamplesTotal += result.acceptedSamples;
    totalAcceptedSamples += result.acceptedSamples;

    AudioResampler::FreeOutputBuffer(resampledData);

    // NOTE: Gap detection REMOVED.
    // The MediaEngine pull model handles all timing by pulling exact sample
    // counts based on video timeline. This encoder just encodes what it receives.
    // No HARD RESYNC, no warping - just simple PTS = samplesCount.

    if (firstTimestamp < 0 && recordingStartUs >= 0) {
        firstTimestamp = timestamp;
        DLL_Log("[AudioEnc] First audio packet accepted: samples=%lld FIFO=%d PTS=0",
                static_cast<long long>(result.acceptedSamples), av_audio_fifo_size(audioFifo));
    }

    // Track latest packet timestamp for PTS calculation
    // This ensures audio PTS uses the same clock source as video (QPC)
    lastPacketTimestampMs = timestamp;

    // Encode while we have enough samples. PCM and some lossless encoders do
    // not report a fixed frame size; for those, emit the currently available
    // chunk promptly instead of waiting for an arbitrary large buffer.
    const int fixedFrameSize = codecCtx->frame_size;
    constexpr int kMaxVariableFrameSamples = 4096;
    const int logFrameSize = fixedFrameSize > 0 ? fixedFrameSize : kMaxVariableFrameSamples;

    // Periodic FIFO status (reduced frequency to avoid log spam)
    if (fifoLogCounter++ % 5000 == 0) {
        int currentSize = av_audio_fifo_size(audioFifo);

        // Warn if over 50% capacity (approx 1-2 seconds depending on sample rate)
        if (currentSize > fifoCapacity / 2) {
            DLL_Log("[AudioEnc] WARN: Audio FIFO high: %d/%d samples", currentSize, fifoCapacity);
        } else {
            DLL_Log("[AudioEnc] FIFO size=%d, frame_size=%d%s", currentSize, logFrameSize,
                    fixedFrameSize > 0 ? "" : " (variable)");
        }
    }

    while (true) {
        int fifoSamples = av_audio_fifo_size(audioFifo);
        if (fifoSamples <= 0 || (fixedFrameSize > 0 && fifoSamples < fixedFrameSize)) {
            break;
        }

        int frame_size = fixedFrameSize > 0 ? fixedFrameSize : std::min(fifoSamples, kMaxVariableFrameSamples);
        if (frame_size <= 0) {
            break;
        }

        // Make frame writable
        ret = av_frame_make_writable(frame);
        if (ret < 0) {
            DLL_Log("[AudioEnc] Failed to make frame writable: %d", ret);
            result.failed = true;
            result.submittedSamples = samplesCount - submittedBefore;
            return result;
        }

        frame->nb_samples = frame_size;

        // Read from FIFO into frame
        ret = av_audio_fifo_read(audioFifo, (void**)frame->data, frame_size);
        if (ret < frame_size) {
            DLL_Log("[AudioEnc] Failed to read from FIFO: got %d, expected %d", ret, frame_size);
            result.failed = true;
            result.submittedSamples = samplesCount - submittedBefore;
            return result;
        }

        // Set PTS using simple sample counting from 0 (CFR audio)
        // This matches how video PTS is calculated - simple frame counting
        // Video: pts = frame_number (0, 1, 2, 3...)
        // Audio: pts = samples_encoded (0, 4096, 8192...)
        // Both are constant-rate clocks that stay perfectly synchronized
        frame->pts = samplesCount;

        // Debug: Log first 10 frames for each encoder to track PTS
        if (frameLogCounter++ < 10) {
            DLL_Log(
                "[AudioEnc] FRAME PTS DEBUG: pts=%lld (%.3f sec) "
                "samplesCount=%lld streamIdx=%d ptsOffset=0",
                (long long)frame->pts, (double)frame->pts / codecCtx->sample_rate, (long long)samplesCount,
                streamIndex);
        }

        // Encode frame
        ret = avcodec_send_frame(codecCtx, frame);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[AudioEnc] avcodec_send_frame failed: %s (code=%d)", errbuf, ret);
            result.failed = true;
            continue;
        }
        // Only advance PTS counter after a successful send so tracking stays accurate
        samplesCount += frame_size;

        // Receive packets
        int pktCount = 0;
        while (true) {
            AVPacket* pkt = av_packet_alloc();
            ret = avcodec_receive_packet(codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&pkt);
                break;
            }
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                DLL_Log("[AudioEnc] avcodec_receive_packet failed: %s", errbuf);
                av_packet_free(&pkt);
                break;
            }

            ApplyPacketDuration(pkt);
            pktCount++;
            // Buffer packets until streamIndex is set by SetStreamIndex().
            // Otherwise we'd write audio packets with the wrong stream index.
            if (streamIndex < 0) {
                if (!warnedOnce) {
                    DLL_Log("[AudioEnc] Buffering audio packets - stream not yet assigned");
                    warnedOnce = true;
                }
                // Clone packet and add to pending buffer instead of dropping
                // Limit pending buffer size to prevent unbounded growth
                static const size_t MAX_PENDING_PACKETS = 1000;
                if (pendingPackets.size() >= MAX_PENDING_PACKETS) {
                    if (!warnedMax) {
                        DLL_Log(
                            "[AudioEnc] WARNING: Pending packet buffer full (%zu), "
                            "dropping oldest packet",
                            MAX_PENDING_PACKETS);
                        warnedMax = true;
                    }
                    AVPacket* oldest = pendingPackets.front();
                    av_packet_free(&oldest);
                    pendingPackets.pop_front();
                }
                AVPacket* cloned = av_packet_clone(pkt);
                if (cloned) {
                    pendingPackets.push_back(cloned);
                } else {
                    DLL_Log("[AudioEnc] ERROR: av_packet_clone failed, dropping packet (size=%d)", pkt->size);
                }
                av_packet_free(&pkt);
                continue;
            }

            // Success - call callback with correct stream index
            pkt->stream_index = streamIndex;
            if (onPacket) {
                onPacket(pkt);
            } else {
                DLL_Log("[AudioEnc] WARNING: onPacket callback is NULL!");
            }
            av_packet_free(&pkt);
        }

        // Log if we didn't get any packets after sending a frame
        if (pktCount == 0) {
            noPacketCount++;
            if (noPacketCount == 1 || noPacketCount % 100 == 1) {
                DLL_Log("[AudioEnc] No packets received after send_frame (count=%d, streamIdx=%d, samplesCount=%lld)",
                        noPacketCount, streamIndex, (long long)samplesCount);
            }
        }
    }
    result.submittedSamples = samplesCount - submittedBefore;
    return result;
}

void AudioEncoder::Stop() {
    if (initDone) {
        Flush();
    }

    // Reset all state for next recording
    DLL_Log("[AudioEnc] Stop: Resetting state (samplesCount=%lld, streamIndex=%d)", (long long)samplesCount,
            streamIndex);

    samplesCount = 0;
    streamIndex = -1;  // Critical for next run
    firstTimestamp = -1;
    recordingStartUs = -1;
    recordingEndUs = 0;
    resampledSamplesTotal = 0;  // Reset resampler output counter for next recording
    lastInputTimestamp = -1;    // Reset continuity tracking
    lastPacketTimestampMs = 0;  // Reset PTS tracker
    warnedOnce = false;         // Reset per-recording warning flags
    warnedMax = false;
    fifoLogCounter = 0;
    frameLogCounter = 0;
    noPacketCount = 0;
    wasDroppingSamples = false;
    totalDroppedSamples = 0;
    totalAcceptedSamples = 0;
    // Clear any pending packets
    for (auto* pkt : pendingPackets) {
        av_packet_free(&pkt);
    }
    pendingPackets.clear();

    // A recording owns one fresh codec context/FIFO/frame/resampler lifetime.
    // Destroy the drained context instead of attempting unsupported generic
    // avcodec_flush_buffers() reuse (AAC/ALAC/FLAC/Opus/PCM all warn or retain
    // codec-private state under that pattern).
    ReleaseCodecResources();
}

void AudioEncoder::Flush() {
    if (!initDone || !codecCtx)
        return;

    DLL_Log("[AudioEncoder] Flushing encoder...");

    // Calculate maximum samples to encode based on video duration
    // This ensures audio ends exactly when video ends (Microsecond precision)
    int64_t targetSamples = INT64_MAX;  // Exact video-authoritative duration
    int64_t maxSamples = INT64_MAX;     // Samples submitted to the codec, including required padding
    if (recordingEndUs > 0 && recordingStartUs >= 0 && recordingEndUs >= recordingStartUs) {
        int64_t durationUs = recordingEndUs - recordingStartUs;

        // Use the same rounded sample target as MediaEngine stop diagnostics so
        // final packets cannot extend beyond the CFR video timeline by a codec
        // frame after metadata has already been clamped.
        targetSamples = ce::audio::ComputeDurationUsToSamples(durationUs, codecCtx->sample_rate);
        maxSamples = targetSamples;

        DLL_Log(
            "[AudioEncoder] Video duration: %lld us, target audio samples: %lld, "
            "current: %lld",
            durationUs, targetSamples, samplesCount);
    }

    const int fixedFrameSize = codecCtx->frame_size;
    const bool supportsVariableFrame =
        codecCtx->codec && (codecCtx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
    const bool supportsSmallLastFrame =
        codecCtx->codec && (codecCtx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);
    const bool canSendShortFrame = allowShortFinalFrame && (supportsVariableFrame || supportsSmallLastFrame);

    // Only round up to a frame boundary for codecs that need padded final
    // submission. AAC/Opus use this path deliberately so their final packet
    // timeline is clamped with explicit skip-samples metadata.
    if (!canSendShortFrame && fixedFrameSize > 0 && maxSamples != INT64_MAX) {
        int64_t rem = maxSamples % fixedFrameSize;
        if (rem != 0) {
            maxSamples += (fixedFrameSize - rem);
        }
    }

    bool finalDiscardSideDataAttached = false;
    int64_t finalDiscardSideDataSamples = 0;
    auto attachEndSkipSideData = [&](AVPacket* pkt, int64_t endSkipSamples) {
        if (!pkt || endSkipSamples <= 0 || endSkipSamples > UINT32_MAX) {
            return false;
        }
        size_t existingSize = 0;
        uint8_t* skipData = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &existingSize);
        uint32_t startSkipSamples = 0;
        uint32_t existingEndSkipSamples = 0;
        uint8_t startSkipReason = 0;
        if (skipData && existingSize >= 10) {
            startSkipSamples = AV_RL32(skipData);
            existingEndSkipSamples = AV_RL32(skipData + 4);
            startSkipReason = skipData[8];
        } else {
            skipData = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
        }
        if (!skipData) {
            DLL_Log("[AudioEncoder] WARNING: failed to add packet end-skip side data: stream=%d endSkip=%lld samples",
                    streamIndex, endSkipSamples);
            return false;
        }

        const uint64_t mergedEndSkip =
            std::min<uint64_t>(UINT32_MAX, static_cast<uint64_t>(existingEndSkipSamples) + endSkipSamples);
        AV_WL32(skipData, startSkipSamples);
        AV_WL32(skipData + 4, static_cast<uint32_t>(mergedEndSkip));
        skipData[8] = startSkipReason;
        skipData[9] = 0;  // padding silence
        finalDiscardSideDataAttached = true;
        finalDiscardSideDataSamples = static_cast<int64_t>(mergedEndSkip);
        DLL_Log(
            "[AudioEncoder] Merged packet skip side data: stream=%d startSkip=%u existingEnd=%u addedEnd=%lld "
            "mergedEnd=%llu samples",
            streamIndex, startSkipSamples, existingEndSkipSamples, endSkipSamples,
            static_cast<unsigned long long>(mergedEndSkip));
        return true;
    };

    // Packets emitted while the terminal frames are submitted stay buffered
    // until EOF. This lets the actual final packet carry end-discard metadata
    // without rewriting encoder PTS/durations or guessing which receive call
    // will produce the last packet.
    std::vector<AVPacket*> flushedPackets;
    auto drainPackets = [&]() {
        while (true) {
            AVPacket* pkt = av_packet_alloc();
            int dret = avcodec_receive_packet(codecCtx, pkt);
            if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
                av_packet_free(&pkt);
                break;
            }
            if (dret < 0) {
                av_packet_free(&pkt);
                break;
            }
            ApplyPacketDuration(pkt);
            pkt->stream_index = streamIndex;
            flushedPackets.push_back(pkt);
        }
    };

    int64_t queuedSilenceSamplesTotal = 0;
    int silenceQueueLogCount = 0;
    auto queueSilenceToFifo = [&](int64_t silenceSamples) -> int {
        if (!audioFifo || silenceSamples <= 0) {
            return 0;
        }
        bool planarPre = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
        int nchPre = codecCtx->ch_layout.nb_channels;
        int bpsPre = av_get_bytes_per_sample(codecCtx->sample_fmt);
        int numPlanesPre = planarPre ? nchPre : 1;
        const size_t zeroBytes = ce::audio::ComputeAudioSampleBufferBytes(silenceSamples, bpsPre, nchPre);
        if (zeroBytes == 0) {
            DLL_Log("[AudioEncoder] Silence queue skipped invalid format: silenceSamples=%lld nch=%d bps=%d planar=%d",
                    silenceSamples, nchPre, bpsPre, (int)planarPre);
            return 0;
        }

        std::vector<uint8_t> zeroBuf(zeroBytes, 0);
        std::vector<uint8_t*> planePtrs(numPlanesPre);
        for (int plane = 0; plane < numPlanesPre; plane++) {
            // For planar: each channel gets its own region; for interleaved:
            // the single plane contains all channels packed together.
            planePtrs[plane] =
                zeroBuf.data() + ce::audio::ComputeAudioPlaneOffsetBytes(plane, silenceSamples, bpsPre, planarPre);
        }
        int written = av_audio_fifo_write(audioFifo, (void**)planePtrs.data(), (int)silenceSamples);
        const int64_t beforeQueued = queuedSilenceSamplesTotal;
        if (written > 0) {
            queuedSilenceSamplesTotal += written;
        }
        const bool crossedSecond =
            codecCtx->sample_rate > 0 && written > 0 &&
            (queuedSilenceSamplesTotal / codecCtx->sample_rate) != (beforeQueued / codecCtx->sample_rate);
        if (written <= 0 || silenceQueueLogCount < 3 || crossedSecond) {
            DLL_Log(
                "[AudioEncoder] Silence queue: requested=%lld wrote=%d totalQueued=%lld nch=%d bps=%d planar=%d "
                "bytes=%zu planes=%d (samplesCount=%lld targetSamples=%lld codecLimitSamples=%lld)",
                silenceSamples, written, queuedSilenceSamplesTotal, nchPre, bpsPre, (int)planarPre, zeroBytes,
                numPlanesPre, samplesCount, targetSamples, maxSamples);
        }
        ++silenceQueueLogCount;
        return written;
    };

    // Feed any required silence through the normal FIFO-drain encoder path. This
    // avoids sparse container gaps and keeps AAC/Opus/ALAC/FLAC/PCM handling
    // identical while still chunking long silent tails safely.
    DLL_Log(
        "[AudioEncoder] Post-duration: fixedFrameSize=%d canSendShortFrame=%d "
        "audioFifo=%p codecCtx=%p",
        fixedFrameSize, (int)canSendShortFrame, (void*)audioFifo, (void*)codecCtx);

    // Encode any remaining samples in FIFO and generate silence as needed (up
    // to the codec submission limit).
    if (audioFifo && frame) {
        int frame_size = codecCtx->frame_size ? codecCtx->frame_size : 4096;
        int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);
        int channels = codecCtx->ch_layout.nb_channels;
        bool planar = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
        int numPlanes = planar ? channels : 1;

        while (true) {
            int fifoSize = av_audio_fifo_size(audioFifo);
            if (fifoSize <= 0) {
                if (maxSamples == INT64_MAX || samplesCount >= maxSamples) {
                    break;
                }
                const int64_t remainingSilence = maxSamples - samplesCount;
                const int silenceChunk = (int)std::min<int64_t>(remainingSilence, std::max<int>(frame_size, 1));
                if (queueSilenceToFifo(silenceChunk) <= 0) {
                    break;
                }
                fifoSize = av_audio_fifo_size(audioFifo);
            }

            int64_t remainingAllowed = maxSamples - samplesCount;
            if (remainingAllowed <= 0) {
                DLL_Log(
                    "[AudioEncoder] Already at sample limit, discarding %d "
                    "buffered samples",
                    fifoSize);
                av_audio_fifo_reset(audioFifo);
                break;
            }

            int samplesToRead =
                (int)std::min<int64_t>((int64_t)fifoSize, std::min<int64_t>((int64_t)frame_size, remainingAllowed));
            // Use an exact short final frame only for codecs that are known to
            // represent it cleanly. AAC/Opus use fixed-frame padding plus explicit
            // trailing discard metadata.
            int samplesToSend =
                canSendShortFrame ? samplesToRead : ((codecCtx->frame_size > 0) ? codecCtx->frame_size : samplesToRead);

            if (samplesToSend <= 0)
                break;

            if (frame->nb_samples != samplesToSend) {
                av_frame_unref(frame);
                frame->nb_samples = samplesToSend;
                frame->format = codecCtx->sample_fmt;
                av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout);
                frame->sample_rate = codecCtx->sample_rate;
                int bret = av_frame_get_buffer(frame, 0);
                if (bret < 0) {
                    break;
                }
            }

            int ret = av_frame_make_writable(frame);
            if (ret < 0) {
                break;
            }

            int rret = av_audio_fifo_read(audioFifo, (void**)frame->data, samplesToRead);
            if (rret < samplesToRead) {
                break;
            }

            if (samplesToSend > samplesToRead) {
                int framePlaneStride = sampleSize * (planar ? 1 : channels);
                int padBytes = (samplesToSend - samplesToRead) * framePlaneStride;
                for (int p = 0; p < numPlanes && frame->data[p]; p++) {
                    uint8_t* dst = (uint8_t*)frame->data[p] + (samplesToRead * framePlaneStride);
                    memset(dst, 0, padBytes);
                }
            }

            frame->pts = samplesCount;

            ret = avcodec_send_frame(codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) {
                drainPackets();
                ret = avcodec_send_frame(codecCtx, frame);
            }
            if (ret < 0) {
                break;
            }
            samplesCount += samplesToSend;
            drainPackets();
        }
    }

    // Calculate discard padding for sample-accurate trimming.
    // With canSendShortFrame the last frame is exactly the right size, so
    // discardPaddingSamples == 0 in most cases.
    // For fixed-frame codecs (canSendShortFrame=false) maxSamples was rounded up,
    // so the last encoded frame may overshoot the target by up to (frame_size-1)
    // samples; those excess samples must be signalled for decoder-side discard.
    int64_t discardPaddingSamples = 0;
    if (!canSendShortFrame && recordingEndUs > 0 && maxSamples != INT64_MAX) {
        discardPaddingSamples = samplesCount - targetSamples;

        if (discardPaddingSamples > 0 && codecCtx->frame_size > 0 && discardPaddingSamples < codecCtx->frame_size) {
            DLL_Log(
                "[AudioEncoder] Setting trailing_padding for sample-accurate "
                "end: %lld samples (target=%lld, actual=%lld)",
                discardPaddingSamples, targetSamples, samplesCount);
            codecCtx->trailing_padding = (int)discardPaddingSamples;
        }
    }

    DLL_Log(
        "[AudioEncoder] Flush: stream=%d samplesCount=%lld targetSamples=%lld codecLimitSamples=%lld "
        "discardPadding=%lld priming=%d trailing=%d",
        streamIndex, samplesCount, targetSamples, maxSamples, discardPaddingSamples, codecCtx->initial_padding,
        codecCtx->trailing_padding);
    // Drain exactly once. A drained encoder is destroyed by Stop(); encoder
    // contexts are never reused through avcodec_flush_buffers().
    const int drainStartResult = avcodec_send_frame(codecCtx, nullptr);
    if (drainStartResult < 0 && drainStartResult != AVERROR_EOF) {
        char errbuf[256];
        av_strerror(drainStartResult, errbuf, sizeof(errbuf));
        DLL_Log("[AudioEncoder] ERROR: failed to begin encoder drain: %s", errbuf);
        finalizationReport.protocolError = true;
    }
    // Final drain
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret == AVERROR_EOF) {
            finalizationReport.drainReachedEof = true;
            av_packet_free(&pkt);
            break;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[AudioEncoder] ERROR: encoder drain returned EAGAIN before EOF");
            finalizationReport.protocolError = true;
            av_packet_free(&pkt);
            break;
        }
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[AudioEncoder] ERROR: encoder drain failed before EOF: %s", errbuf);
            finalizationReport.protocolError = true;
            av_packet_free(&pkt);
            break;
        }
        ApplyPacketDuration(pkt);
        pkt->stream_index = streamIndex;  // Ensure correct stream index for flushed packets
        flushedPackets.push_back(pkt);
    }

    if (discardPaddingSamples > 0 && !finalDiscardSideDataAttached && !flushedPackets.empty()) {
        AVPacket* lastPkt = flushedPackets.back();
        attachEndSkipSideData(lastPkt, discardPaddingSamples);
    } else if (discardPaddingSamples > 0 && finalDiscardSideDataAttached) {
        DLL_Log(
            "[AudioEncoder] Final discard side data already attached while clamping: stream=%d endSkip=%lld/%lld "
            "samples",
            streamIndex, finalDiscardSideDataSamples, discardPaddingSamples);
    }

    for (AVPacket* pkt : flushedPackets) {
        if (onPacket) {
            onPacket(pkt);
        }
        av_packet_free(&pkt);
    }

    finalizationReport.timelineTargetSamples = targetSamples == INT64_MAX ? samplesCount : targetSamples;
    finalizationReport.inputTimelineSamples = totalAcceptedSamples;
    finalizationReport.codecSubmittedSamples = samplesCount;
    finalizationReport.primingSamples = codecCtx->initial_padding;
    finalizationReport.terminalPaddingSamples = std::max<int64_t>(0, discardPaddingSamples);
    finalizationReport.expectedDecodedSamples = finalizationReport.timelineTargetSamples;
    if (!finalizationReport.drainReachedEof || finalizationReport.durationlessPacketCount > 0 ||
        finalizationReport.codecSubmittedSamples < finalizationReport.timelineTargetSamples) {
        finalizationReport.protocolError = true;
    }
    DLL_Log(
        "[AudioFinalization] encoder=%s stream=%d target=%lld input=%lld expectedSilence=%lld submitted=%lld "
        "priming=%lld terminalPadding=%lld packetEnd=%lld expectedDecoded=%lld packets=%llu bytes=%llu "
        "controlPackets=%llu durationless=%llu drainEof=%d protocolError=%d",
        runtimeContract.encoderName.c_str(), streamIndex,
        static_cast<long long>(finalizationReport.timelineTargetSamples),
        static_cast<long long>(finalizationReport.inputTimelineSamples),
        static_cast<long long>(finalizationReport.expectedSourceSilenceSamples),
        static_cast<long long>(finalizationReport.codecSubmittedSamples),
        static_cast<long long>(finalizationReport.primingSamples),
        static_cast<long long>(finalizationReport.terminalPaddingSamples),
        static_cast<long long>(finalizationReport.packetEndpointSamples),
        static_cast<long long>(finalizationReport.expectedDecodedSamples),
        static_cast<unsigned long long>(finalizationReport.packetCount),
        static_cast<unsigned long long>(finalizationReport.packetBytes),
        static_cast<unsigned long long>(finalizationReport.controlPacketCount),
        static_cast<unsigned long long>(finalizationReport.durationlessPacketCount),
        finalizationReport.drainReachedEof ? 1 : 0, finalizationReport.protocolError ? 1 : 0);
    DLL_Log("[AudioEncoder] Flush complete");
}

#include "audio_encoder.h"
#include "audio_sync_utils.h"
#include "audio_time_utils.h"  // For ce::audio::ParseSampleRateOr
#include "mediaengine.h"       // For DLL_Log
#include <mmreg.h>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
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
    int depth = std::atoi(config.bitDepth.c_str());
    if (depth == 16 || depth == 24 || depth == 32) {
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

bool CodecSupportsSampleFormat(const AVCodec* codec, AVSampleFormat fmt) {
    if (!codec || !codec->sample_fmts) {
        return true;
    }
    for (const AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
        if (*p == fmt) {
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
    if (!codec || !codec->ch_layouts || !layout) {
        return true;  // No restriction advertised by the codec.
    }
    for (const AVChannelLayout* p = codec->ch_layouts; p->nb_channels != 0; ++p) {
        if (av_channel_layout_compare(p, layout) == 0) {
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
    if (!codec || !codec->ch_layouts || !out) {
        return false;
    }
    const AVChannelLayout* firstSupported = nullptr;
    const AVChannelLayout* stereo = nullptr;
    const AVChannelLayout* sameCount = nullptr;
    for (const AVChannelLayout* p = codec->ch_layouts; p->nb_channels != 0; ++p) {
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
    // Stop flushes but doesn't free resources (for multi-recording support)
    Stop();

    // Now free everything - AudioResampler cleans itself up via unique_ptr
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
}

bool AudioEncoder::Init(const AudioConfig& config, std::function<void(AVPacket*)> packetCallback) {
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

    // Clean up existing resources so Init() can be safely called for reinit
    if (codecCtx) {
        avcodec_free_context(&codecCtx);
    }
    if (frame) {
        av_frame_free(&frame);
        frame = nullptr;
    }
    if (audioFifo) {
        av_audio_fifo_free(audioFifo);
        audioFifo = nullptr;
    }
    initDone = false;

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[AudioEncoder] Failed to allocate codec context");
        return false;
    }

    // Lossless/PCM policies honor bit_depth. Lossy codecs encode from the float mix path.
    if (codec->id == AV_CODEC_ID_ALAC) {
        codecCtx->sample_fmt =
            (policy.bitDepth == 16 && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16P)) ? AV_SAMPLE_FMT_S16P
                                                                                             : AV_SAMPLE_FMT_S32P;
    } else if (codec->id == AV_CODEC_ID_FLAC) {
        codecCtx->sample_fmt =
            (policy.bitDepth == 16 && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16)) ? AV_SAMPLE_FMT_S16
                                                                                           : AV_SAMPLE_FMT_S32;
    } else if (codecName == "pcm_s16le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    } else if (codecName == "pcm_s24le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_S32)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S32;
    } else if (codecName == "pcm_f32le" && CodecSupportsSampleFormat(codec, AV_SAMPLE_FMT_FLT)) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_FLT;
    } else if (codec->sample_fmts) {
        // iterating to find best supported format
        const enum AVSampleFormat* p = codec->sample_fmts;
        enum AVSampleFormat best = AV_SAMPLE_FMT_NONE;

        // Preference list: Float Planar > Float > S16 Planar > S16 > S32 Planar
        enum AVSampleFormat preferences[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,  AV_SAMPLE_FMT_S16P,
                                             AV_SAMPLE_FMT_S16,  AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32};

        for (auto pref : preferences) {
            p = codec->sample_fmts;
            while (*p != AV_SAMPLE_FMT_NONE) {
                if (*p == pref) {
                    best = pref;
                    break;
                }
                p++;
            }
            if (best != AV_SAMPLE_FMT_NONE)
                break;
        }

        if (best == AV_SAMPLE_FMT_NONE) {
            // Fallback to first
            best = codec->sample_fmts[0];
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
    if (codec->supported_samplerates) {
        int best_rate = 0;
        for (int i = 0; codec->supported_samplerates[i]; i++) {
            if (codec->supported_samplerates[i] == codecCtx->sample_rate) {
                best_rate = codecCtx->sample_rate;
                break;
            }
            if (!best_rate ||
                abs(codec->supported_samplerates[i] - codecCtx->sample_rate) < abs(best_rate - codecCtx->sample_rate)) {
                best_rate = codec->supported_samplerates[i];
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
        DLL_Log("[AudioEncoder] Channel mask 0x%x resolved to %d channels, overriding configured %d",
                outputChannelMask, chLayout.nb_channels, outputChannels);
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
                    codecName.c_str(), reqDesc, sameCount ? "relabeling" : "downmixing", newDesc,
                    remapped.nb_channels);
            av_channel_layout_uninit(&chLayout);
            av_channel_layout_copy(&chLayout, &remapped);
            if (!sameCount) {
                outputChannels = chLayout.nb_channels;
                outputChannelMask = (chLayout.order == AV_CHANNEL_ORDER_NATIVE)
                                        ? static_cast<uint32_t>(chLayout.u.mask)
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
    }

    int ret = avcodec_open2(codecCtx, codec, &codecOptions);
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

    // Save codec ID and name for recreation in Stop()
    savedCodecId = codec->id;
    savedCodecName = codecName;

    DLL_Log("[AudioEncoder] Codec opened. frame_size=%d", codecCtx->frame_size);

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
        avcodec_free_context(&codecCtx);
        return false;
    }

    initDone = true;
    savedConfig = config;       // Save for potential reinit
    lastPacketTimestampMs = 0;  // Initialize timestamp tracker
    pendingFrameDurations.clear();
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

void AudioEncoder::ApplyPacketDuration(AVPacket* pkt) {
    if (!pkt) {
        return;
    }

    int64_t expectedDuration = 0;
    if (!pendingFrameDurations.empty()) {
        expectedDuration = pendingFrameDurations.front();
        pendingFrameDurations.pop_front();
    }

    if (pkt->duration > 0) {
        return;
    }

    if (expectedDuration > 0) {
        pkt->duration = expectedDuration;
        return;
    }

    if (codecCtx && codecCtx->frame_size > 0) {
        pkt->duration = codecCtx->frame_size;
    }
}

AudioEncoder::EncodeResult AudioEncoder::EncodeSamples(const uint8_t* data, int sizeBytes, int channels,
                                                       int sampleRate, int bitsPerSample, int validBitsPerSample,
                                                       int blockAlign, bool isFloat, int64_t timestamp) {
    return EncodeSamples(data, sizeBytes, channels, sampleRate, bitsPerSample, validBitsPerSample, blockAlign, isFloat,
                         0, timestamp);
}

AudioEncoder::EncodeResult AudioEncoder::EncodeSamples(const uint8_t* data, int sizeBytes, int channels,
                                                       int sampleRate, int bitsPerSample, int validBitsPerSample,
                                                       int blockAlign, bool isFloat, uint32_t channelMask,
                                                       int64_t timestamp) {
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

    // DEFERRED RESET: SetRecordingStart was called from video thread - apply
    // reset now with logging
    if (needsReset.exchange(false, std::memory_order_acq_rel)) {
        int64_t deferredStartUs = pendingStartUs.exchange(0, std::memory_order_acq_rel);
        int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
        DLL_Log(
            "[AudioEnc] RECORDING START RESET: startUs=%lld, OLD "
            "samplesCount=%lld, resampledTotal=%lld, FIFO=%d",
            (long long)deferredStartUs, (long long)samplesCount, (long long)resampledSamplesTotal, fifoSize);

        recordingStartUs = deferredStartUs;
        // SetRecordingEndUs can race ahead of the first real sample when a
        // packetless co-mixed source delayed this encoder until stop.  Do not
        // erase that already-published boundary while applying the deferred
        // start reset.
        firstTimestamp = -1;
        samplesCount = 0;
        resampledSamplesTotal = 0;
        lastPacketTimestampMs = 0;
        pendingFrameDurations.clear();
        lastInputTimestamp = -1;
        if (audioFifo)
            av_audio_fifo_reset(audioFifo);
        if (resampler)
            resampler->Reset();

        DLL_Log("[AudioEnc] Reset complete - audio will restart from PTS=0");
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
        DLL_Log("[AudioEnc] Resampler initialized: %dHz %dch mask=0x%x %s%d -> %dHz %dch mask=0x%x fmt=%d",
                sampleRate, channels, channelMask, isFloat ? "float" : "int", bitsPerSample, codecCtx->sample_rate,
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
    const int MAX_FIFO_SAMPLES =
        std::max(codecCtx->sample_rate * 5, currentFifoSize + std::max(convertedSamples, 0));
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
        pendingFrameDurations.push_back(frame_size);
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

    // Only drain the audio FIFO to start fresh for next recording
    if (audioFifo) {
        av_audio_fifo_reset(audioFifo);
    }

    // Reset all state for next recording
    DLL_Log("[AudioEnc] Stop: Resetting state (samplesCount=%lld, streamIndex=%d)", (long long)samplesCount,
            streamIndex);

    samplesCount = 0;
    streamIndex = -1;  // Critical for next run
    firstTimestamp = -1;
    recordingStartUs = -1;
    recordingEndUs = 0;
    pendingFrameDurations.clear();
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
    // Clear any pending packets
    for (auto* pkt : pendingPackets) {
        av_packet_free(&pkt);
    }
    pendingPackets.clear();

    // Flush resampler buffers if they exist
    if (resampler) {
        // Recreating it is safest to clear internal buffers
        resampler.reset();
    }

    // Do NOT free codecCtx/frame here. Freeing an ALAC codec that is in EOF state
    // (after avcodec_send_frame(nullptr) in Flush()) can crash inside FFmpeg.
    // Init() (called via Reinit() before the next recording) already frees and
    // recreates these resources, so deferred cleanup is safe.
    // The destructor also handles cleanup if no next recording occurs.
    initDone = false;
}

void AudioEncoder::Flush() {
    if (!initDone || !codecCtx)
        return;

    DLL_Log("[AudioEncoder] Flushing encoder...");

    if (needsReset.exchange(false)) {
        const int64_t deferredStartUs = pendingStartUs.exchange(0, std::memory_order_acq_rel);
        const int64_t preservedEndUs = recordingEndUs;
        int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
        DLL_Log(
            "[AudioEnc] RECORDING START RESET during flush: startUs=%lld, OLD "
            "samplesCount=%lld, resampledTotal=%lld, FIFO=%d, endUs=%lld",
            (long long)deferredStartUs, (long long)samplesCount, (long long)resampledSamplesTotal, fifoSize,
            (long long)preservedEndUs);

        recordingStartUs = deferredStartUs;
        recordingEndUs = preservedEndUs;
        firstTimestamp = -1;
        samplesCount = 0;
        resampledSamplesTotal = 0;
        lastPacketTimestampMs = 0;
        pendingFrameDurations.clear();
        lastInputTimestamp = -1;
        if (audioFifo) {
            av_audio_fifo_reset(audioFifo);
        }
        if (resampler) {
            resampler->Reset();
        }
    }

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
        uint8_t* skipData = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (!skipData) {
            DLL_Log("[AudioEncoder] WARNING: failed to add packet end-skip side data: stream=%d endSkip=%lld samples",
                    streamIndex, endSkipSamples);
            return false;
        }

        AV_WL32(skipData, 0);
        AV_WL32(skipData + 4, (uint32_t)endSkipSamples);
        skipData[8] = 0;
        skipData[9] = 0;  // padding silence
        finalDiscardSideDataAttached = true;
        finalDiscardSideDataSamples = endSkipSamples;
        DLL_Log("[AudioEncoder] Added packet end-skip side data: stream=%d endSkip=%lld samples", streamIndex,
                endSkipSamples);
        return true;
    };

    auto packetWithinEndBoundary = [&](AVPacket* pkt) {
        if (!pkt || targetSamples == INT64_MAX || pkt->pts == AV_NOPTS_VALUE) {
            return true;
        }
        const ce::audio::PacketEndClamp clamp =
            ce::audio::ClampPacketDurationToTargetSamples(pkt->pts, pkt->duration, targetSamples);
        if (!clamp.keep) {
            DLL_Log("[AudioEncoder] Dropping packet beyond recording end: pts=%lld max=%lld",
                    (long long)pkt->pts, (long long)targetSamples);
            return false;
        }
        if (clamp.clamped) {
            const int64_t originalDuration = pkt->duration;
            DLL_Log("[AudioEncoder] Clamping final packet duration: pts=%lld dur=%lld -> %lld max=%lld",
                    (long long)pkt->pts, (long long)pkt->duration, (long long)clamp.durationSamples,
                    (long long)targetSamples);
            pkt->duration = clamp.durationSamples;
            if (!finalDiscardSideDataAttached) {
                attachEndSkipSideData(pkt, originalDuration - clamp.durationSamples);
            }
        }
        return true;
    };

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
            if (packetWithinEndBoundary(pkt)) {
                pkt->stream_index = streamIndex;
                if (onPacket)
                    onPacket(pkt);
            }
            av_packet_free(&pkt);
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
            DLL_Log(
                "[AudioEncoder] Silence queue skipped invalid format: silenceSamples=%lld nch=%d bps=%d planar=%d",
                silenceSamples, nchPre, bpsPre, (int)planarPre);
            return 0;
        }

        std::vector<uint8_t> zeroBuf(zeroBytes, 0);
        std::vector<uint8_t*> planePtrs(numPlanesPre);
        for (int plane = 0; plane < numPlanesPre; plane++) {
            // For planar: each channel gets its own region; for interleaved:
            // the single plane contains all channels packed together.
            planePtrs[plane] = zeroBuf.data() +
                               ce::audio::ComputeAudioPlaneOffsetBytes(plane, silenceSamples, bpsPre, planarPre);
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
                const int silenceChunk =
                    (int)std::min<int64_t>(remainingSilence, std::max<int>(frame_size, 1));
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
            samplesCount += samplesToSend;

            ret = avcodec_send_frame(codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) {
                drainPackets();
                ret = avcodec_send_frame(codecCtx, frame);
            }
            if (ret < 0) {
                break;
            }
            pendingFrameDurations.push_back(samplesToSend);
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
    if (!pendingFrameDurations.empty()) {
        DLL_Log("[AudioEncoder] Flush: pending duration slots before final drain=%zu", pendingFrameDurations.size());
    }

    // Send NULL frame to trigger final drain of codec's internal buffer.
    // After draining, avcodec_flush_buffers() resets the codec out of EOF state
    // so it can be safely freed by avcodec_free_context (called from Init() on
    // next recording start) without triggering double-free bugs in ALAC.
    avcodec_send_frame(codecCtx, nullptr);
    // Final drain
    std::vector<AVPacket*> flushedPackets;
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            break;
        }
        ApplyPacketDuration(pkt);
        if (!packetWithinEndBoundary(pkt)) {
            av_packet_free(&pkt);
            continue;
        }
        pkt->stream_index = streamIndex;  // Ensure correct stream index for flushed packets
        flushedPackets.push_back(pkt);
    }

    if (discardPaddingSamples > 0 && !finalDiscardSideDataAttached && !flushedPackets.empty()) {
        AVPacket* lastPkt = flushedPackets.back();
        attachEndSkipSideData(lastPkt, discardPaddingSamples);
    } else if (discardPaddingSamples > 0 && finalDiscardSideDataAttached) {
        DLL_Log(
            "[AudioEncoder] Final discard side data already attached while clamping: stream=%d endSkip=%lld/%lld samples",
            streamIndex, finalDiscardSideDataSamples, discardPaddingSamples);
    }

    for (AVPacket* pkt : flushedPackets) {
        if (onPacket) {
            onPacket(pkt);
        }
        av_packet_free(&pkt);
    }

    // Reset the codec out of EOF/draining state so avcodec_free_context (called
    // from Init() or the destructor) operates on a clean codec rather than one
    // that has been drained to EOF. This prevents potential double-free bugs in
    // some codec implementations (e.g. ALAC) when the context is freed after EOF.
    avcodec_flush_buffers(codecCtx);
    pendingFrameDurations.clear();

    DLL_Log("[AudioEncoder] Flush complete");
}

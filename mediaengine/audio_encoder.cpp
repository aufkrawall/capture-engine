#include "audio_encoder.h"
#include "mediaengine.h"  // For DLL_Log

extern "C" {
#include <libavutil/audio_fifo.h>
}

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

    std::string codecName = config.codec.empty() ? "aac" : config.codec;
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName.c_str());
    if (!codec) {
        DLL_Log("[AudioEncoder] Codec not found: %s", codecName.c_str());
        return false;
    }

    DLL_Log("[AudioEncoder] Using codec: %s (id=%d)", codecName.c_str(), codec->id);

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

    // ALAC requires specific sample format; for others (Opus, AAC), prefer Float > S16 > others
    if (codec->id == AV_CODEC_ID_ALAC) {
        codecCtx->sample_fmt = AV_SAMPLE_FMT_S32P;
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

    if (config.bitrate > 0)
        codecCtx->bit_rate = config.bitrate * 1000;

    // Parse sample rate - "default" means use 48000, otherwise parse as int
    int sampleRate = 48000;  // Default fallback
    if (!config.sampleRate.empty() && config.sampleRate != "default") {
        sampleRate = std::stoi(config.sampleRate);
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

    // Channel Layout - Stereo (use av_channel_layout_copy for proper FFmpeg
    // handling)
    AVChannelLayout chLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&codecCtx->ch_layout, &chLayout);

    DLL_Log("[AudioEncoder] Opening codec: %s sample_rate=%d sample_fmt=%d", codecName.c_str(), codecCtx->sample_rate,
            codecCtx->sample_fmt);

    // ALAC: must set bits_per_raw_sample BEFORE avcodec_open2.
    // The encoder reads this during init to write the magic cookie (extradata).
    // Setting it after open causes a mismatch: extradata says 16-bit, stream
    // header says 32-bit, which makes decoders produce silence or white noise.
    if (codec->id == AV_CODEC_ID_ALAC) {
        codecCtx->bits_per_raw_sample = 24;
    }

    // Allow experimental codecs (like Opus in some builds)
    codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    int ret = avcodec_open2(codecCtx, codec, nullptr);
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

void AudioEncoder::EncodeSamples(const uint8_t* data, int sizeBytes, int channels, int sampleRate, int bitsPerSample,
                                 int validBitsPerSample, int blockAlign, bool isFloat, int64_t timestamp) {
    // If encoder was invalidated (reopen failed in Stop), try to reinit
    if (!initDone && !savedConfig.codec.empty()) {
        DLL_Log("[AudioEnc] Attempting reinit after previous failure");
        if (!Init(savedConfig, onPacket)) {
            DLL_Log("[AudioEnc] Reinit failed, cannot encode");
            return;
        }
    }

    if (!initDone || !codecCtx || !data || sizeBytes <= 0)
        return;

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
        recordingEndUs = 0;
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

        int fifoFrameSize = codecCtx->frame_size;
        if (fifoFrameSize == 0)
            fifoFrameSize = 4096;
        fifoPtsOffset_ = fifoFrameSize;

        DLL_Log(
            "[AudioEnc] Reset complete - audio will start from PTS=%lld "
            "(fifo=%lld, anchor=%lld) with fade-in",
            (long long)(fifoPtsOffset_ + anchorPtsOffset_), (long long)fifoPtsOffset_, (long long)anchorPtsOffset_);
    }

    // CRITICAL: Discard audio samples that arrive before first video frame
    // This ensures 0ms A/V sync - audio should not be encoded until video starts
    if (recordingStartUs < 0) {
        // Recording start not set yet (waiting for first video frame)
        // Silently discard this audio data
        return;
    }

    // CRITICAL: Discard audio samples that arrive after last video frame
    // This ensures audio track ends exactly when video ends
    // Check against recordingEndUs (using microsecond precision)
    int64_t timestampUs = timestamp * 1000;
    if (recordingEndUs > 0 && timestampUs > recordingEndUs) {
        // Recording has ended, discard this audio data
        return;
    }

    // Build input format descriptor
    AudioResampler::InputFormat inputFmt;
    inputFmt.channels = channels;
    inputFmt.sampleRate = sampleRate;
    inputFmt.bitsPerSample = bitsPerSample;
    inputFmt.validBitsPerSample = validBitsPerSample;
    inputFmt.isFloat = isFloat;
    inputFmt.blockAlign = blockAlign;

    // Initialize or reinitialize resampler if format changed
    bool needsInit = !resampler || !resampler->IsReady();
    if (!needsInit) {
        // Check if format changed
        needsInit =
            (currentInputFormat.channels != inputFmt.channels || currentInputFormat.sampleRate != inputFmt.sampleRate ||
             currentInputFormat.bitsPerSample != inputFmt.bitsPerSample ||
             currentInputFormat.validBitsPerSample != inputFmt.validBitsPerSample ||
             currentInputFormat.isFloat != inputFmt.isFloat);
    }

    if (needsInit) {
        if (!resampler) {
            resampler = std::make_unique<AudioResampler>();
        }

        AudioResampler::OutputFormat outputFmt;
        outputFmt.channels = codecCtx->ch_layout.nb_channels;
        outputFmt.sampleRate = codecCtx->sample_rate;
        outputFmt.sampleFmt = codecCtx->sample_fmt;

        if (!resampler->Init(inputFmt, outputFmt)) {
            DLL_Log("[AudioEnc] Failed to init resampler");
            return;
        }

        currentInputFormat = inputFmt;
        DLL_Log("[AudioEnc] Resampler initialized: %dHz %dch %s%d -> %dHz %dch fmt=%d", sampleRate, channels,
                isFloat ? "float" : "int", bitsPerSample, codecCtx->sample_rate, outputFmt.channels,
                (int)outputFmt.sampleFmt);
    }

    // Resample using AudioResampler
    uint8_t** resampledData = nullptr;
    int convertedSamples = 0;

    if (!resampler->Process(data, sizeBytes, &resampledData, &convertedSamples)) {
        DLL_Log("[AudioEnc] Resample failed");
        return;
    }

    if (convertedSamples <= 0) {
        AudioResampler::FreeOutputBuffer(resampledData);
        return;
    }

    // Track cumulative resampler output for drift calculation
    resampledSamplesTotal += convertedSamples;

    // NOTE: Fade-in is applied upstream in PullAndEncodeAudio (mediaengine.cpp)
    // before samples reach this encoder. Applying a second fade here would
    // produce a squared fade curve and an overly long startup artifact.

    // SAFETY: Check FIFO size BEFORE writing to prevent overflow
    // Dropping NEWEST samples maintains timeline continuity (avoids temporal jumps)
    // whereas draining OLDEST samples causes A/V desync and clicks
    if (codecCtx->sample_rate <= 0) {
        DLL_Log("[AudioEnc] ERROR: sample_rate=%d, codec context invalid, skipping encode", codecCtx->sample_rate);
        AudioResampler::FreeOutputBuffer(resampledData);
        return;
    }
    const int MAX_FIFO_SAMPLES = codecCtx->sample_rate * 5;
    const int CROSSFADE_SAMPLES = 64;
    int currentFifoSize = av_audio_fifo_size(audioFifo);
    int samplesToWrite = convertedSamples;
    bool applyingFadeOut = false;

    if (currentFifoSize + convertedSamples > MAX_FIFO_SAMPLES) {
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
        }

        wasDroppingSamples = true;
        totalDroppedSamples += convertedSamples - samplesToWrite;

        if (samplesToWrite == 0) {
            AudioResampler::FreeOutputBuffer(resampledData);
            return;
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
    }

    AudioResampler::FreeOutputBuffer(resampledData);

    // NOTE: Gap detection REMOVED.
    // The MediaEngine pull model handles all timing by pulling exact sample
    // counts based on video timeline. This encoder just encodes what it receives.
    // No HARD RESYNC, no warping - just simple PTS = samplesCount.

    if (firstTimestamp < 0 && recordingStartUs > 0) {
        firstTimestamp = timestamp;

        // Simple reset: counters should already be 0 from deferred reset.
        // Just log and FIFO clear as safety.
        if (samplesCount != 0 || resampledSamplesTotal != 0) {
            DLL_Log(
                "[AudioEnc] First packet safety reset: samplesCount=%lld -> 0, "
                "resampledTotal=%lld -> 0",
                (long long)samplesCount, (long long)resampledSamplesTotal);
            samplesCount = 0;
            resampledSamplesTotal = 0;
            if (resampler)
                resampler->ResetClockTracking();
            if (audioFifo) {
                av_audio_fifo_reset(audioFifo);
            }
        }

        DLL_Log("[AudioEnc] First audio packet processing (PTS=0)");
    }

    // Track latest packet timestamp for PTS calculation
    // This ensures audio PTS uses the same clock source as video (QPC)
    lastPacketTimestampMs = timestamp;

    // Encode while we have enough samples
    int frame_size = codecCtx->frame_size;
    if (frame_size == 0) {
        frame_size = 4096;  // Default for variable codecs
    }

    // Periodic FIFO status (reduced frequency to avoid log spam)
    if (fifoLogCounter++ % 5000 == 0) {
        int currentSize = av_audio_fifo_size(audioFifo);

        // Warn if over 50% capacity (approx 1-2 seconds depending on sample rate)
        if (currentSize > fifoCapacity / 2) {
            DLL_Log("[AudioEnc] WARN: Audio FIFO high: %d/%d samples", currentSize, fifoCapacity);
        } else {
            DLL_Log("[AudioEnc] FIFO size=%d, frame_size=%d", currentSize, frame_size);
        }
    }

    while (av_audio_fifo_size(audioFifo) >= frame_size) {
        // Make frame writable
        ret = av_frame_make_writable(frame);
        if (ret < 0) {
            DLL_Log("[AudioEnc] Failed to make frame writable: %d", ret);
            return;
        }

        frame->nb_samples = frame_size;

        // Read from FIFO into frame
        ret = av_audio_fifo_read(audioFifo, (void**)frame->data, frame_size);
        if (ret < frame_size) {
            DLL_Log("[AudioEnc] Failed to read from FIFO: got %d, expected %d", ret, frame_size);
            return;
        }

        // Set PTS using simple sample counting from 0 (CFR audio)
        // This matches how video PTS is calculated - simple frame counting
        // Video: pts = frame_number (0, 1, 2, 3...)
        // Audio: pts = samples_encoded (0, 4096, 8192...)
        // Both are constant-rate clocks that stay perfectly synchronized
        frame->pts = samplesCount + fifoPtsOffset_ + anchorPtsOffset_;

        // Debug: Log first 10 frames for each encoder to track PTS
        if (frameLogCounter++ < 10) {
            DLL_Log(
                "[AudioEnc] FRAME PTS DEBUG: pts=%lld (%.3f sec) "
                "samplesCount=%lld streamIdx=%d ptsOffset=%lld",
                (long long)frame->pts, (double)frame->pts / codecCtx->sample_rate, (long long)samplesCount, streamIndex,
                (long long)(fifoPtsOffset_ + anchorPtsOffset_));
        }

        // Encode frame
        ret = avcodec_send_frame(codecCtx, frame);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[AudioEnc] avcodec_send_frame failed: %s (code=%d)", errbuf, ret);
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
            if (noPacketCount % 100 == 1) {
                DLL_Log("[AudioEnc] No packets received after send_frame (count=%d)", noPacketCount);
            }
        }
    }
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
    fifoPtsOffset_ = 0;
    anchorPtsOffset_ = 0;

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

    // Calculate maximum samples to encode based on video duration
    // This ensures audio ends exactly when video ends (Microsecond precision)
    int64_t maxSamples = INT64_MAX;  // No limit by default
    if (recordingEndUs > 0 && recordingStartUs >= 0 && recordingEndUs >= recordingStartUs) {
        int64_t durationUs = recordingEndUs - recordingStartUs;

        // av_rescale_rnd for precise sample count from microseconds
        // Use AV_ROUND_UP to ensure audio is never shorter than video
        maxSamples = av_rescale_rnd(durationUs, codecCtx->sample_rate, 1000000, AV_ROUND_UP);

        DLL_Log(
            "[AudioEncoder] Video duration: %lld us, max audio samples: %lld, "
            "current: %lld",
            durationUs, maxSamples, samplesCount);
    }

    const int fixedFrameSize = codecCtx->frame_size;
    const bool supportsVariableFrame =
        codecCtx->codec && (codecCtx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
    const bool supportsSmallLastFrame =
        codecCtx->codec && (codecCtx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);
    const bool canSendShortFrame = supportsVariableFrame || supportsSmallLastFrame;

    // Only round up to frame boundary for codecs that require full-size frames.
    // Codecs with AV_CODEC_CAP_SMALL_LAST_FRAME (ALAC, AAC, FLAC, Opus) accept a
    // short final frame, so we target the exact sample count instead.
    if (!canSendShortFrame && fixedFrameSize > 0 && maxSamples != INT64_MAX) {
        int64_t rem = maxSamples % fixedFrameSize;
        if (rem != 0) {
            maxSamples += (fixedFrameSize - rem);
        }
    }

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
            if (onPacket)
                onPacket(pkt);
            av_packet_free(&pkt);
        }
    };

    // PRE-FILL FIFO WITH SILENCE so the FIFO-drain loop covers any shortfall.
    // This replaces a separate avcodec_send_frame(silenceFrame) path that was
    // unreliable for some codecs (ALAC returned EINVAL). The FIFO-drain path is
    // proven to work, so feeding silence through it is safer.
    DLL_Log(
        "[AudioEncoder] Post-duration: fixedFrameSize=%d canSendShortFrame=%d "
        "audioFifo=%p codecCtx=%p",
        fixedFrameSize, (int)canSendShortFrame, (void*)audioFifo, (void*)codecCtx);

    if (maxSamples != INT64_MAX && audioFifo) {
        int preFifoSize = av_audio_fifo_size(audioFifo);
        int64_t totalCovered = samplesCount + preFifoSize;
        int64_t silenceNeeded = maxSamples - totalCovered;
        if (silenceNeeded > 0) {
            // Cap to avoid runaway allocation (e.g. video duration >> audio)
            silenceNeeded = std::min(silenceNeeded, (int64_t)(codecCtx->sample_rate * 3));
            bool planarPre = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
            int nchPre = codecCtx->ch_layout.nb_channels;
            int bpsPre = av_get_bytes_per_sample(codecCtx->sample_fmt);
            // Allocate a full zeroed buffer with separate regions per channel so
            // av_audio_fifo_write gets independent (non-aliased) plane pointers.
            int numPlanesPre = planarPre ? nchPre : 1;
            std::vector<uint8_t> zeroBuf((size_t)silenceNeeded * bpsPre * numPlanesPre, 0);
            std::vector<uint8_t*> planePtrs(nchPre);
            for (int ch = 0; ch < nchPre; ch++) {
                // For planar: each channel gets its own region; for interleaved: all
                // channels share the single interleaved buffer.
                int planeIdx = planarPre ? ch : 0;
                planePtrs[ch] = zeroBuf.data() + (size_t)planeIdx * silenceNeeded * bpsPre;
            }
            DLL_Log(
                "[AudioEncoder] Silence pre-fill: silenceNeeded=%lld "
                "preFifoSize=%d nchPre=%d bpsPre=%d planar=%d",
                silenceNeeded, preFifoSize, nchPre, bpsPre, (int)planarPre);
            int written = av_audio_fifo_write(audioFifo, (void**)planePtrs.data(), (int)silenceNeeded);
            DLL_Log(
                "[AudioEncoder] Silence pre-fill: wrote %d / %lld samples "
                "(samplesCount=%lld, maxSamples=%lld)",
                written, silenceNeeded, samplesCount, maxSamples);
        }
    }

    // Encode any remaining samples in FIFO (up to maxSamples limit)
    int fifoSize = audioFifo ? av_audio_fifo_size(audioFifo) : 0;
    if (fifoSize > 0 && frame) {
        int frame_size = codecCtx->frame_size ? codecCtx->frame_size : 4096;
        int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);
        int channels = codecCtx->ch_layout.nb_channels;
        bool planar = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
        int numPlanes = planar ? channels : 1;

        while (fifoSize > 0) {
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
            // Use an exact (possibly short) final frame when the codec supports it.
            // Codecs with AV_CODEC_CAP_SMALL_LAST_FRAME (ALAC, AAC, FLAC, Opus)
            // accept fewer samples than frame_size for the final frame, which gives
            // sample-accurate end alignment without any trailing_padding tricks.
            // For codecs that require fixed-size frames, zero-pad to frame_size.
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

            if (samplesToRead == fifoSize) {
                const int FADE_SAMPLES = codecCtx->sample_rate / 20;
                int fadeStart = std::max(0, samplesToRead - FADE_SAMPLES);

                for (int p = 0; p < numPlanes && frame->data[p]; p++) {
                    if (codecCtx->sample_fmt == AV_SAMPLE_FMT_FLT || codecCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
                        float* fData = (float*)frame->data[p];
                        for (int i = fadeStart; i < samplesToRead; i++) {
                            float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
                            float gain = fadePos < 1.0f ? fadePos : 1.0f;
                            if (numPlanes == 1) {
                                for (int c = 0; c < channels; c++)
                                    fData[i * channels + c] *= gain;
                            } else {
                                fData[i] *= gain;
                            }
                        }
                    } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S16 ||
                               codecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                        int16_t* sData = (int16_t*)frame->data[p];
                        for (int i = fadeStart; i < samplesToRead; i++) {
                            float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
                            float gain = fadePos < 1.0f ? fadePos : 1.0f;
                            if (numPlanes == 1) {
                                for (int c = 0; c < channels; c++)
                                    sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
                            } else {
                                sData[i] = (int16_t)(sData[i] * gain);
                            }
                        }
                    } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 ||
                               codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                        int32_t* sData = (int32_t*)frame->data[p];
                        for (int i = fadeStart; i < samplesToRead; i++) {
                            float fadePos = (float)(samplesToRead - 1 - i) / FADE_SAMPLES;
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

            frame->pts = samplesCount + fifoPtsOffset_ + anchorPtsOffset_;
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

            fifoSize = av_audio_fifo_size(audioFifo);
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
        int64_t durationUs = recordingEndUs - recordingStartUs;
        int64_t targetSamples = av_rescale_rnd(durationUs, codecCtx->sample_rate, 1000000, AV_ROUND_DOWN);
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
        "[AudioEncoder] Flush: samplesCount=%lld maxSamples=%lld "
        "discardPadding=%lld",
        samplesCount, maxSamples, discardPaddingSamples);
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
        pkt->stream_index = streamIndex;  // Ensure correct stream index for flushed packets
        flushedPackets.push_back(pkt);
    }

    if (discardPaddingSamples > 0 && !flushedPackets.empty()) {
        AVPacket* lastPkt = flushedPackets.back();
        // AV_PKT_DATA_SKIP_SAMPLES expects 8 bytes: 32-bit start skip, 32-bit end skip
        uint32_t* skipData = (uint32_t*)av_packet_new_side_data(lastPkt, AV_PKT_DATA_SKIP_SAMPLES, 8);
        if (skipData) {
            skipData[0] = 0;                                // No skip from start
            skipData[1] = (uint32_t)discardPaddingSamples;  // Skip from end
        }
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

#include "audio_encoder_internal.h"

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
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    float fadePos = (float)(samplesToWrite - 1 - i) / CROSSFADE_SAMPLES;
                    float gain = fadePos < 1.0f ? fadePos : 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                            sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);
                    } else {
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        sData[i] = (int16_t)(sData[i] * gain);
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                int32_t* sData = (int32_t*)resampledData[p];
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                for (int i = fadeStart; i < samplesToWrite; i++) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    float fadePos = (float)(samplesToWrite - 1 - i) / CROSSFADE_SAMPLES;
                    float gain = fadePos < 1.0f ? fadePos : 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                            sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);
                    } else {
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                int16_t* sData = (int16_t*)resampledData[p];
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                for (int i = 0; i < fadeEnd; i++) {
                    float gain = (float)(i + 1) / CROSSFADE_SAMPLES;  // NOLINT(bugprone-narrowing-conversions)
                    if (gain > 1.0f)
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        gain = 1.0f;
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    if (numPlanes == 1) {
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int16_t)(sData[i * channels + c] * gain);  // NOLINT(bugprone-narrowing-conversions)
                    } else {
                        sData[i] = (int16_t)(sData[i] * gain);  // NOLINT(bugprone-narrowing-conversions)
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    }
                }
            } else if (codecCtx->sample_fmt == AV_SAMPLE_FMT_S32 || codecCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
                int32_t* sData = (int32_t*)resampledData[p];
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                for (int i = 0; i < fadeEnd; i++) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    float gain = (float)(i + 1) / CROSSFADE_SAMPLES;
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    if (gain > 1.0f)
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        gain = 1.0f;
                    if (numPlanes == 1) {
                        for (int c = 0; c < channels; c++)
                            sData[i * channels + c] = (int32_t)(sData[i * channels + c] * gain);  // NOLINT(bugprone-narrowing-conversions)
                    } else {
                        sData[i] = (int32_t)(sData[i] * gain);  // NOLINT(bugprone-narrowing-conversions)
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

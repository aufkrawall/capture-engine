#include "audio_encoder_internal.h"

#include "audio_fault_accounting.h"

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
        // Recording has ended, discard this audio data (trimmed, never a hole)
        result.trimmedSamples = ce::audio::ComputeOutputRateChunkSamples(blockAlign > 0 ? sizeBytes / blockAlign : 0,
                                                                         sampleRate, codecCtx->sample_rate);
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
            // The caller already consumed this chunk (erased from every source before
            // the call): place its range as a silence hole exactly like the resample
            // failure below so every later sample keeps its exact position. The hole
            // count is booked by the caller (acceptedSamples stays 0 here).
            const int64_t inputSamples = blockAlign > 0 ? sizeBytes / blockAlign : 0;
            PlaceRefusedChunkHole(
                ce::audio::ComputeOutputRateChunkSamples(inputSamples, sampleRate, codecCtx->sample_rate), result);
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
        // The caller already consumed this chunk (erased from every source before
        // the call), so its timeline range must still be placed: record it as an
        // explicit silence hole instead of letting every later sample shift early.
        // The size is the input chunk mapped to the codec rate (the swr state is
        // unknown after the failure).
        const int64_t inputSamples = blockAlign > 0 ? sizeBytes / blockAlign : 0;
        PlaceRefusedChunkHole(
            ce::audio::ComputeOutputRateChunkSamples(inputSamples, sampleRate, codecCtx->sample_rate), result);
        return result;
    }

    if (convertedSamples <= 0) {
        AudioResampler::FreeOutputBuffer(resampledData);
        return result;
    }

    // NOTE: Fade-in is applied upstream in PullAndEncodeAudio (mediaengine.cpp)
    // before samples reach this encoder. Applying a second fade here would
    // produce a squared fade curve and an overly long startup artifact.

    // SAFETY: reject a dead codec context before indexing its format fields
    if (codecCtx->sample_rate <= 0) {
        DLL_Log("[AudioEnc] ERROR: sample_rate=%d, codec context invalid, skipping encode", codecCtx->sample_rate);
        AudioResampler::FreeOutputBuffer(resampledData);
        result.failed = true;
        return result;
    }
    // Accept the complete batch. The FIFO is grown to fit before the write below,
    // so a batch is taken in full (no five-second truncation); only the recording
    // -end clamp may shorten a write.
    int currentFifoSize = av_audio_fifo_size(audioFifo);
    int samplesToWrite = convertedSamples;

    const int64_t allowedSamples = SamplesAllowedBeforeRecordingEnd();
    if (allowedSamples >= 0) {
        const int64_t maxSamples =
            ce::audio::ComputeDurationUsToSamples(recordingEndUs - recordingStartUs, codecCtx->sample_rate);
        if (allowedSamples <= 0) {
            if (endDropLogCount++ < 5) {
                DLL_Log(
                    "[AudioEnc] End boundary reached: dropping %d samples before FIFO write "
                    "(encoded=%lld fifo=%d max=%lld)",
                    convertedSamples, (long long)samplesCount, currentFifoSize, (long long)maxSamples);
            }
            AudioResampler::FreeOutputBuffer(resampledData);
            result.trimmedSamples = convertedSamples;
            return result;
        }
        if (samplesToWrite > allowedSamples) {
            DLL_Log(
                "[AudioEnc] Clamping audio write at recording end: write=%d -> %lld "
                "(encoded=%lld fifo=%d max=%lld)",
                samplesToWrite, (long long)allowedSamples, (long long)samplesCount, currentFifoSize,
                (long long)maxSamples);
            samplesToWrite = static_cast<int>(std::min<int64_t>(allowedSamples, INT_MAX));
            result.trimmedSamples = convertedSamples - samplesToWrite;
        }
    }

    // av_audio_fifo_write grows the FIFO automatically and is all-or-nothing: it
    // writes exactly samplesToWrite samples or fails without truncating the batch.
    // (The former five-second FIFO overflow guard was dead code - its limit always
    // evaluated to >= the needed size - so it guards nothing and is not replaced.)
    // Size the FIFO up front whenever the space check fails so an allocation
    // failure refuses the whole batch cleanly (failed result; the refused tail
    // becomes an explicit hole at the caller) instead of erroring mid-write over
    // the caller's already-consumed chunk.
    if (av_audio_fifo_space(audioFifo) < samplesToWrite) {
        const int reallocRet = av_audio_fifo_realloc(audioFifo, currentFifoSize + samplesToWrite);
        if (reallocRet < 0) {
            char errbuf[256];
            av_strerror(reallocRet, errbuf, sizeof(errbuf));
            DLL_Log("[AudioEnc] FIFO realloc to %d samples failed: %s; refusing batch", currentFifoSize + samplesToWrite,
                    errbuf);
            AudioResampler::FreeOutputBuffer(resampledData);
            result.failed = true;
            AppendSilenceHole(ce::audio::ComputeConsumedChunkHoleSamples(samplesToWrite, 0, true));
            return result;
        }
    }
    int ret = av_audio_fifo_write(audioFifo, (void**)resampledData, samplesToWrite);
    if (ret < samplesToWrite) {
        // All-or-nothing write: this is a hard failure (e.g. allocation) and nothing
        // or only a prefix entered the FIFO. acceptedSamples reports what really
        // entered so the caller can book the refused remainder as an explicit hole.
        // The refused tail sits at the FIFO tail, so its hole is appended as silence:
        // pending samples must keep their exact positions (a PTS jump here would
        // re-place all of them).
        DLL_Log("[AudioEnc] Failed to write to audio FIFO: wrote %d of %d", ret, samplesToWrite);
        result.failed = true;
        AppendSilenceHole(ce::audio::ComputeConsumedChunkHoleSamples(samplesToWrite, std::max(ret, 0), true));
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
        // Grow-to-fit means the allocation can exceed its initial size; measure the
        // real capacity instead of quoting the initial allocation size.
        const int currentCapacity = currentSize + av_audio_fifo_space(audioFifo);

        // Warn if over 50% capacity (approx 1-2 seconds depending on sample rate)
        if (currentSize > currentCapacity / 2) {
            DLL_Log("[AudioEnc] WARN: Audio FIFO high: %d/%d samples", currentSize, currentCapacity);
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
            // Consume whatever left the FIFO so the PTS cursor moves past it and no
            // later frame is re-placed over this range.
            samplesCount += std::max(ret, 0);
            AccountContentHole(std::max(ret, 0));
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

        // The frame's samples already left the FIFO, so they are consumed whether or
        // not the codec accepts the frame. AVERROR(EAGAIN) only means the codec's
        // packet buffer is full: drain packets and resend the SAME frame (bounded)
        // rather than dropping it and shifting every later frame one frame early.
        bool sent = false;
        constexpr int kMaxSendAttempts = 8;
        for (int attempt = 0; attempt < kMaxSendAttempts; ++attempt) {
            ret = avcodec_send_frame(codecCtx, frame);
            if (ret >= 0) {
                sent = true;
                break;
            }
            if (ret != AVERROR(EAGAIN)) {
                break;  // terminal failure; this frame becomes a recorded hole
            }
            ReceivePackets();  // free codec packet-buffer space, then resend
        }
        if (!sent) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[AudioEnc] avcodec_send_frame failed: %s (code=%d); accounting %d samples as a recorded hole",
                    errbuf, ret, frame_size);
            result.failed = true;
            AccountContentHole(frame_size);
        }
        // Advance regardless of send success. These samples were consumed from the
        // FIFO front - exactly [samplesCount, samplesCount + frame_size) - so the
        // cursor jump places their hole at its exact timeline position and every
        // later frame keeps its absolute PTS (a failed frame is a recorded hole,
        // never a re-placed frame). Do NOT reorder this below any early exit.
        samplesCount += frame_size;

        // Receive packets (also after a failed send so frames accepted earlier in
        // this call still reach the mux promptly instead of waiting for the next).
        const int pktCount = ReceivePackets();

        // Log if we didn't get any packets after sending a frame
        if (sent && pktCount == 0) {
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

int AudioEncoder::ReceivePackets() {
    int pktCount = 0;
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        const int ret = avcodec_receive_packet(codecCtx, pkt);
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
    return pktCount;
}

int64_t AudioEncoder::SamplesAllowedBeforeRecordingEnd() const {
    if (!codecCtx || codecCtx->sample_rate <= 0 || recordingEndUs <= 0 || recordingStartUs < 0 ||
        recordingEndUs < recordingStartUs) {
        return -1;
    }
    const int64_t maxSamples =
        ce::audio::ComputeDurationUsToSamples(recordingEndUs - recordingStartUs, codecCtx->sample_rate);
    return ce::audio::ComputeAudioSamplesAllowedBeforeEnd(maxSamples, samplesCount,
                                                          audioFifo ? av_audio_fifo_size(audioFifo) : 0);
}

void AudioEncoder::PlaceRefusedChunkHole(int64_t chunkSamplesAtCodecRate, EncodeResult& result) {
    const int64_t chunkSamples = std::max<int64_t>(0, chunkSamplesAtCodecRate);
    const int64_t allowed = SamplesAllowedBeforeRecordingEnd();
    const int64_t placed = allowed >= 0 ? std::min(chunkSamples, allowed) : chunkSamples;
    result.trimmedSamples = chunkSamples - placed;
    if (result.trimmedSamples > 0) {
        DLL_Log("[AudioEnc] Refused chunk crosses the recording end: placing %lld hole samples, trimming %lld",
                (long long)placed, (long long)result.trimmedSamples);
    }
    AppendSilenceHole(ce::audio::ComputeConsumedChunkHoleSamples(placed, 0, true));
}

void AudioEncoder::AppendSilenceHole(int64_t holeSamples) {
    // Reject a dead codec context before indexing its format fields (same rule as
    // the encode path above).
    if (holeSamples <= 0 || holeSamples > INT_MAX || !audioFifo || !codecCtx || codecCtx->sample_rate <= 0) {
        return;
    }
    // The hole sits at the FIFO tail (a consumed intake range that could not enter
    // the FIFO), so it is placed as silence in the stream: every pending sample
    // keeps its exact timeline position, which a PTS jump would destroy. The hole
    // itself is counted by the caller (ComputeConsumedChunkHoleSamples); this only
    // places it.
    const bool planar = av_sample_fmt_is_planar(codecCtx->sample_fmt) != 0;
    const int channels = codecCtx->ch_layout.nb_channels;
    const int bytesPerSample = av_get_bytes_per_sample(codecCtx->sample_fmt);
    const size_t zeroBytes = ce::audio::ComputeAudioSampleBufferBytes(holeSamples, bytesPerSample, channels);
    int written = 0;
    if (zeroBytes > 0) {
        std::vector<uint8_t> zeroBuf(zeroBytes, 0);
        std::vector<uint8_t*> planePtrs(planar ? channels : 1);
        for (int plane = 0; plane < static_cast<int>(planePtrs.size()); ++plane) {
            planePtrs[plane] =
                zeroBuf.data() + ce::audio::ComputeAudioPlaneOffsetBytes(plane, holeSamples, bytesPerSample, planar);
        }
        written = av_audio_fifo_write(audioFifo, (void**)planePtrs.data(), static_cast<int>(holeSamples));
    }
    if (written >= holeSamples) {
        return;
    }
    // The silence write itself failed (e.g. the same allocation failure that refused
    // the batch). Only an empty FIFO can absorb the residual as a PTS jump without
    // re-placing pending samples; otherwise the residual is logged loudly because
    // later samples will shift early by it.
    const int64_t residual = holeSamples - std::max(written, 0);
    if (av_audio_fifo_size(audioFifo) <= 0) {
        samplesCount += residual;
    } else {
        if (residualHoleLogCount++ < 10) {
            DLL_Log(
                "[AudioEnc] ERROR: intake hole of %lld samples only silence-filled %d with %d samples still pending; "
                "the %lld-sample residual cannot be placed exactly and later samples shift early",
                (long long)holeSamples, written, av_audio_fifo_size(audioFifo), (long long)residual);
        }
    }
}

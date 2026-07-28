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

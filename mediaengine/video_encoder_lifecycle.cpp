#include "video_encoder_internal.h"

void VideoEncoder::CleanupResources() {
    // Check if we should preserve encoder-owned textures (DXVK zero-copy path).
    // The Vulkan layer imported our KMT handles — destroying them invalidates the pipeline.
    bool preserveEncoderTextures =
        pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) && sharedCaptureTexturesCreated;

    // Free any queued packets (defensive)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
            packetQueue.pop();
            av_packet_free(&pkt);
        }
    }

    currentQueueBytes = 0;

    if (stream)
        stream = nullptr;
    if (fmtCtx) {
        avformat_free_context(fmtCtx);
        fmtCtx = nullptr;
    }
    if (codecCtx) {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }

    if (hwFramesCtx)
        av_buffer_unref(&hwFramesCtx);
    if (hwDeviceCtx)
        av_buffer_unref(&hwDeviceCtx);

    if (preserveEncoderTextures) {

        // Keep D3D11 device/context alive — textures are bound to it.
        // Only free FFmpeg HW contexts; they'll be recreated in EnsureDevice().
        if (d3d11DeviceCtx)
            av_buffer_unref(&d3d11DeviceCtx);
        if (d3d11FramesCtx)
            av_buffer_unref(&d3d11FramesCtx);
        // Reset initDone so EnsureDevice() rebuilds FFmpeg contexts but reuses the device
        initDone = false;
    }

    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (cachedSharedTextures[i]) {
            cachedSharedTextures[i]->Release();
            cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
    }

    if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
    }
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (!preserveEncoderTextures) {
        for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
            if (sharedCaptureTextures[i]) {
                sharedCaptureTextures[i]->Release();
                sharedCaptureTextures[i] = nullptr;
            }
            if (sharedCaptureHandles[i]) {
                CloseHandle(sharedCaptureHandles[i]);
                sharedCaptureHandles[i] = nullptr;
            }
        }
        if (sharedCaptureFence) {
            sharedCaptureFence->Release();
            sharedCaptureFence = nullptr;
        }
        if (sharedCaptureFenceHandle) {
            CloseHandle(sharedCaptureFenceHandle);
            sharedCaptureFenceHandle = nullptr;
        }
        sharedCaptureTexturesCreated = false;
        sharedCaptureTextureFormat = 0;
    }

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    if (!preserveEncoderTextures) {
        TrimD3D11Residency(d3d11Device, d3d11Context, "encoder");
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        if (d3d11DeviceCtx)
            av_buffer_unref(&d3d11DeviceCtx);
        if (d3d11FramesCtx)
            av_buffer_unref(&d3d11FramesCtx);
    }

    initDone = false;
    fileOpened = false;
    startPts = -1;
    inputFrameCount = 0;
    outputFrameCount = 0;
    skippedFrameCount = 0;
    duplicatedFrameCount = 0;
    cursorAwareRepeatRenderCount = 0;
    encodeFrameCounter = 0;
    lastLogFrameCount = 0;
    nextOutputTime_ms = -1;
    lastEncodeTimeUs = 0;
    lastFenceWaitUs = 0;
    lastFrameDeferred.store(false, std::memory_order_relaxed);
    encodedDurationUs.store(0, std::memory_order_relaxed);
    currentQueuePackets.store(0, std::memory_order_relaxed);
    peakQueueBytes.store(0, std::memory_order_relaxed);
    peakQueuePackets.store(0, std::memory_order_relaxed);
    muxBackpressureCount.store(0, std::memory_order_relaxed);
    muxBackpressureWaitUs.store(0, std::memory_order_relaxed);
    muxBackpressureMaxWaitUs.store(0, std::memory_order_relaxed);
    packetDurationClampCount.store(0, std::memory_order_relaxed);
    negativePtsCount.store(0, std::memory_order_relaxed);
    nonMonotonicPtsCount.store(0, std::memory_order_relaxed);
    lastQueuedVideoPts = AV_NOPTS_VALUE;
    lastAssignedVideoPts = -1;
    encoderSubmitQpcByPts.clear();
    encoderSendAccumUs = 0;
    encoderSendCalls = 0;
    encoderReceiveAccumUs = 0;
    encoderReceiveCalls = 0;
    encoderPacketLatencyAccumUs = 0;
    encoderPacketLatencySamples = 0;
    encoderPacketLatencyMaxUs = 0;
    encoderEagainDrainCount = 0;
    encoderTimingLastLogTick = 0;
    qsvSurfaceMappingLogged = false;
    qsvSurfaceMappingFailures = 0;
    asyncWriteErrorCount = 0;
    if (outputReservation.CleanupOwnedFile()) {
        DLL_Log("[VideoEncoder] Removed unpublished staging output during cleanup: %s", outputFilename.c_str());
    }
}

void VideoEncoder::ReleasePreservedEncoderTextures() {
    if (!sharedCaptureTexturesCreated)
        return;

    DLL_Log("[VideoEncoder] Releasing preserved encoder textures (game exited)");

    // Clear shared memory flags so a new game won't try to import stale handles
    if (pSharedMem) {
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
    }

    // Release encoder-owned KMT textures (mirrors !preserveEncoderTextures path in CleanupResources)
    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (sharedCaptureTextures[i]) {
            sharedCaptureTextures[i]->Release();
            sharedCaptureTextures[i] = nullptr;
        }
        if (sharedCaptureHandles[i]) {
            CloseHandle(sharedCaptureHandles[i]);
            sharedCaptureHandles[i] = nullptr;
        }
        sharedCaptureKmtHandles[i] = nullptr;
    }
    if (sharedCaptureFence) {
        sharedCaptureFence->Release();
        sharedCaptureFence = nullptr;
    }
    if (sharedCaptureFenceHandle) {
        CloseHandle(sharedCaptureFenceHandle);
        sharedCaptureFenceHandle = nullptr;
    }
    sharedCaptureTexturesCreated = false;
    sharedCaptureTextureFormat = 0;

    // Release D3D11 device and all resources that depend on it
    CleanupVideoProcessor();
    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }
    TrimD3D11Residency(d3d11Device, d3d11Context, "preserved-encoder");

    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (hwFramesCtx)
        av_buffer_unref(&hwFramesCtx);
    if (hwDeviceCtx)
        av_buffer_unref(&hwDeviceCtx);
    if (d3d11DeviceCtx)
        av_buffer_unref(&d3d11DeviceCtx);
    if (d3d11FramesCtx)
        av_buffer_unref(&d3d11FramesCtx);

    initDone = false;
    DLL_Log("[VideoEncoder] Preserved encoder textures released");
}

void VideoEncoder::Stop() {
    bool wasRecording = recordingRequested;
    recordingRequested = false;
    bool writerStillOwnsEncoderResources = false;

    if (wasRecording) {
        const uint32_t phase = pSharedMem ? pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed)
                                          : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
        const uint32_t totalFrames =
            pSharedMem ? pSharedMem->runtimeState.framesEncoded.load(std::memory_order_relaxed) : 0;
        const uint32_t liveFrames =
            pSharedMem ? pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_relaxed) : 0;
        const uint32_t drainFrames =
            pSharedMem ? pSharedMem->runtimeState.drainFramesEncoded.load(std::memory_order_relaxed) : 0;
        DLL_Log(
            "[VideoEncoder] Recording stats: input=%lld output=%lld runtime=%u skipped=%lld duplicated=%lld phase=%s "
            "live=%u drain=%u cursorAwareRepeatRenders=%lld backpressure=%u peakMux=%uKB peakPkts=%u",
            inputFrameCount, outputFrameCount, totalFrames, skippedFrameCount, duplicatedFrameCount,
            CapturePipelinePhaseToString(phase), liveFrames, drainFrames, cursorAwareRepeatRenderCount,
            muxBackpressureCount.load(std::memory_order_relaxed),
            peakQueueBytes.load(std::memory_order_relaxed) / 1024u, peakQueuePackets.load(std::memory_order_relaxed));

        // Final packet type distribution summary
        if (packetStats.totalPackets > 0) {
            int total = packetStats.totalPackets;
            int64_t avgKey = packetStats.keyframeCount > 0 ? packetStats.keyframeBytes / packetStats.keyframeCount : 0;
            int64_t avgRef = packetStats.refCount > 0 ? packetStats.refBytes / packetStats.refCount : 0;
            int64_t avgB = packetStats.bframeCount > 0 ? packetStats.bframeBytes / packetStats.bframeCount : 0;
            DLL_Log(
                "[VideoEncoder] FINAL PACKET STATS (%d pkts): "
                "Key=%d(avg %lldKB) Ref=%d(avg %lldKB) "
                "SEF=%d(%d%%) B-small=%d(avg %lldB)",
                total, packetStats.keyframeCount, avgKey / 1024, packetStats.refCount, avgRef / 1024,
                packetStats.sefCount, packetStats.sefCount * 100 / total, packetStats.bframeCount, avgB);
        }
    }

    if (wasRecording && writerRunning) {
        DLL_Log("[VideoEncoder] Stop: Signaling finalize (queueBytes=%zu)...", currentQueueBytes.load());
        isStopping = true;
        queueCV.notify_all();
        // Fall through to join — ensures file is fully closed before returning
    }

    // Always wait for writer thread to finish (writes trailer + closes file).
    // Use phase-aware bounded waits: trailer/probe finalization can be slower
    // than packet drain on busy disks, but the async writer must remain the
    // only owner of the muxer until it either completes or definitively times out.
    if (writerThread.joinable()) {
        const uint64_t waitStartMs = GetTickCount64();
        constexpr uint64_t kSlowFinalizeWarnMs = 5000;
        constexpr uint64_t kWriterFinalizeTimeoutMs = 30000;
        DLL_Log("[VideoEncoder] Stop: Waiting for writer thread to finish (phase=%s timeout=%llums)...",
                WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(kWriterFinalizeTimeoutMs));

        bool writerCompleted = false;
        while (true) {
            writerCompleted = WriterFinishedWithin(writerFinished, 250);
            if (writerCompleted) {
                break;
            }
            const uint64_t elapsedMs = GetTickCount64() - waitStartMs;
            const uint32_t phase = writerFinalizePhase.load(std::memory_order_relaxed);
            if (elapsedMs >= kSlowFinalizeWarnMs &&
                !writerFinalizeSlowWarningLogged.exchange(true, std::memory_order_acq_rel)) {
                DLL_Log(
                    "[VideoEncoder] Stop: WARNING writer_finalize_slow phase=%s elapsed=%llums queueBytes=%zu "
                    "queuePackets=%u; async writer still owns FFmpeg context",
                    WriterFinalizePhaseName(phase), static_cast<unsigned long long>(elapsedMs),
                    currentQueueBytes.load(std::memory_order_relaxed),
                    currentQueuePackets.load(std::memory_order_relaxed));
            }
            if (elapsedMs >= kWriterFinalizeTimeoutMs) {
                break;
            }
        }

        if (writerCompleted) {
            writerThread.join();
            if (writerFinalizeTimedOut.exchange(false, std::memory_order_acq_rel)) {
                DLL_Log("[VideoEncoder] Stop: Timed-out writer completed on a later stop; muxer ownership recovered.");
            }
            const uint64_t elapsedMs = GetTickCount64() - waitStartMs;
            DLL_Log("[VideoEncoder] Stop: Writer thread joined phase=%s elapsed=%llums.",
                    WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(elapsedMs));
        } else {
            writerFinalizeTimedOut.store(true, std::memory_order_release);
            const uint32_t timedOutPhase = writerFinalizePhase.load(std::memory_order_relaxed);
            // A live writer owns more than fmtCtx: the post-mux probe still
            // reads outputFilename and will perform CleanupResources on exit.
            // Never race it with synchronous cleanup merely because the muxer
            // file was already closed.
            writerStillOwnsEncoderResources = true;
            DLL_Log(
                "[VideoEncoder] Stop: ERROR writer_finalize_timeout phase=%s timeout=%llums elapsed=%llums "
                "queueBytes=%zu queuePackets=%u writerRetainsEncoderResources=%d; "
                "skipping synchronous finalize",
                WriterFinalizePhaseName(timedOutPhase),
                static_cast<unsigned long long>(kWriterFinalizeTimeoutMs),
                static_cast<unsigned long long>(GetTickCount64() - waitStartMs),
                currentQueueBytes.load(std::memory_order_relaxed), currentQueuePackets.load(std::memory_order_relaxed),
                writerStillOwnsEncoderResources ? 1 : 0);
        }
    }

    if (writerStillOwnsEncoderResources) {
        return;
    }

    // Fallback: if thread wasn't running and file is still open, close it now
    if (fileOpened && !writerRunning.load(std::memory_order_acquire)) {
        DLL_Log("[VideoEncoder] Sync Stop: Finalizing file...");
        if (fmtCtx) {
            int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (finalDurationUs > 0) {
                LogPacketTimelineSummary(finalDurationUs);
            }
            const int trailerResult = av_write_trailer(fmtCtx);
            if (trailerResult < 0) {
                DLL_Log("[VideoEncoder] Sync Stop: ERROR av_write_trailer failed: %d", trailerResult);
            }
            if (finalDurationUs > 0) {
                LogFinalDurationSummary(fmtCtx, finalDurationUs, muxBackpressureCount.load(std::memory_order_relaxed),
                                        peakQueueBytes.load(std::memory_order_relaxed),
                                        peakQueuePackets.load(std::memory_order_relaxed),
                                        lastEncoderOverloadTickMs.load(std::memory_order_relaxed) > 0,
                                        lastMuxOverloadTickMs.load(std::memory_order_relaxed) > 0);
            }
            int closeResult = 0;
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0) {
                    DLL_Log("[VideoEncoder] Sync Stop: ERROR avio_closep failed: %d", closeResult);
                }
            }
            fileOpened = false;
            const bool published = FinalizeOutputPublication(trailerResult, closeResult, finalDurationUs);
            DLL_Log("[VideoEncoder] mux_closed file='%s' finalDurationUs=%lld", outputFilename.c_str(),
                    (long long)finalDurationUs);
            if (published && finalDurationUs > 0) {
                RunPostMuxDurationProbeBounded(outputFilename, finalDurationUs,
                                video_encoder_kPostMuxProbeTimeoutMs);
            }
        }
    }

    CleanupResources();
}

void VideoEncoder::Cancel() {
    discardOutputRequested.store(true, std::memory_order_release);
    DLL_Log("[VideoEncoder] Cancellation requested; any unpublished output will be discarded");
    Stop();
}

// Async Packet Writer Loop
void VideoEncoder::AsyncWriteLoop() {
    DLL_Log("[VideoEncoder] Async Writer Thread Started");

    while (writerRunning || isStopping) {
        std::unique_lock<std::mutex> lock(queueMutex);

        // Wait for packets or stop signal
        queueCV.wait(lock, [this] { return !packetQueue.empty() || isStopping || !writerRunning; });

        // Drain queue
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
            packetQueue.pop();
            currentQueuePackets.store(SaturatingToUint32(packetQueue.size()), std::memory_order_relaxed);

            size_t pktSize = pkt->size + sizeof(AVPacket);
            currentQueueBytes -= pktSize;

            lock.unlock();  // Release lock while doing I/O

            if (fileOpened && fmtCtx) {
                // Log last few audio/video packets to verify PTS alignment
                if (pkt->stream_index != stream->index) {
                    ++audioWriteLogCount;
                    if (audioWriteLogCount <= 5 || audioWriteLogCount % 500 == 0) {
                        AVStream* ast = fmtCtx->streams[pkt->stream_index];
                        int64_t aPtsUs = av_rescale_q(pkt->pts, ast->time_base, AVRational{1, 1000000});
                        AVStream* vst = fmtCtx->streams[stream->index];
                        int64_t lastVUs = lastMuxerVideoPtsUs.load(std::memory_order_relaxed);
                        DLL_Log("[MuxAudio] pkt#%d pts=%lld tb=%d/%d ptsUs=%lld lastVideoPtsUs=%lld diffMs=%lld",
                                audioWriteLogCount, (long long)pkt->pts, ast->time_base.num, ast->time_base.den, aPtsUs,
                                lastVUs, (aPtsUs - lastVUs) / 1000);
                    }
                } else {
                    lastMuxerVideoPtsUs.store(av_rescale_q(pkt->pts, stream->time_base, AVRational{1, 1000000}),
                                              std::memory_order_relaxed);
                }
                const int writtenStreamIndex = pkt->stream_index;
                const int64_t writtenPts = pkt->pts;
                const int64_t writtenDts = pkt->dts;
                const int64_t writtenDuration = pkt->duration;
                const AVRational writtenTimeBase = fmtCtx->streams[pkt->stream_index]->time_base;
                const uint32_t writtenTerminalDiscardSamples = GetPacketTerminalDiscardSamples(pkt);
                const int writtenSampleRate = fmtCtx->streams[pkt->stream_index]->codecpar
                                                  ? fmtCtx->streams[pkt->stream_index]->codecpar->sample_rate
                                                  : 0;
                int ret = av_interleaved_write_frame(fmtCtx, pkt);
                if (ret >= 0) {
                    RecordWrittenPacketTimeline(writtenStreamIndex, writtenPts, writtenDts, writtenDuration,
                                                writtenTimeBase, writtenTerminalDiscardSamples, writtenSampleRate);
                }
                if (ret < 0) {
                    if (asyncWriteErrorCount++ < 10) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        DLL_Log(
                            "[VideoEncoder] ERROR: av_interleaved_write_frame failed: "
                            "%d (%s) pts=%lld",
                            ret, errbuf, pkt->pts);
                    }
                }
            }

            av_packet_free(&pkt);
            PublishRuntimeState();
            lock.lock();  // Re-acquire lock
        }

        // Handle Stop/Flush signal
        if (isStopping) {
            writerFinalizePhase.store(kWriterPhaseFinalizeStarting, std::memory_order_release);
            DLL_Log("[VideoEncoder] Async Finalize: Starting...");

            // 1. Flush Encoder if valid
            if (initDone && codecCtx && fileOpened) {
                writerFinalizePhase.store(kWriterPhaseFlushingEncoder, std::memory_order_release);
                DLL_Log("[VideoEncoder] Async Finalize: Flushing encoder...");
                avcodec_send_frame(codecCtx, nullptr);

                AVPacket* pkt = av_packet_alloc();
                int flushedCount = 0;
                while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                    // We need to set stream index and rescale PTS here
                    // Note: We use the same write logic as WriteFrame but simplified
                    pkt->stream_index = stream->index;

                    if (!NormalizeHdrPacketIfNeeded(pkt)) {
                        av_packet_unref(pkt);
                        flushedCount++;
                        continue;
                    }

                    av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
                    if (pkt->dts == AV_NOPTS_VALUE) {
                        pkt->dts = pkt->pts;
                    }
                    if (pkt->duration <= 0) {
                        // Use same Bresenham-aware duration as WriteFrame for
                        // consistent frame timing in the last flushed packets.
                        if (!savedConfig.useVFR && codecCtx->time_base.den > 0) {
                            int64_t frameNum =
                                av_rescale_q_rnd(pkt->pts, stream->time_base, codecCtx->time_base, AV_ROUND_NEAR_INF);
                            int64_t nextPts = av_rescale_q(frameNum + 1, codecCtx->time_base, stream->time_base);
                            pkt->duration = nextPts - pkt->pts;
                        }
                        if (pkt->duration <= 0) {
                            int fps = codecCtx->framerate.num;
                            if (fps > 0) {
                                pkt->duration = av_rescale(1, stream->time_base.den, fps);
                            }
                        }
                        if (pkt->duration <= 0) {
                            pkt->duration = av_rescale_q(1, codecCtx->time_base, stream->time_base);
                        }
                        if (pkt->duration <= 0) {
                            pkt->duration = 1;
                        }
                    }
                    // Clamp flushed packet duration to sane range
                    {
                        int64_t maxDuration = av_rescale_q(2, codecCtx->time_base, stream->time_base);
                        if (maxDuration < 2)

                            maxDuration = 2;
                        if (pkt->duration > maxDuration)
                            pkt->duration = maxDuration;
                    }

                    if (pkt->pts != AV_NOPTS_VALUE) {
                        int64_t packetEnd = pkt->pts + pkt->duration;
                        int64_t packetEndUs = av_rescale_q(packetEnd, stream->time_base, AVRational{1, 1000000});
                        int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
                        if (packetEndUs > prevEndUs) {
                            encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
                        }
                    }

                    const int flushedStreamIndex = pkt->stream_index;
                    const int64_t flushedPts = pkt->pts;
                    const int64_t flushedDts = pkt->dts;
                    const int64_t flushedDuration = pkt->duration;
                    const AVRational flushedTimeBase = stream->time_base;
                    if (av_interleaved_write_frame(fmtCtx, pkt) >= 0) {
                        RecordWrittenPacketTimeline(flushedStreamIndex, flushedPts, flushedDts, flushedDuration,
                                                    flushedTimeBase, 0, 0);
                    }
                    av_packet_unref(pkt);
                    flushedCount++;
                }
                av_packet_free(&pkt);
                DLL_Log("[VideoEncoder] Async Finalize: Flushed %d remaining packets", flushedCount);
            }

            // 2. Write Trailer and Close File
            if (fmtCtx && fileOpened) {
                writerFinalizePhase.store(kWriterPhaseWritingTrailer, std::memory_order_release);
                DLL_Log("[VideoEncoder] Async Finalize: Writing Trailer...");
                int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
                if (finalDurationUs > 0) {
                    LogPacketTimelineSummary(finalDurationUs);
                    DLL_Log("[VideoEncoder] Async Finalize: packet-derived duration target was %lld us",
                            finalDurationUs);
                }
                const int trailerResult = av_write_trailer(fmtCtx);
                if (trailerResult < 0) {
                    DLL_Log("[VideoEncoder] Async Finalize: ERROR av_write_trailer failed: %d", trailerResult);
                }
                if (finalDurationUs > 0) {
                    for (unsigned s = 0; s < fmtCtx->nb_streams; s++) {
                        AVStream* st = fmtCtx->streams[s];
                        int64_t firstPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
                        int64_t lastPts = firstPts;
                        if (st->duration > 0) {
                            lastPts = firstPts + st->duration;
                        }
                        int64_t firstPtsUs = av_rescale_q(firstPts, st->time_base, AVRational{1, 1000000});
                        int64_t lastPtsUs = av_rescale_q(lastPts, st->time_base, AVRational{1, 1000000});
                        DLL_Log(
                            "[PTS ALIGN METADATA] Stream %u (codec=%s): first=%lldus last=%lldus dur=%lldus tb=%d/%lld",
                            s,
                            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                                ? "video"
                                : (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? "audio" : "unknown"),
                            (long long)firstPtsUs, (long long)lastPtsUs, (long long)(lastPtsUs - firstPtsUs),
                            st->time_base.num, (long long)st->time_base.den);
                    }
                    LogFinalDurationSummary(fmtCtx, finalDurationUs,
                                            muxBackpressureCount.load(std::memory_order_relaxed),
                                            peakQueueBytes.load(std::memory_order_relaxed),
                                            peakQueuePackets.load(std::memory_order_relaxed),
                                            lastEncoderOverloadTickMs.load(std::memory_order_relaxed) > 0,
                                            lastMuxOverloadTickMs.load(std::memory_order_relaxed) > 0);
                }
                int closeResult = 0;
                if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    closeResult = avio_closep(&fmtCtx->pb);
                    if (closeResult < 0) {
                        DLL_Log("[VideoEncoder] Async Finalize: ERROR avio_closep failed: %d", closeResult);
                    }
                }
                fileOpened = false;
                const bool published = FinalizeOutputPublication(trailerResult, closeResult, finalDurationUs);
                DLL_Log("[VideoEncoder] mux_closed file='%s' finalDurationUs=%lld", outputFilename.c_str(),
                        (long long)finalDurationUs);
                if (published && finalDurationUs > 0) {
                    writerFinalizePhase.store(kWriterPhasePostMuxProbe, std::memory_order_release);
                    RunPostMuxDurationProbeBounded(outputFilename, finalDurationUs,
                                video_encoder_kPostMuxProbeTimeoutMs);
                }
                DLL_Log("[VideoEncoder] Async Finalize: Output file closed.");
            }

            // IMPORTANT: we still hold queueMutex (lock) here.
            // CleanupResources() also locks queueMutex to drain packetQueue.
            // Unlock first to avoid self-deadlock during finalize.
            lock.unlock();

            writerFinalizePhase.store(kWriterPhaseCleanup, std::memory_order_release);
            CleanupResources();

            isStopping = false;
            writerRunning = false;
            writerFinalizePhase.store(kWriterPhaseComplete, std::memory_order_release);
            DLL_Log("[VideoEncoder] Async Finalize: Complete.");
            break;  // Exit thread
        }
    }

    DLL_Log("[VideoEncoder] Async Writer Thread Stopped");
}

            packetStats.refCount++;
        }

        // Log packet type distribution every 600 packets (~5 seconds at 120fps)
        if (packetStats.totalPackets > 0 && packetStats.totalPackets % 600 == 0) {
            int total = packetStats.totalPackets;
            int64_t avgKey = packetStats.keyframeCount > 0 ? packetStats.keyframeBytes / packetStats.keyframeCount : 0;
            int64_t avgRef = packetStats.refCount > 0 ? packetStats.refBytes / packetStats.refCount : 0;
            int64_t avgB = packetStats.bframeCount > 0 ? packetStats.bframeBytes / packetStats.bframeCount : 0;
            DLL_Log(
                "[VideoEncoder] PACKET STATS (%d pkts): "
                "Key=%d(avg %lldKB) Ref=%d(avg %lldKB) "
                "SEF=%d(%d%%) B-small=%d(avg %lldB)",
                total, packetStats.keyframeCount, avgKey / 1024, packetStats.refCount, avgRef / 1024,
                packetStats.sefCount, total > 0 ? packetStats.sefCount * 100 / total : 0, packetStats.bframeCount,
                avgB);

            // Warn about B-frame quality oscillation
            if (packetStats.sefCount + packetStats.bframeCount > total / 3 && avgRef > 0 && avgB > 0 &&
                avgB < avgRef / 50) {
                DLL_Log(
                    "[VideoEncoder] WARNING: B-frame quality oscillation detected! "
                    "B-frames average %lldB vs reference frames %lldKB (ratio 1:%lld). "
                    "Consider b_frames=0 for smoothest capture.",
                    avgB, avgRef / 1024, avgRef / (avgB > 0 ? avgB : 1));
            }
        }
    }

    // Rescale timestamps properly using FFmpeg's exact rational math
    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
    if (pkt->stream_index == stream->index && pkt->dts == AV_NOPTS_VALUE) {
        pkt->dts = pkt->pts;
    }

    // DEBUG: Log PTS after rescaling and detect corruption
    if (pkt->stream_index == stream->index) {
        if (vidDebugCount++ < 20 || pkt->pts < 0) {
            DLL_Log("[VideoEncoder] PTS PRECISE: frame=%lld pts_us=%lld st_tb=%d/%d", pkt->pts, pkt->pts,
                    st->time_base.num, st->time_base.den);
        }

        // DEBUG LEAK: Log queue stats every 100 video frames
        if (vidDebugCount % 100 == 0) {
            size_t qBytes = currentQueueBytes.load();
            size_t qSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                qSize = packetQueue.size();
            }
            DLL_Log("[VideoEncoder] QUEUE STATS: Count=%zu Bytes=%zu (Max=%zu)", qSize, qBytes, MAX_QUEUE_BYTES);

            // Memory safety check
            if (qBytes > MAX_QUEUE_BYTES) {
                DLL_Log("[VideoEncoder] CRITICAL: Queue exceeds limit! Dropping disabled?");
            }
        }
    }

    // CRITICAL: For video packets, explicitly set duration after rescaling.
    // In CFR mode, the Bresenham PTS distribution (e.g. 120fps at 1/1000 time_base
    // produces gaps of 8,8,9,8,8,9...) means a fixed duration of 8 leaves a 1ms
    // gap for every 9ms step.  Compute each frame's exact duration from the
    // sequential PTS difference so it always matches the actual PTS spacing.
    if (pkt->stream_index == stream->index) {
        int64_t preClampDuration = pkt->duration;
        int fps = codecCtx->framerate.num;
        if (fps <= 0)
            fps = 60;
        if (!savedConfig.useVFR) {
            // CFR: derive per-frame duration from the Bresenham PTS sequence.
            // Round-trip the already-rescaled PTS back to the codec frame number
            // so the calculation is independent of packet arrival order (critical
            // for B-frame codecs that output packets in decode order).
            //
            // Try codec-provided duration first (NVENC sets this correctly including
            // for AV1 B-frames and SEF packets), fall back to round-trip rescaling.
            if (pkt->duration > 0) {
                // Codec provided a valid duration — use it directly
            } else {
                int64_t frameNum = av_rescale_q_rnd(pkt->pts, st->time_base, codecCtx->time_base, AV_ROUND_NEAR_INF);
                int64_t nextPts = av_rescale_q(frameNum + 1, codecCtx->time_base, st->time_base);
                pkt->duration = nextPts - pkt->pts;
            }
            // Clamp duration to sane range for CFR: [1, fps] to prevent
            // 0-duration or extreme-duration packets from corrupting the
            // MKV container timeline.  AV1 SEF packets can produce duration=0
            // from the codec, and round-trip rescaling can produce 0 or 2
            // due to integer rounding at non-power-of-2 FPS.
            int64_t maxDuration = av_rescale_q(2, codecCtx->time_base, st->time_base);
            if (maxDuration < 2)
                maxDuration = 2;
            if (pkt->duration <= 0)
                pkt->duration = 1;
            if (pkt->duration > maxDuration)
                pkt->duration = maxDuration;
        } else {
            pkt->duration = av_rescale(1, st->time_base.den, fps);
        }
        if (pkt->duration <= 0)
            pkt->duration = 1;
        if (pkt->duration != preClampDuration) {
            packetDurationClampCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Track authoritative encoded video duration from packet timeline.
    if (pkt->stream_index == stream->index) {
        if (pkt->pts < 0 || pkt->dts < 0) {
            negativePtsCount.fetch_add(1, std::memory_order_relaxed);
        }
        int64_t packetTimelinePts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (lastQueuedVideoPts != AV_NOPTS_VALUE && packetTimelinePts != AV_NOPTS_VALUE &&
            packetTimelinePts < lastQueuedVideoPts) {
            nonMonotonicPtsCount.fetch_add(1, std::memory_order_relaxed);
            DLL_Log("[VideoEncoder] WARNING: non-monotonic packet pts prev=%lld cur=%lld dur=%lld",
                    static_cast<long long>(lastQueuedVideoPts), static_cast<long long>(packetTimelinePts),
                    static_cast<long long>(pkt->duration));
        }
        if (packetTimelinePts != AV_NOPTS_VALUE) {
            lastQueuedVideoPts = packetTimelinePts;
        }
        int64_t packetPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (packetPts != AV_NOPTS_VALUE) {
            int64_t packetDuration = pkt->duration;
            if (packetDuration <= 0) {
                packetDuration = av_rescale_q(1, codec_tb, st->time_base);
                if (packetDuration <= 0) {
                    packetDuration = 1;
                }
            }
            int64_t packetEnd = packetPts + packetDuration;
            int64_t packetEndUs = av_rescale_q(packetEnd, st->time_base, AVRational{1, 1000000});
            int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (packetEndUs > prevEndUs) {
                encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
            }
        }
    }

    // ASYNC WRITE: Push to queue instead of writing directly

    // IMPORTANT: Never drop encoded packets, it causes visible corruption.
    // Instead apply backpressure to the encode thread.
    // If storage is extremely slow, this will manifest as stutter/dropped input
    // frames (FrameQueue will drop/duplicate), but the bitstream stays valid.
    uint64_t backpressureWaitUs = 0;
    for (;;) {
        size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
        if (qBytes <= MAX_QUEUE_BYTES) {
            break;
        }

        lastMuxOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
        muxBackpressureCount.fetch_add(1, std::memory_order_relaxed);
        PublishRuntimeState();

        static int overloadLogCount = 0;
        if (overloadLogCount++ % 60 == 0) {
            DLL_Log(
                "[VideoEncoder] WARNING: Packet queue overloaded (%zu bytes) - "
                "applying backpressure",
                qBytes);
        }

        // Wait briefly for writer to drain.
        const auto waitStart = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCV.wait_for(lock, std::chrono::milliseconds(2), [this] {
            return currentQueueBytes.load(std::memory_order_relaxed) <= MAX_QUEUE_BYTES || isStopping || !writerRunning;
        });
        backpressureWaitUs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart)
                .count());
        if (isStopping || !writerRunning) {
            break;
        }
    }
    if (backpressureWaitUs > 0) {
        const uint32_t waitUs32 = SaturatingToUint32(backpressureWaitUs);
        muxBackpressureWaitUs.store(waitUs32, std::memory_order_relaxed);
        UpdateAtomicPeak(muxBackpressureMaxWaitUs, waitUs32);
    }

    AVPacket* clonePkt = av_packet_clone(pkt);
    if (clonePkt) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            packetQueue.push(clonePkt);
            currentQueueBytes += clonePkt->size + sizeof(AVPacket);
            currentQueuePackets.store(SaturatingToUint32(packetQueue.size()), std::memory_order_relaxed);
        }
        UpdateAtomicPeak(peakQueueBytes, SaturatingToUint32(currentQueueBytes.load(std::memory_order_relaxed)));
        UpdateAtomicPeak(peakQueuePackets, currentQueuePackets.load(std::memory_order_relaxed));
        PublishRuntimeState();
        queueCV.notify_one();
    }
}

void VideoEncoder::PublishRuntimeState() {
    if (!pSharedMem) {
        return;
    }

    // Keep this cheap and lock-free: only atomics.
    uint32_t flags = 0;
    uint64_t nowMs = GetTickCount64();

    constexpr uint64_t kOverloadHoldMs = 1000;
    uint64_t encTick = lastEncoderOverloadTickMs.load(std::memory_order_relaxed);
    uint64_t muxTick = lastMuxOverloadTickMs.load(std::memory_order_relaxed);

    if (encTick != 0 && (nowMs - encTick) <= kOverloadHoldMs) {
        flags |= ce::capture_policy::kEncoderOverloadFlagEncoder;
    }
    if (pSharedMem->runtimeState.encoderBottlenecked.load(std::memory_order_relaxed) != 0) {
        flags |= ce::capture_policy::kEncoderOverloadFlagEncoder;
    }
    if (muxTick != 0 && (nowMs - muxTick) <= kOverloadHoldMs) {
        flags |= ce::capture_policy::kEncoderOverloadFlagMux;
    }

    pSharedMem->runtimeState.encoderOverloadFlags.store(flags, std::memory_order_relaxed);
    const double encodeMs = static_cast<double>(std::max<int64_t>(lastEncodeTimeUs, 0)) / 1000.0;
    UpdateAdaptiveGpuThreadPriority(nowMs, encodeMs, (flags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0);
    const double sustainFps = ce::capture_policy::GetEncoderSustainableOutputFps(encodeMs);
    const uint32_t sustainFpsX100 =
        sustainFps > 0.0 ? static_cast<uint32_t>(std::clamp(sustainFps * 100.0, 0.0, 4294967295.0)) : 0u;
    pSharedMem->runtimeState.encoderSustainFpsX100.store(sustainFpsX100, std::memory_order_relaxed);

    size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
    uint32_t qBytes32 = (qBytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)qBytes;
    pSharedMem->runtimeState.muxQueueBytes.store(qBytes32, std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePackets.store(currentQueuePackets.load(std::memory_order_relaxed),
                                                   std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePeakBytes.store(peakQueueBytes.load(std::memory_order_relaxed),
                                                     std::memory_order_relaxed);
    pSharedMem->runtimeState.muxQueuePeakPackets.store(peakQueuePackets.load(std::memory_order_relaxed),
                                                       std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureCount.store(muxBackpressureCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureWaitUs.store(muxBackpressureWaitUs.load(std::memory_order_relaxed),
                                                         std::memory_order_relaxed);
    pSharedMem->runtimeState.muxBackpressureMaxWaitUs.store(muxBackpressureMaxWaitUs.load(std::memory_order_relaxed),
                                                            std::memory_order_relaxed);
    pSharedMem->runtimeState.packetDurationClamps.store(packetDurationClampCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
    pSharedMem->runtimeState.negativePtsCount.store(negativePtsCount.load(std::memory_order_relaxed),
                                                    std::memory_order_relaxed);
    pSharedMem->runtimeState.nonMonotonicPtsCount.store(nonMonotonicPtsCount.load(std::memory_order_relaxed),
                                                        std::memory_order_relaxed);
}

bool VideoEncoder::EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, int64_t timestamp,
                               uint32_t sourcePid, int width, int height, int format, bool isHDR, bool isShmem,
                               int shmemSlot) {
    if (!recordingRequested)
        return false;

    lastFrameDeferred.store(false, std::memory_order_relaxed);

    // Debug: Log every 60th frame entry to verify loop
    if (encodeFrameCounter % 60 == 0) {
        DLL_Log("[VideoEncoder] EncodeFrame Entry: PID=%u Handle=%p FenceVal=%llu", sourcePid, sharedHandle,
                fenceValue);
    }

    const bool wants10BitInput =
        isHDR || ce::video_format::IsHighPrecisionRgbInputFormat(static_cast<DXGI_FORMAT>(format));
    if (!initDone || isHDR != currentIsHDR || wants10BitInput != currentUse10BitInput) {
        const bool reinitializingActiveRecording = initDone;
        const std::string preservedOutputFilename = outputFilename;
        auto preservedOutputReservation = std::move(outputReservation);
        if (reinitializingActiveRecording) {
            DLL_Log("[VideoEncoder] Format mode changed (hdr=%d->%d use10bit=%d->%d). Re-initializing...", currentIsHDR,
                    isHDR, currentUse10BitInput, wants10BitInput);
            Stop();  // Clean up existing encoder
            initDone = false;
            // Also need to clear codecOpenFailed?
            codecOpenFailed = false;
        }

        currentIsHDR = isHDR;
        currentUse10BitInput = wants10BitInput;
        // Re-Init with saved config (Init uses currentIsHDR to pick format)
        if (!Init(savedConfig, width, height, savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for format mode change");
            return false;
        }
        if (reinitializingActiveRecording) {
            if (!preservedOutputFilename.empty()) {
                outputReservation = std::move(preservedOutputReservation);
                outputFilename = preservedOutputFilename;
                DLL_Log("[VideoEncoder] Preserving output filename across format mode re-init: %s",
                        outputFilename.c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first frame: %s",
                    outputFilename.c_str());
            BeginDeferredRecording();
        }
    }

    // Use captured frame dimensions if not yet set or changed
    if (this->width != width || this->height != height) {
        if (this->width == 0) {
            DLL_Log("[VideoEncoder] Initial resolution discovered: %dx%d (Input: %dx%d)", width, height, width, height);
        } else {
            DLL_Log("[VideoEncoder] Resolution CHANGE detected: %dx%d -> %dx%d", this->width, this->height, width,
                    height);
            if (!fileOpened && initDone) {
                // Pre-warm used stale/wrong dimensions. Reset codec and container
                // so EnsureDevice() reinitializes them at the correct resolution
                // before the file header is written.
                DLL_Log("[VideoEncoder] Reinitializing encoder at correct resolution (pre-file-open)");
                CleanupVideoProcessor();
                avcodec_free_context(&codecCtx);
                if (hwFramesCtx) {
                    av_buffer_unref(&hwFramesCtx);
                }
                if (hwDeviceCtx) {
                    av_buffer_unref(&hwDeviceCtx);
                }
                if (d3d11FramesCtx) {
                    av_buffer_unref(&d3d11FramesCtx);
                    d3d11FramesCtx = nullptr;
                }
                stream = nullptr;
                if (fmtCtx) {
                    avformat_free_context(fmtCtx);
                    fmtCtx = nullptr;
                    AllocateOutputContextForContainer(&fmtCtx, savedConfig);
                }
                const AVCodec* c = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
                if (c)
                    codecCtx = avcodec_alloc_context3(c);
                audioStreamIndex = -1;
                initDone = false;
            }
        }
        this->width = width;
        this->height = height;
    }

    if (!EnsureDevice())
        return false;

    // Fall through to D3D11 path below

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            if (!outputReservation.ReleaseToWriter()) {
                DLL_Log("[VideoEncoder] ERROR: Reserved output identity changed before mux open: %s",
                        outputFilename.c_str());
                return false;
            }
            // Use 256KB buffer for better performance on slow storage (HDD/network)
            // Default is 32KB which causes many small writes
            int ret = avio_open2(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (ret < 0) {
                DLL_Log("Failed to open output file: %d", ret);
                return false;
            }

            // Allocate custom buffer (256KB) for improved write performance
            const int bufferSize = 256 * 1024;
            [[maybe_unused]] unsigned char* buffer = nullptr;
        }

        // Debug: Log stream info before write_header
        DLL_Log("[VideoEncoder] fmtCtx has %d streams before write_header", fmtCtx->nb_streams);
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            AVStream* s = fmtCtx->streams[i];
            AVCodecParameters* cp = s->codecpar;
            DLL_Log(
                "[VideoEncoder] Stream %d: type=%d codec_id=%d w=%d h=%d "
                "extradata=%p extradata_size=%d",
                i, cp->codec_type, cp->codec_id, cp->width, cp->height, cp->extradata, cp->extradata_size);
        }

        // Pre-allocate space for MKV cues (seek index) at the front of the file.
        // Without this, cues are written at the END and many players can't seek
        // or show correct duration without reading the whole file first.
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "2000000", 0);  // 2MB
        }
        if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmtCtx)) {
            DLL_Log("[VideoEncoder] ERROR: Matroska timestamp_precision=1000 is required but unavailable");
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close rejected output: %d", closeResult);
            }
            return false;
        }

        if (!ValidateFormatContextForHeader(fmtCtx)) {
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close invalid output context: %d", closeResult);
            }
            return false;
        }

        int ret = avformat_write_header(fmtCtx, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("Failed to write header: %d (%s)", ret, errbuf);
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close output after header failure: %d", closeResult);
            }
            return false;
        }

        // Log actual stream time_base after muxer init (MKV may override)
        DLL_Log("[VideoEncoder] Stream time_base after write_header: %d/%d (codec: %d/%d)", stream->time_base.num,
                stream->time_base.den, codecCtx->time_base.num, codecCtx->time_base.den);

        // Force header to hit disk immediately. This prevents 0KB files when
        // subsequent writes fail and makes I/O errors surface at the true failure
        // point.
        if (fmtCtx->pb) {
            avio_flush(fmtCtx->pb);
            if (fmtCtx->pb->error < 0) {
                DLL_Log("Failed to flush header: %d", fmtCtx->pb->error);
                if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                    const int closeResult = avio_closep(&fmtCtx->pb);
                    if (closeResult < 0)
                        DLL_Log("[VideoEncoder] ERROR: Failed to close output after flush failure: %d", closeResult);
                }
                return false;
            }
        }
        fileOpened = true;
    }

    // Frame rate control is now handled by capture engine (time-based sampling)
    // We just encode every frame we receive using frame counter for CFR output
    inputFrameCount++;

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;

    // Log frame stats periodically (about once per second at the configured FPS)
    // Detect new recording start (startPts is -1) and reset counters
    if (startPts < 0) {
        needsCounterReset = true;  // Mark that we need to reset on first frame
    }

    if (outputFrameCount - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && timestamp > startPts) {
            // Inject-mode timestamps are in microseconds.
            double elapsedSec = (double)(timestamp - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? ((double)outputFrameCount / elapsedSec) : 0.0;
            DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs", (double)outputFrameCount, outputFps, elapsedSec);
        }
        lastLogFrameCount = outputFrameCount;
    }

    // Reset counters on new recording
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log("[VideoEncoder] Reset frameCounter for new recording");
    }

    encodeFrameCounter++;

    // Performance timing for this frame
    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(timestamp);

    // Calculate frame timing for smoothness analysis
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    auto frameStart = PerfTimer::now();

    ID3D11Texture2D* bgraTex = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;
    int cacheSlot = -1;

    if (isShmem) {
        if (pShmem && pSharedMem && pSharedMem->GetShmemMappingCreated()) {
            // Shmem Path: Upload pixels to our owned texture
            int texIdx = 0;  // Reuse first shared capture texture (we own it)
            bgraTex = sharedCaptureTextures[texIdx];

            if (bgraTex) {
                // Validation of slot
                int slot = (shmemSlot >= 0 && shmemSlot < 2) ? shmemSlot : 0;
                uint8_t* pSrc = pShmem->GetData(slot);

                if (pSrc) {
                    D3D11_BOX box;
                    box.left = 0;
                    box.right = pSharedMem->GetWidth();  // Use current frame resolution
                    box.top = 0;
                    box.bottom = pSharedMem->GetHeight();
                    box.front = 0;
                    box.back = 1;

                    // We need a pitch. Use pSharedMem->width * 4 if not stored in
                    // ShmemBuffer Actually ShmemBuffer has pitch.
                    d3d11Context->UpdateSubresource(bgraTex, 0, &box, pSrc, pShmem->pitch, 0);
                }
                bgraTex->AddRef();     // For consistency with Release() below
                d3d11Fence = nullptr;  // No fence for shmem
            }
        }
    } else {
        // Check if layer told us to use our own encoder textures directly
        // (DXVK zero-copy path: layer imported our KMT handles into Vulkan)
        if (pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) &&
            sharedCaptureTexturesCreated) {
            // Find which encoder texture matches by KMT handle
            int matchIdx = -1;
            for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
                if (sharedCaptureKmtHandles[i] == sharedHandle) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                bgraTex = sharedCaptureTextures[matchIdx];
            }
            if (bgraTex) {
                bgraTex->AddRef();

                HANDLE directFenceHandle = fenceHandle;
                if ((!directFenceHandle || directFenceHandle == INVALID_HANDLE_VALUE) && pSharedMem) {
                    directFenceHandle = reinterpret_cast<HANDLE>(pSharedMem->encoderTextures.GetFenceHandle());
                }

                if (directFenceHandle && directFenceHandle != INVALID_HANDLE_VALUE && fenceValue > 0) {
                    HANDLE directFenceHandleAlt = NormalizeSourceHandleForWow64(directFenceHandle, sourcePid);
                    const bool hasDirectFenceAlt = (directFenceHandleAlt != directFenceHandle);

                    if (sourcePid > 0 && sourcePid == cachedSourcePid && cachedFenceHandle == directFenceHandle &&
                        cachedD3D11Fence) {
                        d3d11Fence = cachedD3D11Fence;
                        d3d11Fence->AddRef();
                    } else {
                        ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));
                        HRESULT fenceHr = E_FAIL;
                        if (hProcess) {
                            ce::HandleGuard dupFence;
                            if (DuplicateHandle(hProcess.get(), directFenceHandle, GetCurrentProcess(),
                                                dupFence.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                fenceHr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            }
                            if (FAILED(fenceHr) && hasDirectFenceAlt) {
                                ce::HandleGuard dupFenceAlt;
                                if (DuplicateHandle(hProcess.get(), directFenceHandleAlt, GetCurrentProcess(),
                                                    dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                    fenceHr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                                }
                            }
                        }
                        if (FAILED(fenceHr) && !g_HandleFailureCache.ShouldSkipFence(directFenceHandle)) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandle, &d3d11Fence);
                        }
                        if (FAILED(fenceHr) && hasDirectFenceAlt) {
                            fenceHr = CallOpenSharedFence(d3d11Device, directFenceHandleAlt, &d3d11Fence);
                        }

                        if (d3d11Fence) {
                            if (cachedD3D11Fence) {
                                cachedD3D11Fence->Release();
                            }
                            cachedD3D11Fence = d3d11Fence;
                            cachedD3D11Fence->AddRef();
                            cachedFenceHandle = directFenceHandle;
                            cachedSourcePid = sourcePid;
                        } else if (encodeFrameCounter < 20) {
                            DLL_Log(
                                "[VideoEncoder] Frame %d: Failed to open encoder-texture fence handle=%p value=%llu "
                                "pid=%u",
                                encodeFrameCounter, directFenceHandle, static_cast<unsigned long long>(fenceValue),
                                sourcePid);
                        }
                    }
                }
            }
            if (matchIdx >= 0 && encodeFrameCounter < 10) {
                DLL_Log(
                    "[VideoEncoder] Frame %d: Using encoder-owned texture[%d] directly (encoder fence=%p value=%llu)",
                    encodeFrameCounter, matchIdx, fenceHandle, static_cast<unsigned long long>(fenceValue));
            }
        }

        if (!bgraTex) {
            // Standard shared handle path
            HANDLE sharedHandleAlt = NormalizeSourceHandleForWow64(sharedHandle, sourcePid);
            HANDLE fenceHandleAlt = NormalizeSourceHandleForWow64(fenceHandle, sourcePid);
            const bool hasSharedAlt = (sharedHandleAlt != sharedHandle);
            const bool hasFenceAlt = (fenceHandleAlt != fenceHandle);

            // Check cache for valid fence and texture (Quad-Buffered Cache)
            // Texture caching works independently of fence (for D3D11 KMT path)
            cacheSlot = -1;
            bool skipFence = (fenceValue == 0 || fenceHandle == 0 || fenceHandle == INVALID_HANDLE_VALUE);
            bool fenceValid = !skipFence && (sourcePid > 0 && sourcePid == cachedSourcePid &&
                                             fenceHandle == cachedFenceHandle && cachedD3D11Fence);

            // For texture matching, we only need matching PID and handle
            // (fence-independent)
            bool pidMatches = (sourcePid > 0 && sourcePid == cachedSourcePid);

            // Search for cached texture by handle (works with or without fence)
            if (pidMatches) {
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == sharedHandle && cachedSharedTextures[i]) {
                        cacheSlot = i;
                        break;
                    }
                }
            } else if (sourcePid > 0) {
                // New process -> Clear all cache
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
                cachedFenceHandle = nullptr;
                cachedSourcePid = sourcePid;  // Remember new PID
            }

            // ID3D11Texture2D *bgraTex = nullptr; // Moved up
            // ID3D11Fence *d3d11Fence = nullptr;   // Moved up

            if (cacheSlot >= 0) {

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
                RunPostMuxDurationProbeBounded(outputFilename, finalDurationUs);
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

// ============================================================================
// D3D11 Video Processor for GPU-accelerated BGRA → NV12 conversion
// ============================================================================

bool VideoEncoder::InitVideoProcessor() {
    if (videoProcessorInit)
        return true;

    if (!d3d11Device) {
        DLL_Log("[VideoProcessor] D3D11 device not available");
        return false;
    }

    HRESULT hr;
    const bool outputIsHDR = ShouldEncodeHdrOutput();

    // Get video device interface
    hr = d3d11Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoDevice. HR=%x", hr);
        return false;
    }

    // Get video context
    hr = d3d11Context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&videoContext);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoContext. HR=%x", hr);
        return false;
    }
    hr = videoContext->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&videoContext1);
    if (FAILED(hr)) {
        videoContext1 = nullptr;
        DLL_Log(
            "[VideoProcessor] ID3D11VideoContext1 unavailable (HR=%x); SDR uses the legacy VP contract and HDR "
            "output uses the direct P010 shader without VideoProcessor color conversion",
            hr);
    }

    // Store input dimensions (captured frame size)
    inputWidth = width;
    inputHeight = height;

    // Determine output dimensions based on scaling config
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        outputWidth = savedConfig.scaling.outputWidth;
        outputHeight = savedConfig.scaling.outputHeight;
    } else {
        // No scaling or native resolution
        outputWidth = width;
        outputHeight = height;
    }

    // NV12 output textures require even-aligned dimensions
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    outputWidth = outputWidth & ~1u;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    outputHeight = outputHeight & ~1u;
    if (outputWidth == 0 || outputHeight == 0) {
        DLL_Log("[VideoProcessor] Dimensions too small after NV12 alignment");
        return false;
    }

    // Check if scaling is actually needed (input != output)
    scalingEnabled = (inputWidth != outputWidth || inputHeight != outputHeight);

    if (scalingEnabled && !outputIsHDR) {
        DLL_Log("[VideoProcessor] GPU SCALING ENABLED: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (!scalingEnabled) {
        DLL_Log("[VideoProcessor] Scaling disabled (input matches output: %dx%d)", inputWidth, inputHeight);
    }

    // Create video processor enumerator with potentially different input/output
    // dims
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inputWidth;
    contentDesc.InputHeight = inputHeight;
    contentDesc.OutputWidth = outputWidth;
    contentDesc.OutputHeight = outputHeight;
    contentDesc.Usage =
        (savedConfig.scaling.quality == "best") ? D3D11_VIDEO_USAGE_OPTIMAL_QUALITY : D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create enumerator. HR=%x", hr);
        return false;
    }

    // Create video processor
    try {
        hr = videoDevice->CreateVideoProcessor(videoProcessorEnum, 0, &videoProcessor);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create processor. HR=%x", hr);
        return false;
    }

    // Capture textures are progressive desktop/game frames. Driver automatic
    // processing may apply temporal video heuristics that are inappropriate
    // for pixel-sharp UI. A separate cursor is already point-composited into
    // this RGB stream before the VP sees it.
    videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 0, FALSE);
    D3D11_VIDEO_FRAME_FORMAT mainFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    BOOL mainAutoProcessing = TRUE;
    videoContext->VideoProcessorGetStreamFrameFormat(videoProcessor, 0, &mainFrameFormat);
    videoContext->VideoProcessorGetStreamAutoProcessingMode(videoProcessor, 0, &mainAutoProcessing);
    DLL_Log("[VideoProcessor] Deterministic single-stream processing: progressive=%d auto=%d",
            mainFrameFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE ? 1 : 0, mainAutoProcessing ? 1 : 0);

    // Configure scaling filter if scaling is enabled
    if (scalingEnabled && !outputIsHDR) {
        // Map sharpness (0-100) directly to D3D11 VP edge enhancement
        bool enableEdgeEnhancement = (savedConfig.scaling.sharpness > 0);
        int edgeEnhancementLevel = savedConfig.scaling.sharpness;

        if (enableEdgeEnhancement) {
            // Check if edge enhancement is supported
            D3D11_VIDEO_PROCESSOR_FILTER_RANGE filterRange = {};
            hr = videoProcessorEnum->GetVideoProcessorFilterRange(D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
                                                                  &filterRange);

            if (SUCCEEDED(hr)) {
                // Map our 0-100 level to the actual VP filter range
                int filterValue = filterRange.Default;
                if (filterRange.Maximum > filterRange.Minimum) {
                    filterValue = filterRange.Minimum +
                                  (edgeEnhancementLevel * (filterRange.Maximum - filterRange.Minimum) / 100);
                }

                videoContext->VideoProcessorSetStreamFilter(
                    videoProcessor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, filterValue);

                DLL_Log(
                    "[VideoProcessor] Scaling: quality=%s, sharpness=%d "
                    "(filterValue=%d, range=%d-%d)",
                    savedConfig.scaling.quality.c_str(), edgeEnhancementLevel, filterValue, filterRange.Minimum,
                    filterRange.Maximum);
            } else {
                DLL_Log(
                    "[VideoProcessor] Edge enhancement (sharpness) not supported "
                    "by hardware");
            }
        } else {
            DLL_Log("[VideoProcessor] Scaling: quality=%s, sharpness=0 (disabled)",
                    savedConfig.scaling.quality.c_str());
        }

        // CRITICAL: Set source and destination rectangles for scaling
        // Without these, VideoProcessorBlt fails with E_INVALIDARG
        RECT sourceRect = {0, 0, (LONG)inputWidth, (LONG)inputHeight};
        RECT destRect = {0, 0, (LONG)outputWidth, (LONG)outputHeight};

        // Stream 0: Source rect = full input frame
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor, 0, TRUE, &sourceRect);
        // Stream 0: Dest rect = full output frame (scaled)
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 0, TRUE, &destRect);
        // Output target = full output surface
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor, TRUE, &destRect);

        DLL_Log("[VideoProcessor] Scaling rects: source=%dx%d dest=%dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (scalingEnabled) {
        DLL_Log(
            "[HDR P010] Shader scaling configured: source=%dx%d dest=%dx%d filter=bilinear lumaSharpness=%d",
            inputWidth, inputHeight, outputWidth, outputHeight, savedConfig.scaling.sharpness);
    }

    const OutputRangeMode outputRange = GetEffectiveOutputRange(savedConfig.colorRange, outputIsHDR);

    // Configure color space: Full-range RGB input from capture -> requested YCbCr output range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.Usage = 0;          // 0 = Playback, 1 = Video processing
    inputColorSpace.RGB_Range = 0;      // 0 = Full range (0-255), 1 = Studio (16-235)
    inputColorSpace.YCbCr_Matrix = 1;   // 0 = BT.601, 1 = BT.709
    inputColorSpace.YCbCr_xvYCC = 0;    // 0 = Conventional, 1 = Extended
    inputColorSpace.Nominal_Range = 2;  // 2 = Full (0-255) for input

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.Usage = 0;
    outputColorSpace.RGB_Range = outputRange == OutputRangeMode::kFull ? 0 : 1;
    outputColorSpace.YCbCr_Matrix = 1;  // BT.709
    outputColorSpace.YCbCr_xvYCC = 0;
    outputColorSpace.Nominal_Range = outputRange == OutputRangeMode::kFull ? 2 : 1;

    videoContext->VideoProcessorSetStreamColorSpace(videoProcessor, 0, &inputColorSpace);
    videoContext->VideoProcessorSetOutputColorSpace(videoProcessor, &outputColorSpace);
    const char* colorConversionSuffix =
        outputIsHDR ? "; HDR output bypasses VideoProcessor color conversion via direct P010 plane shaders"
                    : (videoContext1 ? "; ColorSpace1 overrides this per frame" : "");
    DLL_Log("[VideoProcessor] Legacy color-space baseline: Full RGB (0-255) -> %s YCbCr (%s, BT.709)%s",
            outputRange == OutputRangeMode::kFull ? "Full" : "Limited",
            outputRange == OutputRangeMode::kFull ? "0-255" : "16-235", colorConversionSuffix);

    // AVHWFrame textures are the VP output surfaces. They are allocated on
    // demand by libavutil and retained by NVENC for exactly as long as each
    // submitted frame remains in flight.
    const bool use10BitOutput = ShouldUse10BitOutput();
    const DXGI_FORMAT outputFormat = use10BitOutput ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    UINT formatSupport = 0;
    hr = d3d11Device->CheckFormatSupport(outputFormat, &formatSupport);
    if (SUCCEEDED(hr)) {
        DLL_Log("[VideoProcessor] Output fmt=%d formatSupport=0x%x", outputFormat, formatSupport);
    } else {
        DLL_Log("[VideoProcessor] CheckFormatSupport(fmt=%d) failed. HR=%x", outputFormat, hr);
    }
    DLL_Log("[VideoProcessor] Using AVHWFrame-owned %s output textures at %dx%d", use10BitOutput ? "P010" : "NV12",
            outputWidth, outputHeight);

    if (!outputIsHDR) {
        // Desktop Duplication textures may not support direct VP input views.
        // SDR output retains the compatibility staging copy; HDR output writes P010 planes
        // directly and deliberately does not allocate this legacy surface.
        D3D11_TEXTURE2D_DESC bgraDesc = {};
        bgraDesc.Width = inputWidth;
        bgraDesc.Height = inputHeight;
        bgraDesc.MipLevels = 1;
        bgraDesc.ArraySize = 1;
        bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bgraDesc.SampleDesc.Count = 1;
        bgraDesc.Usage = D3D11_USAGE_DEFAULT;
        bgraDesc.BindFlags = 0;

        hr = d3d11Device->CreateTexture2D(&bgraDesc, nullptr, &bgraStagingTexture);

        if (FAILED(hr)) {
            DLL_Log("[VideoProcessor] Failed to create BGRA staging texture. HR=%x", hr);
            return false;
        }
        DLL_Log("[VideoProcessor] Created BGRA staging texture at %dx%d for DD compatibility", inputWidth,
                inputHeight);
    } else {
        DLL_Log("[HDR P010] Legacy BGRA/VideoProcessor staging allocation skipped");
    }

    videoProcessorInit = true;

    if (outputIsHDR) {
        DLL_Log("[HDR P010] Initialized direct shader target for %dx%d -> %dx%d RGB10/PQ -> %s P010", inputWidth,
                inputHeight, outputWidth, outputHeight, DescribeOutputRange(outputRange));
    } else if (scalingEnabled) {
        DLL_Log(
            "[VideoProcessor] Initialized with SCALING: %dx%d -> %dx%d "
            "RGB→%s",
            inputWidth, inputHeight, outputWidth, outputHeight, use10BitOutput ? "P010" : "NV12");
    } else {
        DLL_Log("[VideoProcessor] Initialized for %dx%d RGB→%s (no scaling)", outputWidth, outputHeight,
                use10BitOutput ? "P010" : "NV12");
    }
    return true;
}

VideoEncoder::CursorSourceRestore::~CursorSourceRestore() {
    if (!active || !context || !target || !backup || width == 0 || height == 0) {
        return;
    }
    const D3D11_BOX backupBox = {0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup, 0, &backupBox);
}

void VideoEncoder::CleanupCursorCompositionResources() {
    if (cursorRestoreTexture) {
        cursorRestoreTexture->Release();
        cursorRestoreTexture = nullptr;
    }
    if (cursorCompositeTexture) {
        cursorCompositeTexture->Release();
        cursorCompositeTexture = nullptr;
    }
}

bool VideoEncoder::PrepareVideoProcessorCursorInput(ID3D11Texture2D* source, bool overlayCursor,
                                                    CursorSourceRestore* restore, ID3D11Texture2D** preparedSource) {
    if (!source || !restore || !preparedSource || !d3d11Device || !d3d11Context) {
        return false;
    }
    *preparedSource = source;
    if (!overlayCursor || !cursorRenderer || !cursorCaptureState.IsVisible()) {
        return true;
    }

    if (!cursorRenderer->Init(d3d11Device, d3d11Context)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Failed to initialize RGB cursor renderer; video conversion continues without this "
                "separate cursor draw");
        }
        return true;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    RECT cursorRect = {};
    if (!cursorRenderer->GetCursorFrameRect(static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height),
                                            cursorCaptureState, &cursorRect)) {
        if (cursorPrecompositionFailureLogs++ < 5) {

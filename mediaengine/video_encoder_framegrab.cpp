#include "video_encoder_internal.h"

// EncodeFrameD3D11: Direct D3D11 texture encoding for framegrab
// mode Zero-copy path - texture is converted RGB/BGRA -> NV12/P010 directly on GPU
bool VideoEncoder::PrepareFrameD3D11(ID3D11Texture2D* bgraTexture, uint32_t frameWidth, uint32_t frameHeight,
                                     bool isHDR) {
    if (!recordingRequested)
        return false;

    ReleaseInjectDeviceStateForScreenGrab();


    // Use captured frame dimensions if not yet set
    if (width == 0 || height == 0) {
        width = (int)frameWidth;
        height = (int)frameHeight;
        DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
    }

    // Ensure D3D11 device is available (we need it for Video
    // Processor)
    if (!d3d11Device || !d3d11Context) {
        if (!AdoptTextureDevice(bgraTexture)) {
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    bgraTexture->GetDesc(&texDesc);
    const bool wants10BitInput = ce::video_format::IsHighPrecisionRgbInputFormat(texDesc.Format);
    if (!initDone || isHDR != currentIsHDR || wants10BitInput != currentUse10BitInput) {
        const bool reinitializingActiveRecording = initDone;
        const std::string preservedOutputFilename = outputFilename;
        auto preservedOutputReservation = std::move(outputReservation);
        if (reinitializingActiveRecording) {
            DLL_Log("[VideoEncoder] WGC mode changed (fmt=%d hdr=%d->%d use10bit=%d->%d). Re-initializing...",
                    texDesc.Format, currentIsHDR, isHDR, currentUse10BitInput, wants10BitInput);
            Stop();
            initDone = false;
            codecOpenFailed = false;
        }

        currentIsHDR = isHDR;
        currentUse10BitInput = wants10BitInput;
        use10BitPipeline = ShouldUse10BitOutput();
        if (!Init(savedConfig, width ? width : (int)frameWidth, height ? height : (int)frameHeight,
                  savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for WGC format mode change");
            return false;
        }
        if (!AdoptTextureDevice(bgraTexture)) {
            DLL_Log("[VideoEncoder] Failed to adopt WGC texture device after format mode change");
            return false;
        }
        if (reinitializingActiveRecording) {
            if (!preservedOutputFilename.empty()) {
                outputReservation = std::move(preservedOutputReservation);
                outputFilename = preservedOutputFilename;
                DLL_Log("[VideoEncoder] Preserving output filename across WGC mode re-init: %s",
                        OutputTargetForLog().c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first WGC frame: %s",
                    OutputTargetForLog().c_str());
            BeginDeferredRecording();
        }
    }

    // Ensure encoder is initialized with hardware context
    if (!EnsureDevice())
        return false;

    return OpenOutputAndWriteHeader();
}

bool VideoEncoder::EncodeFrameD3D11(ID3D11Texture2D* bgraTexture, int64_t pts, uint32_t frameWidth,
                                    uint32_t frameHeight, bool isHDR, int32_t captureLeft, int32_t captureTop,
                                    bool useExplicitCfrTimeline) {
    if (!recordingRequested)
        return false;

    inputFrameCount++;

    if (!PrepareFrameD3D11(bgraTexture, frameWidth, frameHeight, isHDR)) {
        return false;
    }

    // Detect new recording start and reset counters (using class members)
    if (startPts < 0) {
        needsCounterReset = true;
    }

    // Reset counters on new recording
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log(
            "[VideoEncoder] ScreenGrab: Reset encodeFrameCounter for new "
            "recording");
    }

    encodeFrameCounter++;

    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(pts);

    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (video_encoder_g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(pts - video_encoder_g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;

    // Log frame stats periodically (about once per second at the configured FPS)
    if (encodeFrameCounter - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && pts > startPts) {
            double elapsedSec = static_cast<double>(pts - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? ((double)encodeFrameCounter / elapsedSec) : 0.0;
            DLL_Log("[FPS] Framegrab: %.1f frames, %.1f fps over %.1fs", (double)encodeFrameCounter, outputFps,
                    elapsedSec);
        }
        lastLogFrameCount = encodeFrameCounter;
    }

    // Performance timing
    auto frameStart = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    if (!useDirectRgbPath && captureCursor && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            return false;
        }
    }

    const bool recomposeOverlaysForRepeats = DynamicOverlayRecompositionActive();
    if (!recomposeOverlaysForRepeats && repeatSourceNeedsOverlayRecompose) {
        InvalidateRepeatSourceFrameTexture();
    }

    auto beforeConvert = PerfTimer::now();
    AVFrame* d3d11Frame = av_frame_alloc();
    if (!d3d11Frame) {
        return false;
    }
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = codecCtx->width;
    d3d11Frame->height = codecCtx->height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] Frame %d: Failed to allocate D3D11 HW frame for direct RGB path: %d",
                    encodeFrameCounter, frameRet);
            av_frame_free(&d3d11Frame);
            return false;
        }

        if (!PrepareD3D11TextureForEncode(bgraTexture, (ID3D11Texture2D*)d3d11Frame->data[0], CursorCompositionActive(),
                                          captureLeft, captureTop, true, 1)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // WGC/DXGI/inject keep a hardware cursor separate whenever Windows
        // permits it. Point-composite that cursor into RGB before the single
        // VP conversion so its filtering matches a Windows-embedded cursor.
        // Scoped Lock for D3D11 Immediate Context (protects Blt/CopyResource)
        bool convertSuccess =
            ConvertBGRAtoNV12(bgraTexture, d3d11Frame, CursorCompositionActive(), true, captureLeft, captureTop, 1);

        if (!convertSuccess) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    auto afterConvert = PerfTimer::now();
    double convertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    if (!ApplyFrameColorMetadata(d3d11Frame, codecCtx, savedConfig.hdrNominalPeakNits)) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    // Calculate PTS — pts is in microseconds
    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? pts : startPts.load(std::memory_order_relaxed);
    const int64_t targetPts = ComputeTargetVideoPts(pts, savedConfig.useVFR, savedConfig.fps, effectiveStartPts,
                                                    lastAssignedVideoPts, useExplicitCfrTimeline);

    // Encode
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    auto drainPackets = [&]() {
        while (true) {
            int ret = avcodec_receive_packet(codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            packetCount++;
            pkt->stream_index = stream->index;

            // Debug: Log output packet PTS
            if (encodeFrameCounter < 30 || encodeFrameCounter % 1000 == 0) {
                DLL_Log(
                    "[Framegrab DEBUG] Received pkt: pts=%lld dts=%lld "
                    "size=%d "
                    "flags=%d",
                    pkt->pts, pkt->dts, pkt->size, pkt->flags);
            }

            // Set packet duration based on VFR/CFR mode (matches inject-mode logic)
            if (savedConfig.useVFR) {
                // VFR: time_base is 1/1000000, so duration is 1 frame in microseconds
                pkt->duration = 1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60);
            } else {
                // CFR: time_base is 1/fps, duration is 1 unit
                pkt->duration = 1;
            }

            if (onPacket)
                onPacket(pkt);
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] ScreenGrab send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] ScreenGrab send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    bool success = true;

    d3d11Frame->pts = targetPts;
    AVFrame* encoderInputFrame = PrepareEncoderInputFrame(d3d11Frame);

    // Debug: Log input frame PTS
    if (encodeFrameCounter < 20 || encodeFrameCounter % 1000 == 0) {
        DLL_Log("[Framegrab DEBUG] Sending frame %d with input PTS=%lld", encodeFrameCounter, d3d11Frame->pts);
    }

    if (encoderInputFrame) {
        success = sendFrame(encoderInputFrame);
        if (encoderInputFrame != d3d11Frame) {
            av_frame_free(&encoderInputFrame);
        }
        if (success) {
            lastAssignedVideoPts = d3d11Frame->pts;
            outputFrameCount++;
            const bool cachedDynamicOverlaySource =
                recomposeOverlaysForRepeats && CacheRepeatSourceFrameTexture(
                                                   bgraTexture, frameWidth, frameHeight, isHDR, captureLeft, captureTop);
            if (recomposeOverlaysForRepeats && !cachedDynamicOverlaySource) {
                repeatSourceNeedsOverlayRecompose = false;
            }
            // Keep one full-frame cache copy per accepted frame. Dynamic
            // overlays retain the uncomposited RGB source so camera/cursor
            // state can advance on CFR repeats; the ordinary path retains the
            // already converted frame.
            // Dynamic composition keeps the larger RGB source. Seed one
            // converted fallback, then normally refresh it only when a repeat
            // is rendered. Overload degradation refreshes it from each fresh
            // accepted frame so a held slot can never jump to old content.
            if (!cachedDynamicOverlaySource || !repeatFrameTexture)
                CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
        }
    } else {
        success = false;
    }

    if (!success) {
        av_packet_free(&pkt);
        av_frame_free(&d3d11Frame);
        return false;
    }

    if (commitsStartPts) {
        startPts.store(effectiveStartPts, std::memory_order_relaxed);
        DLL_Log("[VideoEncoder] Framegrab recording started at PTS %lld us", static_cast<long long>(effectiveStartPts));
    }
    video_encoder_g_lastFramePts = pts;
    auto afterEncode = PerfTimer::now();
    double encodeMs = PerfTimer::elapsed_ms(afterConvert, afterEncode);
    double totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    lastEncodeTimeUs = static_cast<int64_t>(PerfTimer::elapsed_ms(beforeConvert, afterEncode) * 1000.0);
    lastFenceWaitUs = 0;

    av_packet_free(&pkt);

    // Log only severe slow frames individually. The per-second summary below captures
    // steady-state encode timing without flooding the log with routine single-frame spikes.
    if (totalMs > expectedFrameMs * 2.0) {
        std::string features = "";
        if (IsConfiguredNvencLookaheadActive(savedConfig.lookahead))
            features += "Lookahead ";
        if (savedConfig.spatialAq || savedConfig.temporalAq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (IsConfiguredNvencMultipassActive(savedConfig))
            features += "Multipass ";

        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms (%s) convert=%.2f "
            "encode=%.2f packets=%d [Features: %s] timing=cpu-wall-or-submit",
            encodeFrameCounter, totalMs, "SLOW!", convertMs, encodeMs, packetCount, features.c_str());
    }

    // Log periodic stats (about once per second at the configured FPS)
    if (encodeFrameCounter % fpsLogIntervalFrames == 0) {
        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms convert=%.2f "
            "encode=%.2f packets=%d skipped=%lld duplicated=%lld timing=cpu-wall-or-submit",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount, skippedFrameCount, duplicatedFrameCount);
        if (stats.actualPtsDiff > 0) {
            const double jitter = static_cast<double>(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log("[Framegrab SMOOTHNESS] Expected=%0.2fms Actual=%0.2fms Jitter=%0.2fms",
                    static_cast<double>(stats.expectedPtsDiff), static_cast<double>(stats.actualPtsDiff), jitter);
        }
    }

    ObserveFreshFrameDynamicOverlayPressure(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    av_frame_free(&d3d11Frame);
    return true;
}

void VideoEncoder::ObserveFreshFrameDynamicOverlayPressure(ID3D11Texture2D* acceptedConvertedFrame) {
    const double freshServiceMs = static_cast<double>(std::max<int64_t>(lastEncodeTimeUs, 0)) / 1000.0;
    const double intervalMs = 1000.0 / static_cast<double>(std::max(savedConfig.fps, 1));
    const auto decision = ce::capture_policy::UpdateCfrDynamicOverlayRepeatState(
        dynamicOverlayRepeatState, DynamicOverlayRecompositionActive(), freshServiceMs, intervalMs);
    if (decision.frozen &&
        (!acceptedConvertedFrame || !CacheRepeatFrameTexture(acceptedConvertedFrame))) {
        // An old fallback is not a safe degradation surface: replaying it after
        // this accepted fresh frame would itself create a temporal regression.
        if (decision.entered && dynamicOverlayRepeatState.episodes > 0) {
            --dynamicOverlayRepeatState.episodes;
        }
        dynamicOverlayRepeatState.frozen = false;
        dynamicOverlayRepeatState.overloadConfirmFrames = 0;
        dynamicOverlayRepeatState.recoveryConfirmFrames = 0;
        return;
    }
    if (decision.entered) {
        DLL_Log(
            "[VideoEncoder] Dynamic-overlay repeat degradation entered: fresh=%.2fms interval=%.2fms "
            "episodes=%llu (held CFR slots reuse fully composited pixels)",
            freshServiceMs, intervalMs,
            static_cast<unsigned long long>(dynamicOverlayRepeatState.episodes));
    } else if (decision.exited) {
        DLL_Log(
            "[VideoEncoder] Dynamic-overlay repeat degradation exited: fresh=%.2fms interval=%.2fms "
            "frozenRepeats=%llu",
            freshServiceMs, intervalMs,
            static_cast<unsigned long long>(dynamicOverlayRepeatState.frozenRepeats));
    }
}

bool VideoEncoder::RepeatLastFrame(int64_t timestamp, bool useExplicitCfrTimeline) {
    if (!recordingRequested) {
        return false;
    }

    lastFrameDeferred.store(false, std::memory_order_relaxed);

    const bool dynamicRepeatSourceAvailable = repeatSourceNeedsOverlayRecompose && repeatSourceFrameTexture;
    const bool freezeDynamicOverlaysForRepeat =
        dynamicOverlayRepeatState.frozen && dynamicRepeatSourceAvailable && repeatFrameTexture;
    const bool recomposeOverlaysForRepeat = dynamicRepeatSourceAvailable && !freezeDynamicOverlaysForRepeat;
    if (!repeatFrameTexture && !recomposeOverlaysForRepeat) {
        DLL_Log("[VideoEncoder] RepeatLastFrame requested without cached frame");
        return false;
    }

    if (!d3d11Device || !d3d11Context || !EnsureDevice()) {
        return false;
    }

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] RepeatLastFrame requested before output file was opened");
        return false;
    }

    inputFrameCount++;

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;
    if (startPts < 0) {
        startPts = timestamp;
        needsCounterReset = true;
    }
    if (outputFrameCount - lastLogFrameCount >= fpsLogIntervalFrames) {
        if (startPts >= 0 && timestamp > startPts) {
            double elapsedSec = static_cast<double>(timestamp - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? (static_cast<double>(outputFrameCount) / elapsedSec) : 0.0;
            DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs", static_cast<double>(outputFrameCount), outputFps,
                    elapsedSec);
        }
        lastLogFrameCount = outputFrameCount;
    }
    if (needsCounterReset) {
        encodeFrameCounter = 0;
        lastLogFrameCount = 0;
        needsCounterReset = false;
        DLL_Log("[VideoEncoder] Reset frameCounter for repeated-frame path");
    }

    encodeFrameCounter++;
    duplicatedFrameCount++;

    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(timestamp);
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (video_encoder_g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - video_encoder_g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    // Re-encode the cached texture. Encoded packets are reference-dependent
    // bitstream units and cannot be replayed safely with rewritten timestamps.
    auto frameStart = PerfTimer::now();

    auto allocateD3D11RepeatFrame = [&]() -> AVFrame* {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return nullptr;
        }
        frame->format = AV_PIX_FMT_D3D11;
        frame->width = codecCtx->width;
        frame->height = codecCtx->height;
        frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
        return frame;
    };

    AVFrame* d3d11Frame = allocateD3D11RepeatFrame();
    if (!d3d11Frame) {
        return false;
    }

    auto beforeConvert = PerfTimer::now();
    bool populatedFromRepeatSource = false;
    if (recomposeOverlaysForRepeat) {
        populatedFromRepeatSource = PopulateD3D11FrameFromRepeatSource(d3d11Frame);
        if (!populatedFromRepeatSource) {
            if (!repeatOverlayRecomposeFallbackLogged) {
                DLL_Log("[VideoEncoder] Dynamic-overlay repeat recompose failed; falling back to cached frame");
                repeatOverlayRecomposeFallbackLogged = true;
            }
            av_frame_free(&d3d11Frame);
            if (!repeatFrameTexture) {
                return false;
            }
            d3d11Frame = allocateD3D11RepeatFrame();
            if (!d3d11Frame) {
                return false;
            }
        }
    }

    if (!populatedFromRepeatSource) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] RepeatLastFrame failed to allocate D3D11 HW frame: %d", frameRet);
            av_frame_free(&d3d11Frame);
            return false;
        }

        {
            D3D11ScopedLock lock;
            d3d11Context->CopyResource(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]), repeatFrameTexture);
        }
    }

    auto afterConvert = PerfTimer::now();
    if (!ApplyFrameColorMetadata(d3d11Frame, codecCtx, savedConfig.hdrNominalPeakNits)) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps, startPts,
                                                    lastAssignedVideoPts, useExplicitCfrTimeline);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    int packetCount = 0;
    auto drainPackets = [&]() {
        while (true) {
            int ret = avcodec_receive_packet(codecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] RepeatLastFrame receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            pkt->stream_index = stream->index;
            pkt->duration = savedConfig.useVFR ? (1000000 / std::max(savedConfig.fps, 1)) : 1;
            if (onPacket) {
                onPacket(pkt);
            }
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] RepeatLastFrame send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] RepeatLastFrame send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    d3d11Frame->pts = targetPts;
    AVFrame* encoderInputFrame = PrepareEncoderInputFrame(d3d11Frame);

    auto encodeStart = PerfTimer::now();
    const bool success = encoderInputFrame && sendFrame(encoderInputFrame);
    auto afterEncode = PerfTimer::now();
    if (encoderInputFrame != d3d11Frame) {
        av_frame_free(&encoderInputFrame);
    }

    if (!success) {
        av_packet_free(&pkt);
        av_frame_free(&d3d11Frame);
        return false;
    }

    stats.textureOpenMs = 0.0;
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);
    stats.packetsProduced = packetCount;

    lastEncodeTimeUs = static_cast<int64_t>(PerfTimer::elapsed_ms(beforeConvert, afterEncode) * 1000.0);
    lastFenceWaitUs = 0;

    video_encoder_g_lastFramePts = timestamp;
    lastAssignedVideoPts = d3d11Frame->pts;

    video_encoder_g_framesEncoded++;
    outputFrameCount++;
    if (populatedFromRepeatSource) {
        overlayAwareRepeatRenderCount++;
        CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    } else if (freezeDynamicOverlaysForRepeat) {
        ++dynamicOverlayRepeatState.frozenRepeats;
    }
    if (!DynamicOverlayRecompositionActive()) {
        InvalidateRepeatSourceFrameTexture();
        dynamicOverlayRepeatState.frozen = false;
        dynamicOverlayRepeatState.overloadConfirmFrames = 0;
        dynamicOverlayRepeatState.recoveryConfirmFrames = 0;
    }
    video_encoder_g_totalFenceWait += stats.fenceWaitMs;
    video_encoder_g_totalColorConvert += stats.colorConvertMs;
    video_encoder_g_totalEncode += stats.encodeMs;
    if (stats.totalMs > video_encoder_g_maxFrameTime) {
        video_encoder_g_maxFrameTime = stats.totalMs;
    }
    if (stats.totalMs > expectedFrameMs * 2.0) {
        video_encoder_g_slowFrameCount++;
    }

    av_packet_free(&pkt);
    av_frame_free(&d3d11Frame);
    return true;
}

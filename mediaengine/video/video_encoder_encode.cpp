#include "video_encoder_internal.h"
#include "frame_fence_policy.h"
#include "common/logging/log_meter.h"

#include <utility>

#include "encode_geometry_policy.h"

bool VideoEncoder::EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, int64_t timestamp,
                               uint32_t sourcePid, int frameWidth, int frameHeight, int format, bool isHDR, bool isShmem,
                               int shmemSlot) {
    lastFrameDeferred.store(false, std::memory_order_relaxed);
    if (removedFrameTransport.Matches(reinterpret_cast<uintptr_t>(fenceHandle), sourcePid, cachedTransportGeneration))
        return false;
    if (!recordingRequested)
        return false;

    // Debug: Log every 60th frame entry to verify loop
    if (encodeFrameCounter % 60 == 0) {
        DLL_Log("[VideoEncoder] EncodeFrame Entry: PID=%u Handle=%p FenceVal=%llu", sourcePid, sharedHandle,
                fenceValue);
    }

    const bool wants10BitInput =
        isHDR || ce::video_format::IsHighPrecisionRgbInputFormat(static_cast<DXGI_FORMAT>(format));
    const bool contractChanged =
        initDone && (isHDR != currentIsHDR || wants10BitInput != currentUse10BitInput);
    if (ce::encode_geometry::ClassifySourceChange(fileOpened, contractChanged, static_cast<uint32_t>(frameWidth),
                                                  static_cast<uint32_t>(frameHeight), lockedGeometryWidth,
                                                  lockedGeometryHeight) ==
        ce::encode_geometry::SourceChangeAction::kFinalizeAndStop) {
        RequestStopForSourceContractChange("inject source", isHDR, wants10BitInput);
        return false;
    }
    if (!ReinitForFormatModeChange(isHDR, wants10BitInput, frameWidth, frameHeight))
        return false;

    if (!HandleResolutionChange(frameWidth, frameHeight))
        return false;

    if (!EnsureDevice())
        return false;

    if (!OpenOutputAndWriteHeader())
        return false;

    const int fpsLogIntervalFrames = (savedConfig.fps > 0) ? savedConfig.fps : 60;
    LogFrameRateStats(timestamp, fpsLogIntervalFrames);


    // Performance timing for this frame
    FrameStats stats;
    stats.frameNumber = encodeFrameCounter;
    stats.ptsMs = RoundUsToMs(timestamp);

    // Calculate frame timing for smoothness analysis
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (video_encoder_g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - video_encoder_g_lastFramePts);
        stats.expectedPtsDiff = RoundUsToMs(static_cast<int64_t>(expectedFrameMs * 1000.0));
    }

    auto frameStart = PerfTimer::now();

    ID3D11Texture2D* bgraTex = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;

    if (!ResolveFrameInput(sharedHandle, fenceHandle, fenceValue, sourcePid, format, isShmem, shmemSlot,
                           &bgraTex, &d3d11Fence))
        return false;

    std::chrono::high_resolution_clock::time_point afterFence;
    if (!WaitForFrameFence(d3d11Fence, fenceValue, bgraTex, stats, afterFence, fenceHandle, sourcePid))
        return false;
    ce::ComGuard<ID3D11Texture2D> bgraTextureGuard(bgraTex);
    if (!isShmem && !PrepareSourceReadCompletion())
        return false;
    {
        D3D11ScopedLock lock;
        gpuStageMonitor.BeginStage(ce::media::gpu_stage::Path::Fresh, d3d11Device, d3d11Context);
    }
    // Every exit from here on may have queued GPU reads of the producer's slot
    // (fit, conversion, repeat-source staging); mark their completion on all of
    // them so the caller never returns the slot ahead of the reads. SHMEM input
    // was already copied out of the producer's buffer on the CPU. The success
    // path marks it right after the last read, ahead of the encoder submit, so
    // the marker never sits behind NVENC work on the device timeline.
    struct SourceReadCompletionGuard {
        VideoEncoder* encoder;
        bool active;
        void Complete() {
            if (active)
                encoder->SignalSourceReadCompletion();
            active = false;
        }
        ~SourceReadCompletionGuard() { Complete(); }
    } sourceReadCompletion{this, !isShmem};
    // A source resized after the output opened is fitted into the locked geometry.
    ID3D11Texture2D* fittedSource = nullptr;
    if (!FitSourceToLockedGeometry(bgraTextureGuard.get(), &fittedSource))
        return false;
    if (fittedSource)
        bgraTextureGuard.reset(fittedSource);

    auto afterOpen = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    std::chrono::high_resolution_clock::time_point afterConvert;
    AVFrame* d3d11Frame = nullptr;
    if (!ConvertFrameToYuv(bgraTextureGuard.get(), useDirectRgbPath, &d3d11Frame, stats, afterConvert))
        return false;
    {
        D3D11ScopedLock lock;
        gpuStageMonitor.EndStage(ce::media::gpu_stage::Path::Fresh, d3d11Context);
    }

    // Inject capture historically repeats the already converted frame. Retain
    // that zero-extra-copy default unless the face camera actually needs to
    // advance independently of the game frame.
    const bool recomposeOverlaysForRepeats = FaceCameraCompositionActive();
    const bool stagedDynamicOverlaySource =
        recomposeOverlaysForRepeats && StageRepeatSourceFrameTexture(bgraTextureGuard.get());
    sourceReadCompletion.Complete();
    {
        D3D11ScopedLock lock;
        gpuStageMonitor.MarkSourceRead(ce::media::gpu_stage::Path::Fresh, d3d11Context);
    }

    // The converted frame becomes the repeat cache after the submit; copy it
    // now, while it is still CE's, instead of after NVENC took it.
    RepeatFrameStageScope repeatFrameStage{this};
    if (!stagedDynamicOverlaySource || !repeatFrameTexture)
        StageRepeatFrameTexture(EncoderFrameTexture(d3d11Frame));

    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? timestamp : startPts.load(std::memory_order_relaxed);
    const bool encodeSuccess = SubmitFrameForEncode(d3d11Frame, timestamp, effectiveStartPts, frameStart,
                                                    afterOpen, afterConvert, afterFence, stats);
    if (!encodeSuccess) {
        DiscardStagedRepeatSourceFrameTexture();
        av_frame_free(&d3d11Frame);
        return false;
    }

    if (commitsStartPts) {
        startPts.store(effectiveStartPts, std::memory_order_relaxed);
        DLL_Log("[VideoEncoder] Recording started at PTS %lld us", static_cast<long long>(effectiveStartPts));
    }
    video_encoder_g_lastFramePts = timestamp;
    lastAssignedVideoPts = d3d11Frame->pts;

    // Update global stats
    video_encoder_g_framesEncoded++;
    outputFrameCount++;
    if (stagedDynamicOverlaySource) {
        CommitStagedRepeatSourceFrameTexture(static_cast<uint32_t>(this->width), static_cast<uint32_t>(this->height), isHDR, 0,
                                             0);
        // Seed one converted fallback without adding a second copy to every
        // accepted frame. Successful dynamic repeats normally refresh it;
        // overload degradation refreshes it from each accepted fresh frame.
        if (!repeatFrameTexture)
            CacheRepeatFrameTexture(EncoderFrameTexture(d3d11Frame));
    } else {
        // A stale/failed camera can retire itself during this frame's
        // composition, making recomposeOverlaysForRepeats false. Drop any
        // older RGB repeat source now that the accepted converted frame is a
        // camera-free fallback; otherwise it would linger until a later CFR
        // repeat happened to consume it.
        InvalidateRepeatSourceFrameTexture();
        CacheRepeatFrameTexture(EncoderFrameTexture(d3d11Frame));
    }
    video_encoder_g_totalFenceWait += stats.fenceWaitMs;
    video_encoder_g_totalColorConvert += stats.colorConvertMs;
    video_encoder_g_totalEncode += stats.encodeMs;
    if (stats.totalMs > video_encoder_g_maxFrameTime)
        video_encoder_g_maxFrameTime = stats.totalMs;
    if (stats.totalMs > expectedFrameMs * 2)
        video_encoder_g_slowFrameCount++;


    LogFramePerformance(stats, expectedFrameMs, fpsLogIntervalFrames);
    ObserveFreshFrameDynamicOverlayPressure(EncoderFrameTexture(d3d11Frame));
    {
        D3D11ScopedLock lock;
        gpuStageMonitor.MarkFrameDone(ce::media::gpu_stage::Path::Fresh, d3d11Context);
    }

    av_frame_free(&d3d11Frame);  // Releases D3D11 Tex

    return true;
}

bool VideoEncoder::ReinitForFormatModeChange(bool isHDR, bool wants10BitInput, int newWidth, int newHeight) {
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
        if (!Init(savedConfig, newWidth, newHeight, savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for format mode change");
            return false;
        }
        if (reinitializingActiveRecording) {
            if (!preservedOutputFilename.empty()) {
                outputReservation = std::move(preservedOutputReservation);
                outputFilename = preservedOutputFilename;
                DLL_Log("[VideoEncoder] Preserving output filename across format mode re-init: %s",
                        OutputTargetForLog().c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first frame: %s",
                    OutputTargetForLog().c_str());
            BeginDeferredRecording();
        }
    }
    return true;
}

bool VideoEncoder::HandleResolutionChange(int newWidth, int newHeight) {
    // Use captured frame dimensions if not yet set or changed
    if (this->width != newWidth || this->height != newHeight) {
        if (this->width == 0) {
            DLL_Log("[VideoEncoder] Initial resolution discovered: %dx%d (Input: %dx%d)", newWidth, newHeight, newWidth, newHeight);
        } else if (fileOpened) {
            // The committed output keeps its geometry; FitSourceToLockedGeometry
            // places the resized source into it (logged there per new size).
            return true;
        } else {
            DLL_Log("[VideoEncoder] Resolution CHANGE detected: %dx%d -> %dx%d", this->width, this->height, newWidth,
                    newHeight);
            if (!fileOpened && initDone) {
                // Pre-warm used stale/wrong dimensions. Reset codec and container
                // so EnsureDevice() reinitializes them at the correct resolution
                // before the file header is written.
                DLL_Log("[VideoEncoder] Reinitializing encoder at correct resolution (pre-file-open)");
                CleanupVideoProcessor();
                avcodec_free_context(&codecCtx);
                ReleaseEncoderFramesContexts();
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
        this->width = newWidth;
        this->height = newHeight;
    }
    return true;
}

bool VideoEncoder::OpenOutputAndWriteHeader() {
    if (fileOpened)
        return true;

    DLL_Log("[VideoEncoder] Opening output target: %s", OutputTargetForLog().c_str());
    auto closeRejectedOutput = [this](const char* reason) {
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE) && fmtCtx->pb) {
            ArmOutputIoDeadline();
            const int closeResult = avio_closep(&fmtCtx->pb);
            ClearOutputIoDeadline();
            if (closeResult < 0)
                DLL_Log("[VideoEncoder] ERROR: Failed to close %s output: %d", reason, closeResult);
        }
    };

    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* ioOptions = nullptr;
        // Bound every blocking output operation on both paths (see
        // ArmOutputIoDeadline): this is what turns a hung storage write into a
        // reportable error instead of a writer thread wedged forever.
        fmtCtx->interrupt_callback.callback = InterruptOutputIo;
        fmtCtx->interrupt_callback.opaque = this;
        if (liveOutput) {
            fmtCtx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
            fmtCtx->max_delay = 0;
            fmtCtx->max_interleave_delta = 100000;
            ConfigureLiveMuxTimestampOffset();
            av_dict_set(&ioOptions, "rw_timeout", "5000000", 0);
            av_dict_set(&ioOptions, "tcp_nodelay", "1", 0);
            if (outputFilename.rfind("rtmps://", 0) == 0)
                av_dict_set(&ioOptions, "tls_verify", "1", 0);
        } else if (!outputReservation.ReleaseToWriter()) {
            DLL_Log("[VideoEncoder] ERROR: Reserved output identity changed before mux open: %s",
                    OutputTargetForLog().c_str());
            return false;
        }

        ArmOutputIoDeadline();
        const AVIOInterruptCB* interruptCallback = &fmtCtx->interrupt_callback;
        const int openResult = avio_open2(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE,
                                          interruptCallback, &ioOptions);
        ClearOutputIoDeadline();
        av_dict_free(&ioOptions);
        if (openResult < 0) {
            DLL_Log("[VideoEncoder] Failed to open output target: %d", openResult);
            RequestLiveOutputFailure("open", openResult);
            return false;
        }
    }

    DLL_Log("[VideoEncoder] fmtCtx has %d streams before write_header", fmtCtx->nb_streams);
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream* currentStream = fmtCtx->streams[i];
        AVCodecParameters* parameters = currentStream->codecpar;
        DLL_Log(
            "[VideoEncoder] Stream %d: type=%d codec_id=%d w=%d h=%d extradata=%p extradata_size=%d",
            i, parameters->codec_type, parameters->codec_id, parameters->width, parameters->height,
            parameters->extradata, parameters->extradata_size);
    }

    if (fmtCtx->priv_data) {
        if (liveOutput) {
            av_opt_set(fmtCtx->priv_data, "flvflags", "no_duration_filesize", 0);
        } else {
            // Pre-allocate space for MKV cues at the front of seekable recordings.
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "2000000", 0);
        }
    }
    if (!liveOutput && !ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmtCtx)) {
        DLL_Log("[VideoEncoder] ERROR: Matroska timestamp_precision=1000 is required but unavailable");
        closeRejectedOutput("rejected");
        return false;
    }
    if (!ValidateFormatContextForHeader(fmtCtx)) {
        closeRejectedOutput("invalid");
        return false;
    }

    ArmOutputIoDeadline();
    const int headerResult = avformat_write_header(fmtCtx, nullptr);
    ClearOutputIoDeadline();
    if (headerResult < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(headerResult, error, sizeof(error));
        DLL_Log("[VideoEncoder] Failed to write output header: %d (%s)", headerResult, error);
        RequestLiveOutputFailure("write_header", headerResult);
        closeRejectedOutput("header-failed");
        return false;
    }

    DLL_Log("[VideoEncoder] Stream time_base after write_header: %d/%d (codec: %d/%d)", stream->time_base.num,
            stream->time_base.den, codecCtx->time_base.num, codecCtx->time_base.den);
    if (fmtCtx->pb) {
        ArmOutputIoDeadline();
        avio_flush(fmtCtx->pb);
        ClearOutputIoDeadline();
        if (fmtCtx->pb->error < 0) {
            const int flushError = fmtCtx->pb->error;
            DLL_Log("[VideoEncoder] Failed to flush output header: %d", flushError);
            RequestLiveOutputFailure("flush_header", flushError);
            closeRejectedOutput("flush-failed");
            return false;
        }
    }

    fileOpened = true;
    lockedGeometryWidth = static_cast<uint32_t>(width);
    lockedGeometryHeight = static_cast<uint32_t>(height);
    DLL_Log("[VideoEncoder] Output header accepted target=%s mode=%s geometry=%ux%u", OutputTargetForLog().c_str(),
            liveOutput ? "live" : "recording", lockedGeometryWidth, lockedGeometryHeight);
    return true;
}

void VideoEncoder::LogFrameRateStats(int64_t timestamp, int fpsLogIntervalFrames) {
    inputFrameCount++;


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

}

bool VideoEncoder::WaitForFrameFence(ID3D11Fence*& d3d11Fence, uint64_t fenceValue,
                                     ID3D11Texture2D* bgraTex, FrameStats& stats,
                                     std::chrono::high_resolution_clock::time_point& afterFence,
                                     HANDLE inputFenceHandle, uint32_t sourcePid) {
    // 2. Wait on Synchronization using D3D11 Fence
    // PROTECTED: Immediate Context access
    D3D11ScopedLock lock;

    HRESULT hr = S_OK;

    // D3D11 FENCE PATH (Async GPU sync)
    auto beforeFence = PerfTimer::now();
    if (d3d11Fence) {
        const uint64_t completedValue = d3d11Fence->GetCompletedValue();
        const auto readiness = ce::frame_fence::Classify(completedValue, fenceValue);
        if (readiness == ce::frame_fence::Readiness::Removed) {
            static ce::log_meter::ChangeGate removalLog(1000);
            const HRESULT deviceReason = d3d11Device->GetDeviceRemovedReason();
            const auto log = removalLog.Observe(ce::log_meter::FieldKey(cachedTransportGeneration, deviceReason),
                                                GetTickCount64());
            if (log) {
                DLL_Log("[VideoEncoder] Shared fence removed: generation=%u deviceReason=0x%08X; "
                        "rejecting removed transport without submitting a GPU wait%s", cachedTransportGeneration,
                        static_cast<unsigned>(deviceReason), ce::log_meter::SuppressedNote(log.suppressed).c_str());
            }
            bgraTex->Release();
            d3d11Fence->Release();
            d3d11Fence = nullptr;
            removedFrameTransport.Mark(reinterpret_cast<uintptr_t>(inputFenceHandle), sourcePid, cachedTransportGeneration);
            for (int slot = 0; slot < SHARED_TEXTURE_SLOT_COUNT; ++slot) {
                if (cachedSharedTextures[slot]) cachedSharedTextures[slot]->Release();
                cachedSharedTextures[slot] = nullptr;
                cachedTextureHandles[slot] = nullptr;
            }
            if (cachedD3D11Fence) cachedD3D11Fence->Release();
            cachedD3D11Fence = nullptr;
            cachedFenceHandle = nullptr;
            lastFrameDeferred.store(false, std::memory_order_relaxed);
            return false;
        }
        // Check if fence is already satisfied (avoid SetEvent overhead if possible)
        if (readiness == ce::frame_fence::Readiness::Pending) {
            if (!fenceEvent) {
                fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!fenceEvent) {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                }
            }
            if (SUCCEEDED(hr)) {
                hr = d3d11Fence->SetEventOnCompletion(fenceValue, fenceEvent);
            }
            // Non-blocking fence check: poll with 0ms timeout instead of
            // blocking the encoder thread. At 100% GPU, the fence may take
            // 1-5ms to signal — blocking for 200ms collapses the cadence.
            // If the fence isn't ready, we skip this frame (return false)
            // and the Bresenham produces a duplicate. Stutter > corruption.
            DWORD waitRes = SUCCEEDED(hr) ? WaitForSingleObject(fenceEvent, 0) : WAIT_FAILED;
            if (waitRes == WAIT_FAILED && SUCCEEDED(hr)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
            }
            if (waitRes == WAIT_TIMEOUT) {
                // Fence not ready — skip this frame, encoder thread stays responsive.
                // A completed value that never advances means media waits on a
                // fence the producer no longer signals (e.g. a stale handle slot).
                static uint64_t s_lastFenceDeferLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastFenceDeferLogTick >= 1000) {
                    s_lastFenceDeferLogTick = nowTick;
                    DLL_Log(
                        "[VideoEncoder] Frame %d: fence not ready (value=%llu completed=%llu generation=%u); "
                        "deferring frame",
                        encodeFrameCounter, static_cast<unsigned long long>(fenceValue),
                        static_cast<unsigned long long>(d3d11Fence->GetCompletedValue()), cachedTransportGeneration);
                }
                bgraTex->Release();
                d3d11Fence->Release();
                d3d11Fence = nullptr;
                lastFrameDeferred.store(true, std::memory_order_relaxed);
                return false;
            }
        }
        // Past this point the CPU has seen the producer's value complete, so
        // the slot's writes are finished and no GPU Wait is queued. A satisfied
        // Wait still split CE's context at a scheduler sync point on every
        // fresh frame, an operation dxgi_dup never issues (session
        // 20261015_223320: inject p4 blocked with NVENC 84% busy, dxgi_dup p6
        // sustained at 96-100%).
    }
    afterFence = PerfTimer::now();
    stats.fenceWaitMs = PerfTimer::elapsed_ms(beforeFence, afterFence);

    if (d3d11Fence) {
        d3d11Fence->Release();
        d3d11Fence = nullptr;
    }

    if (FAILED(hr)) {
        static ce::log_meter::ChangeGate waitFailureLog(1000);
        const auto log = waitFailureLog.Observe(ce::log_meter::FieldKey(hr, cachedTransportGeneration),
                                                GetTickCount64());
        if (log) {
            DLL_Log("[VideoEncoder] Shared fence completion check failed HR=%x generation=%u%s", hr,
                    cachedTransportGeneration, ce::log_meter::SuppressedNote(log.suppressed).c_str());
        }
        bgraTex->Release();
        return false;
    }

    return true;
}

bool VideoEncoder::ConvertFrameToYuv(ID3D11Texture2D* bgraTex, bool useDirectRgbPath, AVFrame** outFrame,
                                     FrameStats& stats, std::chrono::high_resolution_clock::time_point& afterConvert) {
    AVFrame* d3d11Frame = nullptr;

    // 4. Ensure Video Processor is initialized before RGB -> YUV conversion.
    if (!useDirectRgbPath && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            return false;
        }
    }

    auto beforeConvert = PerfTimer::now();
    d3d11Frame = av_frame_alloc();
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

        if (!PrepareD3D11TextureForEncode(bgraTex, EncoderFrameTexture(d3d11Frame), CursorCompositionActive(), 0,
                                          0)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // 6. Point-composite a separate cursor in RGB, then convert the one
        // deterministic RGB stream to NV12/P010 on the GPU.
        if (!ConvertBGRAtoNV12(bgraTex, d3d11Frame, CursorCompositionActive(), true)) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    afterConvert = PerfTimer::now();
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    if (!ApplyFrameColorMetadata(d3d11Frame, codecCtx, savedConfig.hdrNominalPeakNits)) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    // Calculate relative PTS (start from 0) — timestamp is in microseconds

    *outFrame = d3d11Frame;
    return true;
}

bool VideoEncoder::SubmitFrameForEncode(AVFrame* d3d11Frame, int64_t timestamp, int64_t effectiveStartPts,
                                         std::chrono::high_resolution_clock::time_point frameStart, std::chrono::high_resolution_clock::time_point afterOpen,
                                         std::chrono::high_resolution_clock::time_point afterConvert,
                                         std::chrono::high_resolution_clock::time_point afterFence, FrameStats& stats) {
    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps,
                                                    effectiveStartPts, lastAssignedVideoPts, false);


    // 5. Encode (Direct D3D11 Frame) - with proper packet draining
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    // Pure encoding time measurement (excluding color conversion/wait)
    auto encodeStart = PerfTimer::now();

    bool success = true;

    d3d11Frame->pts = targetPts;
    AVFrame* encoderInputFrame = PrepareEncoderInputFrame(d3d11Frame);

    if (encoderInputFrame) {
        success = SendFrameToEncoder(encoderInputFrame, ce::media::gpu_stage::Path::Fresh, "Inject", pkt,
                                     &packetCount);
    } else {
        success = false;
    }
    if (encoderInputFrame != d3d11Frame) {
        av_frame_free(&encoderInputFrame);
    }

    auto afterEncode = PerfTimer::now();
    stats.ptsMs = RoundUsToMs(timestamp);
    stats.textureOpenMs = PerfTimer::elapsed_ms(frameStart, afterOpen);
    stats.colorConvertMs = PerfTimer::elapsed_ms(afterOpen, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    // Update last frame encode time (in microseconds)
    // This is robust against timer noise/underflow compared to (Total - Wait).
    lastEncodeTimeUs = (int64_t)(PerfTimer::elapsed_ms(afterFence, afterEncode) * 1000.0);
    lastFenceWaitUs = (int64_t)(stats.fenceWaitMs * 1000.0);
    stats.packetsProduced = packetCount;

    av_packet_free(&pkt);

    return success;
}

void VideoEncoder::LogFramePerformance(const FrameStats& stats, double expectedFrameMs,
                                       int fpsLogIntervalFrames) {
    // Log individual slow frames for debugging
    // Log more frequently for performance tuning (every 30 frames)
    if (stats.totalMs > expectedFrameMs * 2 || encodeFrameCounter <= 5 || encodeFrameCounter % 30 == 0) {
        std::string features = "";
        if (IsConfiguredNvencLookaheadActive(savedConfig.lookahead))
            features += "Lookahead ";
        if (savedConfig.spatialAq || savedConfig.temporalAq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (IsConfiguredNvencMultipassActive(savedConfig))
            features += "Multipass ";

        const char* slowLabel = (stats.totalMs > expectedFrameMs * 2) ? "(SLOW!)" : "";

        DLL_Log(
            "[PERF] Frame %d: TOTAL=%.2fms %s fence=%.2f convert=%.2f "
            "encode=%.2f pts=%lldms packets=%d [Features: %s] timing=cpu-wall-or-submit",
            encodeFrameCounter, stats.totalMs, slowLabel, stats.fenceWaitMs, stats.colorConvertMs, stats.encodeMs,
            stats.ptsMs, stats.packetsProduced, features.c_str());
    }

    // Periodic performance summary (about once per second at the configured FPS)
    if (encodeFrameCounter % fpsLogIntervalFrames == 0) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double avgFence = video_encoder_g_totalFenceWait / video_encoder_g_framesEncoded;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double avgConvert = video_encoder_g_totalColorConvert / video_encoder_g_framesEncoded;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double avgEncode = video_encoder_g_totalEncode / video_encoder_g_framesEncoded;
        double avgTotal = avgFence + avgConvert + avgEncode;

        // Identify bottleneck
        const char* bottleneck = "ENCODE";
        double maxTime = avgEncode;
        if (avgFence > maxTime) {
            bottleneck = "FENCE_WAIT";
            maxTime = avgFence;
        }
        if (avgConvert > maxTime) {
            bottleneck = "COLOR_CONV";
            maxTime = avgConvert;
        }

        DLL_Log(
            "[PERF SUMMARY] Frames=%lld Avg: total=%.2fms fence=%.2f "
            "convert=%.2f "
            "encode=%.2f | Max=%.2fms SlowFrames=%d | Bottleneck=%s | timing=cpu-wall-or-submit",
            video_encoder_g_framesEncoded, avgTotal, avgFence, avgConvert, avgEncode, video_encoder_g_maxFrameTime, video_encoder_g_slowFrameCount, bottleneck);

        // Frame timing analysis for smoothness
        if (stats.actualPtsDiff > 0) {
            const double jitter = static_cast<double>(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log("[SMOOTHNESS] Expected=%0.2fms Actual=%0.2fms Jitter=%0.2fms",
                    static_cast<double>(stats.expectedPtsDiff), static_cast<double>(stats.actualPtsDiff), jitter);
        }
    }
}

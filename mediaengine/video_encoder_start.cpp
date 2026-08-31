#include "video_encoder_internal.h"

void VideoEncoder::ApplyGpuThreadPriority(int priority, const char* reason) {
    if (!d3d11Device) {
        return;
    }

    priority = std::clamp(priority, -7, 7);
    if (priority == currentGpuThreadPriority && reason && std::strcmp(reason, "initial") != 0) {
        return;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(d3d11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) && dxgiDevice) {
        HRESULT phr = dxgiDevice->SetGPUThreadPriority(priority);
        if (SUCCEEDED(phr)) {
            INT actual = 0;
            const HRESULT readbackHr = dxgiDevice->GetGPUThreadPriority(&actual);
            if (SUCCEEDED(readbackHr) && actual == priority) {
                currentGpuThreadPriority = priority;
                DLL_Log("[VideoEncoder] GPU Thread Priority requested=%d actual=%d verified=1 (%s)", priority, actual,
                        reason ? reason : "update");
            } else {
                DLL_Log(
                    "[VideoEncoder] GPU Thread Priority readback mismatch requested=%d actual=%d setHr=%x "
                    "readbackHr=%x verified=0 (%s)",
                    priority, actual, phr, readbackHr, reason ? reason : "update");
            }
        } else {
            DLL_Log("[VideoEncoder] Failed to set GPU Thread Priority %d (%s): HR=%x", priority,
                    reason ? reason : "update", phr);
        }
        dxgiDevice->Release();
    }
}

void VideoEncoder::UpdateAdaptiveGpuThreadPriority(uint64_t nowMs, double encodeMs, bool encoderPressureActive) {
    if (gpuPriority != 0 || savedConfig.fps <= 0 || !d3d11Device) {
        return;
    }

    const double frameIntervalMs = 1000.0 / static_cast<double>(savedConfig.fps);
    if (ce::capture_policy::IsAdaptiveEncoderGpuPriorityPressureActive(encodeMs, frameIntervalMs,
                                                                       encoderPressureActive)) {
        if (gpuPriorityPressureSinceMs == 0) {
            gpuPriorityPressureSinceMs = nowMs;
            DLL_Log("[VideoEncoder] Adaptive GPU priority pressure started: encode=%.2fms budget=%.2fms flag=%d",
                    encodeMs, frameIntervalMs, encoderPressureActive ? 1 : 0);
        }
        gpuPriorityHealthySinceMs = 0;
        if (currentGpuThreadPriority < 1 && nowMs - gpuPriorityPressureSinceMs >= 2000) {
            ApplyGpuThreadPriority(1, "adaptive encoder pressure");
        }
        return;
    }


    if (ce::capture_policy::ShouldResetAdaptiveEncoderGpuPriorityPressure(encodeMs, frameIntervalMs,
                                                                          encoderPressureActive)) {
        gpuPriorityPressureSinceMs = 0;
        if (gpuPriorityHealthySinceMs == 0) {
            gpuPriorityHealthySinceMs = nowMs;
        }
        if (currentGpuThreadPriority != 0 && nowMs - gpuPriorityHealthySinceMs >= 5000) {
            ApplyGpuThreadPriority(0, "adaptive encoder recovered");
        }
    } else {
        gpuPriorityHealthySinceMs = 0;
    }
}

int VideoEncoder::AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    if (!fmtCtx)
        return -1;

    const AVCodec* codec = nullptr;
    if (audioCtx) {
        codec = audioCtx->codec;
    } else {
        std::string codecName = config.codec.empty() ? "aac" : config.codec;
        std::transform(codecName.begin(), codecName.end(), codecName.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (codecName == "pcm") {
            codecName = config.bitDepth == "16" ? "pcm_s16le" : (config.bitDepth == "32" ? "pcm_f32le" : "pcm_s24le");
        } else if (codecName == "opus") {
            codecName = "libopus";
        }
        codec = avcodec_find_encoder_by_name(codecName.c_str());
    }

    if (!codec)
        return -1;
    AVStream* st = avformat_new_stream(fmtCtx, codec);
    if (!st)
        return -1;

    if (audioCtx) {
        // Correct way: copy parameters including extradata
        avcodec_parameters_from_context(st->codecpar, audioCtx);
        int sampleRate = audioCtx->sample_rate > 0 ? audioCtx->sample_rate : st->codecpar->sample_rate;
        if (sampleRate <= 0) {
            sampleRate = 48000;
        }
        st->time_base = {1, sampleRate};
    } else {
        // Fallback (might fail for extradata-dependent codecs). ParseSampleRateOr
        // never throws on a malformed config sample_rate (unlike std::stoi).
        int sampleRate = ce::audio::ParseSampleRateOr(config.sampleRate, 48000);
        st->time_base = {1, sampleRate};
        st->codecpar->codec_id = codec->id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate = sampleRate;
        const int channels =
            std::clamp(config.downmix ? 2 : (config.outputChannels > 0 ? config.outputChannels : 2), 1, 8);
        if (config.outputChannelMask != 0 && !config.downmix) {
            av_channel_layout_from_mask(&st->codecpar->ch_layout, config.outputChannelMask);
        } else {
            av_channel_layout_default(&st->codecpar->ch_layout, channels);
        }
    }

    if (track > 0) {
        std::string title = "Track " + std::to_string(track);
        av_dict_set(&st->metadata, "title", title.c_str(), 0);
    }
    return st->index;
}

void VideoEncoder::SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx) {
    savedAudioConfig = config;
    savedAudioCodecCtx = audioCtx;

    // Also add to multi-source array for compatibility
    // Clear previous contexts first (SetAudioContext is for single-source mode)
    audioContexts.clear();

    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = config.tracks.empty() ? 0 : config.tracks[0];
    ctx.streamIndex = -1;
    audioContexts.push_back(ctx);
}

int VideoEncoder::AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = track;
    ctx.streamIndex = -1;

    for (auto it = audioContexts.begin(); it != audioContexts.end(); ++it) {
        if (it->track == track) {
            it->config = config;
            it->codecCtx = audioCtx;
            it->streamIndex = -1;
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d replaced existing entry", track);
            return track;
        }
        if (it->track > track) {
            audioContexts.insert(it, ctx);
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());
            return ctx.track;
        }
    }

    audioContexts.push_back(ctx);

    DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());

    return ctx.track;
}

void VideoEncoder::ClearAudioContexts() {
    audioContexts.clear();
    audioStreamIndex = -1;
}

int VideoEncoder::GetAudioStreamIndex(int track) const {
    // Backward compatible: track -1 returns first stream index
    if (track < 0) {
        if (!audioContexts.empty()) {
            return audioContexts[0].streamIndex;
        }
        return audioStreamIndex;
    }

    // Find stream index for specific track
    for (const auto& ctx : audioContexts) {
        if (ctx.track == track) {
            return ctx.streamIndex;
        }
    }

    return -1;
}

void VideoEncoder::BeginDeferredRecording() {
    codecOpenFailed = false;
    writerFinalizeTimedOut.store(false, std::memory_order_relaxed);
    writerFinalizeSlowWarningLogged.store(false, std::memory_order_relaxed);
    writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
    discardOutputRequested.store(false, std::memory_order_relaxed);
    outputPublished.store(false, std::memory_order_relaxed);
    liveOutputFailed.store(false, std::memory_order_relaxed);
    outputIoAbort.store(false, std::memory_order_relaxed);
    outputIoDeadlineMs.store(0, std::memory_order_relaxed);
    encodedDurationUs.store(0, std::memory_order_relaxed);
    lastAssignedVideoPts = -1;
    lastFrameDeferred.store(false, std::memory_order_relaxed);
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
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();

    audioPacketCount = 0;
    videoPacketCount = 0;
    vidDebugCount = 0;
    asyncWriteErrorCount = 0;
    packetStats.Reset();
    ResetPacketTimelineDiagnostics();

    recordingRequested = true;
    needsCounterReset = true;
    DLL_Log("[VideoEncoder] Start Recording Requested (Deferred).");

    video_encoder_g_lastFramePts = -1;
    video_encoder_g_framesEncoded = 0;
    video_encoder_g_totalFenceWait = 0.0;
    video_encoder_g_totalColorConvert = 0.0;
    video_encoder_g_totalEncode = 0.0;
    video_encoder_g_maxFrameTime = 0.0;
    video_encoder_g_slowFrameCount = 0;

    if (!writerRunning) {
        writerRunning = true;
        writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
        // The task and its future are created together so a joinable writer
        // thread always has a valid completion future to wait on.
        std::packaged_task<void()> writerTask([this] { AsyncWriteLoop(); });
        writerFinished = writerTask.get_future();
        writerThread = std::thread(std::move(writerTask));
        DLL_Log("[VideoEncoder] Started Writer Thread");
    }
}

bool VideoEncoder::Start() {
    // Ensure previous recording is fully finalized and resources cleaned up.
    // Stop() will signal the async finalize if needed, then we wait for it to
    // finish.
    Stop();
    if (writerThread.joinable()) {
        if (writerFinalizeTimedOut.load(std::memory_order_acquire)) {
            if (!WriterFinishedWithin(writerFinished, 0)) {
                DLL_Log(
                    "[VideoEncoder] Start: ERROR previous writer finalize is still running (phase=%s); refusing "
                    "new recording to preserve muxer ownership",
                    WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)));
                return false;
            }
            DLL_Log("[VideoEncoder] Start: Previous timed-out writer completed before restart; joining now.");
            writerThread.join();
            writerFinalizeTimedOut.store(false, std::memory_order_release);
        } else {
            DLL_Log("[VideoEncoder] Start: Waiting for previous recording to finalize...");
            writerThread.join();
        }
    }

    if (liveOutput && !ce::live_stream::IsValidLiveStreamTarget(savedConfig.outputDir)) {
        DLL_Log("[LiveStream] Refusing to start because the configured endpoint is invalid (endpoint=<redacted>)");
        RequestLiveOutputFailure("validate_endpoint", AVERROR(EINVAL));
        return false;
    }

    // If fmtCtx was freed by Stop(), recreate it for the new recording
    if (!fmtCtx) {
        DLL_Log("[VideoEncoder] Creating new output format context for container: %s", savedConfig.container.c_str());

        if (AllocateOutputContextForContainer(&fmtCtx, savedConfig) < 0) {
            DLL_Log("[VideoEncoder] Failed to allocate new format context");
            return false;
        }
    }

    // If codecCtx was freed by Stop(), recreate it
    if (!codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] Codec not found: %s", savedConfig.encoder.c_str());
            return false;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            DLL_Log("[VideoEncoder] Failed to alloc new codec context");
            return false;
        }

        codecCtx->width = width;
        codecCtx->height = height;

        // Apply configured pixel format
        ResolvedVideoFormat resolvedFormat;
        std::string resolvedError;
        std::string resolvedWarning;
        if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                                &resolvedError, &resolvedWarning)) {
            DLL_Log("%s", resolvedError.c_str());
            avcodec_free_context(&codecCtx);
            return false;
        }
        if (!resolvedWarning.empty()) {
            DLL_Log("%s", resolvedWarning.c_str());
        }
        codecCtx->pix_fmt = resolvedFormat.codecPixFmt;

        DLL_Log("[VideoEncoder] Recreated codec context for new recording");

        if (hwFramesCtx) {
            codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
            codecCtx->extra_hw_frames = 5;
        }
    }

    // Pre-warm device and codec to reduce first-frame latency
    // This moves heavy initialization (D3D11 device, codec open, video processor)
    // from first frame to Start() call, avoiding game stutter on recording start
    // IMPORTANT: Only pre-warm if we already have valid dimensions from common
    // discovery
    if ((luidLow != 0 || luidHigh != 0) && width > 0 && height > 0 && !initDone) {
        DLL_Log("[VideoEncoder] Pre-warming device and codec (%dx%d)...", width, height);
        auto prewarmStart = PerfTimer::now();

        if (!EnsureDevice()) {
            DLL_Log("[VideoEncoder] Pre-warm failed, will retry on first frame");
        } else {
            auto prewarmEnd = PerfTimer::now();
            double prewarmMs = PerfTimer::elapsed_ms(prewarmStart, prewarmEnd);
            DLL_Log(
                "[VideoEncoder] Pre-warm complete in %.2fms (device init, codec "
                "open)",
                prewarmMs);
        }
    }

    if (liveOutput) {
        outputReservation.CleanupOwnedFile();
        outputFilename = savedConfig.outputDir;
        DLL_Log("[LiveStream] Prepared live output endpoint=<redacted>");
    } else if (!outputReservation) {
        outputReservation = ReserveOutputStagingFile(savedConfig);
        if (!outputReservation) {
            return false;
        }
        outputFilename = outputReservation.Utf8Path();
        DLL_Log("[VideoEncoder] Reserved staging output for recording: %s",
                OutputTargetForLog().c_str());
    }

    BeginDeferredRecording();

    return true;
}

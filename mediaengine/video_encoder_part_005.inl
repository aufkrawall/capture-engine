
    // Apply explicit GPU priority only. With gpu_priority=0 the encoder starts
    // neutral and raises priority adaptively only if encode time sustains real
    // pressure, so capture does not fight the game during healthy 10-bit runs.
    if (d3d11Device) {
        if (gpuPriority != 0) {
            DLL_Log(
                "[VideoEncoder] Explicit gpu_priority=%d configured; adaptive encoder GPU priority is bypassed for "
                "this recording",
                gpuPriority);
        }
        ApplyGpuThreadPriority(gpuPriority, "initial");
    }

    // CreateSharedCaptureTextures can run before Start() recreates codec/container
    // contexts after a previous Stop(). In that pre-start phase we only need the
    // D3D11 device for texture allocation; defer FFmpeg HW context wiring until
    // Start() has rebuilt fmtCtx/codecCtx.
    if (!codecCtx || !fmtCtx) {
        if (!recordingRequested) {
            DLL_Log(
                "[VideoEncoder] EnsureDevice: device-only init (fmtCtx=%p codecCtx=%p), "
                "deferring codec prewarm to Start()",
                (void*)fmtCtx, (void*)codecCtx);
            return true;
        }

        DLL_Log("[VideoEncoder] EnsureDevice failed: missing contexts while recording (fmtCtx=%p codecCtx=%p)",
                (void*)fmtCtx, (void*)codecCtx);
        return false;
    }

    // Set up FFmpeg HW device context with our D3D11 device (shared for both
    // paths)
    if (!d3d11DeviceCtx) {
        // 2. Wrap in AVHWDeviceContext - for screengrab mode using shared device
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

        // Get base device from our QI'd interface
        ce::ComGuard<ID3D11Device> baseDevice;
        if (FAILED(d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)baseDevice.addressof()))) {
            return false;
        }

        d3d11Ctx->device = baseDevice.get();
        // FFmpeg expects to own a reference, so we AddRef.
        // The ComPtr will release our local reference when it goes out of scope.
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0) {
            // If init fails, we rely on ComPtr to release baseDevice.
            // We also need to clean up the partially created context.
            av_buffer_unref(&d3d11DeviceCtx);
            return false;
        }
        // baseDevice releases its ref here, but FFmpeg holds one via AddRef above.
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    }
    if (!codec) {
        DLL_Log("[VideoEncoder] EnsureDevice: Codec not found for format resolution");
        return false;
    }

    ResolvedVideoFormat resolvedFormat;
    std::string resolvedError;
    std::string resolvedWarning;
    if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                            &resolvedError,
                            &resolvedWarning)) {
        DLL_Log("%s", resolvedError.c_str());
        return false;
    }
    if (!resolvedWarning.empty()) {
        DLL_Log("%s", resolvedWarning.c_str());
    }

    codecCtx->pix_fmt = resolvedFormat.codecPixFmt;

    // 3. D3D11 Frames Context
    if (codecCtx->hw_frames_ctx) {
        av_buffer_unref(&codecCtx->hw_frames_ctx);
    }
    if (codecCtx->hw_device_ctx) {
        av_buffer_unref(&codecCtx->hw_device_ctx);
    }
    if (hwFramesCtx) {
        av_buffer_unref(&hwFramesCtx);
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    d3d11FramesCtx = av_hwframe_ctx_alloc(d3d11DeviceCtx);
    if (!d3d11FramesCtx) {
        DLL_Log("[VideoEncoder] Failed to allocate D3D11 frames context");
        return false;
    }
    AVHWFramesContext* d11Frames = (AVHWFramesContext*)d3d11FramesCtx->data;
    AVD3D11VAFramesContext* d11FramesHw = (AVD3D11VAFramesContext*)d11Frames->hwctx;
    d11Frames->format = AV_PIX_FMT_D3D11;
    // RGB->YUV output is written directly into AVHWFrame-owned textures by
    // ID3D11VideoProcessor. NVENC then retains the AVFrame until that input is
    // no longer in flight, so lookahead/B-frame depth cannot recycle a surface
    // that the encoder still references.
    d11FramesHw->BindFlags |= D3D11_BIND_RENDER_TARGET;

    d11Frames->sw_format = resolvedFormat.d3d11SwFormat;
    if (!DeviceSupportsHwFrameSwFormat(d3d11DeviceCtx, resolvedFormat.d3d11SwFormat)) {
        DLL_Log("[VideoEncoder] D3D11 HW frames do not support sw_format=%s on this device",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
        return false;
    }
    if (resolvedFormat.usesVideoProcessor) {
        if (resolvedFormat.use10Bit) {
            DLL_Log("[VideoEncoder] Using P010 (10-bit) sw_format for D3D11 HW frames");
        }
    } else {
        DLL_Log("[VideoEncoder] Using direct D3D11 RGB 4:4:4 path with sw_format=%s",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
    }

    int framesWidth = width;
    int framesHeight = height;
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        framesWidth = savedConfig.scaling.outputWidth;
        framesHeight = savedConfig.scaling.outputHeight;
    }

    if (resolvedFormat.requiresEvenDimensions) {
        framesWidth = framesWidth & ~1;
        framesHeight = framesHeight & ~1;
    }

    d11Frames->width = framesWidth;
    d11Frames->height = framesHeight;
    d11Frames->initial_pool_size = 0;

    if (av_hwframe_ctx_init(d3d11FramesCtx) < 0) {
        DLL_Log("[VideoEncoder] Failed to init D3D11 frames context");
        return false;
    }

    if (resolvedFormat.codecPixFmt == AV_PIX_FMT_QSV) {
        int ret = av_hwdevice_ctx_create_derived(&hwDeviceCtx, AV_HWDEVICE_TYPE_QSV, d3d11DeviceCtx, 0);
        if (ret < 0 || !hwDeviceCtx) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[VideoEncoder] Failed to derive oneVPL/QSV device from the capture D3D11 device: %d (%s)", ret,
                    errbuf);
            codecOpenFailed = true;
            return false;
        }
        if (!DeviceSupportsHwFrameSwFormat(hwDeviceCtx, resolvedFormat.d3d11SwFormat)) {
            DLL_Log("[VideoEncoder] oneVPL/QSV device does not support sw_format=%s on the selected adapter",
                    GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
            codecOpenFailed = true;
            return false;
        }

        ret = av_hwframe_ctx_create_derived(&hwFramesCtx, AV_PIX_FMT_QSV, hwDeviceCtx, d3d11FramesCtx,
                                            AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        if (ret < 0 || !hwFramesCtx) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[VideoEncoder] Failed to derive zero-copy QSV frames from D3D11: %d (%s)", ret, errbuf);
            codecOpenFailed = true;
            return false;
        }
        DLL_Log("[VideoEncoder] oneVPL/QSV active on the capture adapter via direct D3D11 surface mapping");
    } else {
        hwDeviceCtx = av_buffer_ref(d3d11DeviceCtx);
        hwFramesCtx = av_buffer_ref(d3d11FramesCtx);
        if (!hwDeviceCtx || !hwFramesCtx) {
            DLL_Log("[VideoEncoder] Failed to reference D3D11 hardware contexts for encoder input");
            return false;
        }
    }

    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    if (!codecCtx->hw_device_ctx || !codecCtx->hw_frames_ctx) {
        DLL_Log("[VideoEncoder] Failed to attach active hardware contexts to codec");
        return false;
    }
    codecCtx->extra_hw_frames = 5;
    codecCtx->width = framesWidth;
    codecCtx->height = framesHeight;

    return ConfigureAndOpenCodec();
}

AVFrame* VideoEncoder::PrepareEncoderInputFrame(AVFrame* d3d11Frame) {
    if (!d3d11Frame) {
        return nullptr;
    }
    if (!UsesQsvHardwareFrames(savedConfig.encoder)) {
        return d3d11Frame;
    }
    if (!hwFramesCtx || d3d11Frame->format != AV_PIX_FMT_D3D11) {
        DLL_Log("[VideoEncoder] Cannot map QSV input: missing derived frames context or non-D3D11 source");
        return nullptr;
    }

    AVFrame* qsvFrame = av_frame_alloc();
    if (!qsvFrame) {
        return nullptr;
    }
    qsvFrame->format = AV_PIX_FMT_QSV;
    qsvFrame->width = d3d11Frame->width;
    qsvFrame->height = d3d11Frame->height;
    qsvFrame->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    if (!qsvFrame->hw_frames_ctx) {
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    const int mapResult =
        av_hwframe_map(qsvFrame, d3d11Frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (mapResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(mapResult, errbuf, sizeof(errbuf));
        qsvSurfaceMappingFailures++;
        if (qsvSurfaceMappingFailures <= 3 || qsvSurfaceMappingFailures % 300 == 0) {
            DLL_Log("[VideoEncoder] Direct D3D11-to-QSV surface mapping failed: %d (%s), failures=%u", mapResult,
                    errbuf, qsvSurfaceMappingFailures);
        }
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    const int copyResult = av_frame_copy_props(qsvFrame, d3d11Frame);
    if (copyResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(copyResult, errbuf, sizeof(errbuf));
        qsvSurfaceMappingFailures++;
        if (qsvSurfaceMappingFailures <= 3 || qsvSurfaceMappingFailures % 300 == 0) {
            DLL_Log("[VideoEncoder] Failed to copy frame properties to QSV surface: %d (%s), failures=%u",
                    copyResult, errbuf, qsvSurfaceMappingFailures);
        }
        av_frame_free(&qsvFrame);
        return nullptr;
    }

    if (!qsvSurfaceMappingLogged) {
        DLL_Log("[VideoEncoder] First D3D11 frame mapped directly to a oneVPL/QSV surface (no CPU transfer)");
        qsvSurfaceMappingLogged = true;
    }
    return qsvFrame;
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

    g_lastFramePts = -1;
    g_framesEncoded = 0;
    g_totalFenceWait = 0.0;
    g_totalColorConvert = 0.0;
    g_totalEncode = 0.0;
    g_maxFrameTime = 0.0;
    g_slowFrameCount = 0;

    if (!writerRunning) {
        writerRunning = true;
        writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
        writerThread = std::thread(&VideoEncoder::AsyncWriteLoop, this);
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
            HANDLE hThread = writerThread.native_handle();
            DWORD waitResult = WaitForSingleObject(hThread, 0);
            if (waitResult != WAIT_OBJECT_0) {
                DLL_Log(
                    "[VideoEncoder] Start: ERROR previous writer finalize is still running (result=%lu); refusing "
                    "new recording to preserve muxer ownership",
                    waitResult);
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

    if (!outputReservation) {
        outputReservation = ReserveOutputStagingFile(savedConfig);
        if (!outputReservation) {
            return false;
        }
        outputFilename = outputReservation.Utf8Path();
        DLL_Log("[VideoEncoder] Reserved staging output for recording: %s", outputFilename.c_str());
    }

    BeginDeferredRecording();

    return true;
}

bool VideoEncoder::NormalizeHdrPacketIfNeeded(AVPacket* packet) {
    if (!packet || !stream || packet->stream_index != stream->index || !ShouldEncodeHdrOutput()) {
        return true;
    }

    const int result =
        ce::video_metadata::NormalizeHdrPacketMetadata(packet, stream->codecpar, codecCtx->time_base);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, error, sizeof(error));
        DLL_Log("[HDR Metadata] ERROR: Failed to normalize packet-carried sequence header: %d (%s)", result, error);
        discardOutputRequested.store(true, std::memory_order_release);
        return false;
    }
    if (result > 0 && !hdrPacketMetadataLogged) {
        DLL_Log(
            "[HDR Metadata] Normalized packet-carried sequence header and NEW_EXTRADATA; ordinary packets bypass "
            "the metadata filter");
        hdrPacketMetadataLogged = true;
    }
    return true;
}

void VideoEncoder::WriteFrame(AVPacket* pkt) {
    if (!fileOpened || !fmtCtx)
        return;

    if (!NormalizeHdrPacketIfNeeded(pkt)) {
        return;
    }

    // Rescale timestamps from codec time_base to stream time_base
    AVStream* st = fmtCtx->streams[pkt->stream_index];
    AVRational codec_tb;

    if (pkt->stream_index == stream->index) {
        // Video packet - use video codec time_base
        codec_tb = codecCtx->time_base;
    } else {
        // Audio packet - audio time_base is typically 1/sample_rate
        // The audio encoder uses PTS = sample_count, so time_base is {1,
        // sample_rate}
        codec_tb = {1, st->codecpar->sample_rate};

        if (audioPacketCount++ % 100 == 0) {
            DLL_Log(
                "[VideoEncoder] Queuing audio pkt #%d size=%d pts=%lld "
                "dur=%lld stream_idx=%d",
                audioPacketCount, pkt->size, (long long)pkt->pts, (long long)pkt->duration, pkt->stream_index);
        }
    }

    // Debug: log first 20 video packets with DTS to verify B-frame ordering
    if (pkt->stream_index == stream->index && videoPacketCount++ < 20) {
        bool isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        bool isTiny = (pkt->size <= 5 && codecCtx->max_b_frames > 0);
        bool isTemporalDelimiter = false;
        if (isTiny && pkt->size >= 3 && pkt->data) {
            uint8_t obuType = (pkt->data[0] >> 3) & 0x0F;
            if (obuType == 2)
                isTemporalDelimiter = true;
        }
        const char* type = isKeyframe ? "KEY" : (isTemporalDelimiter ? "TD" : (isTiny ? "SEF" : "DATA"));
        DLL_Log(
            "[VideoEncoder] Queuing video pkt #%d: pts=%lld dts=%lld dur=%lld "
            "size=%d %s codec_tb=%d/%d st_tb=%d/%d",
            videoPacketCount, (long long)pkt->pts, (long long)pkt->dts, (long long)pkt->duration, pkt->size, type,
            codec_tb.num, codec_tb.den, st->time_base.num, st->time_base.den);
    }

    // Track packet types for B-frame quality diagnostics
    if (pkt->stream_index == stream->index) {
        packetStats.totalPackets++;
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            packetStats.keyframeBytes += pkt->size;
            packetStats.keyframeCount++;
        } else if (pkt->size <= 5 && codecCtx->max_b_frames > 0) {
            // Check for AV1 temporal delimiter OBUs.
            // Temporal delimiters (OBU type 2) have header byte 0x12 at pkt->data[0].
            // They are normal AV1 frame-boundary markers that players ignore.
            bool isTemporalDelimiter = false;
            if (pkt->size >= 3 && pkt->data) {
                // OBU header byte: bits 3-6 = obu_type, type 2 = temporal delimiter
                uint8_t obuType = (pkt->data[0] >> 3) & 0x0F;
                if (obuType == 2) {
                    isTemporalDelimiter = true;
                }
            }
            if (!isTemporalDelimiter) {
                packetStats.sefBytes += pkt->size;
                packetStats.sefCount++;
            }
        } else if (pkt->size < 2000 && codecCtx->max_b_frames > 0) {
            // Likely a leaf B-frame with near-zero bit allocation.
            // Only classify when B-frames are active — small P-frames are
            // normal in non-B-frame mode and shouldn't be flagged.
            packetStats.bframeBytes += pkt->size;
            packetStats.bframeCount++;
        } else {
            packetStats.refBytes += pkt->size;

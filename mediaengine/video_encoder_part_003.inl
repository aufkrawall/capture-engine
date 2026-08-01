
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

void VideoEncoder::ResetPacketTimelineDiagnostics() {
    writtenPacketTimelines.clear();
    lastMuxerVideoPtsUs.store(0, std::memory_order_relaxed);
}

void VideoEncoder::RecordWrittenPacketTimeline(int streamIndex, int64_t pts, int64_t dts, int64_t duration,
                                               AVRational timeBase, uint32_t terminalDiscardSamples, int sampleRate) {
    if (streamIndex < 0 || !fmtCtx || static_cast<unsigned int>(streamIndex) >= fmtCtx->nb_streams ||
        !HasValidStreamTimeBase(fmtCtx->streams[streamIndex])) {
        return;
    }

    const int64_t packetPts = pts != AV_NOPTS_VALUE ? pts : dts;
    if (packetPts == AV_NOPTS_VALUE) {
        return;
    }

    if (writtenPacketTimelines.size() < fmtCtx->nb_streams) {
        writtenPacketTimelines.resize(fmtCtx->nb_streams);
    }
    if (static_cast<size_t>(streamIndex) >= writtenPacketTimelines.size()) {
        return;
    }

    const int64_t packetStartUs = av_rescale_q(packetPts, timeBase, AVRational{1, 1000000});
    const int64_t packetDurationUs = duration > 0 ? av_rescale_q(duration, timeBase, AVRational{1, 1000000}) : 0;
    const int64_t terminalDiscardUs = ce::mux::ComputeAudioPaddingDurationUs(terminalDiscardSamples, sampleRate);
    ce::mux::ObservePacketTimeline(writtenPacketTimelines[streamIndex], packetStartUs, packetDurationUs,
                                   terminalDiscardUs);
}

void VideoEncoder::LogPacketTimelineSummary(int64_t finalDurationUs) const {
    if (!fmtCtx || finalDurationUs <= 0 || writtenPacketTimelines.empty()) {
        return;
    }

    int64_t maxVideoEndUs = 0;
    int64_t minAudioEndUs = 0;
    int64_t maxAudioEndUs = 0;
    int64_t maxRawAudioEndUs = 0;
    int64_t maxPacketDeltaUs = 0;
    uint32_t videoStreamCount = 0;
    uint64_t videoPacketCount = 0;
    int64_t maxVideoPtsGapUs = 0;
    uint32_t audioStreamCount = 0;
    uint32_t audioPastTargetCount = 0;

    for (unsigned int i = 0; i < fmtCtx->nb_streams && i < writtenPacketTimelines.size(); ++i) {
        const AVStream* st = fmtCtx->streams[i];
        if (!st || !st->codecpar) {
            continue;
        }
        const auto& timeline = writtenPacketTimelines[i];
        if (!timeline.seen) {
            continue;
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++videoStreamCount;
            videoPacketCount += timeline.packetCount;
            maxVideoEndUs = std::max(maxVideoEndUs, timeline.lastEndUs);
            maxVideoPtsGapUs = std::max(maxVideoPtsGapUs, timeline.maxForwardStartGapUs);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioStreamCount;
            if (minAudioEndUs == 0 || timeline.lastDecodedEndUs < minAudioEndUs) {
                minAudioEndUs = timeline.lastDecodedEndUs;
            }
            maxAudioEndUs = std::max(maxAudioEndUs, timeline.lastDecodedEndUs);
            maxRawAudioEndUs = std::max(maxRawAudioEndUs, timeline.lastEndUs);
            if (timeline.lastDecodedEndUs > finalDurationUs + 1000) {
                ++audioPastTargetCount;
            }
        }
    }

    const int64_t referenceVideoEndUs = maxVideoEndUs > 0 ? maxVideoEndUs : finalDurationUs;
    if (audioStreamCount > 0) {
        maxPacketDeltaUs = std::max(ce::mux::ComputeDurationDeltaUs(referenceVideoEndUs, minAudioEndUs),
                                    ce::mux::ComputeDurationDeltaUs(referenceVideoEndUs, maxAudioEndUs));
    }

    DLL_Log(
        "[VideoEncoder] Final packet timeline: target=%lld us videoEnd=%lld us audioMinEnd=%lld us "
        "audioMaxEnd=%lld us maxPacketDelta=%lld us streams(v=%u a=%u) audioPastTarget=%u rawAudioMaxEnd=%lld us",
        finalDurationUs, maxVideoEndUs, minAudioEndUs, maxAudioEndUs, maxPacketDeltaUs, videoStreamCount,
        audioStreamCount, audioPastTargetCount, maxRawAudioEndUs);
    if (!savedConfig.useVFR && savedConfig.fps > 0 && videoStreamCount > 0) {
        const int64_t expectedPackets = av_rescale_rnd(finalDurationUs, savedConfig.fps, 1000000, AV_ROUND_NEAR_INF);
        const int64_t emittedPackets = static_cast<int64_t>(videoPacketCount);
        const int64_t missingPackets = std::max<int64_t>(0, expectedPackets - emittedPackets);
        const double maxPtsGapTicks = static_cast<double>(maxVideoPtsGapUs) * savedConfig.fps / 1000000.0;
        const bool coverageComplete = missingPackets == 0 && maxPtsGapTicks <= 1.01;
        DLL_Log(
            "[VideoEncoder] CFR packet coverage: expected=%lld emitted=%lld missing=%lld maxPtsGapUs=%lld "
            "maxPtsGapTicks=%.3f complete=%d fps=%d",
            expectedPackets, emittedPackets, missingPackets, maxVideoPtsGapUs, maxPtsGapTicks, coverageComplete ? 1 : 0,
            savedConfig.fps);
        if (!coverageComplete) {
            DLL_Log(
                "[VideoEncoder] ERROR: CFR artifact failed packet-continuity validation: expected=%lld emitted=%lld "
                "missing=%lld maxPtsGapTicks=%.3f (required <=1.01)",
                expectedPackets, emittedPackets, missingPackets, maxPtsGapTicks);
        }
    }

    constexpr int64_t kPacketDurationWarningToleranceUs = 1000;
    if (audioPastTargetCount > 0 || (audioStreamCount > 0 && maxPacketDeltaUs > kPacketDurationWarningToleranceUs)) {
        DLL_Log(
            "[VideoEncoder] WARNING: Packet-level A/V duration mismatch exceeded %lld us tolerance "
            "(target=%lld videoEnd=%lld audioMinEnd=%lld audioMaxEnd=%lld maxPacketDelta=%lld)",
            kPacketDurationWarningToleranceUs, finalDurationUs, maxVideoEndUs, minAudioEndUs, maxAudioEndUs,
            maxPacketDeltaUs);
    }
}

uint64_t VideoEncoder::GetWrittenVideoPacketCount() const {
    if (!fmtCtx || !stream || stream->index < 0 ||
        static_cast<size_t>(stream->index) >= writtenPacketTimelines.size()) {
        return 0;
    }
    return writtenPacketTimelines[static_cast<size_t>(stream->index)].packetCount;
}

bool VideoEncoder::FinalizeOutputPublication(int trailerResult, int closeResult, int64_t finalDurationUs) {
    outputPublished.store(false, std::memory_order_relaxed);
    const uint64_t writtenVideoPackets = GetWrittenVideoPacketCount();
    const auto disposition = ce::mux::SelectVideoOutputDisposition(
        discardOutputRequested.load(std::memory_order_acquire), trailerResult, closeResult, finalDurationUs,
        writtenVideoPackets);
    if (disposition != ce::mux::VideoOutputDisposition::kPublish) {
        const bool removed = outputReservation.CleanupOwnedFile();
        DLL_Log(
            "[VideoEncoder] output_discarded reason=%s durationUs=%lld videoPackets=%llu trailer=%d close=%d "
            "removed=%d staging='%s'",
            ce::mux::VideoOutputDispositionToString(disposition), static_cast<long long>(finalDurationUs),
            static_cast<unsigned long long>(writtenVideoPackets), trailerResult, closeResult, removed ? 1 : 0,
            outputFilename.c_str());
        return false;
    }

    const fs::path outputDirectory = ce::capture_output::ResolveCaptureDirectory(
        savedConfig.outputDir, ce::capture_output::GetExecutableDirectory());
    const std::wstring extension(savedConfig.container.begin(), savedConfig.container.end());
    if (!outputReservation.PublishToNewPath(outputDirectory, L"capture", extension)) {
        const DWORD publishError = GetLastError();
        outputReservation.Publish();
        DLL_Log(
            "[VideoEncoder] output_renamed_to_staging error=%lu durationUs=%lld videoPackets=%llu "
            "staging='%s' (file is playable but was not renamed to final name)",
            publishError, static_cast<long long>(finalDurationUs),
            static_cast<unsigned long long>(writtenVideoPackets), outputFilename.c_str());
        outputPublished.store(true, std::memory_order_release);
        return true;
    }

    outputFilename = outputReservation.Utf8Path();
    DLL_Log("[VideoEncoder] output_published file='%s' durationUs=%lld videoPackets=%llu", outputFilename.c_str(),
            static_cast<long long>(finalDurationUs), static_cast<unsigned long long>(writtenVideoPackets));
    outputPublished.store(true, std::memory_order_release);
    return true;
}

bool VideoEncoder::Init(const VideoConfig& config, int width, int height, int fps,
                        std::function<void(AVPacket*)> packetCallback) {
    // Clear handle failure cache from previous recording session
    g_HandleFailureCache.Clear();
    DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d", config.encoder.c_str(), width, height,
            fps);

    // Disable buffering to see logs immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    DLL_Log("[VideoEncoder] Step 1: Setting member variables");
    this->width = width;
    this->height = height;
    this->captureCursor = config.captureCursor;
    this->gpuPriority = config.gpuPriority;
    this->onPacket = packetCallback;
    hdrPacketMetadataLogged = false;

    // Initialize cursor renderer if cursor capture enabled
    if (captureCursor) {
        cursorRenderer = std::make_unique<CursorRenderer>();
        DLL_Log("[VideoEncoder] Cursor capture enabled (renderer created)");
    }

    DLL_Log("[VideoEncoder] Step 2: Setting av_log level");
    av_log_set_level(AV_LOG_WARNING);

    DLL_Log("[VideoEncoder] Step 3: Deferring staging output reservation until recording start");
    outputReservation.CleanupOwnedFile();
    outputFilename.clear();

    DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
    if (AllocateOutputContextForContainer(&fmtCtx, config) < 0) {
        DLL_Log("[VideoEncoder] Failed to alloc output context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void*)fmtCtx);

    DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoder.c_str());
    if (!codec) {
        DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
        return false;
    }
    DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void*)codec, codec->name);

    DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[VideoEncoder] Failed to alloc codec context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void*)codecCtx);

    // Store config for use in EnsureDevice()
    savedConfig = config;

    DLL_Log("[VideoEncoder] Init Complete - returning true");
    // Defer device creation to EnsureDevice()
    return true;
}

void VideoEncoder::SetAdapterLUID(int32_t low, int32_t high) {
    this->luidLow = low;
    this->luidHigh = high;
}

void VideoEncoder::SetDimensions(uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        this->width = w;
        this->height = h;
        DLL_Log("[VideoEncoder] SetDimensions: %dx%d", w, h);
    }
}

bool VideoEncoder::AdoptTextureDevice(ID3D11Texture2D* texture) {
    if (!texture) {
        return false;
    }

    ID3D11Device* texDevice = nullptr;
    texture->GetDevice(&texDevice);
    if (!texDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to get D3D11 device from texture");
        return false;
    }

    ID3D11Device5* adoptedDevice = nullptr;
    HRESULT hr = texDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&adoptedDevice);
    if (FAILED(hr) || !adoptedDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11Device5 from texture device. HR=%x", hr);
        texDevice->Release();
        return false;
    }

    ID3D11DeviceContext* immediateContext = nullptr;
    texDevice->GetImmediateContext(&immediateContext);
    ID3D11DeviceContext4* adoptedContext = nullptr;
    if (immediateContext) {
        hr = immediateContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&adoptedContext);
        immediateContext->Release();
    } else {
        hr = E_NOINTERFACE;
    }
    texDevice->Release();

    if (FAILED(hr) || !adoptedContext) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11DeviceContext4 from texture device. HR=%x", hr);
        adoptedDevice->Release();
        return false;
    }

    if (d3d11Context) {
        d3d11Context->Release();
    }
    if (d3d11Device) {
        d3d11Device->Release();
    }

    d3d11Device = adoptedDevice;
    d3d11Context = adoptedContext;
    return true;
}

void VideoEncoder::ReleaseInjectDeviceStateForScreenGrab() {
    const bool hadInjectLuid = (luidLow != 0 || luidHigh != 0);
    const bool hadSharedCapture = sharedCaptureTexturesCreated;
    if (!hadInjectLuid && !hadSharedCapture) {
        return;
    }

    DLL_Log("[VideoEncoder] ScreenGrab: Releasing inject device state (luid=%08x %08x shared=%d)", luidLow, luidHigh,
            hadSharedCapture ? 1 : 0);
    luidLow = 0;
    luidHigh = 0;

    if (pSharedMem) {
        pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
    }

    if (hadSharedCapture) {
        ReleasePreservedEncoderTextures();
        return;
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
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    TrimD3D11Residency(d3d11Device, d3d11Context, "screen-grab-switch");
    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (hwFramesCtx) {
        av_buffer_unref(&hwFramesCtx);
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    if (d3d11DeviceCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    initDone = false;
}

AVPixelFormat VideoEncoder::GetActiveD3D11SwFormat() const {
    if (!d3d11FramesCtx) {
        return AV_PIX_FMT_NONE;
    }

    const auto* framesCtx = reinterpret_cast<const AVHWFramesContext*>(d3d11FramesCtx->data);
    if (!framesCtx) {
        return AV_PIX_FMT_NONE;
    }
    return framesCtx->sw_format;
}

bool VideoEncoder::PrepareD3D11TextureForEncode(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture,
                                                bool overlayCursor, int captureOriginX, int captureOriginY,
                                                bool allowCursorHandleVisibilityFallback,
                                                uint64_t keyedMutexAcquireKey) {
    if (!srcTexture || !dstTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);
    UpdateSdrWhiteLevelForCaptureArea(captureOriginX, captureOriginY, srcDesc.Width, srcDesc.Height);
    if (currentIsHDR && !ce::video_format::IsFp16RgbInputFormat(srcDesc.Format) &&
        !ce::video_format::IsHdr10RgbInputFormat(srcDesc.Format)) {
        DLL_Log("[HDR Color] Direct encode path refuses unsupported HDR source format %d", srcDesc.Format);
        return false;
    }

    struct KeyedMutexGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedMutexGuard() {
            if (!mutex) {
                return;
            }
            if (acquired) {
                mutex->ReleaseSync(0);
            }
            mutex->Release();
        }
    } keyedMutexGuard;

    srcTexture->QueryInterface(IID_PPV_ARGS(&keyedMutexGuard.mutex));
    if (keyedMutexGuard.mutex) {
        const HRESULT kmHr = keyedMutexGuard.mutex->AcquireSync(keyedMutexAcquireKey, 1000);
        if (kmHr != S_OK) {
            DLL_Log("[VideoEncoder] Direct D3D11 encode path could not acquire keyed mutex: HR=%x", kmHr);
            keyedMutexGuard.mutex->Release();
            keyedMutexGuard.mutex = nullptr;
            return false;
        }
        keyedMutexGuard.acquired = true;
    }

    const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(srcDesc.Format);
    if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
        DLL_Log("[VideoEncoder] Direct D3D11 encode path does not support source format %d", srcDesc.Format);
        return false;
    }
    const ce::video_format::RgbColorTransform colorTransform =
        ce::video_format::GetRgbColorTransform(srcDesc.Format, currentIsHDR, ShouldEncodeHdrOutput());

    ID3D11Texture2D* srvSourceTexture = srcTexture;
    ID3D11Texture2D* srvCompatTexture = nullptr;
    if ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        D3D11_TEXTURE2D_DESC srvDesc = srcDesc;
        srvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        srvDesc.MiscFlags = 0;
        srvDesc.CPUAccessFlags = 0;
        srvDesc.Usage = D3D11_USAGE_DEFAULT;

        HRESULT hr = d3d11Device->CreateTexture2D(&srvDesc, nullptr, &srvCompatTexture);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create SRV-compatible staging texture: HR=%x", hr);
            return false;
        }
        d3d11Context->CopyResource(srvCompatTexture, srcTexture);
        srvSourceTexture = srvCompatTexture;
    }

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstTexture->GetDesc(&dstDesc);

    ID3D11Texture2D* normalizedTexture = nullptr;
    if (dstDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        normalizedTexture = RenderFullscreenCopy(srvSourceTexture, dstDesc.Width, dstDesc.Height, inputSrvFormat,
                                                 DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture, swapRBTextureRTV,
                                                 swapRBTexWidth, swapRBTexHeight, "RGB444-BGRA", colorTransform,
                                                 sdrWhiteNits);
    } else if (dstDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        normalizedTexture =
            RenderFullscreenCopy(srvSourceTexture, dstDesc.Width, dstDesc.Height, inputSrvFormat,
                                 DXGI_FORMAT_R10G10B10A2_UNORM, rgb10IntermediateTexture, rgb10IntermediateRTV,
                                 rgb10IntermediateWidth, rgb10IntermediateHeight, "RGB444-RGB10", colorTransform,
                                 sdrWhiteNits);
    } else {
        DLL_Log("[VideoEncoder] Direct D3D11 encode path encountered unsupported destination format %d",
                dstDesc.Format);
    }

    if (srvCompatTexture) {
        srvCompatTexture->Release();
    }
    if (!normalizedTexture) {
        return false;
    }

    if (overlayCursor && captureCursor && cursorRenderer) {
        if (!cursorRenderer->Init(d3d11Device, d3d11Context)) {
            static bool cursorInitLogged = false;
            if (!cursorInitLogged) {
                DLL_Log("[VideoEncoder] Failed to initialize cursor renderer for direct D3D11 encode path");
                cursorInitLogged = true;
            }
        } else {
            const CursorColorMode cursorColorMode =
                ShouldEncodeHdrOutput() && dstDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? CursorColorMode::Hdr10Pq
                                                                                           : CursorColorMode::Sdr;
            const float cursorPaperWhiteNits =
                cursorColorMode == CursorColorMode::Sdr ? 80.0f : sdrWhiteNits;
            cursorRenderer->CompositeOntoFrame(normalizedTexture, (int)dstDesc.Width, (int)dstDesc.Height,
                                               cursorCaptureState, cursorColorMode, cursorPaperWhiteNits);
        }
    }

    d3d11Context->CopyResource(dstTexture, normalizedTexture);
    normalizedTexture->Release();
    return true;
}

bool VideoEncoder::CacheRepeatFrameTexture(ID3D11Texture2D* sourceTexture) {
    if (!sourceTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    sourceTexture->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC cacheDesc = srcDesc;
    cacheDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cacheDesc.MiscFlags = 0;
    cacheDesc.CPUAccessFlags = 0;
    cacheDesc.Usage = D3D11_USAGE_DEFAULT;

    bool needsRecreate = true;
    if (repeatFrameTexture) {
        D3D11_TEXTURE2D_DESC existingDesc = {};
        repeatFrameTexture->GetDesc(&existingDesc);
        needsRecreate = existingDesc.Width != cacheDesc.Width || existingDesc.Height != cacheDesc.Height ||
                        existingDesc.Format != cacheDesc.Format || existingDesc.BindFlags != cacheDesc.BindFlags ||
                        existingDesc.ArraySize != cacheDesc.ArraySize ||
                        existingDesc.MipLevels != cacheDesc.MipLevels ||
                        existingDesc.SampleDesc.Count != cacheDesc.SampleDesc.Count ||
                        existingDesc.SampleDesc.Quality != cacheDesc.SampleDesc.Quality;
    }

    if (needsRecreate) {
        if (repeatFrameTexture) {
            repeatFrameTexture->Release();
            repeatFrameTexture = nullptr;
        }

        HRESULT hr = d3d11Device->CreateTexture2D(&cacheDesc, nullptr, &repeatFrameTexture);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create repeat-frame texture: HR=%x fmt=%d %ux%u", hr, cacheDesc.Format,
                    cacheDesc.Width, cacheDesc.Height);
            return false;
        }
    }

    D3D11ScopedLock lock;
    d3d11Context->CopyResource(repeatFrameTexture, sourceTexture);
    return true;
}

void VideoEncoder::InvalidateRepeatSourceFrameTexture() {
    if (repeatSourceFrameTexture) {
        repeatSourceFrameTexture->Release();
        repeatSourceFrameTexture = nullptr;
    }
    repeatSourceNeedsCursorRecompose = false;
    repeatSourceFrameWidth = 0;
    repeatSourceFrameHeight = 0;
    repeatSourceFrameIsHDR = false;
    repeatSourceCaptureOriginX = 0;
    repeatSourceCaptureOriginY = 0;
}

bool VideoEncoder::CacheRepeatSourceFrameTexture(ID3D11Texture2D* sourceTexture, uint32_t frameWidth,
                                                 uint32_t frameHeight, bool isHDR, int captureOriginX,
                                                 int captureOriginY) {
    if (!sourceTexture || !d3d11Device || !d3d11Context) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    sourceTexture->GetDesc(&srcDesc);

    struct KeyedSourceGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedSourceGuard() {
            if (!mutex) {
                return;
            }
            if (acquired) {
                mutex->ReleaseSync(0);
            }
            mutex->Release();
        }
    } keyedSourceGuard;

    sourceTexture->QueryInterface(IID_PPV_ARGS(&keyedSourceGuard.mutex));
    if (keyedSourceGuard.mutex) {
        // Fresh split-device screen frames have already been consumed at key
        // 1 by the conversion path, which returns ownership to key 0. The
        // cursor-aware repeat cache is a second read and must explicitly own
        // that key; copying without it produced black repeat frames while
        // every fresh frame remained valid.
        const HRESULT kmHr = keyedSourceGuard.mutex->AcquireSync(0, 0);
        if (kmHr != S_OK) {
            ++repeatSourceCacheKeyedAcquireFailCount;
            if (repeatSourceCacheKeyedAcquireFailCount <= 5) {
                DLL_Log(
                    "[VideoEncoder] Cursor-aware repeat source cache keyed-mutex acquire failed: "
                    "HR=%x failures=%llu",
                    kmHr, static_cast<unsigned long long>(repeatSourceCacheKeyedAcquireFailCount));
            }
            return false;
        }
        keyedSourceGuard.acquired = true;
        if (!repeatSourceCacheKeyedMutexLogged) {
            DLL_Log("[VideoEncoder] Cursor-aware repeat source cache synchronized at keyed mutex 0->0");
            repeatSourceCacheKeyedMutexLogged = true;
        }
    }

    D3D11_TEXTURE2D_DESC cacheDesc = srcDesc;
    cacheDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    cacheDesc.MiscFlags = 0;
    cacheDesc.CPUAccessFlags = 0;
    cacheDesc.Usage = D3D11_USAGE_DEFAULT;

    bool needsRecreate = true;
    if (repeatSourceFrameTexture) {
        D3D11_TEXTURE2D_DESC existingDesc = {};
        repeatSourceFrameTexture->GetDesc(&existingDesc);
        needsRecreate = existingDesc.Width != cacheDesc.Width || existingDesc.Height != cacheDesc.Height ||
                        existingDesc.Format != cacheDesc.Format || existingDesc.BindFlags != cacheDesc.BindFlags ||
                        existingDesc.ArraySize != cacheDesc.ArraySize ||
                        existingDesc.MipLevels != cacheDesc.MipLevels ||
                        existingDesc.SampleDesc.Count != cacheDesc.SampleDesc.Count ||
                        existingDesc.SampleDesc.Quality != cacheDesc.SampleDesc.Quality;
    }

    if (needsRecreate) {
        InvalidateRepeatSourceFrameTexture();
        HRESULT hr = d3d11Device->CreateTexture2D(&cacheDesc, nullptr, &repeatSourceFrameTexture);
        if (FAILED(hr)) {
            if (!repeatSourceCacheFailureLogged) {
                DLL_Log("[VideoEncoder] Failed to create cursor-aware repeat source texture: HR=%x fmt=%d %ux%u", hr,
                        cacheDesc.Format, cacheDesc.Width, cacheDesc.Height);
                repeatSourceCacheFailureLogged = true;
            }
            return false;
        }
    }

    {
        D3D11ScopedLock lock;
        d3d11Context->CopyResource(repeatSourceFrameTexture, sourceTexture);
        if (keyedSourceGuard.acquired) {
            // Submit the read before publishing key 0 back to the producer.
            // This is a queue flush, not a CPU/GPU completion wait.
            d3d11Context->Flush();
        }

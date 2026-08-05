#include "video_encoder_internal.h"

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

    }

    repeatSourceNeedsCursorRecompose = true;
    repeatSourceFrameWidth = frameWidth;
    repeatSourceFrameHeight = frameHeight;
    repeatSourceFrameIsHDR = isHDR;
    repeatSourceCaptureOriginX = captureOriginX;
    repeatSourceCaptureOriginY = captureOriginY;
    return true;
}

bool VideoEncoder::PopulateD3D11FrameFromRepeatSource(AVFrame* d3d11Frame) {
    if (!d3d11Frame || !repeatSourceFrameTexture || !repeatSourceNeedsCursorRecompose) {
        return false;
    }

    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    if (!useDirectRgbPath && captureCursor && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            return false;
        }
    }

    if (useDirectRgbPath) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, d3d11Frame, 0);
        if (frameRet < 0 || !d3d11Frame->data[0]) {
            DLL_Log("[VideoEncoder] RepeatLastFrame failed to allocate direct RGB repeat frame: %d", frameRet);
            return false;
        }

        return PrepareD3D11TextureForEncode(
            repeatSourceFrameTexture, reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]),
            CursorCompositionActive(), repeatSourceCaptureOriginX, repeatSourceCaptureOriginY, true);
    }

    if (!ConvertBGRAtoNV12(repeatSourceFrameTexture, d3d11Frame, CursorCompositionActive(), false,
                           repeatSourceCaptureOriginX, repeatSourceCaptureOriginY)) {
        return false;
    }

    d3d11Frame->width = scalingEnabled ? outputWidth : width;
    d3d11Frame->height = scalingEnabled ? outputHeight : height;
    return true;
}

bool VideoEncoder::CreateSharedCaptureTextures(uint32_t w, uint32_t h, uint32_t fmt, SharedMemoryLayout* sharedMem) {
    if (sharedCaptureTexturesCreated) {
        if (sharedCaptureTextureFormat == fmt) {
            return true;  // Already created with same format
        }
        // Format changed (e.g. DX9 BGRA→DX11 RGBA) — destroy and recreate
        DLL_Log("[VideoEncoder] KMT texture format changed %d -> %d, recreating", sharedCaptureTextureFormat, fmt);
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
            if (sharedCaptureTextures[i]) {
                sharedCaptureTextures[i]->Release();
                sharedCaptureTextures[i] = nullptr;
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
    }

    if (!d3d11Device) {
        DLL_Log("[VideoEncoder] CreateSharedCaptureTextures: No D3D11 device");
        return false;
    }

    DLL_Log("[VideoEncoder] Creating shared capture textures: %dx%d format=%d", w, h, fmt);

    // Create encoder-owned KMT shared textures (global WDDM handles for DXVK Vulkan import).
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
        // KMT-only texture (D3D11_RESOURCE_MISC_SHARED only)
        D3D11_TEXTURE2D_DESC kmtDesc = {};
        kmtDesc.Width = w;
        kmtDesc.Height = h;
        kmtDesc.MipLevels = 1;
        kmtDesc.ArraySize = 1;
        kmtDesc.Format = (DXGI_FORMAT)fmt;
        kmtDesc.SampleDesc.Count = 1;
        kmtDesc.SampleDesc.Quality = 0;
        kmtDesc.Usage = D3D11_USAGE_DEFAULT;
        kmtDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        kmtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = d3d11Device->CreateTexture2D(&kmtDesc, nullptr, &sharedCaptureTextures[i]);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create KMT shared texture %d: HR=%x", i, hr);
            return false;
        }

        // Get KMT handle via IDXGIResource::GetSharedHandle
        IDXGIResource* dxgiRes = nullptr;
        hr = sharedCaptureTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            DLL_Log("[VideoEncoder] Failed to get IDXGIResource for KMT texture %d: HR=%x", i, hr);
            return false;
        }

        hr = dxgiRes->GetSharedHandle(&sharedCaptureKmtHandles[i]);
        dxgiRes->Release();

        if (FAILED(hr) || !sharedCaptureKmtHandles[i]) {
            DLL_Log("[VideoEncoder] Failed to get KMT handle for texture %d: HR=%x", i, hr);
            return false;
        }

        DLL_Log("[VideoEncoder] Created KMT shared texture %d, kmtHandle=%p", i, sharedCaptureKmtHandles[i]);
    }

    // Create event for CPU-side fence waiting
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create shared fence
    HRESULT hr = d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedCaptureFence));
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to create shared fence: HR=%x", hr);
        return false;
    }

    // Export fence handle - CreateSharedHandle is on the fence object, not the
    // device
    hr = sharedCaptureFence->CreateSharedHandle(nullptr,      // Security attributes
                                                GENERIC_ALL,  // Access rights
                                                nullptr,      // Name (optional)
                                                &sharedCaptureFenceHandle);
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to export fence handle: HR=%x", hr);
        return false;
    }

    DLL_Log("[VideoEncoder] Created shared fence, handle=%p", sharedCaptureFenceHandle);

    // Publish to shared memory
    if (sharedMem) {
        this->pSharedMem = sharedMem;
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; i++) {
            sharedMem->encoderTextures.SetKmtTextureHandle(i, (uint64_t)sharedCaptureKmtHandles[i]);
        }
        sharedMem->encoderTextures.SetFenceHandle((uint64_t)sharedCaptureFenceHandle);
        sharedMem->encoderTextures.SetWidth(w);
        sharedMem->encoderTextures.SetHeight(h);
        sharedMem->encoderTextures.SetFormat(fmt);
        sharedMem->encoderTextures.kmtReady.store(true, std::memory_order_release);
        sharedMem->encoderTextures.ready.store(true, std::memory_order_release);
        DLL_Log("[VideoEncoder] Published encoder KMT textures to shared memory");
    }

    sharedCaptureTextureFormat = fmt;
    sharedCaptureTexturesCreated = true;
    return true;
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
    if (video_encoder_g_lastFramePts >= 0) {
        stats.actualPtsDiff = RoundUsToMs(timestamp - video_encoder_g_lastFramePts);
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
                        if (FAILED(fenceHr) && !video_encoder_g_HandleFailureCache.ShouldSkipFence(directFenceHandle)) {
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

                // Full Cache Hit
                bgraTex = cachedSharedTextures[cacheSlot];
                d3d11Fence = cachedD3D11Fence;  // May be null for D3D11 KMT path (no fence)
                bgraTex->AddRef();
                if (d3d11Fence) {
                    d3d11Fence->AddRef();
                }

                if (encodeFrameCounter % kCacheLogIntervalFrames == 1) {
                    DLL_Log("[VideoEncoder] Using cached handles (pid=%u, slot=%d, frame=%d)", sourcePid, cacheSlot,
                            encodeFrameCounter);
                }
            } else {
                // Cache Miss (Partial or Full)
                // Use RAII to ensure handle is closed if we return early
                ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));

                if (!hProcess) {
                    DLL_Log("[VideoEncoder] Frame %d: Failed to Open Process %u", encodeFrameCounter, sourcePid);
                    return false;
                }

                // 1. Handle Fence (Reuse if valid, Open if not)
                if (skipFence) {
                    d3d11Fence = nullptr;
                    if (encodeFrameCounter % 60 == 0)
                        DLL_Log("[VideoEncoder] Frame %d: SkipFence is true (Val=%llu Hnd=%p)", encodeFrameCounter,
                                fenceValue, fenceHandle);
                } else if (fenceValid) {
                    d3d11Fence = cachedD3D11Fence;
                    d3d11Fence->AddRef();
                } else {
                    ce::HandleGuard dupFence;
                    HRESULT hr = E_FAIL;

                    // CRITICAL: Always DuplicateHandle first to validate handles.
                    if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                        FALSE, DUPLICATE_SAME_ACCESS)) {
                        // Handle duplicated successfully - safe to call OpenSharedFence
                        hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                        if (FAILED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr, dupFence.get());
                        }
                    } else {
                        DWORD err = GetLastError();
                        if (encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u Hnd=%p)", err, sourcePid,
                                    fenceHandle);
                        }
                        // Last resort: try direct handle (may work for same-process or KMT handles)
                        if (!video_encoder_g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
                            hr = CallOpenSharedFence(d3d11Device, fenceHandle, &d3d11Fence);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Try direct first
                        hr = CallOpenSharedFence(d3d11Device, fenceHandleAlt, &d3d11Fence);
                        if (FAILED(hr)) {
                            if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                                dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                            }
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] OpenSharedFence(direct) failed: HR=%x (Hnd=%p), trying "
                            "DuplicateHandle...",
                            hr, fenceHandle);
                    }

                    // Fallback to DuplicateHandle path (for handles that support it)
                    if (FAILED(hr)) {
                        if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(), 0,
                                            FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            if (FAILED(hr)) {
                                DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr,
                                        dupFence.get());
                            }
                        } else {
                            DWORD err = GetLastError();
                            DLL_Log(
                                "[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u "
                                "Hnd=%p)",
                                err, sourcePid, fenceHandle);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Use DuplicateHandle first for safety
                        if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                            dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }

                    if (d3d11Fence && encodeFrameCounter < 10) {
                        DLL_Log("[VideoEncoder] Successfully opened shared fence for PID %u", sourcePid);
                    }
                    // Cache Fence if successfully opened
                    if (d3d11Fence) {
                        if (cachedD3D11Fence)
                            cachedD3D11Fence->Release();
                        cachedD3D11Fence = d3d11Fence;
                        cachedD3D11Fence->AddRef();
                        cachedFenceHandle = fenceHandle;
                        cachedSourcePid = sourcePid;
                    }
                }

                // 2. Open Texture (We know it's missing if we are here)
                ce::HandleGuard dupTex;
                HRESULT hr = E_FAIL;
                HRESULT hrNtDirect = E_FAIL;
                HRESULT hrNtDup = E_FAIL;
                HRESULT hrKmtDup = E_FAIL;
                HRESULT hrNtAltDirect = E_FAIL;
                HRESULT hrNtAltDup = E_FAIL;
                HRESULT hrKmtAltDup = E_FAIL;
                HRESULT hrKmtDirect = E_FAIL;
                HRESULT hrKmtAltDirect = E_FAIL;

                if (encodeFrameCounter < 10) {
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Opening shared texture: handle=%p, "
                        "sourcePid=%u (cached=%u, match=%s), format=%d",
                        encodeFrameCounter, sharedHandle, sourcePid, cachedSourcePid,
                        (sourcePid == cachedSourcePid) ? "yes" : "no", format);
                }

                if (sharedHandle == NULL) {
                    DLL_Log("[VideoEncoder] Frame %d: Error: sharedHandle is NULL", encodeFrameCounter);
                } else {
                    // D3D11 OpenSharedResource can throw SEH exceptions for invalid handles or
                    // incompatible formats. DuplicateHandle first to validate handle accessibility.
                    // Even duplicated handles can fail if D3D12/D3D11 devices are incompatible.
                    ce::HandleGuard dupTexDirect;
                    bool handleValid = DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                                                       dupTexDirect.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS);
                    if (handleValid) {
                        // Handle duplicated - try OpenSharedResource1 with the valid handle
                        hr = CallOpenSharedResource1(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrNtDup = hr;
                    } else {
                        hrNtDup = HRESULT_FROM_WIN32(GetLastError());
                        if (encodeFrameCounter < 10)
                            DLL_Log("[VideoEncoder] Frame %d: DuplicateHandle for texture failed: %p",
                                    encodeFrameCounter, sharedHandle);
                    }

                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: OpenSharedResource1(direct=%p) "
                            "failed HR=%x. Trying KMT path...",
                            encodeFrameCounter, sharedHandle, hr);
                    }

                    // Fallback to KMT path with duplicated handle
                    if (FAILED(hr) && handleValid) {
                        hr = CallOpenSharedResource(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrKmtDup = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened duplicated handle %p via KMT path",
                                    encodeFrameCounter, dupTexDirect.get());
                        }
                    }

                    // Try original handle as last resort (may work for same-process)
                    if (FAILED(hr) && !video_encoder_g_HandleFailureCache.ShouldSkipTexture(sharedHandle)) {
                        hr = CallOpenSharedResource(d3d11Device, sharedHandle, IID_PPV_ARGS(&bgraTex));
                        hrKmtDirect = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened handle %p via KMT direct path", encodeFrameCounter,
                                    sharedHandle);
                        }
                    }

                    // WOW64 producers publish a 32-bit handle value in the
                    // shared ABI. Try its normalized representation once, but
                    // do not retry either representation after a sleep: ring
                    // publication already supplies the required ordering.
                    if (FAILED(hr) && hasSharedAlt) {
                        ce::HandleGuard dupTexAlt;
                        if (DuplicateHandle(hProcess.get(), sharedHandleAlt, GetCurrentProcess(), dupTexAlt.addressof(),
                                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedResource1(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                            hrNtAltDup = hr;
                            if (FAILED(hr)) {
                                hr = CallOpenSharedResource(d3d11Device, dupTexAlt.get(), IID_PPV_ARGS(&bgraTex));
                                hrKmtAltDup = hr;
                            }
                        } else {
                            hrNtAltDup = HRESULT_FROM_WIN32(GetLastError());
                        }
                        if (FAILED(hr) && !video_encoder_g_HandleFailureCache.ShouldSkipTexture(sharedHandleAlt)) {
                            hr = CallOpenSharedResource(d3d11Device, sharedHandleAlt, IID_PPV_ARGS(&bgraTex));
                            hrKmtAltDirect = hr;
                        }
                    }

                    // Frame-ring publication uses release/acquire ordering, so
                    // a published handle cannot become more valid after an
                    // arbitrary sleep. Immediate retries only stalled the CFR
                    // encoder by up to 6 ms and repeated the same failing driver
                    // calls. Defer the frame to the existing bounded lineage
                    // retry path instead; a later publication/device state can
                    // then be observed without blocking the real-time thread.
                    if (FAILED(hr)) {
                        lastFrameDeferred.store(true, std::memory_order_relaxed);
                    }
                }  // end of else (sharedHandle != NULL)

                if (FAILED(hr)) {
                    static std::atomic<int> s_openDetailLogCount{0};
                    if (s_openDetailLogCount.fetch_add(1, std::memory_order_relaxed) < 16) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: Open detail h=%p alt=%p ntDir=%x ntDup=%x ntAltDir=%x "
                            "ntAltDup=%x "
                            "kmtDup=%x kmtAltDup=%x kmtDir=%x kmtAltDir=%x",
                            encodeFrameCounter, sharedHandle, sharedHandleAlt, hrNtDirect, hrNtDup, hrNtAltDirect,
                            hrNtAltDup, hrKmtDup, hrKmtAltDup, hrKmtDirect, hrKmtAltDirect);
                    }
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Failed to OpenSharedResource (NT/KMT) "
                        "HR=%x, handle=%p, sourcePid=%u, format=%d",
                        encodeFrameCounter, hr, sharedHandle, sourcePid, format);
                    // Clean up fence if we opened it but failed texture
                    if (d3d11Fence) {
                        d3d11Fence->Release();
                    }
                    return false;
                }

                // Cache Texture
                // Find empty cache slot.
                int targetSlot = 0;
                for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
                    if (cachedTextureHandles[i] == nullptr) {
                        targetSlot = i;
                        break;
                    }
                    if (i == SHARED_TEXTURE_SLOT_COUNT - 1)
                        targetSlot = 0;  // Fallback to 0 if all full
                }

                if (cachedSharedTextures[targetSlot]) {
                    cachedSharedTextures[targetSlot]->Release();
                }

                cachedSharedTextures[targetSlot] = bgraTex;
                cachedSharedTextures[targetSlot]->AddRef();
                cachedTextureHandles[targetSlot] = sharedHandle;

                // hProcess, dupTex, dupFence are auto-closed by RAII here
                cacheSlot = targetSlot;
            }
        }  // End of if (!bgraTex) - standard shared handle path
    }  // End of isShmem else block

    // 2. Wait on Synchronization using D3D11 Fence
    // PROTECTED: Immediate Context access
    D3D11ScopedLock lock;

    HRESULT hr = S_OK;

    // D3D11 FENCE PATH (Async GPU sync)
    auto beforeFence = PerfTimer::now();
    if (d3d11Fence) {
        // CPU-side timeout to prevent GPU hangs (Resilience improvement)
        if (!fenceEvent) {
            fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        // Check if fence is already satisfied (avoid SetEvent overhead if possible)
        if (d3d11Fence->GetCompletedValue() < fenceValue) {
            d3d11Fence->SetEventOnCompletion(fenceValue, fenceEvent);
            // Non-blocking fence check: poll with 0ms timeout instead of
            // blocking the encoder thread. At 100% GPU, the fence may take
            // 1-5ms to signal — blocking for 200ms collapses the cadence.
            // If the fence isn't ready, we skip this frame (return false)
            // and the Bresenham produces a duplicate. Stutter > corruption.
            DWORD waitRes = WaitForSingleObject(fenceEvent, 0);
            if (waitRes == WAIT_TIMEOUT) {
                // Fence not ready — skip this frame, encoder thread stays responsive
                bgraTex->Release();
                d3d11Fence->Release();
                d3d11Fence = nullptr;
                lastFrameDeferred.store(true, std::memory_order_relaxed);
                return false;
            }
        }

        // Async GPU Wait (plus CPU timeout check above)
        d3d11Context->Wait(d3d11Fence, fenceValue);
    }
    auto afterFence = PerfTimer::now();
    stats.fenceWaitMs = PerfTimer::elapsed_ms(beforeFence, afterFence);

    if (d3d11Fence) {
        d3d11Fence->Release();
        d3d11Fence = nullptr;
    }

    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Frame %d: Failed to Wait on Fence. HR=%x", encodeFrameCounter, hr);
        bgraTex->Release();
        return false;
    }

    auto afterOpen = PerfTimer::now();
    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    // 4. Ensure Video Processor is initialized before RGB -> YUV conversion.
    if (!useDirectRgbPath && !videoProcessorInit) {
        if (!InitVideoProcessor()) {
            DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
            bgraTex->Release();
            return false;
        }
    }

    auto beforeConvert = PerfTimer::now();
    AVFrame* d3d11Frame = av_frame_alloc();
    if (!d3d11Frame) {
        bgraTex->Release();
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
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        if (!PrepareD3D11TextureForEncode(bgraTex, (ID3D11Texture2D*)d3d11Frame->data[0], CursorCompositionActive(), 0,
                                          0)) {
            DLL_Log("[VideoEncoder] Frame %d: Direct D3D11 RGB preparation failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }
    } else {
        // 6. Point-composite a separate cursor in RGB, then convert the one
        // deterministic RGB stream to NV12/P010 on the GPU.
        if (!ConvertBGRAtoNV12(bgraTex, d3d11Frame, CursorCompositionActive(), true)) {
            DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
            bgraTex->Release();
            av_frame_free(&d3d11Frame);
            return false;
        }

        d3d11Frame->width = scalingEnabled ? outputWidth : width;
        d3d11Frame->height = scalingEnabled ? outputHeight : height;
    }

    auto afterConvert = PerfTimer::now();
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    bgraTex->Release();
    if (!ApplyFrameColorMetadata(d3d11Frame, codecCtx, savedConfig.hdrNominalPeakNits)) {
        av_frame_free(&d3d11Frame);
        return false;
    }

    // Calculate relative PTS (start from 0) — timestamp is in microseconds
    const bool commitsStartPts = startPts.load(std::memory_order_relaxed) < 0;
    const int64_t effectiveStartPts = commitsStartPts ? timestamp : startPts.load(std::memory_order_relaxed);
    const int64_t targetPts = ComputeTargetVideoPts(timestamp, savedConfig.useVFR, savedConfig.fps, effectiveStartPts,
                                                    lastAssignedVideoPts, false);

    // 5. Encode (Direct D3D11 Frame) - with proper packet draining
    AVPacket* pkt = av_packet_alloc();
    int packetCount = 0;

    // Pure encoding time measurement (excluding color conversion/wait)
    auto encodeStart = PerfTimer::now();

    // Helper lambda to drain all available packets
    auto drainPackets = [&]() {
        while (true) {
            const auto receiveStart = PerfTimer::now();
            int ret = avcodec_receive_packet(codecCtx, pkt);
            const auto receiveEnd = PerfTimer::now();
            encoderReceiveAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(receiveStart, receiveEnd) * 1000.0));
            ++encoderReceiveCalls;
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                DLL_Log("[VideoEncoder] avcodec_receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            const int64_t packetKey = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const auto submitIt = encoderSubmitQpcByPts.find(packetKey);
            if (submitIt != encoderSubmitQpcByPts.end()) {
                LARGE_INTEGER nowQpc = {};
                LARGE_INTEGER frequency = {};
                QueryPerformanceCounter(&nowQpc);
                QueryPerformanceFrequency(&frequency);
                if (frequency.QuadPart > 0 && nowQpc.QuadPart >= submitIt->second) {
                    const uint64_t latencyUs = static_cast<uint64_t>(nowQpc.QuadPart - submitIt->second) * 1000000ull /
                                               static_cast<uint64_t>(frequency.QuadPart);
                    encoderPacketLatencyAccumUs += latencyUs;
                    ++encoderPacketLatencySamples;
                    encoderPacketLatencyMaxUs = std::max(encoderPacketLatencyMaxUs, SaturatingToUint32(latencyUs));
                }
                encoderSubmitQpcByPts.erase(submitIt);
            }
            pkt->stream_index = stream->index;  // Ensure video stream index

            // Duration Logic
            if (savedConfig.useVFR) {
                // For VFR, duration is variable. Best guess is target frame duration.
                // Since time_base is 1us, duration is in us.
                pkt->duration = 1000000 / savedConfig.fps;
            } else {
                // For CFR, duration is 1 unit (1/FPS)
                pkt->duration = 1;
            }

            if (onPacket)
                onPacket(pkt);
            av_packet_unref(pkt);
        }
    };

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        auto timedSend = [&](AVFrame* sendTarget) {
            const auto sendStart = PerfTimer::now();
            const int result = avcodec_send_frame(codecCtx, sendTarget);
            const auto sendEnd = PerfTimer::now();
            encoderSendAccumUs +=
                static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(sendStart, sendEnd) * 1000.0));
            ++encoderSendCalls;
            return result;
        };
        int ret = timedSend(frame);
        int retries = 0;
        if (ret == AVERROR(EAGAIN))
            ++encoderEagainDrainCount;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = timedSend(frame);
            retries++;
        }
        if (ret == AVERROR(EAGAIN)) {
            DLL_Log("[VideoEncoder] avcodec_send_frame remained EAGAIN after %d drain attempts", retries);
            return false;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] avcodec_send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        if (frame && frame->pts != AV_NOPTS_VALUE) {
            LARGE_INTEGER submitted = {};
            QueryPerformanceCounter(&submitted);
            encoderSubmitQpcByPts[frame->pts] = submitted.QuadPart;
            while (encoderSubmitQpcByPts.size() > 256)
                encoderSubmitQpcByPts.erase(encoderSubmitQpcByPts.begin());
        }
        drainPackets();
        return true;
    };

    bool success = true;

    d3d11Frame->pts = targetPts;
    AVFrame* encoderInputFrame = PrepareEncoderInputFrame(d3d11Frame);

    if (encoderInputFrame) {
        success = sendFrame(encoderInputFrame);
    } else {
        success = false;
    }
    if (encoderInputFrame != d3d11Frame) {
        av_frame_free(&encoderInputFrame);
    }

    const uint64_t timingNow = GetTickCount64();
    if (encoderTimingLastLogTick == 0 || timingNow - encoderTimingLastLogTick >= 1000) {
        const uint64_t sendAvgUs = encoderSendCalls > 0 ? encoderSendAccumUs / encoderSendCalls : 0;
        const uint64_t receiveAvgUs = encoderReceiveCalls > 0 ? encoderReceiveAccumUs / encoderReceiveCalls : 0;
        const uint64_t packetLatencyAvgUs =
            encoderPacketLatencySamples > 0 ? encoderPacketLatencyAccumUs / encoderPacketLatencySamples : 0;
        DLL_Log(
            "[VideoEncoder Timing] sendAvg=%lluus receiveAvg=%lluus submitToPacket=%llu/%uus "
            "eagainDrain=%u pendingPts=%zu",
            static_cast<unsigned long long>(sendAvgUs), static_cast<unsigned long long>(receiveAvgUs),
            static_cast<unsigned long long>(packetLatencyAvgUs), encoderPacketLatencyMaxUs, encoderEagainDrainCount,
            encoderSubmitQpcByPts.size());
        encoderSendAccumUs = 0;
        encoderSendCalls = 0;
        encoderReceiveAccumUs = 0;
        encoderReceiveCalls = 0;
        encoderPacketLatencyAccumUs = 0;
        encoderPacketLatencySamples = 0;
        encoderPacketLatencyMaxUs = 0;
        encoderEagainDrainCount = 0;
        encoderTimingLastLogTick = timingNow;
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

    if (!success) {
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
    CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
    video_encoder_g_totalFenceWait += stats.fenceWaitMs;
    video_encoder_g_totalColorConvert += stats.colorConvertMs;
    video_encoder_g_totalEncode += stats.encodeMs;
    if (stats.totalMs > video_encoder_g_maxFrameTime)
        video_encoder_g_maxFrameTime = stats.totalMs;
    if (stats.totalMs > expectedFrameMs * 2)
        video_encoder_g_slowFrameCount++;

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

    av_frame_free(&d3d11Frame);  // Releases D3D11 Tex

    return true;
}

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
                        outputFilename.c_str());
            }
            BeginDeferredRecording();
        } else if (!preservedOutputFilename.empty()) {
            outputReservation = std::move(preservedOutputReservation);
            outputFilename = preservedOutputFilename;
            DLL_Log("[VideoEncoder] Restored deferred staging output for first WGC frame: %s",
                    outputFilename.c_str());
            BeginDeferredRecording();
        }
    }

    // Ensure encoder is initialized with hardware context
    if (!EnsureDevice())
        return false;

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            if (!outputReservation.ReleaseToWriter()) {
                DLL_Log("[VideoEncoder] ERROR: Reserved output identity changed before WGC mux open: %s",
                        outputFilename.c_str());
                return false;
            }
            int ret = avio_open(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                DLL_Log("Failed to open output file: %d", ret);
                return false;
            }
        }
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "2000000", 0);
        }
        if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(fmtCtx)) {
            DLL_Log("[VideoEncoder] ERROR: Matroska timestamp_precision=1000 is required but unavailable");
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close rejected WGC output: %d", closeResult);
            }
            return false;
        }
        if (!ValidateFormatContextForHeader(fmtCtx)) {
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close invalid WGC output context: %d", closeResult);
            }
            return false;
        }

        const int headerResult = avformat_write_header(fmtCtx, nullptr);
        if (headerResult < 0) {
            DLL_Log("Failed to write header: %d", headerResult);
            if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
                const int closeResult = avio_closep(&fmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("[VideoEncoder] ERROR: Failed to close WGC output after header failure: %d", closeResult);
            }
            return false;
        }
        fileOpened = true;
    }

    return true;
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

    const bool recomposeCursorForRepeats = CursorCompositionActive() && cursorRenderer;
    if (!recomposeCursorForRepeats && repeatSourceNeedsCursorRecompose) {
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
            CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
            if (recomposeCursorForRepeats &&
                !CacheRepeatSourceFrameTexture(bgraTexture, frameWidth, frameHeight, isHDR, captureLeft, captureTop)) {
                repeatSourceNeedsCursorRecompose = false;
            }
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

    av_frame_free(&d3d11Frame);
    return true;
}

bool VideoEncoder::RepeatLastFrame(int64_t timestamp, bool useExplicitCfrTimeline) {
    if (!recordingRequested) {
        return false;
    }

    lastFrameDeferred.store(false, std::memory_order_relaxed);

    const bool recomposeCursorForRepeat = repeatSourceNeedsCursorRecompose && repeatSourceFrameTexture;
    if (!repeatFrameTexture && !recomposeCursorForRepeat) {
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
    if (recomposeCursorForRepeat) {
        populatedFromRepeatSource = PopulateD3D11FrameFromRepeatSource(d3d11Frame);
        if (!populatedFromRepeatSource) {
            if (!repeatCursorRecomposeFallbackLogged) {
                DLL_Log("[VideoEncoder] Cursor-aware repeat recompose failed; falling back to cached duplicate frame");
                repeatCursorRecomposeFallbackLogged = true;
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
        cursorAwareRepeatRenderCount++;
        CacheRepeatFrameTexture(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]));
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

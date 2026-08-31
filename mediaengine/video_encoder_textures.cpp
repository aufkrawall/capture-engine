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

    const FaceCameraColorMode faceCameraColorMode =
        ShouldEncodeHdrOutput() && dstDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM
            ? FaceCameraColorMode::Hdr10Pq
            : FaceCameraColorMode::Sdr;
    const float faceCameraPaperWhiteNits =
        faceCameraColorMode == FaceCameraColorMode::Sdr ? 80.0f : sdrWhiteNits;
    CompositeFaceCameraOntoRgb(normalizedTexture, faceCameraColorMode, faceCameraPaperWhiteNits);

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
    if (pendingRepeatSourceFrameTexture) {
        pendingRepeatSourceFrameTexture->Release();
        pendingRepeatSourceFrameTexture = nullptr;
    }
    pendingRepeatSourceFrameValid = false;
    repeatSourceNeedsOverlayRecompose = false;
    repeatSourceFrameWidth = 0;
    repeatSourceFrameHeight = 0;
    repeatSourceFrameIsHDR = false;
    repeatSourceCaptureOriginX = 0;
    repeatSourceCaptureOriginY = 0;
}

namespace {

void ConfigureRepeatSourceCacheDesc(D3D11_TEXTURE2D_DESC* desc) {
    if (!desc)
        return;
    desc->BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc->MiscFlags = 0;
    desc->CPUAccessFlags = 0;
    desc->Usage = D3D11_USAGE_DEFAULT;

    // Repeat sources are encoder-owned, unlike the producer's shared capture
    // texture. Give compatible single-sample RGB caches render-target binding
    // so camera/cursor CFR recomposition can use small transactional restores
    // instead of allocating and copying another full frame on every repeat.
    const DXGI_FORMAT viewFormat = ce::video_format::GetRgbShaderResourceViewFormat(desc->Format);
    if (desc->SampleDesc.Count == 1 && viewFormat != DXGI_FORMAT_UNKNOWN) {
        desc->BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
}

bool RepeatSourceTextureMatches(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& wanted) {
    if (!texture)
        return false;
    D3D11_TEXTURE2D_DESC existing = {};
    texture->GetDesc(&existing);
    constexpr UINT kRenderTargetBind = D3D11_BIND_RENDER_TARGET;
    const bool compatibleBindFlags =
        existing.BindFlags == wanted.BindFlags ||
        ((wanted.BindFlags & kRenderTargetBind) != 0 &&
         existing.BindFlags == (wanted.BindFlags & ~kRenderTargetBind));
    return existing.Width == wanted.Width && existing.Height == wanted.Height && existing.Format == wanted.Format &&
           compatibleBindFlags && existing.ArraySize == wanted.ArraySize &&
           existing.MipLevels == wanted.MipLevels && existing.SampleDesc.Count == wanted.SampleDesc.Count &&
           existing.SampleDesc.Quality == wanted.SampleDesc.Quality;
}

HRESULT CreateRepeatSourceTexture(ID3D11Device* device, D3D11_TEXTURE2D_DESC* desc,
                                  ID3D11Texture2D** texture, bool* usedRenderTargetFallback) {
    if (!device || !desc || !texture)
        return E_INVALIDARG;
    *texture = nullptr;
    if (usedRenderTargetFallback)
        *usedRenderTargetFallback = false;
    HRESULT hr = device->CreateTexture2D(desc, nullptr, texture);
    constexpr UINT kRenderTargetBind = D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr) && (desc->BindFlags & kRenderTargetBind) != 0) {
        if (*texture) {
            (*texture)->Release();
            *texture = nullptr;
        }
        desc->BindFlags &= ~kRenderTargetBind;
        hr = device->CreateTexture2D(desc, nullptr, texture);
        if (SUCCEEDED(hr) && usedRenderTargetFallback)
            *usedRenderTargetFallback = true;
    }
    if (FAILED(hr) && *texture) {
        (*texture)->Release();
        *texture = nullptr;
    }
    return hr;
}

struct RepeatSourceKeyedGuard {
    IDXGIKeyedMutex* mutex = nullptr;
    bool acquired = false;

    ~RepeatSourceKeyedGuard() {
        if (!mutex)
            return;
        if (acquired)
            mutex->ReleaseSync(0);
        mutex->Release();
    }
};

bool AcquireRepeatSource(ID3D11Texture2D* sourceTexture, RepeatSourceKeyedGuard* guard) {
    if (!sourceTexture || !guard)
        return false;
    sourceTexture->QueryInterface(IID_PPV_ARGS(&guard->mutex));
    if (!guard->mutex)
        return true;
    const HRESULT hr = guard->mutex->AcquireSync(0, 0);
    guard->acquired = hr == S_OK;
    return guard->acquired;
}

}  // namespace

bool VideoEncoder::StageRepeatSourceFrameTexture(ID3D11Texture2D* sourceTexture) {
    pendingRepeatSourceFrameValid = false;
    if (!sourceTexture || !d3d11Device || !d3d11Context)
        return false;

    RepeatSourceKeyedGuard keyedSourceGuard;
    if (!AcquireRepeatSource(sourceTexture, &keyedSourceGuard)) {
        ++repeatSourceCacheKeyedAcquireFailCount;
        if (repeatSourceCacheKeyedAcquireFailCount <= 5) {
            DLL_Log("[VideoEncoder] Staged dynamic-overlay source keyed-mutex acquire failed: failures=%llu",
                    static_cast<unsigned long long>(repeatSourceCacheKeyedAcquireFailCount));
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC cacheDesc = {};
    sourceTexture->GetDesc(&cacheDesc);
    ConfigureRepeatSourceCacheDesc(&cacheDesc);
    if (!RepeatSourceTextureMatches(pendingRepeatSourceFrameTexture, cacheDesc)) {
        if (pendingRepeatSourceFrameTexture) {
            pendingRepeatSourceFrameTexture->Release();
            pendingRepeatSourceFrameTexture = nullptr;
        }
        bool usedRenderTargetFallback = false;
        const HRESULT hr = CreateRepeatSourceTexture(d3d11Device, &cacheDesc, &pendingRepeatSourceFrameTexture,
                                                     &usedRenderTargetFallback);
        if (FAILED(hr)) {
            if (!repeatSourceCacheFailureLogged) {
                DLL_Log("[VideoEncoder] Failed to stage dynamic-overlay repeat source: HR=%x fmt=%d %ux%u", hr,
                        cacheDesc.Format, cacheDesc.Width, cacheDesc.Height);
                repeatSourceCacheFailureLogged = true;
            }
            return false;
        }
        if (usedRenderTargetFallback && !repeatSourceRenderTargetFallbackLogged) {
            DLL_Log(
                "[VideoEncoder] Dynamic-overlay repeat source lacks render-target support; "
                "CFR overlay updates use the full-frame GPU compatibility path");
            repeatSourceRenderTargetFallbackLogged = true;
        }
    }

    {
        D3D11ScopedLock lock;
        d3d11Context->CopyResource(pendingRepeatSourceFrameTexture, sourceTexture);
        if (keyedSourceGuard.acquired)
            d3d11Context->Flush();
    }
    pendingRepeatSourceFrameValid = true;
    return true;
}

void VideoEncoder::CommitStagedRepeatSourceFrameTexture(uint32_t frameWidth, uint32_t frameHeight, bool isHDR,
                                                        int captureOriginX, int captureOriginY) {
    if (!pendingRepeatSourceFrameValid || !pendingRepeatSourceFrameTexture)
        return;
    std::swap(repeatSourceFrameTexture, pendingRepeatSourceFrameTexture);
    pendingRepeatSourceFrameValid = false;
    repeatSourceNeedsOverlayRecompose = true;
    repeatSourceFrameWidth = frameWidth;
    repeatSourceFrameHeight = frameHeight;
    repeatSourceFrameIsHDR = isHDR;
    repeatSourceCaptureOriginX = captureOriginX;
    repeatSourceCaptureOriginY = captureOriginY;
}

void VideoEncoder::DiscardStagedRepeatSourceFrameTexture() {
    pendingRepeatSourceFrameValid = false;
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
                    "[VideoEncoder] Dynamic-overlay repeat source cache keyed-mutex acquire failed: "
                    "HR=%x failures=%llu",
                    kmHr, static_cast<unsigned long long>(repeatSourceCacheKeyedAcquireFailCount));
            }
            return false;
        }
        keyedSourceGuard.acquired = true;
        if (!repeatSourceCacheKeyedMutexLogged) {
            DLL_Log("[VideoEncoder] Dynamic-overlay repeat source cache synchronized at keyed mutex 0->0");
            repeatSourceCacheKeyedMutexLogged = true;
        }
    }

    D3D11_TEXTURE2D_DESC cacheDesc = srcDesc;
    ConfigureRepeatSourceCacheDesc(&cacheDesc);

    const bool needsRecreate = !RepeatSourceTextureMatches(repeatSourceFrameTexture, cacheDesc);

    if (needsRecreate) {
        if (repeatSourceFrameTexture) {
            repeatSourceFrameTexture->Release();
            repeatSourceFrameTexture = nullptr;
        }
        repeatSourceNeedsOverlayRecompose = false;
        bool usedRenderTargetFallback = false;
        const HRESULT hr = CreateRepeatSourceTexture(d3d11Device, &cacheDesc, &repeatSourceFrameTexture,
                                                     &usedRenderTargetFallback);
        if (FAILED(hr)) {
            if (!repeatSourceCacheFailureLogged) {
                DLL_Log("[VideoEncoder] Failed to create dynamic-overlay repeat source texture: HR=%x fmt=%d %ux%u", hr,
                        cacheDesc.Format, cacheDesc.Width, cacheDesc.Height);
                repeatSourceCacheFailureLogged = true;
            }
            return false;
        }
        if (usedRenderTargetFallback && !repeatSourceRenderTargetFallbackLogged) {
            DLL_Log(
                "[VideoEncoder] Dynamic-overlay repeat source lacks render-target support; "
                "CFR overlay updates use the full-frame GPU compatibility path");
            repeatSourceRenderTargetFallbackLogged = true;
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

    repeatSourceNeedsOverlayRecompose = true;
    repeatSourceFrameWidth = frameWidth;
    repeatSourceFrameHeight = frameHeight;
    repeatSourceFrameIsHDR = isHDR;
    repeatSourceCaptureOriginX = captureOriginX;
    repeatSourceCaptureOriginY = captureOriginY;
    return true;
}

bool VideoEncoder::PopulateD3D11FrameFromRepeatSource(AVFrame* d3d11Frame) {
    if (!d3d11Frame || !repeatSourceFrameTexture || !repeatSourceNeedsOverlayRecompose) {
        return false;
    }

    const AVPixelFormat activeSwFormat = GetActiveD3D11SwFormat();
    const bool useDirectRgbPath = IsDirectRgbD3D11SwFormat(activeSwFormat);

    if (!useDirectRgbPath && !videoProcessorInit) {
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

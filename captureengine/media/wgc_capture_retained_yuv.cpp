#include "wgc_capture_internal.h"

#include "wgc_shared_planar.h"

// Retained NV12/P010 smoothness reservoir (llm-wiki/wgc-reservoir-vram-design.md).
// The reservoir keeps its delay and frame count; each admitted frame is
// converted at ingress by the media engine with the encoder's own shaders and
// parameters, so the slot holds the codes the encoder would have produced.
// With a split capture device (always for DXGI duplication) the slot is a
// shared NT keyed-mutex texture of the encoder's device, converted on the
// capture device and published at key 1 exactly like the RGB slots.

#if HAS_WGC

uint32_t WGCCapture::Impl::BitsPerPixelForFormat(DXGI_FORMAT format) const {
    switch (format) {
        case DXGI_FORMAT_NV12:
            return 12;
        case DXGI_FORMAT_P010:
            return 24;
        default:
            return BytesPerPixelForFormat(format) * 8u;
    }
}

bool WGCCapture::Impl::SharesEncoderContext() const {
    return d3dDevice_ && encoderDevice_ && d3dDevice_ == encoderDevice_;
}

bool WGCCapture::Impl::IsRetainedYuvRoute(DXGI_FORMAT sourceFormat) const {
    return retainedRouteValid_ && retainedRoute_.yuvFormat != 0 && retainedRouteSourceFormat_ == sourceFormat;
}

void WGCCapture::Impl::RefreshRetainedRoute(uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat) {
    const bool hdr = captureIsHDR_.load(std::memory_order_relaxed);
    const bool sameDevice = SharesEncoderContext();
    bool splitShareNv12 = false;
    bool splitShareP010 = false;
    if (retainedRouteValid_ && retainedRouteSourceFormat_ == sourceFormat && retainedRouteWidth_ == width &&
        retainedRouteHeight_ == height && retainedRouteHdr_ == hdr && retainedRouteSameDevice_ == sameDevice) {
        return;
    }
    retainedRoute_ = MediaEngineRetainedFrameRoute{};
    const char* reason = nullptr;
    if (!retainedYuvEnabled_) {
        reason = "disabled by wgc_retained_yuv=false";
    } else if (!MediaEngine_QueryRetainedFrameRoute || !MediaEngine_ConvertToRetainedFrame) {
        reason = "media engine has no retained-frame API";
    } else {
        // Never called with the D3D11 context lock held: the query waits for the
        // media engine's API lock, which an encode holds while it takes that lock.
        MediaEngineRetainedFrameQuery query;
        query.sourceFormat = static_cast<uint32_t>(sourceFormat);
        query.width = width;
        query.height = height;
        query.sourceIsHdr = hdr;
        query.sameDevice = sameDevice;
        if (!sameDevice && !retainedSplitShareFailed_) {
            ProbeSplitYuvSharing(&splitShareNv12, &splitShareP010);
        }
        query.splitShareNv12 = splitShareNv12;
        query.splitShareP010 = splitShareP010;
        if (!MediaEngine_QueryRetainedFrameRoute(&query, &retainedRoute_)) {
            retainedRoute_ = MediaEngineRetainedFrameRoute{};
            reason = "route query rejected";
        }
    }
    if (reason) {
        std::snprintf(retainedRoute_.reason, sizeof(retainedRoute_.reason), "%s", reason);
    }
    retainedRouteValid_ = true;
    retainedRouteSourceFormat_ = sourceFormat;
    retainedRouteWidth_ = width;
    retainedRouteHeight_ = height;
    retainedRouteHdr_ = hdr;
    retainedRouteSameDevice_ = sameDevice;
    const DXGI_FORMAT yuvFormat = static_cast<DXGI_FORMAT>(retainedRoute_.yuvFormat);
    LogInfo("[WGC] Retained reservoir route: source=%s %ux%u hdr=%d sameDevice=%d splitShare=nv12:%d,p010:%d "
            "splitShareFailed=%d backend=%s -> %s (%s) bitsPerPixel=%u->%u",
            DxgiFormatName(sourceFormat), width, height, hdr ? 1 : 0, sameDevice ? 1 : 0, splitShareNv12 ? 1 : 0,
            splitShareP010 ? 1 : 0, retainedSplitShareFailed_ ? 1 : 0, useDuplicationBackend_ ? "dxgi_dup" : "wgc",
            yuvFormat != 0 ? DxgiFormatName(yuvFormat) : "RGB", retainedRoute_.reason,
            BitsPerPixelForFormat(sourceFormat), BitsPerPixelForFormat(yuvFormat != 0 ? yuvFormat : sourceFormat));
}

void WGCCapture::Impl::ProbeSplitYuvSharing(bool* nv12, bool* p010) const {
    const bool split = usingDedicatedCaptureDevice_ && encoderDevice_ && d3dDevice_ && encoderDevice_ != d3dDevice_;
    *nv12 = split && ce::capture_policy::DeviceSharesPlanarFormat(encoderDevice_, DXGI_FORMAT_NV12) &&
            ce::capture_policy::DeviceSharesPlanarFormat(d3dDevice_, DXGI_FORMAT_NV12);
    *p010 = split && ce::capture_policy::DeviceSharesPlanarFormat(encoderDevice_, DXGI_FORMAT_P010) &&
            ce::capture_policy::DeviceSharesPlanarFormat(d3dDevice_, DXGI_FORMAT_P010);
}

bool WGCCapture::Impl::OpenSplitYuvPoolSlot(uint32_t index) {
    ID3D11Texture2D* slot = index < texturePool_.size() ? texturePool_[index] : nullptr;
    if (!slot || index >= captureTexturePool_.size() || index >= captureTextureMutexPool_.size()) {
        return false;
    }
    IDXGIResource1* resource = nullptr;
    HANDLE handle = nullptr;
    HRESULT hr = slot->QueryInterface(IID_PPV_ARGS(&resource));
    if (SUCCEEDED(hr) && resource) {
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &handle);
    }
    SafeRelease(resource);
    ID3D11Device1* device1 = nullptr;
    if (SUCCEEDED(hr) && handle) {
        hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&device1));
    }
    if (SUCCEEDED(hr) && device1) {
        hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&captureTexturePool_[index]));
    }
    SafeRelease(device1);
    if (handle) {
        CloseHandle(handle);  // the opened texture keeps the shared resource alive
    }
    if (SUCCEEDED(hr) && captureTexturePool_[index]) {
        hr = captureTexturePool_[index]->QueryInterface(IID_PPV_ARGS(&captureTextureMutexPool_[index]));
    }
    if (FAILED(hr) || !captureTexturePool_[index] || !captureTextureMutexPool_[index]) {
        LogWarn("[WGC] Shared retained-YUV slot %u could not be opened on the capture device: 0x%08lX", index,
                (unsigned long)hr);
        return false;
    }
    return true;
}

bool WGCCapture::Impl::FallBackFromSplitYuvPool(uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat,
                                                uint32_t index, const char* stage, HRESULT hr) {
    LogWarn("[WGC] Shared retained-YUV pool unavailable (%s failed at slot %u: 0x%08lX, fmt=%s); rebuilding the "
            "RGB reservoir for this capture",
            stage, index, (unsigned long)hr, DxgiFormatName(static_cast<DXGI_FORMAT>(retainedRoute_.yuvFormat)));
    ReleaseTexturePool();
    retainedSplitShareFailed_ = true;
    retainedRouteValid_ = false;
    return EnsureTexturePool(width, height, sourceFormat);
}

bool WGCCapture::Impl::CreateRetainedYuvPlaneViews(ID3D11Texture2D* slot, uint32_t index) {
    if (!slot || index >= poolRenderTargetViews_.size() || index >= poolChromaRenderTargetViews_.size()) {
        return false;
    }
    ID3D11Device3* device3 = nullptr;
    if (FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&device3))) || !device3) {
        LogError("[WGC] Retained-YUV pool needs ID3D11Device3 for plane views");
        return false;
    }
    const bool eightBit = static_cast<DXGI_FORMAT>(retainedRoute_.yuvFormat) == DXGI_FORMAT_NV12;
    auto planeView = [&](DXGI_FORMAT format, UINT plane, ID3D11RenderTargetView** out) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.PlaneSlice = plane;
        ID3D11RenderTargetView1* view = nullptr;
        const HRESULT hr = device3->CreateRenderTargetView1(slot, &desc, &view);
        *out = view;
        return hr;
    };
    const HRESULT lumaHr =
        planeView(eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM, 0, &poolRenderTargetViews_[index]);
    const HRESULT chromaHr = planeView(eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1,
                                       &poolChromaRenderTargetViews_[index]);
    SafeRelease(device3);
    if (FAILED(lumaHr) || FAILED(chromaHr)) {
        LogError("[WGC] Retained-YUV plane RTV creation failed for pool[%u] fmt=%s: 0x%08lX/0x%08lX", index,
                 DxgiFormatName(static_cast<DXGI_FORMAT>(retainedRoute_.yuvFormat)), (unsigned long)lumaHr,
                 (unsigned long)chromaHr);
        return false;
    }
    return true;
}

bool WGCCapture::Impl::ConvertFrameToRetainedYuvSlot(ID3D11Texture2D* sourceTexture,
                                                     const D3D11_TEXTURE2D_DESC& sourceDesc, uint32_t index,
                                                     bool* usedStaging) {
    const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format);
    if (inputSrvFormat == DXGI_FORMAT_UNKNOWN || index >= poolRenderTargetViews_.size()) {
        return false;
    }
    ID3D11ShaderResourceView* sourceSrv = nullptr;
    if (!CreatePoolCopySourceSrv(sourceTexture, sourceDesc, inputSrvFormat, &sourceSrv, usedStaging)) {
        return false;
    }
    MediaEngineRetainedFrameConversion conversion;
    conversion.sourceView = sourceSrv;
    conversion.width = sourceDesc.Width;
    conversion.height = sourceDesc.Height;
    conversion.lumaTarget = poolRenderTargetViews_[index];
    conversion.chromaTarget = poolChromaRenderTargetViews_[index];
    conversion.route = retainedRoute_;
    const bool converted = MediaEngine_ConvertToRetainedFrame(&conversion);
    SafeRelease(sourceSrv);
    if (!converted) {
        const uint32_t failures = retainedYuvConvertFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures <= 4 || (failures % 1000u) == 0u) {
            LogWarn("[WGC] Retained-YUV ingress conversion failed: slot=%u sourceFmt=%s retainedFmt=%s failures=%u",
                    index, DxgiFormatName(sourceDesc.Format),
                    DxgiFormatName(static_cast<DXGI_FORMAT>(retainedRoute_.yuvFormat)), failures);
        }
    }
    return converted;
}

#endif

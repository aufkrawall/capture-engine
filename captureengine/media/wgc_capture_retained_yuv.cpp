#include "wgc_capture_internal.h"

// Retained NV12/P010 smoothness reservoir (llm-wiki/wgc-reservoir-vram-design.md).
// The reservoir keeps its delay and frame count; each admitted frame is
// converted at ingress by the media engine with the encoder's own shaders and
// parameters, so the slot holds the codes the encoder would have produced.

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
    LogInfo("[WGC] Retained reservoir route: source=%s %ux%u hdr=%d sameDevice=%d backend=%s -> %s (%s) "
            "bitsPerPixel=%u->%u",
            DxgiFormatName(sourceFormat), width, height, hdr ? 1 : 0, sameDevice ? 1 : 0,
            useDuplicationBackend_ ? "dxgi_dup" : "wgc", yuvFormat != 0 ? DxgiFormatName(yuvFormat) : "RGB",
            retainedRoute_.reason, BitsPerPixelForFormat(sourceFormat),
            BitsPerPixelForFormat(yuvFormat != 0 ? yuvFormat : sourceFormat));
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

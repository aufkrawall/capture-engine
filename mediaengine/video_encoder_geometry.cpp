#include "video_encoder_internal.h"

#include "encode_geometry_policy.h"

// A capture source that changes after the output opened (see
// encode_geometry_policy.h): size changes are fitted into the locked frame
// geometry, colour-contract changes end the recording with its file intact.

void VideoEncoder::SetCursorCaptureState(const ce::cursor::CaptureState& state) {
    rawCursorCaptureState = state;
    ApplyGeometryFitToCursorState();
}

void VideoEncoder::ApplyGeometryFitToCursorState() {
    if (!geometryFitActive) {
        cursorCaptureState = rawCursorCaptureState;
        return;
    }
    const ce::encode_geometry::Rect fit{geometryFitX, geometryFitY, geometryFitW, geometryFitH};
    cursorCaptureState =
        ce::encode_geometry::AdjustCursorForFit(rawCursorCaptureState, geometryFitSourceWidth, geometryFitSourceHeight,
                                                lockedGeometryWidth, lockedGeometryHeight, fit);
}

void VideoEncoder::ReleaseGeometryFitResources() {
    if (geometryFitRTV) {
        geometryFitRTV->Release();
        geometryFitRTV = nullptr;
    }
    if (geometryFitTexture) {
        geometryFitTexture->Release();
        geometryFitTexture = nullptr;
    }
    geometryFitFormat = DXGI_FORMAT_UNKNOWN;
    lastFittedSourceWidth = 0;
    lastFittedSourceHeight = 0;
    geometryFitActive = false;
    cursorCaptureState = rawCursorCaptureState;
}

void VideoEncoder::RequestStopForSourceContractChange(const char* reason, bool sourceIsHdr, bool sourceIs10Bit) {
    if (sourceContractChanged.exchange(true, std::memory_order_acq_rel)) {
        return;  // already requested; frames of the new contract are refused until the stop lands
    }
    DLL_Log(
        "[VideoEncoder] ERROR: capture colour contract changed after the output opened (%s: hdr=%d->%d "
        "10bit=%d->%d); the encoder cannot convert into the committed contract, and re-opening the output would "
        "overwrite it. Ending the recording here; everything recorded so far is kept and the completion is "
        "reported as degraded%s",
        reason ? reason : "unknown", currentIsHDR ? 1 : 0, sourceIsHdr ? 1 : 0, currentUse10BitInput ? 1 : 0,
        sourceIs10Bit ? 1 : 0, isStopping.load(std::memory_order_acquire) ? "" : " - stopping the recording now");
    if (!isStopping.load(std::memory_order_acquire) && pSharedMem) {
        pSharedMem->runtimeState.cmdStopRecording.store(true, std::memory_order_release);
    }
}

bool VideoEncoder::FitSourceToLockedGeometry(ID3D11Texture2D* source, ID3D11Texture2D** fitted) {
    if (!fitted) {
        return false;
    }
    *fitted = nullptr;
    if (!source) {
        return false;
    }
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    if (ce::encode_geometry::ClassifySourceChange(fileOpened, false, sourceDesc.Width, sourceDesc.Height,
                                                  lockedGeometryWidth, lockedGeometryHeight) !=
        ce::encode_geometry::SourceChangeAction::kFitIntoLockedFrame) {
        if (geometryFitActive) {
            geometryFitActive = false;
            ApplyGeometryFitToCursorState();
            DLL_Log("[VideoEncoder] Source matches the recording's %ux%u geometry again; fitting ended",
                    lockedGeometryWidth, lockedGeometryHeight);
        }
        return true;
    }

    const ce::encode_geometry::Rect fit = ce::encode_geometry::FitSourceIntoFrame(
        sourceDesc.Width, sourceDesc.Height, lockedGeometryWidth, lockedGeometryHeight);
    const bool newSourceSize =
        sourceDesc.Width != lastFittedSourceWidth || sourceDesc.Height != lastFittedSourceHeight;
    if (newSourceSize) {
        lastFittedSourceWidth = sourceDesc.Width;
        lastFittedSourceHeight = sourceDesc.Height;
        DLL_Log(
            "[VideoEncoder] Source size changed mid-recording: %ux%u (fmt=%d) fitted into the recording's %ux%u at "
            "%d,%d %dx%d (aspect preserved, black borders)",
            sourceDesc.Width, sourceDesc.Height, sourceDesc.Format, lockedGeometryWidth, lockedGeometryHeight, fit.x,
            fit.y, fit.width, fit.height);
    }
    if (fit.width <= 0 || fit.height <= 0 || !EnsureSwapRBShader()) {
        return false;
    }

    const DXGI_FORMAT typedFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format);
    if (typedFormat == DXGI_FORMAT_UNKNOWN) {
        if (newSourceSize) {
            DLL_Log("[VideoEncoder] Cannot fit resized source: unsupported format %d", sourceDesc.Format);
        }
        return false;
    }

    if (!geometryFitTexture || geometryFitFormat != typedFormat) {
        if (geometryFitRTV) {
            geometryFitRTV->Release();
            geometryFitRTV = nullptr;
        }
        if (geometryFitTexture) {
            geometryFitTexture->Release();
            geometryFitTexture = nullptr;
        }
        D3D11_TEXTURE2D_DESC fitDesc = {};
        fitDesc.Width = lockedGeometryWidth;
        fitDesc.Height = lockedGeometryHeight;
        fitDesc.MipLevels = 1;
        fitDesc.ArraySize = 1;
        fitDesc.Format = typedFormat;
        fitDesc.SampleDesc.Count = 1;
        fitDesc.Usage = D3D11_USAGE_DEFAULT;
        fitDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = d3d11Device->CreateTexture2D(&fitDesc, nullptr, &geometryFitTexture);
        if (SUCCEEDED(hr)) {
            hr = d3d11Device->CreateRenderTargetView(geometryFitTexture, nullptr, &geometryFitRTV);
        }
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create %ux%u fit target fmt=%d: HR=%x", lockedGeometryWidth,
                    lockedGeometryHeight, typedFormat, hr);
            ReleaseGeometryFitResources();
            return false;
        }
        geometryFitFormat = typedFormat;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = typedFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    HRESULT hr = d3d11Device->CreateShaderResourceView(source, &srvDesc, sourceView.addressof());
    if (FAILED(hr)) {
        if (newSourceSize) {
            DLL_Log("[VideoEncoder] Cannot fit resized source: SRV creation failed fmt=%d bind=%x HR=%x",
                    sourceDesc.Format, sourceDesc.BindFlags, hr);
        }
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[VideoEncoder] Cannot fit resized source: constant buffer map failed HR=%x", hr);
        return false;
    }
    // Same constant layout as RenderFullscreenCopy: pass-through colour, the
    // source keeps its own encoding; only its size changes.
    memset(mapped.pData, 0, 32);
    static_cast<uint32_t*>(mapped.pData)[0] = static_cast<uint32_t>(ce::video_format::RgbColorTransform::kNone);
    static_cast<float*>(mapped.pData)[5] = 203.0f;
    d3d11Context->Unmap(swapRBShaderCB, 0);

    // Opaque black is zero in every supported RGB encoding (sRGB, PQ, scRGB).
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    d3d11Context->ClearRenderTargetView(geometryFitRTV, black);
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = static_cast<float>(fit.x);
    viewport.TopLeftY = static_cast<float>(fit.y);
    viewport.Width = static_cast<float>(fit.width);
    viewport.Height = static_cast<float>(fit.height);
    viewport.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &viewport);
    d3d11Context->OMSetRenderTargets(1, &geometryFitRTV, nullptr);
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShader(swapRBShaderPS, nullptr, 0);
    ID3D11ShaderResourceView* view = sourceView.get();
    d3d11Context->PSSetShaderResources(0, 1, &view);
    // Linear filtering: a scaled source must not alias.
    d3d11Context->PSSetSamplers(0, 1, &hdrP010Sampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);
    d3d11Context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSrv);

    // The cursor is composited into the fitted frame: map its capture area onto
    // the fitted rectangle instead of the whole frame (repeats reuse this fit).
    geometryFitActive = true;
    geometryFitX = fit.x;
    geometryFitY = fit.y;
    geometryFitW = fit.width;
    geometryFitH = fit.height;
    geometryFitSourceWidth = sourceDesc.Width;
    geometryFitSourceHeight = sourceDesc.Height;
    ApplyGeometryFitToCursorState();
    geometryFitTexture->AddRef();
    *fitted = geometryFitTexture;
    return true;
}

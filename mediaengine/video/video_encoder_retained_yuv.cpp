#include "video_encoder_internal.h"

#include <cstdio>
#include <cstring>

#include "common/logging/log_meter.h"

// Retained NV12/P010 WGC/DXGI frames (llm-wiki/wgc-reservoir-vram-design.md).
// The reservoir slot already holds the codes this encoder's RGB conversion
// would produce, so a fresh or repeated output slot is one copy into the
// AVHWFrame plus, when visible, the face camera and cursor patched in YUV at
// this slot's own state. The slot itself is never written.

void RetainedYuvOverlayLayer::Release() {
    if (view)
        view->Release();
    if (renderTarget)
        renderTarget->Release();
    if (texture)
        texture->Release();
    view = nullptr;
    renderTarget = nullptr;
    texture = nullptr;
    width = 0;
    height = 0;
}

void RetainedYuvOverlays::Release() {
    patcher.Release();
    camera.Release();
    cursor.Release();
}

void VideoEncoder::CleanupRetainedYuvResources() {
    if (retainedYuv.frames > 0) {
        DLL_Log("[RetainedYuv] Encoder summary: frames=%llu overlayPatched=%llu patchFailures=%llu",
                static_cast<unsigned long long>(retainedYuv.frames),
                static_cast<unsigned long long>(retainedYuv.patchedFrames),
                static_cast<unsigned long long>(retainedYuv.patchFailures));
    }
    retainedYuv.Release();
    retainedYuv.frames = 0;
    retainedYuv.patchedFrames = 0;
    retainedYuv.patchFailures = 0;
    retainedYuv.lastSummaryTick = 0;
    retainedYuv.firstFrameLogged = false;
    retainedYuv.sharedSlotLogged = false;
}

void VideoEncoder::ResolveRetainedFrameRoute(const MediaEngineRetainedFrameQuery& query,
                                             MediaEngineRetainedFrameRoute* route) {
    const auto sourceFormat = static_cast<DXGI_FORMAT>(query.sourceFormat);
    ce::video_format::RetainedFrameRouteInputs in;
    in.sourceFormat = sourceFormat;
    in.width = query.width;
    in.height = query.height;
    in.sourceIsHdr = query.sourceIsHdr;
    in.hdrOutput = ce::video_format::ShouldEncodeHdrOutput(query.sourceIsHdr, savedConfig.colorSpace);
    // ShouldUse10BitOutput() as it will be once this source's RGB is the input.
    const bool prefer10Bit = savedConfig.bitDepth == "10" ||
                             (savedConfig.bitDepth != "8" &&
                              (ce::video_format::IsHighPrecisionRgbInputFormat(sourceFormat) ||
                               sourcePrefers10Bit_.load(std::memory_order_relaxed)));
    ResolveAutoEncoder("retained-route");  // the route depends on the encoder's formats
    const AVCodec* codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    ResolvedVideoFormat resolved;
    std::string error;
    std::string warning;
    in.formatResolved = codec && ResolveVideoFormat(savedConfig, in.hdrOutput, prefer10Bit, codec, &resolved, &error,
                                                    &warning);
    in.use10BitOutput = resolved.use10Bit;
    in.encoderUsesYuvInput = resolved.usesVideoProcessor;
    in.scaling = savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 &&
                 savedConfig.scaling.outputHeight > 0;
    in.sameDevice = query.sameDevice;
    in.splitShareNv12 = query.splitShareNv12;
    in.splitShareP010 = query.splitShareP010;
    in.enabled = true;
    in.geometryMismatch = lockedGeometryWidth != 0 &&
                          (lockedGeometryWidth != query.width || lockedGeometryHeight != query.height);
    const auto decision = ce::video_format::DecideRetainedFrameRoute(in);

    *route = MediaEngineRetainedFrameRoute{};
    route->yuvFormat = decision.yuv ? static_cast<uint32_t>(decision.format) : 0u;
    route->normalizeTransform = static_cast<uint32_t>(decision.normalizeTransform);
    route->rgbLevels = decision.rgbLevels;
    route->hdrOutput = in.hdrOutput;
    route->bt2020 = _stricmp(savedConfig.colorSpace.c_str(), "bt2020") == 0;
    route->fullRange = WantsFullOutputRange(savedConfig.colorRange);
    std::snprintf(route->reason, sizeof(route->reason), "%s", decision.reason);
    if (decision.yuv)
        retainedYuv.rgbLevels.store(decision.rgbLevels, std::memory_order_relaxed);
    DLL_Log("[RetainedYuv] Route query: source=%d %ux%u hdr=%d sameDevice=%d splitShare=nv12:%d,p010:%d -> %s "
            "format=%d transform=%s rgbLevels=%u hdrOutput=%d bt2020=%d fullRange=%d encoder=%s",
            static_cast<int>(sourceFormat), query.width, query.height, query.sourceIsHdr ? 1 : 0,
            query.sameDevice ? 1 : 0, query.splitShareNv12 ? 1 : 0, query.splitShareP010 ? 1 : 0, decision.reason,
            static_cast<int>(decision.format),
            ce::video_format::DescribeRgbColorTransform(decision.normalizeTransform), decision.rgbLevels,
            in.hdrOutput ? 1 : 0, route->bt2020 ? 1 : 0, route->fullRange ? 1 : 0, savedConfig.encoder.c_str());
}

bool VideoEncoder::RenderRetainedYuvLayer(RetainedYuvOverlayLayer& layer, const RECT& rect) {
    const UINT layerWidth = static_cast<UINT>(rect.right - rect.left);
    const UINT layerHeight = static_cast<UINT>(rect.bottom - rect.top);
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return false;
    if (!layer.texture || layer.width < layerWidth || layer.height < layerHeight) {
        // Grow-only, so a moving cursor or a fixed camera never reallocates.
        const UINT newWidth = std::max(layerWidth, layer.width);
        const UINT newHeight = std::max(layerHeight, layer.height);
        layer.Release();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = newWidth;
        desc.Height = newHeight;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = d3d11Device->CreateTexture2D(&desc, nullptr, &layer.texture);
        if (SUCCEEDED(hr))
            hr = d3d11Device->CreateRenderTargetView(layer.texture, nullptr, &layer.renderTarget);
        if (SUCCEEDED(hr))
            hr = d3d11Device->CreateShaderResourceView(layer.texture, nullptr, &layer.view);
        if (FAILED(hr)) {
            DLL_Log("[RetainedYuv] Overlay layer allocation failed: %ux%u HR=%x", newWidth, newHeight, hr);
            layer.Release();
            return false;
        }
        layer.width = newWidth;
        layer.height = newHeight;
    }
    // Premultiplied layer: the renderers blend straight alpha onto zero.
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    d3d11Context->ClearRenderTargetView(layer.renderTarget, transparent);
    return true;
}

bool VideoEncoder::ConvertRetainedYuvFrame(ID3D11Texture2D* input, AVFrame* outputFrame, bool overlayCursor,
                                           int captureOriginX, int captureOriginY, uint64_t keyedMutexAcquireKey) {
    if (!input || !outputFrame || !d3d11FramesCtx || !d3d11Device || !d3d11Context)
        return false;
    D3D11_TEXTURE2D_DESC inputDesc = {};
    input->GetDesc(&inputDesc);
    if (!outputFrame->buf[0]) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, outputFrame, 0);
        if (frameRet < 0 || !outputFrame->data[0]) {
            DLL_Log("[RetainedYuv] Failed to allocate AVHWFrame-owned output: %d", frameRet);
            return false;
        }
    }
    ID3D11Texture2D* output = EncoderFrameTexture(outputFrame);
    if (!output)
        return false;
    const UINT outputSlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(outputFrame->data[1]));
    D3D11_TEXTURE2D_DESC outputDesc = {};
    output->GetDesc(&outputDesc);
    if (outputDesc.Format != inputDesc.Format || outputDesc.Width != inputDesc.Width ||
        outputDesc.Height != inputDesc.Height) {
        const auto log = diagnostics.retainedMismatchLog.Observe(
            ce::log_meter::FieldKey(inputDesc.Format, outputDesc.Format, inputDesc.Width, outputDesc.Width),
            GetTickCount64());
        if (log) {
            DLL_Log("[RetainedYuv] Retained frame does not match the encoder input: fmt=%d %ux%u vs fmt=%d %ux%u "
                    "(frame refused)%s",
                    inputDesc.Format, inputDesc.Width, inputDesc.Height, outputDesc.Format, outputDesc.Width,
                    outputDesc.Height, ce::log_meter::SuppressedNote(log.suppressed).c_str());
        }
        return false;
    }
    UpdateSdrWhiteLevelForCaptureArea(captureOriginX, captureOriginY, inputDesc.Width, inputDesc.Height);
    const bool hdr = ShouldEncodeHdrOutput();
    const float paperWhiteNits = currentIsHDR ? sdrWhiteNits : 80.0f;
    const UINT frameWidth = inputDesc.Width;
    const UINT frameHeight = inputDesc.Height;

    // A split capture device (DXGI duplication) publishes the slot at keyed
    // mutex key 1; the copy and the overlay patch both read it, so the key is
    // held across both and returned to 0, as the RGB conversion does.
    struct KeyedMutexGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;
        ~KeyedMutexGuard() {
            if (!mutex)
                return;
            if (acquired)
                mutex->ReleaseSync(0);
            mutex->Release();
        }
    } keyedMutexGuard;
    input->QueryInterface(IID_PPV_ARGS(&keyedMutexGuard.mutex));
    if (keyedMutexGuard.mutex) {
        const HRESULT kmHr = keyedMutexGuard.mutex->AcquireSync(keyedMutexAcquireKey, 1000);
        if (kmHr != S_OK) {
            const auto log =
                diagnostics.retainedKeyedMutexLog.Observe(ce::log_meter::FieldKey(static_cast<uint32_t>(kmHr)), GetTickCount64());
            if (log) {
                DLL_Log("[RetainedYuv] Shared slot keyed-mutex AcquireSync(%llu) failed: HR=%x (frame refused)%s",
                        static_cast<unsigned long long>(keyedMutexAcquireKey), static_cast<unsigned>(kmHr),
                        ce::log_meter::SuppressedNote(log.suppressed).c_str());
            }
            return false;
        }
        keyedMutexGuard.acquired = true;
        if (!retainedYuv.sharedSlotLogged) {
            retainedYuv.sharedSlotLogged = true;
            DLL_Log("[RetainedYuv] Encoder reads shared retained slots at keyed mutex %llu->0 (split capture device)",
                    static_cast<unsigned long long>(keyedMutexAcquireKey));
        }
    }

    D3D11ScopedLock lock(instance.d3d11Mutex);
    d3d11Context->CopySubresourceRegion(output, D3D11CalcSubresource(0, outputSlice, outputDesc.MipLevels), 0, 0, 0,
                                        input, 0, nullptr);
    ++retainedYuv.frames;

    // Face camera first, cursor topmost: each draws alone into its layer in
    // the domain the RGB path blended it in (sRGB, or linear scRGB for HDR).
    YuvOverlayPatcher::Layer cameraLayer;
    YuvOverlayPatcher::Layer cursorLayer;
    if (savedConfig.faceCamera.enabled && savedConfig.faceCamera.opacity > 0.0f) {
        EnsureFaceCameraStarted();
        RECT rect = {};
        if (faceCameraRenderer)
            faceCameraRenderer->SetContentTargetQpc(FaceCameraContentQpc());
        if (faceCameraRenderer && faceCameraRenderer->InitRenderer(d3d11Device, d3d11Context) &&
            faceCameraRenderer->GetOverlayFrameRect(frameWidth, frameHeight, frameWidth, frameHeight, &rect) &&
            RenderRetainedYuvLayer(retainedYuv.camera, rect) &&
            faceCameraRenderer->CompositePreparedFrame(retainedYuv.camera.texture, frameWidth, frameHeight,
                                                       hdr ? FaceCameraColorMode::ScRgb : FaceCameraColorMode::Sdr,
                                                       paperWhiteNits, rect.left, rect.top)) {
            cameraLayer = {retainedYuv.camera.view, rect};
        }
    }
    if (overlayCursor && cursorRenderer && cursorCaptureState.IsVisible() &&
        cursorRenderer->Init(d3d11Device, d3d11Context)) {
        RECT rect = {};
        if (cursorRenderer->GetCursorFrameRect(static_cast<int>(frameWidth), static_cast<int>(frameHeight),
                                               cursorCaptureState, &rect) &&
            RenderRetainedYuvLayer(retainedYuv.cursor, rect) &&
            cursorRenderer->CompositeOntoFrame(retainedYuv.cursor.texture, static_cast<int>(frameWidth),
                                               static_cast<int>(frameHeight), cursorCaptureState,
                                               hdr ? CursorColorMode::ScRgb : CursorColorMode::Sdr, paperWhiteNits,
                                               rect.left, rect.top)) {
            cursorLayer = {retainedYuv.cursor.view, rect};
        }
    }
    d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);

    bool patched = false;
    if (cameraLayer.view || cursorLayer.view) {
        CachedHdrP010OutputViews* planes = GetYuvPlaneOutputViews(output, outputSlice, "RetainedYuv");
        YuvOverlayPatcher::Params params;
        params.hdr = hdr;
        params.eightBit = inputDesc.Format == DXGI_FORMAT_NV12;
        params.bt2020 = _stricmp(savedConfig.colorSpace.c_str(), "bt2020") == 0;
        params.fullRange = WantsFullOutputRange(savedConfig.colorRange);
        params.rgbLevels = static_cast<float>(retainedYuv.rgbLevels.load(std::memory_order_relaxed));
        patched = planes && retainedYuv.patcher.Init(d3d11Device, d3d11Context) &&
                  retainedYuv.patcher.Apply(input, planes->lumaView, planes->chromaView, frameWidth, frameHeight,
                                            cameraLayer, cursorLayer, params);
        if (patched) {
            ++retainedYuv.patchedFrames;
        } else if (retainedYuv.patchFailures++ < 5) {
            // Fail open like the RGB path: the frame continues without overlays.
            DLL_Log("[RetainedYuv] Overlay patch failed; frame continues without camera/cursor (camera=%d cursor=%d)",
                    cameraLayer.view ? 1 : 0, cursorLayer.view ? 1 : 0);
        }
    }

    const uint64_t now = GetTickCount64();
    if (!retainedYuv.firstFrameLogged) {
        retainedYuv.firstFrameLogged = true;
        retainedYuv.lastSummaryTick = now;
        DLL_Log("[RetainedYuv] Encoder consumes retained %s %ux%u: copy into the AVHWFrame, overlays patched in YUV "
                "(hdr=%d rgbLevels=%u camera=%d cursor=%d)",
                inputDesc.Format == DXGI_FORMAT_NV12 ? "NV12" : "P010", frameWidth, frameHeight, hdr ? 1 : 0,
                retainedYuv.rgbLevels.load(std::memory_order_relaxed), cameraLayer.view ? 1 : 0,
                cursorLayer.view ? 1 : 0);
    } else if (now - retainedYuv.lastSummaryTick >= 30000) {
        retainedYuv.lastSummaryTick = now;
        DLL_Log("[RetainedYuv] frames=%llu overlayPatched=%llu patchFailures=%llu",
                static_cast<unsigned long long>(retainedYuv.frames),
                static_cast<unsigned long long>(retainedYuv.patchedFrames),
                static_cast<unsigned long long>(retainedYuv.patchFailures));
    }
    vpFirstCallLogged = true;
    return true;
}

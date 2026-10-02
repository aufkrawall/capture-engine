#include "video_encoder_internal.h"

VideoEncoder::FaceCameraSourceRestore::~FaceCameraSourceRestore() {
    if (!active || !context || !target || !backup || width == 0 || height == 0)
        return;
    const D3D11_BOX backupBox = {0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup, 0, &backupBox);
}

bool VideoEncoder::FaceCameraCompositionActive() const {
    return savedConfig.faceCamera.enabled && savedConfig.faceCamera.opacity > 0.0f && faceCameraRenderer &&
           faceCameraRenderer->NeedsDynamicRepeatSource();
}

void VideoEncoder::EnsureFaceCameraStarted() {
    if (!savedConfig.faceCamera.enabled || savedConfig.faceCamera.opacity <= 0.0f || !d3d11Device ||
        !d3d11Context)
        return;
    if (!faceCameraRenderer)
        faceCameraRenderer = std::make_unique<FaceCameraRenderer>(savedConfig.faceCamera);
    if (!faceCameraRenderer->StartCapture(d3d11Device) && faceCameraPrecompositionFailureLogs++ < 5) {
        DLL_Log("[FaceCamera] Camera worker could not start; main video remains active");
    }
    // Inject capture reaches here from Start() before the encoder accepts its
    // first frame. WGC/DXGI reaches it after adopting the existing shared
    // frame-grab device. Shader bytecode was already compiled when the
    // renderer was configured, so neither path compiles HLSL on a video frame.
    if (!faceCameraRenderer->InitRenderer(d3d11Device, d3d11Context) &&
        faceCameraPrecompositionFailureLogs++ < 5) {
        DLL_Log("[FaceCamera] GPU renderer prewarm failed; main video remains active");
    }
}

void VideoEncoder::StopFaceCamera() {
    if (faceCameraRenderer)
        faceCameraRenderer->Stop();
}

void VideoEncoder::CleanupFaceCameraCompositionResources() {
    if (faceCameraRestoreTexture) {
        faceCameraRestoreTexture->Release();
        faceCameraRestoreTexture = nullptr;
    }
    if (faceCameraCompositeTexture) {
        faceCameraCompositeTexture->Release();
        faceCameraCompositeTexture = nullptr;
    }
    faceCameraPrecompositionLogged = false;
    faceCameraFullCopyFallbackLogged = false;
    faceCameraPrecompositionFailureLogs = 0;
}

bool VideoEncoder::PrepareVideoProcessorFaceCameraInput(ID3D11Texture2D* source,
                                                        FaceCameraSourceRestore* restore,
                                                        ID3D11Texture2D** preparedSource) {
    if (!source || !restore || !preparedSource || !d3d11Device || !d3d11Context)
        return false;
    *preparedSource = source;
    if (!savedConfig.faceCamera.enabled || savedConfig.faceCamera.opacity <= 0.0f)
        return true;

    EnsureFaceCameraStarted();
    if (!faceCameraRenderer || !faceCameraRenderer->InitRenderer(d3d11Device, d3d11Context)) {
        if (faceCameraPrecompositionFailureLogs++ < 5)
            DLL_Log("[FaceCamera] GPU renderer initialization failed; frame continues without camera");
        return true;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    const uint32_t displayedWidth = scalingEnabled && outputWidth > 0 ? static_cast<uint32_t>(outputWidth)
                                                                      : sourceDesc.Width;
    const uint32_t displayedHeight = scalingEnabled && outputHeight > 0 ? static_cast<uint32_t>(outputHeight)
                                                                         : sourceDesc.Height;
    RECT cameraRect = {};
    if (!faceCameraRenderer->GetOverlayFrameRect(sourceDesc.Width, sourceDesc.Height, displayedWidth,
                                                 displayedHeight, &cameraRect)) {
        return true;
    }

    const LONG clippedLeft = std::clamp<LONG>(cameraRect.left, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedTop = std::clamp<LONG>(cameraRect.top, 0, static_cast<LONG>(sourceDesc.Height));
    const LONG clippedRight = std::clamp<LONG>(cameraRect.right, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedBottom = std::clamp<LONG>(cameraRect.bottom, 0, static_cast<LONG>(sourceDesc.Height));
    if (clippedLeft >= clippedRight || clippedTop >= clippedBottom)
        return true;

    const UINT regionWidth = static_cast<UINT>(clippedRight - clippedLeft);
    const UINT regionHeight = static_cast<UINT>(clippedBottom - clippedTop);
    ID3D11Texture2D* compositionTarget = source;
    bool useSmallRestore = (sourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && sourceDesc.SampleDesc.Count == 1;

    if (useSmallRestore) {
        bool recreateRestore = faceCameraRestoreTexture == nullptr;
        if (faceCameraRestoreTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            faceCameraRestoreTexture->GetDesc(&existing);
            recreateRestore = existing.Format != sourceDesc.Format || existing.Width < regionWidth ||
                              existing.Height < regionHeight;
        }
        if (recreateRestore) {
            if (faceCameraRestoreTexture) {
                faceCameraRestoreTexture->Release();
                faceCameraRestoreTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC restoreDesc = {};
            restoreDesc.Width = regionWidth;
            restoreDesc.Height = regionHeight;
            restoreDesc.MipLevels = 1;
            restoreDesc.ArraySize = 1;
            restoreDesc.Format = sourceDesc.Format;
            restoreDesc.SampleDesc.Count = 1;
            restoreDesc.Usage = D3D11_USAGE_DEFAULT;
            const HRESULT restoreHr =
                d3d11Device->CreateTexture2D(&restoreDesc, nullptr, &faceCameraRestoreTexture);
            if (FAILED(restoreHr)) {
                useSmallRestore = false;
                if (faceCameraPrecompositionFailureLogs++ < 5) {
                    DLL_Log("[FaceCamera] Small RGB restore texture creation failed: fmt=%d %ux%u HR=%x",
                            sourceDesc.Format, regionWidth, regionHeight, restoreHr);
                }
            }
        }
    }

    if (useSmallRestore) {
        const D3D11_BOX sourceBox = {
            static_cast<UINT>(clippedLeft),  static_cast<UINT>(clippedTop),    0,
            static_cast<UINT>(clippedRight), static_cast<UINT>(clippedBottom), 1,
        };
        d3d11Context->CopySubresourceRegion(faceCameraRestoreTexture, 0, 0, 0, 0, source, 0, &sourceBox);
        restore->context = d3d11Context;
        restore->target = source;
        restore->backup = faceCameraRestoreTexture;
        restore->destinationX = static_cast<UINT>(clippedLeft);
        restore->destinationY = static_cast<UINT>(clippedTop);
        restore->width = regionWidth;
        restore->height = regionHeight;
        restore->active = true;
    } else {
        bool recreateComposite = faceCameraCompositeTexture == nullptr;
        if (faceCameraCompositeTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            faceCameraCompositeTexture->GetDesc(&existing);
            recreateComposite = existing.Width != sourceDesc.Width || existing.Height != sourceDesc.Height ||
                                existing.Format != sourceDesc.Format || existing.ArraySize != sourceDesc.ArraySize ||
                                existing.MipLevels != sourceDesc.MipLevels ||
                                existing.SampleDesc.Count != sourceDesc.SampleDesc.Count ||
                                existing.SampleDesc.Quality != sourceDesc.SampleDesc.Quality;
        }
        if (recreateComposite) {
            if (faceCameraCompositeTexture) {
                faceCameraCompositeTexture->Release();
                faceCameraCompositeTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC compositeDesc = sourceDesc;
            compositeDesc.Usage = D3D11_USAGE_DEFAULT;
            compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            compositeDesc.CPUAccessFlags = 0;
            compositeDesc.MiscFlags = 0;
            const HRESULT compositeHr =
                d3d11Device->CreateTexture2D(&compositeDesc, nullptr, &faceCameraCompositeTexture);
            if (FAILED(compositeHr)) {
                if (faceCameraPrecompositionFailureLogs++ < 5)
                    DLL_Log(
                        "[FaceCamera] RGB fallback texture allocation failed; frame continues without camera: "
                        "fmt=%d bind=%x HR=%x",
                        sourceDesc.Format, sourceDesc.BindFlags, compositeHr);
                return true;
            }
        }
        d3d11Context->CopyResource(faceCameraCompositeTexture, source);
        compositionTarget = faceCameraCompositeTexture;
        *preparedSource = faceCameraCompositeTexture;
        if (!faceCameraFullCopyFallbackLogged) {
            DLL_Log(
                "[FaceCamera] RGB precomposition fallback uses one full-frame GPU compatibility copy: "
                "fmt=%d bind=%x samples=%u",
                sourceDesc.Format, sourceDesc.BindFlags, sourceDesc.SampleDesc.Count);
            faceCameraFullCopyFallbackLogged = true;
        }
    }

    const FaceCameraColorMode colorMode = ce::video_format::IsFp16RgbInputFormat(sourceDesc.Format)
                                              ? FaceCameraColorMode::ScRgb
                                              : (currentIsHDR ? FaceCameraColorMode::Hdr10Pq
                                                              : FaceCameraColorMode::Sdr);
    const float paperWhiteNits = currentIsHDR ? sdrWhiteNits : 80.0f;
    if (!faceCameraRenderer->CompositePreparedFrame(compositionTarget, sourceDesc.Width, sourceDesc.Height,
                                                    colorMode, paperWhiteNits)) {
        if (faceCameraPrecompositionFailureLogs++ < 5)
            DLL_Log("[FaceCamera] GPU composition failed; frame continues without camera");
        return true;
    }
    d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
    if (!faceCameraPrecompositionLogged) {
        DLL_Log(
            "[FaceCamera] RGB precomposition active before the single GPU color-conversion pass: "
            "targetFmt=%d region=%ux%u smallRestore=%d outputLayout=%ux%u",
            sourceDesc.Format, regionWidth, regionHeight, useSmallRestore ? 1 : 0, displayedWidth, displayedHeight);
        faceCameraPrecompositionLogged = true;
    }
    return true;
}

bool VideoEncoder::CompositeFaceCameraOntoRgb(ID3D11Texture2D* target, FaceCameraColorMode colorMode,
                                              float paperWhiteNits) {
    if (!target || !savedConfig.faceCamera.enabled || savedConfig.faceCamera.opacity <= 0.0f)
        return true;
    EnsureFaceCameraStarted();
    if (!faceCameraRenderer || !faceCameraRenderer->InitRenderer(d3d11Device, d3d11Context))
        return true;
    D3D11_TEXTURE2D_DESC desc = {};
    target->GetDesc(&desc);
    RECT ignored = {};
    if (!faceCameraRenderer->GetOverlayFrameRect(desc.Width, desc.Height, desc.Width, desc.Height, &ignored))
        return true;
    if (!faceCameraRenderer->CompositePreparedFrame(target, desc.Width, desc.Height, colorMode, paperWhiteNits) &&
        faceCameraPrecompositionFailureLogs++ < 5) {
        DLL_Log("[FaceCamera] Direct RGB composition failed; frame continues without camera");
    }
    d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

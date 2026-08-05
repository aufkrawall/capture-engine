#include "video_encoder_internal.h"

bool VideoEncoder::InitVideoProcessor() {
    if (videoProcessorInit)
        return true;

    if (!d3d11Device) {
        DLL_Log("[VideoProcessor] D3D11 device not available");
        return false;
    }

    HRESULT hr;
    const bool outputIsHDR = ShouldEncodeHdrOutput();

    // Get video device interface
    hr = d3d11Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoDevice. HR=%x", hr);
        return false;
    }

    // Get video context
    hr = d3d11Context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&videoContext);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoContext. HR=%x", hr);
        return false;
    }
    hr = videoContext->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&videoContext1);
    if (FAILED(hr)) {
        videoContext1 = nullptr;
        DLL_Log(
            "[VideoProcessor] ID3D11VideoContext1 unavailable (HR=%x); SDR uses the legacy VP contract and HDR "
            "output uses the direct P010 shader without VideoProcessor color conversion",
            hr);
    }

    // Store input dimensions (captured frame size)
    inputWidth = width;
    inputHeight = height;

    // Determine output dimensions based on scaling config
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        outputWidth = savedConfig.scaling.outputWidth;
        outputHeight = savedConfig.scaling.outputHeight;
    } else {
        // No scaling or native resolution
        outputWidth = width;
        outputHeight = height;
    }

    // NV12 output textures require even-aligned dimensions
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    outputWidth = outputWidth & ~1u;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    outputHeight = outputHeight & ~1u;
    if (outputWidth == 0 || outputHeight == 0) {
        DLL_Log("[VideoProcessor] Dimensions too small after NV12 alignment");
        return false;
    }

    // Check if scaling is actually needed (input != output)
    scalingEnabled = (inputWidth != outputWidth || inputHeight != outputHeight);

    if (scalingEnabled && !outputIsHDR) {
        DLL_Log("[VideoProcessor] GPU SCALING ENABLED: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (!scalingEnabled) {
        DLL_Log("[VideoProcessor] Scaling disabled (input matches output: %dx%d)", inputWidth, inputHeight);
    }

    // Create video processor enumerator with potentially different input/output
    // dims
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inputWidth;
    contentDesc.InputHeight = inputHeight;
    contentDesc.OutputWidth = outputWidth;
    contentDesc.OutputHeight = outputHeight;
    contentDesc.Usage =
        (savedConfig.scaling.quality == "best") ? D3D11_VIDEO_USAGE_OPTIMAL_QUALITY : D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create enumerator. HR=%x", hr);
        return false;
    }

    // Create video processor
    try {
        hr = videoDevice->CreateVideoProcessor(videoProcessorEnum, 0, &videoProcessor);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create processor. HR=%x", hr);
        return false;
    }

    // Capture textures are progressive desktop/game frames. Driver automatic
    // processing may apply temporal video heuristics that are inappropriate
    // for pixel-sharp UI. A separate cursor is already point-composited into
    // this RGB stream before the VP sees it.
    videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 0, FALSE);
    D3D11_VIDEO_FRAME_FORMAT mainFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    BOOL mainAutoProcessing = TRUE;
    videoContext->VideoProcessorGetStreamFrameFormat(videoProcessor, 0, &mainFrameFormat);
    videoContext->VideoProcessorGetStreamAutoProcessingMode(videoProcessor, 0, &mainAutoProcessing);
    DLL_Log("[VideoProcessor] Deterministic single-stream processing: progressive=%d auto=%d",
            mainFrameFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE ? 1 : 0, mainAutoProcessing ? 1 : 0);

    // Configure scaling filter if scaling is enabled
    if (scalingEnabled && !outputIsHDR) {
        // Map sharpness (0-100) directly to D3D11 VP edge enhancement
        bool enableEdgeEnhancement = (savedConfig.scaling.sharpness > 0);
        int edgeEnhancementLevel = savedConfig.scaling.sharpness;

        if (enableEdgeEnhancement) {
            // Check if edge enhancement is supported
            D3D11_VIDEO_PROCESSOR_FILTER_RANGE filterRange = {};
            hr = videoProcessorEnum->GetVideoProcessorFilterRange(D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
                                                                  &filterRange);

            if (SUCCEEDED(hr)) {
                // Map our 0-100 level to the actual VP filter range
                int filterValue = filterRange.Default;
                if (filterRange.Maximum > filterRange.Minimum) {
                    filterValue = filterRange.Minimum +
                                  (edgeEnhancementLevel * (filterRange.Maximum - filterRange.Minimum) / 100);
                }

                videoContext->VideoProcessorSetStreamFilter(
                    videoProcessor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, filterValue);

                DLL_Log(
                    "[VideoProcessor] Scaling: quality=%s, sharpness=%d "
                    "(filterValue=%d, range=%d-%d)",
                    savedConfig.scaling.quality.c_str(), edgeEnhancementLevel, filterValue, filterRange.Minimum,
                    filterRange.Maximum);
            } else {
                DLL_Log(
                    "[VideoProcessor] Edge enhancement (sharpness) not supported "
                    "by hardware");
            }
        } else {
            DLL_Log("[VideoProcessor] Scaling: quality=%s, sharpness=0 (disabled)",
                    savedConfig.scaling.quality.c_str());
        }

        // CRITICAL: Set source and destination rectangles for scaling
        // Without these, VideoProcessorBlt fails with E_INVALIDARG
        RECT sourceRect = {0, 0, (LONG)inputWidth, (LONG)inputHeight};
        RECT destRect = {0, 0, (LONG)outputWidth, (LONG)outputHeight};

        // Stream 0: Source rect = full input frame
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor, 0, TRUE, &sourceRect);
        // Stream 0: Dest rect = full output frame (scaled)
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 0, TRUE, &destRect);
        // Output target = full output surface
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor, TRUE, &destRect);

        DLL_Log("[VideoProcessor] Scaling rects: source=%dx%d dest=%dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else if (scalingEnabled) {
        DLL_Log(
            "[HDR P010] Shader scaling configured: source=%dx%d dest=%dx%d filter=bilinear lumaSharpness=%d",
            inputWidth, inputHeight, outputWidth, outputHeight, savedConfig.scaling.sharpness);
    }

    const OutputRangeMode outputRange = GetEffectiveOutputRange(savedConfig.colorRange, outputIsHDR);

    // Configure color space: Full-range RGB input from capture -> requested YCbCr output range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.Usage = 0;          // 0 = Playback, 1 = Video processing
    inputColorSpace.RGB_Range = 0;      // 0 = Full range (0-255), 1 = Studio (16-235)
    inputColorSpace.YCbCr_Matrix = 1;   // 0 = BT.601, 1 = BT.709
    inputColorSpace.YCbCr_xvYCC = 0;    // 0 = Conventional, 1 = Extended
    inputColorSpace.Nominal_Range = 2;  // 2 = Full (0-255) for input

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.Usage = 0;
    outputColorSpace.RGB_Range = outputRange == OutputRangeMode::kFull ? 0 : 1;
    outputColorSpace.YCbCr_Matrix = 1;  // BT.709
    outputColorSpace.YCbCr_xvYCC = 0;
    outputColorSpace.Nominal_Range = outputRange == OutputRangeMode::kFull ? 2 : 1;

    videoContext->VideoProcessorSetStreamColorSpace(videoProcessor, 0, &inputColorSpace);
    videoContext->VideoProcessorSetOutputColorSpace(videoProcessor, &outputColorSpace);
    const char* colorConversionSuffix =
        outputIsHDR ? "; HDR output bypasses VideoProcessor color conversion via direct P010 plane shaders"
                    : (videoContext1 ? "; ColorSpace1 overrides this per frame" : "");
    DLL_Log("[VideoProcessor] Legacy color-space baseline: Full RGB (0-255) -> %s YCbCr (%s, BT.709)%s",
            outputRange == OutputRangeMode::kFull ? "Full" : "Limited",
            outputRange == OutputRangeMode::kFull ? "0-255" : "16-235", colorConversionSuffix);

    // AVHWFrame textures are the VP output surfaces. They are allocated on
    // demand by libavutil and retained by NVENC for exactly as long as each
    // submitted frame remains in flight.
    const bool use10BitOutput = ShouldUse10BitOutput();
    const DXGI_FORMAT outputFormat = use10BitOutput ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    UINT formatSupport = 0;
    hr = d3d11Device->CheckFormatSupport(outputFormat, &formatSupport);
    if (SUCCEEDED(hr)) {
        DLL_Log("[VideoProcessor] Output fmt=%d formatSupport=0x%x", outputFormat, formatSupport);
    } else {
        DLL_Log("[VideoProcessor] CheckFormatSupport(fmt=%d) failed. HR=%x", outputFormat, hr);
    }
    DLL_Log("[VideoProcessor] Using AVHWFrame-owned %s output textures at %dx%d", use10BitOutput ? "P010" : "NV12",
            outputWidth, outputHeight);

    if (!outputIsHDR) {
        // Desktop Duplication textures may not support direct VP input views.
        // SDR output retains the compatibility staging copy; HDR output writes P010 planes
        // directly and deliberately does not allocate this legacy surface.
        D3D11_TEXTURE2D_DESC bgraDesc = {};
        bgraDesc.Width = inputWidth;
        bgraDesc.Height = inputHeight;
        bgraDesc.MipLevels = 1;
        bgraDesc.ArraySize = 1;
        bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bgraDesc.SampleDesc.Count = 1;
        bgraDesc.Usage = D3D11_USAGE_DEFAULT;
        bgraDesc.BindFlags = 0;

        hr = d3d11Device->CreateTexture2D(&bgraDesc, nullptr, &bgraStagingTexture);

        if (FAILED(hr)) {
            DLL_Log("[VideoProcessor] Failed to create BGRA staging texture. HR=%x", hr);
            return false;
        }
        DLL_Log("[VideoProcessor] Created BGRA staging texture at %dx%d for DD compatibility", inputWidth,
                inputHeight);
    } else {
        DLL_Log("[HDR P010] Legacy BGRA/VideoProcessor staging allocation skipped");
    }

    videoProcessorInit = true;

    if (outputIsHDR) {
        DLL_Log("[HDR P010] Initialized direct shader target for %dx%d -> %dx%d RGB10/PQ -> %s P010", inputWidth,
                inputHeight, outputWidth, outputHeight, DescribeOutputRange(outputRange));
    } else if (scalingEnabled) {
        DLL_Log(
            "[VideoProcessor] Initialized with SCALING: %dx%d -> %dx%d "
            "RGB→%s",
            inputWidth, inputHeight, outputWidth, outputHeight, use10BitOutput ? "P010" : "NV12");
    } else {
        DLL_Log("[VideoProcessor] Initialized for %dx%d RGB→%s (no scaling)", outputWidth, outputHeight,
                use10BitOutput ? "P010" : "NV12");
    }
    return true;
}

VideoEncoder::CursorSourceRestore::~CursorSourceRestore() {
    if (!active || !context || !target || !backup || width == 0 || height == 0) {
        return;
    }
    const D3D11_BOX backupBox = {0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup, 0, &backupBox);
}

void VideoEncoder::CleanupCursorCompositionResources() {
    if (cursorRestoreTexture) {
        cursorRestoreTexture->Release();
        cursorRestoreTexture = nullptr;
    }
    if (cursorCompositeTexture) {
        cursorCompositeTexture->Release();
        cursorCompositeTexture = nullptr;
    }
}

bool VideoEncoder::PrepareVideoProcessorCursorInput(ID3D11Texture2D* source, bool overlayCursor,
                                                    CursorSourceRestore* restore, ID3D11Texture2D** preparedSource) {
    if (!source || !restore || !preparedSource || !d3d11Device || !d3d11Context) {
        return false;
    }
    *preparedSource = source;
    if (!overlayCursor || !cursorRenderer || !cursorCaptureState.IsVisible()) {
        return true;
    }

    if (!cursorRenderer->Init(d3d11Device, d3d11Context)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Failed to initialize RGB cursor renderer; video conversion continues without this "
                "separate cursor draw");
        }
        return true;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    RECT cursorRect = {};
    if (!cursorRenderer->GetCursorFrameRect(static_cast<int>(sourceDesc.Width), static_cast<int>(sourceDesc.Height),
                                            cursorCaptureState, &cursorRect)) {
        if (cursorPrecompositionFailureLogs++ < 5) {

            DLL_Log(
                "[Cursor] Failed to resolve cursor bitmap/rectangle; video conversion continues without this "
                "separate cursor draw");
        }
        return true;
    }

    const LONG clippedLeft = std::clamp<LONG>(cursorRect.left, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedTop = std::clamp<LONG>(cursorRect.top, 0, static_cast<LONG>(sourceDesc.Height));
    const LONG clippedRight = std::clamp<LONG>(cursorRect.right, 0, static_cast<LONG>(sourceDesc.Width));
    const LONG clippedBottom = std::clamp<LONG>(cursorRect.bottom, 0, static_cast<LONG>(sourceDesc.Height));
    if (clippedLeft >= clippedRight || clippedTop >= clippedBottom) {
        return true;
    }

    const UINT regionWidth = static_cast<UINT>(clippedRight - clippedLeft);
    const UINT regionHeight = static_cast<UINT>(clippedBottom - clippedTop);
    ID3D11Texture2D* compositionTarget = source;
    bool useSmallRestore = (sourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && sourceDesc.SampleDesc.Count == 1;

    if (useSmallRestore) {
        bool recreateRestore = cursorRestoreTexture == nullptr;
        if (cursorRestoreTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            cursorRestoreTexture->GetDesc(&existing);
            recreateRestore =
                existing.Format != sourceDesc.Format || existing.Width < regionWidth || existing.Height < regionHeight;
        }
        if (recreateRestore) {
            if (cursorRestoreTexture) {
                cursorRestoreTexture->Release();
                cursorRestoreTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC restoreDesc = {};
            restoreDesc.Width = regionWidth;
            restoreDesc.Height = regionHeight;
            restoreDesc.MipLevels = 1;
            restoreDesc.ArraySize = 1;
            restoreDesc.Format = sourceDesc.Format;
            restoreDesc.SampleDesc.Count = 1;
            restoreDesc.Usage = D3D11_USAGE_DEFAULT;
            const HRESULT restoreHr = d3d11Device->CreateTexture2D(&restoreDesc, nullptr, &cursorRestoreTexture);
            if (FAILED(restoreHr)) {
                useSmallRestore = false;
                if (cursorPrecompositionFailureLogs++ < 5) {
                    DLL_Log("[Cursor] Small RGB restore texture creation failed: fmt=%d %ux%u HR=%x", sourceDesc.Format,
                            regionWidth, regionHeight, restoreHr);
                }
            }
        }
    }

    if (useSmallRestore) {
        const D3D11_BOX sourceBox = {
            static_cast<UINT>(clippedLeft),  static_cast<UINT>(clippedTop),    0,
            static_cast<UINT>(clippedRight), static_cast<UINT>(clippedBottom), 1,
        };
        d3d11Context->CopySubresourceRegion(cursorRestoreTexture, 0, 0, 0, 0, source, 0, &sourceBox);
        restore->context = d3d11Context;
        restore->target = source;
        restore->backup = cursorRestoreTexture;
        restore->destinationX = static_cast<UINT>(clippedLeft);
        restore->destinationY = static_cast<UINT>(clippedTop);
        restore->width = regionWidth;
        restore->height = regionHeight;
        restore->active = true;
    } else {
        bool recreateComposite = cursorCompositeTexture == nullptr;
        if (cursorCompositeTexture) {
            D3D11_TEXTURE2D_DESC existing = {};
            cursorCompositeTexture->GetDesc(&existing);
            recreateComposite = existing.Width != sourceDesc.Width || existing.Height != sourceDesc.Height ||
                                existing.MipLevels != sourceDesc.MipLevels ||
                                existing.ArraySize != sourceDesc.ArraySize || existing.Format != sourceDesc.Format ||
                                existing.SampleDesc.Count != sourceDesc.SampleDesc.Count ||
                                existing.SampleDesc.Quality != sourceDesc.SampleDesc.Quality;
        }
        if (recreateComposite) {
            if (cursorCompositeTexture) {
                cursorCompositeTexture->Release();
                cursorCompositeTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC compositeDesc = sourceDesc;
            compositeDesc.Usage = D3D11_USAGE_DEFAULT;
            compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            compositeDesc.CPUAccessFlags = 0;
            compositeDesc.MiscFlags = 0;
            const HRESULT compositeHr = d3d11Device->CreateTexture2D(&compositeDesc, nullptr, &cursorCompositeTexture);
            if (FAILED(compositeHr)) {
                if (cursorPrecompositionFailureLogs++ < 5) {
                    DLL_Log(
                        "[Cursor] RGB cursor fallback texture creation failed; video conversion continues without "
                        "this separate cursor draw: fmt=%d bind=%x HR=%x",
                        sourceDesc.Format, sourceDesc.BindFlags, compositeHr);
                }
                return true;
            }
        }
        d3d11Context->CopyResource(cursorCompositeTexture, source);
        compositionTarget = cursorCompositeTexture;
        *preparedSource = cursorCompositeTexture;
        if (!cursorFullCopyFallbackLogged) {
            DLL_Log("[Cursor] RGB precomposition fallback uses a full-frame GPU copy: fmt=%d bind=%x samples=%u",
                    sourceDesc.Format, sourceDesc.BindFlags, sourceDesc.SampleDesc.Count);
            cursorFullCopyFallbackLogged = true;
        }
    }

    const CursorColorMode colorMode = ce::video_format::IsFp16RgbInputFormat(sourceDesc.Format)
                                          ? CursorColorMode::ScRgb
                                          : (currentIsHDR ? CursorColorMode::Hdr10Pq : CursorColorMode::Sdr);
    const float cursorPaperWhiteNits = currentIsHDR ? sdrWhiteNits : 80.0f;
    if (!cursorRenderer->CompositeOntoFrame(compositionTarget, static_cast<int>(sourceDesc.Width),
                                            static_cast<int>(sourceDesc.Height), cursorCaptureState, colorMode,
                                            cursorPaperWhiteNits)) {
        if (cursorPrecompositionFailureLogs++ < 5) {
            DLL_Log(
                "[Cursor] Point-sampled RGB cursor draw failed; video conversion continues without this separate "
                "cursor draw: fmt=%d bind=%x",
                sourceDesc.Format, sourceDesc.BindFlags);
        }
        return true;
    }

    // The video processor must never observe its input simultaneously bound as
    // a graphics render target. All work remains ordered on this one immediate
    // context; no flush or CPU/GPU wait is required.
    d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
    if (!cursorPrecompositionLogged) {
        DLL_Log(
            "[Cursor] Point RGB precomposition before VP active: fmt=%d bind=%x region=%ux%u smallRestore=%d "
            "(separate and Windows-embedded cursors now share the main conversion stream)",
            sourceDesc.Format, sourceDesc.BindFlags, regionWidth, regionHeight, useSmallRestore ? 1 : 0);
        cursorPrecompositionLogged = true;
    }
    return true;
}

void VideoEncoder::CleanupVideoProcessor() {
    for (auto& cached : outputViewCache) {
        if (cached.view) {
            cached.view->Release();
            cached.view = nullptr;
        }
    }
    outputViewCache.clear();
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.lumaView) {
            cached.lumaView->Release();
            cached.lumaView = nullptr;
        }
        if (cached.chromaView) {
            cached.chromaView->Release();
            cached.chromaView = nullptr;
        }
    }
    hdrP010OutputViewCache.clear();

    CleanupCursorCompositionResources();

    if (inputView) {
        inputView->Release();
        inputView = nullptr;
    }
    if (videoProcessor) {
        videoProcessor->Release();
        videoProcessor = nullptr;
    }
    if (videoProcessorEnum) {
        videoProcessorEnum->Release();
        videoProcessorEnum = nullptr;
    }
    if (videoContext) {
        videoContext->Release();
        videoContext = nullptr;
    }
    if (videoContext1) {
        videoContext1->Release();
        videoContext1 = nullptr;
    }
    if (videoDevice) {
        videoDevice->Release();
        videoDevice = nullptr;
    }
    videoProcessorInit = false;
    use10BitPipeline = false;

    if (vpInputFp16StagingRTV) {
        vpInputFp16StagingRTV->Release();
        vpInputFp16StagingRTV = nullptr;
    }
    if (vpInputFp16Staging) {
        vpInputFp16Staging->Release();
        vpInputFp16Staging = nullptr;
    }
    vpInputFp16StagingW = 0;
    vpInputFp16StagingH = 0;

    // Cleanup SwapRB shader resources
    if (swapRBTextureRTV) {
        swapRBTextureRTV->Release();
        swapRBTextureRTV = nullptr;
    }
    if (swapRBTexture) {
        swapRBTexture->Release();
        swapRBTexture = nullptr;
    }
    if (rgb10IntermediateRTV) {
        rgb10IntermediateRTV->Release();
        rgb10IntermediateRTV = nullptr;
    }
    if (rgb10IntermediateTexture) {
        rgb10IntermediateTexture->Release();
        rgb10IntermediateTexture = nullptr;
    }
    if (swapRBSampler) {
        swapRBSampler->Release();
        swapRBSampler = nullptr;
    }
    if (hdrP010Sampler) {
        hdrP010Sampler->Release();
        hdrP010Sampler = nullptr;
    }
    if (swapRBShaderCB) {
        swapRBShaderCB->Release();
        swapRBShaderCB = nullptr;
    }
    if (swapRBShaderPS) {
        swapRBShaderPS->Release();
        swapRBShaderPS = nullptr;
    }
    if (hdrP010LumaPS) {
        hdrP010LumaPS->Release();
        hdrP010LumaPS = nullptr;
    }
    if (hdrP010ChromaPS) {
        hdrP010ChromaPS->Release();
        hdrP010ChromaPS = nullptr;
    }
    if (swapRBShaderVS) {
        swapRBShaderVS->Release();
        swapRBShaderVS = nullptr;
    }
    swapRBShaderCreated = false;
    swapRBTexWidth = 0;
    swapRBTexHeight = 0;
    rgb10IntermediateWidth = 0;
    rgb10IntermediateHeight = 0;

    // Reset per-recording log flags
    vpFirstCallLogged = false;
    vpDeviceCompareLogged = false;
    vpInputViewLogged = false;
    vpFp16CompatLogged = false;
    vpColorContractLogged = false;
    hdrP010DirectLogged = false;
    hdrToSdrLogged = false;
    sdrWhiteMonitor = nullptr;
    sdrWhiteNits = 203.0f;
    fp16VpInputStrategy = Fp16VpInputStrategy::kUnknown;
    cursorPrecompositionLogged = false;
    cursorFullCopyFallbackLogged = false;
    cursorPrecompositionFailureLogs = 0;
}

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

bool VideoEncoder::ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, AVFrame* outputFrame, bool overlayCursor,
                                     bool allowDirectInputView, int captureOriginX, int captureOriginY,
                                     uint64_t keyedMutexAcquireKey) {
    if (!bgraTexture || !outputFrame || !d3d11FramesCtx) {
        DLL_Log("[VideoProcessor] Invalid RGB conversion input or output frame");
        return false;
    }
    D3D11_TEXTURE2D_DESC captureDesc = {};
    bgraTexture->GetDesc(&captureDesc);
    UpdateSdrWhiteLevelForCaptureArea(captureOriginX, captureOriginY, captureDesc.Width, captureDesc.Height);
    const bool outputIsHDR = ShouldEncodeHdrOutput();
    if (!videoProcessorInit) {
        if (!InitVideoProcessor())
            return false;
    }

    if (!outputFrame->buf[0]) {
        const int frameRet = av_hwframe_get_buffer(d3d11FramesCtx, outputFrame, 0);
        if (frameRet < 0 || !outputFrame->data[0]) {
            DLL_Log("[VideoProcessor] Failed to allocate AVHWFrame-owned output: %d", frameRet);
            return false;
        }
    }
    if (!outputFrame->data[0]) {
        DLL_Log("[VideoProcessor] AVHWFrame output is missing its D3D11 texture");
        return false;
    }

    auto* outputTexture = reinterpret_cast<ID3D11Texture2D*>(outputFrame->data[0]);
    const UINT outputArraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(outputFrame->data[1]));
    ID3D11VideoProcessorOutputView* outputView = nullptr;
    if (!outputIsHDR) {
        for (const auto& cached : outputViewCache) {
            if (cached.texture == outputTexture && cached.arraySlice == outputArraySlice) {
                outputView = cached.view;
                break;
            }
        }

        if (!outputView) {
            D3D11_TEXTURE2D_DESC outputTextureDesc = {};
            outputTexture->GetDesc(&outputTextureDesc);
            if (outputArraySlice >= outputTextureDesc.ArraySize) {
                DLL_Log("[VideoProcessor] Invalid AVHWFrame array slice %u for array size %u", outputArraySlice,
                        outputTextureDesc.ArraySize);
                return false;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
            if (outputTextureDesc.ArraySize > 1) {
                outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
                outputViewDesc.Texture2DArray.MipSlice = 0;
                outputViewDesc.Texture2DArray.FirstArraySlice = outputArraySlice;
                outputViewDesc.Texture2DArray.ArraySize = 1;
            } else {
                outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                outputViewDesc.Texture2D.MipSlice = 0;
            }

            HRESULT outputViewHr = E_FAIL;
            try {
                outputViewHr = videoDevice->CreateVideoProcessorOutputView(outputTexture, videoProcessorEnum,
                                                                           &outputViewDesc, &outputView);
            } catch (...) {
                outputViewHr = E_FAIL;
            }
            if (FAILED(outputViewHr) || !outputView) {
                DLL_Log(
                    "[VideoProcessor] Failed to bind AVHWFrame output view: HR=%x fmt=%d bind=%x array=%u slice=%u",
                    outputViewHr, outputTextureDesc.Format, outputTextureDesc.BindFlags, outputTextureDesc.ArraySize,
                    outputArraySlice);
                return false;
            }
            outputViewCache.push_back({outputTexture, outputArraySlice, outputView});
            if (outputViewCache.size() == 1) {
                DLL_Log("[VideoProcessor] AVHWFrame output-view cache active (fmt=%d bind=%x)",
                        outputTextureDesc.Format, outputTextureDesc.BindFlags);
            }
        }
    }

    // Debug: Log texture descriptions on first call per recording
    if (!vpFirstCallLogged) {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        DLL_Log("[VP DEBUG] Source tex: %dx%d fmt=%d bind=%x misc=%x", srcDesc.Width, srcDesc.Height, srcDesc.Format,
                srcDesc.BindFlags, srcDesc.MiscFlags);
    }

    // Track whether this recording is using the direct VP P010 path.
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        const bool shouldUse10BitPipeline =
            ce::video_format::IsHighPrecisionRgbInputFormat(srcDesc.Format) && ShouldUse10BitOutput();
        if (shouldUse10BitPipeline != use10BitPipeline) {
            use10BitPipeline = shouldUse10BitPipeline;
            DLL_Log("[VP] Input fmt=%d, VP output pipeline=%s", srcDesc.Format, use10BitPipeline ? "P010" : "NV12");
        }
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

    bgraTexture->QueryInterface(IID_PPV_ARGS(&keyedMutexGuard.mutex));
    if (keyedMutexGuard.mutex) {
        HRESULT kmHr = keyedMutexGuard.mutex->AcquireSync(keyedMutexAcquireKey, 1000);
        if (kmHr != S_OK) {
            static int kmFailCount = 0;
            if (kmFailCount++ < 5) {
                DLL_Log("[VideoProcessor] KeyedMutex AcquireSync failed: HR=%x", kmHr);
            }
            keyedMutexGuard.mutex->Release();
            keyedMutexGuard.mutex = nullptr;
            return false;
        }
        keyedMutexGuard.acquired = true;
    }

    CursorSourceRestore cursorSourceRestore;
    ID3D11Texture2D* preparedCursorSource = bgraTexture;
    if (!PrepareVideoProcessorCursorInput(bgraTexture, overlayCursor, &cursorSourceRestore, &preparedCursorSource)) {
        return false;
    }

    // Try to create the VP input view directly from the source texture only for
    // inject/shared-handle frames. WGC/direct-texture frames are valid capture
    // inputs, but probing them with CreateVideoProcessorInputView can raise a
    // handled D3D11 C++ exception before we fall back to the already-working
    // staging path.
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;

    // If input is RGBA, swap R/B channels to produce BGRA before VP processing.
    // D3D11 Video Processor expects BGRA input; DXVK KMT textures may be RGBA.
    // A fullscreen shader pass with a BGRA render target handles the byte reorder.
    ID3D11Texture2D* vpInputTexture = preparedCursorSource;
    bool allowVpInputView = allowDirectInputView;
    bool needReleaseConverted = false;
    bool vpInputIsLinear = false;
    bool wantsFp16VpStagingPath = false;
    D3D11_TEXTURE2D_DESC vpInputDesc = {};
    auto releaseConvertedInput = [&]() {
        if (needReleaseConverted && vpInputTexture && vpInputTexture != preparedCursorSource) {
            vpInputTexture->Release();
        }
        needReleaseConverted = false;
    };
    auto prepareHighPrecisionRgb10CompatInput = [&](HRESULT priorHr) -> bool {
        const DXGI_FORMAT sourceFormat = vpInputDesc.Format;
        const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceFormat);
        if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
            DLL_Log("[VP] High-precision RGB10 fallback unsupported source format: fmt=%d", sourceFormat);
            return false;
        }
        const ce::video_format::RgbColorTransform colorTransform =
            ce::video_format::GetRgbColorTransform(sourceFormat, currentIsHDR, outputIsHDR);
        ID3D11Texture2D* converted =
            RenderFullscreenCopy(vpInputTexture, vpInputDesc.Width, vpInputDesc.Height, inputSrvFormat,
                                 DXGI_FORMAT_R10G10B10A2_UNORM, rgb10IntermediateTexture, rgb10IntermediateRTV,
                                 rgb10IntermediateWidth, rgb10IntermediateHeight, "RGB10", colorTransform,
                                 sdrWhiteNits);
        if (!converted) {
            DLL_Log("[VP] Failed to convert high-precision input to RGB10A2 before VP (srcFmt=%d srvFmt=%d)",
                    sourceFormat, inputSrvFormat);
            return false;
        }
        if (needReleaseConverted && vpInputTexture != preparedCursorSource) {
            vpInputTexture->Release();
        }
        vpInputTexture = converted;
        needReleaseConverted = true;
        allowVpInputView = true;
        vpInputIsLinear = ce::video_format::IsFp16RgbInputFormat(sourceFormat) &&
                          colorTransform == ce::video_format::RgbColorTransform::kNone;
        vpInputTexture->GetDesc(&vpInputDesc);
        if (!vpFp16CompatLogged) {
            DLL_Log(
                "[VP] High-precision input normalization: priorHR=%x srcFmt=%d srvFmt=%d final=RGB10A2 transform=%s "
                "output=%s",
                priorHr, sourceFormat, inputSrvFormat, ce::video_format::DescribeRgbColorTransform(colorTransform),
                ShouldUse10BitOutput() ? "P010" : "NV12");
            vpFp16CompatLogged = true;
        }
        if (!outputIsHDR && currentIsHDR && !hdrToSdrLogged) {
            DLL_Log(
                "[HDR->SDR] Whole-frame shader tone map active: transform=%s sourceWhite=%.1f-nit "
                "output=BT709-G22 headroom=80%% passes=1 intermediate=RGB10 gamut=luminance-preserving "
                "driverVPTransfer=0 cpuWait=0 frameAlpha=opaque",
                ce::video_format::DescribeRgbColorTransform(colorTransform), sdrWhiteNits);
            hdrToSdrLogged = true;
        }
        return true;
    };
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        preparedCursorSource->GetDesc(&srcDesc);
        if (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            ID3D11Texture2D* converted = SwapRBChannels(preparedCursorSource, srcDesc.Width, srcDesc.Height);
            if (converted) {
                vpInputTexture = converted;
                needReleaseConverted = true;
                if (!vpFirstCallLogged)
                    DLL_Log("[VP] RGBA input detected - R/B swap applied before VP");
            } else {
                DLL_Log("[VP] R/B swap failed, using original texture");
            }
        }

        // CRITICAL: Validate texture before passing to D3D11 VideoProcessor.
        // D3D11's CreateVideoProcessorInputView throws SEH for incompatible formats,
        // and MinGW catch(...) CANNOT catch SEH exceptions (__fastfail).
        // We must prevent the call by checking format compatibility first.
        vpInputTexture->GetDesc(&vpInputDesc);
        const ce::video_format::RgbColorTransform requiredHdrTransform =
            ce::video_format::GetRgbColorTransform(vpInputDesc.Format, currentIsHDR, outputIsHDR);
        if (currentIsHDR && !ce::video_format::IsFp16RgbInputFormat(vpInputDesc.Format) &&
            !ce::video_format::IsHdr10RgbInputFormat(vpInputDesc.Format)) {
            DLL_Log("[HDR Color] Unsupported HDR source format %d; refusing an unconverted output",
                    vpInputDesc.Format);
            releaseConvertedInput();
            return false;
        }
        const bool normalizeHdrForOutput = currentIsHDR &&
                                           requiredHdrTransform != ce::video_format::RgbColorTransform::kNone;
        if (normalizeHdrForOutput && !prepareHighPrecisionRgb10CompatInput(S_OK)) {
            releaseConvertedInput();
            return false;
        }
        vpInputTexture->GetDesc(&vpInputDesc);
        wantsFp16VpStagingPath = !allowDirectInputView && ce::video_format::IsFp16RgbInputFormat(vpInputDesc.Format);
        if (wantsFp16VpStagingPath && fp16VpInputStrategy == Fp16VpInputStrategy::kUseRgb10Compat) {
            if (!prepareHighPrecisionRgb10CompatInput(S_OK)) {
                releaseConvertedInput();
                return false;
            }
        }
        if (outputIsHDR) {
            const bool converted = ConvertHdrRgb10ToP010(vpInputTexture, outputTexture, outputArraySlice);
            releaseConvertedInput();
            if (!converted) {
                DLL_Log(
                    "[HDR P010] Direct shader conversion failed; refusing the driver VideoProcessor PQ fallback "
                    "that produced corrupt colors");
            }
            vpFirstCallLogged = true;
            return converted;
        }
        bool vpCompatible =
            (vpInputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || vpInputDesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
             vpInputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
             vpInputDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||  // WGC can provide TYPELESS with 10-bit display
             vpInputDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
             vpInputDesc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS || vpInputDesc.Format == DXGI_FORMAT_NV12 ||
             vpInputDesc.Format == DXGI_FORMAT_P010);

        if (!vpCompatible) {
            DLL_Log("[VP] Texture format %d not VP-compatible, frame dropped", vpInputDesc.Format);
            releaseConvertedInput();
            return false;  // Cannot convert - format not supported by VP
        }
    }

    vpFirstCallLogged = true;

    // CRITICAL: CreateVideoProcessorInputView can throw SEH for incompatible formats,
    // and MinGW catch(...) CANNOT catch SEH exceptions (__fastfail).
    // The texture format was pre-validated above, but the try/catch is a safety net.
    ID3D11VideoProcessorInputView* localInputView = nullptr;
    HRESULT hr = E_FAIL;
    if (allowVpInputView) {
        try {
            hr = videoDevice->CreateVideoProcessorInputView(vpInputTexture, videoProcessorEnum, &inputViewDesc,
                                                            &localInputView);
        } catch (...) {
            hr = E_FAIL;
            DLL_Log("[VP] CreateVideoProcessorInputView threw exception (fmt=%d)", vpInputDesc.Format);
        }
    }

    // Log CreateVideoProcessorInputView result on first call per recording
    if (allowVpInputView && !vpInputViewLogged) {
        vpInputViewLogged = true;
        DLL_Log("[VP] CreateVideoProcessorInputView(fmt=%d, bind=%x): HR=%x%s", vpInputDesc.Format,
                vpInputDesc.BindFlags, hr, SUCCEEDED(hr) ? " (direct OK)" : "");
    }

    if (!allowVpInputView || FAILED(hr)) {
        if (allowVpInputView) {
            static bool stagingLogged = false;
            if (!stagingLogged) {
                DLL_Log(
                    "[VP] Direct input view failed (HR=%x), using "
                    "staging copy",
                    hr);
                stagingLogged = true;
            }
        } else {
            static bool stagingBypassLogged = false;
            if (!stagingBypassLogged) {
                DLL_Log("[VP] D3D11 direct-texture path uses staging input by design");
                stagingBypassLogged = true;
            }
        }

        if (bgraStagingTexture) {
            D3D11_TEXTURE2D_DESC stageDesc;
            bgraStagingTexture->GetDesc(&stageDesc);

            // Check if staging texture needs to be recreated with correct
            // format
            if (stageDesc.Format != vpInputDesc.Format || stageDesc.Width != vpInputDesc.Width ||
                stageDesc.Height != vpInputDesc.Height) {
                DLL_Log("[VP] Recreating staging texture: %ux%u fmt %d -> %ux%u fmt %d", stageDesc.Width,
                        stageDesc.Height, stageDesc.Format, vpInputDesc.Width, vpInputDesc.Height, vpInputDesc.Format);
                bgraStagingTexture->Release();
                bgraStagingTexture = nullptr;
            }
        }

        // Create staging texture if needed
        if (!bgraStagingTexture) {
            D3D11_TEXTURE2D_DESC stageDesc = {};
            stageDesc.Width = vpInputDesc.Width;
            stageDesc.Height = vpInputDesc.Height;
            stageDesc.MipLevels = 1;
            stageDesc.ArraySize = 1;
            stageDesc.Format = vpInputDesc.Format;  // Match the texture fed into the VP path.
            stageDesc.SampleDesc.Count = 1;
            stageDesc.Usage = D3D11_USAGE_DEFAULT;
            stageDesc.BindFlags = 0;  // Compatible with VP

            ID3D11Device* baseDevice = nullptr;
            d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)&baseDevice);
            hr = baseDevice->CreateTexture2D(&stageDesc, nullptr, &bgraStagingTexture);
            baseDevice->Release();

            if (FAILED(hr)) {
                DLL_Log("[VP] Failed to create staging texture: HR=%x", hr);
                releaseConvertedInput();
                return false;
            }
            DLL_Log("[VP] Created staging texture: %ux%u fmt=%d", vpInputDesc.Width, vpInputDesc.Height,
                    vpInputDesc.Format);
        }

        // Copy to staging
        ID3D11DeviceContext* ctx = nullptr;
        d3d11Device->GetImmediateContext(&ctx);
        if (ctx) {
            ctx->CopyResource(bgraStagingTexture, vpInputTexture);

            // Debug: Log copy on first few frames
            static int copyCount = 0;
            if (copyCount++ < 5) {
                DLL_Log("[VP] CopyResource to staging - frame %d", copyCount);
            }
            ctx->Release();
        }

        // Create input view from staging texture
        try {
            hr = videoDevice->CreateVideoProcessorInputView(bgraStagingTexture, videoProcessorEnum, &inputViewDesc,
                                                            &localInputView);
        } catch (...) {
            hr = E_FAIL;
        }
        if (FAILED(hr)) {
            const DXGI_FORMAT failedVpInputFormat = vpInputDesc.Format;
            if (ce::video_format::IsHighPrecisionRgbInputFormat(failedVpInputFormat)) {
                if (ce::video_format::IsFp16RgbInputFormat(failedVpInputFormat)) {
                    fp16VpInputStrategy = Fp16VpInputStrategy::kUseRgb10Compat;
                }
                if (!prepareHighPrecisionRgb10CompatInput(hr)) {
                    DLL_Log("[VP] Failed high-precision RGB10A2 compatibility blit after staging input failure");
                    releaseConvertedInput();
                    return false;
                }
                try {
                    hr = videoDevice->CreateVideoProcessorInputView(vpInputTexture, videoProcessorEnum, &inputViewDesc,
                                                                    &localInputView);
                } catch (...) {
                    hr = E_FAIL;
                }
                if (FAILED(hr)) {
                    DLL_Log("[VP] RGB10A2 VP input view failed after high-precision fallback: srcFmt=%d HR=%x",
                            failedVpInputFormat, hr);
                    releaseConvertedInput();
                    return false;
                }
                DLL_Log("[VP] Using RGB10A2 VP input for high-precision source fmt=%d", failedVpInputFormat);
            } else {
                DLL_Log("[VP] Failed to create input view from staging: HR=%x", hr);
                releaseConvertedInput();
                return false;
            }
        } else if (wantsFp16VpStagingPath && fp16VpInputStrategy == Fp16VpInputStrategy::kUnknown) {
            fp16VpInputStrategy = Fp16VpInputStrategy::kUseStaging;
            DLL_Log("[VP] Using native FP16 staging input for %s VP path", ShouldUse10BitOutput() ? "10-bit" : "8-bit");
        }
        // inputTexture = bgraStagingTexture;
    }

    if (videoContext1) {
        std::string configuredColorSpace = savedConfig.colorSpace;
        if (_stricmp(configuredColorSpace.c_str(), "auto") == 0 || configuredColorSpace.empty()) {
            configuredColorSpace = outputIsHDR ? "bt2020" : "bt709";
        } else if (_stricmp(configuredColorSpace.c_str(), "bt2020") == 0) {
            configuredColorSpace = "bt2020";
        } else {
            configuredColorSpace = "bt709";
        }
        const OutputRangeMode outputRange = GetEffectiveOutputRange(savedConfig.colorRange, outputIsHDR);
        const DXGI_COLOR_SPACE_TYPE inputColorSpace =
            GetVideoProcessorInputColorSpace(vpInputDesc.Format, outputIsHDR, vpInputIsLinear);
        const DXGI_COLOR_SPACE_TYPE outputColorSpace =
            GetVideoProcessorOutputColorSpace(ShouldUse10BitOutput(), outputIsHDR, configuredColorSpace, outputRange);
        videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor, 0, inputColorSpace);
        videoContext1->VideoProcessorSetOutputColorSpace1(videoProcessor, outputColorSpace);
        if (!vpColorContractLogged) {
            DLL_Log(
                "[VideoProcessor] ColorSpace1 contract: input=%d output=%d inputFmt=%d sourceHdr=%d outputHdr=%d "
                "bitDepth=%s range=%s",
                static_cast<int>(inputColorSpace), static_cast<int>(outputColorSpace), vpInputDesc.Format,
                currentIsHDR ? 1 : 0, outputIsHDR ? 1 : 0, ShouldUse10BitOutput() ? "10" : "8",
                DescribeOutputRange(outputRange));
            vpColorContractLogged = true;
        }
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = localInputView;

    hr = videoContext->VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream);

    localInputView->Release();

    if (FAILED(hr)) {
        static int bltFailCount = 0;
        if (bltFailCount++ < 5) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            bgraTexture->GetDesc(&srcDesc);
            const HRESULT deviceReason = d3d11Device->GetDeviceRemovedReason();
            DLL_Log(
                "[VideoProcessor] Blt failed. HR=%x streams=1 outputSlice=%u "
                "srcFmt=%d srcW=%u srcH=%u srcBind=%x srcMisc=%x "
                "inputW=%d inputH=%d outputW=%d outputH=%d deviceReason=%x",
                hr, outputArraySlice, srcDesc.Format, srcDesc.Width, srcDesc.Height, srcDesc.BindFlags,
                srcDesc.MiscFlags, inputWidth, inputHeight, outputWidth, outputHeight, deviceReason);
        }
        if (needReleaseConverted)
            vpInputTexture->Release();
        return false;
    }

    if (needReleaseConverted)
        vpInputTexture->Release();

    return true;
}

bool VideoEncoder::EnsureSwapRBShader() {
    if (swapRBShaderCreated)
        return true;

    // ID3DBlob's implementation and vtable live in d3dcompiler_47.dll. Keep the
    // module loaded until every compiler-owned blob below has been consumed and
    // released (ModuleGuard is declared first, so it is destroyed last).
    ce::ModuleGuard d3dCompiler(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
    if (!d3dCompiler) {
        DLL_Log("[SwapRB] Failed to load d3dcompiler_47.dll");
        return false;
    }

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler.get(), "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[SwapRB] Failed to get D3DCompile");
        return false;
    }

    auto compileShader = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& output) -> HRESULT {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT compileHr = d3dCompile(
            ce::video_color::kRgbColorConversionShaderSource,
            strlen(ce::video_color::kRgbColorConversionShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0,
            output.addressof(), errors.addressof());
        if (errors) {
            DLL_Log("[RGBConvert] %s/%s compiler output: %s", entry, target,
                    static_cast<const char*>(errors->GetBufferPointer()));
        }
        return compileHr;
    };

    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> copyPsBlob;

    ce::ComGuard<ID3DBlob> p010YBlob;
    ce::ComGuard<ID3DBlob> p010UvBlob;
    HRESULT hr = compileShader("VS_Main", "vs_4_0", vsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_Main", "ps_4_0", copyPsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010Y", "ps_4_0", p010YBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010UV", "ps_4_0", p010UvBlob);
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader compilation failed: HR=%x", hr);
        return false;
    }

    ce::ComGuard<ID3D11VertexShader> copyVs;
    ce::ComGuard<ID3D11PixelShader> copyPs;
    ce::ComGuard<ID3D11PixelShader> p010Y;
    ce::ComGuard<ID3D11PixelShader> p010Uv;
    hr = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                         copyVs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(copyPsBlob->GetBufferPointer(), copyPsBlob->GetBufferSize(), nullptr,
                                            copyPs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010YBlob->GetBufferPointer(), p010YBlob->GetBufferSize(), nullptr,
                                            p010Y.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010UvBlob->GetBufferPointer(), p010UvBlob->GetBufferSize(), nullptr,
                                            p010Uv.addressof());
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader creation failed: HR=%x", hr);
        return false;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> copySampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, copySampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreateSamplerState failed: HR=%x", hr);
        return false;
    }
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ce::ComGuard<ID3D11SamplerState> p010Sampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, p010Sampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[HDR P010] CreateSamplerState failed: HR=%x", hr);
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ce::ComGuard<ID3D11Buffer> constants;
    hr = d3d11Device->CreateBuffer(&cbDesc, nullptr, constants.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] Create constant buffer failed: HR=%x", hr);
        return false;
    }

    swapRBShaderVS = copyVs.release();
    swapRBShaderPS = copyPs.release();
    hdrP010LumaPS = p010Y.release();
    hdrP010ChromaPS = p010Uv.release();
    swapRBSampler = copySampler.release();
    hdrP010Sampler = p010Sampler.release();
    swapRBShaderCB = constants.release();
    swapRBShaderCreated = true;
    DLL_Log("[RGBConvert] Copy/scRGB/P010 shaders created successfully (compiler lifetime blob-scoped)");
    return true;
}

ID3D11Texture2D* VideoEncoder::RenderFullscreenCopy(ID3D11Texture2D* input, uint32_t w, uint32_t h,
                                                    DXGI_FORMAT inputSrvFormat, DXGI_FORMAT outputFormat,
                                                    ID3D11Texture2D*& cachedTexture, ID3D11RenderTargetView*& cachedRTV,
                                                    uint32_t& cachedWidth, uint32_t& cachedHeight,
                                                    const char* logPrefix,
                                                    ce::video_format::RgbColorTransform colorTransform,
                                                    float toneMapSdrWhiteNits) {
    if (!EnsureSwapRBShader())
        return nullptr;

    if (!cachedTexture || cachedWidth != w || cachedHeight != h) {
        if (cachedRTV) {
            cachedRTV->Release();
            cachedRTV = nullptr;
        }
        if (cachedTexture) {
            cachedTexture->Release();
            cachedTexture = nullptr;
        }

        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = w;
        outDesc.Height = h;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = outputFormat;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11Device->CreateTexture2D(&outDesc, nullptr, &cachedTexture);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create output texture fmt=%d: HR=%x", logPrefix, outputFormat, hr);
            return nullptr;
        }

        hr = d3d11Device->CreateRenderTargetView(cachedTexture, nullptr, &cachedRTV);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create RTV: HR=%x", logPrefix, hr);
            cachedTexture->Release();
            cachedTexture = nullptr;
            return nullptr;
        }

        cachedWidth = w;
        cachedHeight = h;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = inputSrvFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC inputDesc = {};
    input->GetDesc(&inputDesc);
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &srv);
    if (FAILED(hr)) {
        static std::atomic<int> srvFailLogCount{0};
        if (srvFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            DLL_Log("[%s] Failed to create SRV: texFmt=%d srvFmt=%d bind=%x misc=%x HR=%x", logPrefix, inputDesc.Format,
                    inputSrvFormat, inputDesc.BindFlags, inputDesc.MiscFlags, hr);
        }
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[%s] Failed to map shader constant buffer: HR=%x", logPrefix, hr);
        srv->Release();
        return nullptr;
    }
    memset(mapped.pData, 0, 32);
    uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
    cbData[0] = static_cast<uint32_t>(colorTransform);
    float* cbFloats = static_cast<float*>(mapped.pData);
    cbFloats[5] = std::clamp(toneMapSdrWhiteNits, 80.0f, 1000.0f);
    d3d11Context->Unmap(swapRBShaderCB, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &vp);
    d3d11Context->OMSetRenderTargets(1, &cachedRTV, nullptr);
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShader(swapRBShaderPS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &srv);
    d3d11Context->PSSetSamplers(0, 1, &swapRBSampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);
    d3d11Context->Draw(3, 0);

    // Unbind render target and SRV
    ID3D11RenderTargetView* nullRTV = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSRV);
    srv->Release();

    cachedTexture->AddRef();  // Caller releases
    return cachedTexture;
}

bool VideoEncoder::ConvertHdrRgb10ToP010(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT outputArraySlice) {
    if (!input || !output || !EnsureSwapRBShader()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    if (inputDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || outputDesc.Format != DXGI_FORMAT_P010 ||
        outputArraySlice >= outputDesc.ArraySize || (outputDesc.Width & 1) != 0 || (outputDesc.Height & 1) != 0) {
        DLL_Log(
            "[HDR P010] Invalid direct conversion surfaces: inputFmt=%d outputFmt=%d output=%ux%u array=%u slice=%u",
            inputDesc.Format, outputDesc.Format, outputDesc.Width, outputDesc.Height, outputDesc.ArraySize,
            outputArraySlice);
        return false;
    }

    CachedHdrP010OutputViews* outputViews = nullptr;
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.texture == output && cached.arraySlice == outputArraySlice) {
            outputViews = &cached;
            break;
        }
    }
    if (!outputViews) {
        CachedHdrP010OutputViews cached = {};
        cached.texture = output;
        cached.arraySlice = outputArraySlice;

        auto createPlaneView = [&](DXGI_FORMAT format, UINT plane,
                                   ID3D11RenderTargetView1** view) -> HRESULT {
            D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
            desc.Format = format;
            if (outputDesc.ArraySize > 1) {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipSlice = 0;
                desc.Texture2DArray.FirstArraySlice = outputArraySlice;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.PlaneSlice = plane;
            } else {
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = 0;
                desc.Texture2D.PlaneSlice = plane;
            }
            return d3d11Device->CreateRenderTargetView1(output, &desc, view);
        };

        HRESULT hr = createPlaneView(DXGI_FORMAT_R16_UNORM, 0, &cached.lumaView);
        if (SUCCEEDED(hr)) {
            hr = createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, &cached.chromaView);
        }
        if (FAILED(hr) || !cached.lumaView || !cached.chromaView) {
            DLL_Log(
                "[HDR P010] Failed to create plane RTVs: HR=%x bind=%x array=%u slice=%u; refusing corrupt VP "
                "fallback",
                hr, outputDesc.BindFlags, outputDesc.ArraySize, outputArraySlice);
            if (cached.lumaView)
                cached.lumaView->Release();
            if (cached.chromaView)
                cached.chromaView->Release();
            return false;
        }
        hdrP010OutputViewCache.push_back(cached);
        outputViews = &hdrP010OutputViewCache.back();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* inputView = nullptr;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &inputView);
    if (FAILED(hr) || !inputView) {
        DLL_Log("[HDR P010] Failed to create RGB10 input SRV: HR=%x bind=%x", hr, inputDesc.BindFlags);
        return false;
    }

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float padding2[3];
    };
    static_assert(sizeof(CopyConstants) == 32);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[HDR P010] Failed to map shader constants: HR=%x", hr);
        inputView->Release();
        return false;
    }
    const float lumaSharpenStrength =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        scalingEnabled ? std::clamp(savedConfig.scaling.sharpness / 400.0f, 0.0f, 0.25f) : 0.0f;
    const float fullRangeFlag = WantsFullOutputRange(savedConfig.colorRange) ? 1.0f : 0.0f;
    *static_cast<CopyConstants*>(mapped.pData) = {
        0, 0, 1.0f / static_cast<float>(outputDesc.Width), 1.0f / static_cast<float>(outputDesc.Height),
        lumaSharpenStrength, {fullRangeFlag, 0.0f, 0.0f}};
    d3d11Context->Unmap(swapRBShaderCB, 0);

    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &inputView);
    d3d11Context->PSSetSamplers(0, 1, &hdrP010Sampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(outputDesc.Width);
    viewport.Height = static_cast<float>(outputDesc.Height);
    viewport.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* lumaView = outputViews->lumaView;
    d3d11Context->OMSetRenderTargets(1, &lumaView, nullptr);
    d3d11Context->PSSetShader(hdrP010LumaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    // NOLINTNEXTLINE(bugprone-integer-division) - P010 chroma is half-size and dimensions are even by construction
    viewport.Width = static_cast<float>(outputDesc.Width / 2);
    // NOLINTNEXTLINE(bugprone-integer-division) - P010 chroma is half-size and dimensions are even by construction
    viewport.Height = static_cast<float>(outputDesc.Height / 2);
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* chromaView = outputViews->chromaView;
    d3d11Context->OMSetRenderTargets(1, &chromaView, nullptr);
    d3d11Context->PSSetShader(hdrP010ChromaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSrv);
    inputView->Release();

    if (!hdrP010DirectLogged) {
        DLL_Log(
            "[HDR P010] Direct shader conversion active: input=RGB10_PQ_P2020 output=P010_BT2020NCL_%s "
            "matrix=shader chroma=top-left planes=R16/R16G16 scaling=%ux%u->%ux%u lumaSharpen=%.3f "
            "driverVP=0 cpuWait=0",
            WantsFullOutputRange(savedConfig.colorRange) ? "FULL" : "LIMITED", inputDesc.Width, inputDesc.Height,
            outputDesc.Width, outputDesc.Height, lumaSharpenStrength);
        hdrP010DirectLogged = true;
    }
    return true;
}

ID3D11Texture2D* VideoEncoder::SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h) {
    return RenderFullscreenCopy(input, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture,
                                swapRBTextureRTV, swapRBTexWidth, swapRBTexHeight, "SwapRB");
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

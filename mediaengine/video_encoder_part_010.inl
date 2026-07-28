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

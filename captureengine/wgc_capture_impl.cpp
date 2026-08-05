#include "wgc_capture_internal.h"


#if HAS_WGC

WGCCapture::Impl::~Impl() {


        alive_.store(false, std::memory_order_release);
        StopCapture();
        UnsubscribeItemClosed();
        itemCallbackState_->DetachAndDrain();
        ReleaseTexturePool();
        SafeRelease(latestFrame_);
        SafeRelease(cachedTexture_);
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }
        if (frameArrivedEvent_) {
            CloseHandle(frameArrivedEvent_);
            frameArrivedEvent_ = NULL;
        }

}

#endif

#if HAS_WGC

void WGCCapture::Impl::FlagResetNeeded(const char* reason) {


        resetNeeded_.store(true, std::memory_order_release);
        if (reason && *reason) {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            if (resetReason_.empty()) {
                resetReason_ = reason;
            }
        }

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::NeedsReset() const {


        return resetNeeded_.load(std::memory_order_acquire);

}

#endif

#if HAS_WGC

std::string WGCCapture::Impl::ConsumeResetReason() {


        resetNeeded_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(resetReasonMutex_);
        std::string reason = resetReason_;
        resetReason_.clear();
        return reason;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::PerformHDRRecheck() {


        const ULONGLONG now = GetTickCount64();
        lastHDRCheckTick_.store(now, std::memory_order_relaxed);

        const HMONITOR monitor = ResolveTargetMonitor();
        if (!monitor)
            return;

        DXGI_OUTPUT_DESC1 desc1 = {};
        if (QueryOutputDesc1ForMonitor(monitor, desc1)) {
            bool newHDR = ::IsHdrOutputColorSpace(desc1.ColorSpace);
            if (newHDR != captureIsHDR_) {
                // The WGC frame-pool pixel format is immutable. Merely changing
                // the metadata flag would mislabel frames and apply the wrong
                // color conversion; recreate the source/pool on the new mode.
                LogInfo("[WGC] HDR state changed mid-capture: %s -> %s; requesting capture recreation",
                        captureIsHDR_ ? "HDR" : "SDR", newHDR ? "HDR" : "SDR");
                FlagResetNeeded("HDR output state changed");
            }
        }

}

#endif

#if HAS_WGC

void WGCCapture::Impl::RequestHDRRecheckIfDue() {


        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        hdrRecheckPending_.store(true, std::memory_order_relaxed);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::MaybePerformDeferredHDRRecheck() {


        if (!hdrRecheckPending_.exchange(false, std::memory_order_relaxed)) {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        PerformHDRRecheck();

}

#endif

#if HAS_WGC

const char* WGCCapture::Impl::DescribeCaptureFormat() const {


        switch (captureDxgiFormat_) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
            default:
                return "UNKNOWN";
        }

}

#endif

#if HAS_WGC

uint32_t WGCCapture::Impl::BytesPerPixelForFormat(DXGI_FORMAT format) const {


        switch (format) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            default:
                return 4;
        }

}

#endif

#if HAS_WGC

DXGI_FORMAT WGCCapture::Impl::GetRetainedPoolFormat(DXGI_FORMAT sourceFormat) const {


        if (ce::video_format::ShouldApplySdrLinearToSrgbBeforeRgb10(sourceFormat, captureIsHDR_)) {
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        }
        return sourceFormat;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::IsCompactRetainedCopy(DXGI_FORMAT sourceFormat,  DXGI_FORMAT retainedFormat) const {


        return retainedFormat != sourceFormat;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ReleasePoolConversionResources() {


        for (auto* rtv : poolRenderTargetViews_) {
            SafeRelease(rtv);
        }
        poolRenderTargetViews_.clear();
        SafeRelease(poolCopyStagingSrv_);
        SafeRelease(poolCopyStagingTexture_);
        poolCopyStagingWidth_ = 0;
        poolCopyStagingHeight_ = 0;
        poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
        SafeRelease(poolCopyCB_);
        SafeRelease(poolCopySampler_);
        SafeRelease(poolCopyPS_);
        SafeRelease(poolCopyVS_);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ReleaseGpuTimingResources() {


        SafeRelease(gpuTimingEnd_);
        SafeRelease(gpuTimingStart_);
        SafeRelease(gpuTimingDisjoint_);
        gpuTimingPending_ = false;
        gpuTimingActive_ = false;
        gpuTimingSubmitQpc_ = 0;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::EnsurePoolCopyShader() {


        if (poolCopyVS_ && poolCopyPS_ && poolCopySampler_ && poolCopyCB_) {
            return true;
        }
        if (!d3dDevice_) {
            return false;
        }

        HMODULE d3dCompiler = ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll");
        if (!d3dCompiler) {
            LogError("[WGC] Failed to load d3dcompiler_47.dll for retained-copy conversion");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                                 LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto d3dCompile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(d3dCompiler, "D3DCompile"));
        if (!d3dCompile) {
            LogError("[WGC] Failed to resolve D3DCompile for retained-copy conversion");
            FreeLibrary(d3dCompiler);
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy VS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                        "PS_Main", "ps_4_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy PS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            SafeRelease(vsBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &poolCopyVS_);
        SafeRelease(vsBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateVertexShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(psBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }

        hr = d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &poolCopyPS_);
        SafeRelease(psBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreatePixelShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyVS_);
            FreeLibrary(d3dCompiler);
            return false;
        }
        FreeLibrary(d3dCompiler);

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = d3dDevice_->CreateSamplerState(&samplerDesc, &poolCopySampler_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateSamplerState failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, &poolCopyCB_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateBuffer failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopySampler_);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        LogInfo("[WGC] Retained-copy conversion shader created");
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::CreatePoolCopySourceSrv(ID3D11Texture2D* sourceTexture,  const D3D11_TEXTURE2D_DESC& sourceDesc, 
                                 DXGI_FORMAT inputSrvFormat,  ID3D11ShaderResourceView** outSrv,  bool* usedStaging) {


        if (!sourceTexture || !outSrv) {
            return false;
        }
        *outSrv = nullptr;
        if (usedStaging) {
            *usedStaging = false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = inputSrvFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        HRESULT hr = d3dDevice_->CreateShaderResourceView(sourceTexture, &srvDesc, outSrv);
        if (SUCCEEDED(hr) && *outSrv) {
            return true;
        }

        static std::atomic<uint32_t> directSrvFailLogCount{0};
        const uint32_t failLog = directSrvFailLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failLog <= 4) {
            LogInfo(
                "[WGC] Direct WGC source SRV unavailable for retained-copy conversion; using reusable staging "
                "(srcFmt=%s srvFmt=%s bind=0x%X misc=0x%X hr=0x%08lX)",
                DxgiFormatName(sourceDesc.Format), DxgiFormatName(inputSrvFormat), sourceDesc.BindFlags,
                sourceDesc.MiscFlags, (unsigned long)hr);
        }

        if (!poolCopyStagingTexture_ || poolCopyStagingWidth_ != sourceDesc.Width ||
            poolCopyStagingHeight_ != sourceDesc.Height || poolCopyStagingFormat_ != sourceDesc.Format) {
            SafeRelease(poolCopyStagingSrv_);
            SafeRelease(poolCopyStagingTexture_);

            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = sourceDesc.Width;
            stagingDesc.Height = sourceDesc.Height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = sourceDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_DEFAULT;
            stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &poolCopyStagingTexture_);
            if (FAILED(hr) || !poolCopyStagingTexture_) {
                LogError("[WGC] Failed to create retained-copy source staging texture: 0x%08lX", (unsigned long)hr);
                return false;
            }

            hr = d3dDevice_->CreateShaderResourceView(poolCopyStagingTexture_, &srvDesc, &poolCopyStagingSrv_);
            if (FAILED(hr) || !poolCopyStagingSrv_) {
                LogError("[WGC] Failed to create retained-copy source staging SRV: 0x%08lX", (unsigned long)hr);
                SafeRelease(poolCopyStagingTexture_);
                return false;
            }

            poolCopyStagingWidth_ = sourceDesc.Width;
            poolCopyStagingHeight_ = sourceDesc.Height;
            poolCopyStagingFormat_ = sourceDesc.Format;
            LogInfo("[WGC] Retained-copy source staging created: %ux%u fmt=%s bytes=%lluMB", sourceDesc.Width,
                    sourceDesc.Height, DxgiFormatName(sourceDesc.Format),
                    static_cast<unsigned long long>(
                        (ce::capture_policy::EstimateWgcSurfaceBytes(sourceDesc.Width, sourceDesc.Height,
                                                                     BytesPerPixelForFormat(sourceDesc.Format)) +
                         1024ull * 1024ull - 1ull) /
                        (1024ull * 1024ull)));
        }

        d3dContext_->CopyResource(poolCopyStagingTexture_, sourceTexture);
        poolCopyStagingSrv_->AddRef();
        *outSrv = poolCopyStagingSrv_;
        if (usedStaging) {
            *usedStaging = true;
        }
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::RenderFrameToPoolSlot(ID3D11Texture2D* sourceTexture,  const D3D11_TEXTURE2D_DESC& sourceDesc, 
                               ID3D11RenderTargetView* targetRtv,  bool linearToSrgb,  bool* usedStaging) {


        if (!sourceTexture || !targetRtv || !d3dContext_ || !d3dDevice_) {
            return false;
        }
        if (!EnsurePoolCopyShader()) {
            return false;
        }

        const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format);
        if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
            LogError("[WGC] Unsupported retained-copy source format for shader conversion: %s",
                     DxgiFormatName(sourceDesc.Format));
            return false;
        }

        ID3D11ShaderResourceView* sourceSrv = nullptr;
        bool staging = false;
        if (!CreatePoolCopySourceSrv(sourceTexture, sourceDesc, inputSrvFormat, &sourceSrv, &staging)) {
            return false;
        }
        if (usedStaging) {
            *usedStaging = staging;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3dContext_->Map(poolCopyCB_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            LogError("[WGC] Failed to map retained-copy constant buffer: 0x%08lX", (unsigned long)hr);
            SafeRelease(sourceSrv);
            return false;
        }
        uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
        cbData[0] = linearToSrgb ? 1u : 0u;
        cbData[1] = 0;
        cbData[2] = 0;
        cbData[3] = 0;
        d3dContext_->Unmap(poolCopyCB_, 0);

        D3D11ContextStateGuard stateGuard(d3dContext_);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(sourceDesc.Width);
        viewport.Height = static_cast<float>(sourceDesc.Height);
        viewport.MaxDepth = 1.0f;
        d3dContext_->RSSetViewports(1, &viewport);
        d3dContext_->OMSetRenderTargets(1, &targetRtv, nullptr);
        d3dContext_->VSSetShader(poolCopyVS_, nullptr, 0);
        d3dContext_->PSSetShader(poolCopyPS_, nullptr, 0);
        d3dContext_->PSSetShaderResources(0, 1, &sourceSrv);
        d3dContext_->PSSetSamplers(0, 1, &poolCopySampler_);
        d3dContext_->PSSetConstantBuffers(0, 1, &poolCopyCB_);
        d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext_->IASetInputLayout(nullptr);
        d3dContext_->Draw(3, 0);

        ID3D11RenderTargetView* nullRtv = nullptr;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        d3dContext_->OMSetRenderTargets(1, &nullRtv, nullptr);
        d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
        SafeRelease(sourceSrv);
        return true;

}

#endif

#if HAS_WGC

ce::capture_policy::WgcSmoothnessSurfaceBudget WGCCapture::Impl::ComputeTexturePoolBudget(uint32_t width,  uint32_t height, 
                                                                            DXGI_FORMAT format) const {


        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        // The duplication backend has no consumer-owned source frame pool (the
        // OS holds the desktop image), so its entire VRAM budget funds retained
        // copy slots for the smoothness reservoir.
        const bool requiresSourceFramePool = !useDuplicationBackend_;
        return ce::capture_policy::ComputeWgcSmoothnessSurfaceBudget(
            smoothnessBufferEnabled_ ? smoothnessOutputFps_ : 0u, smoothnessBufferEnabled_ ? smoothnessMaxMs_ : 0u,
            width, height, BytesPerPixelForFormat(format), BytesPerPixelForFormat(retainedFormat),

#endif
#if HAS_WGC
            smoothnessVramBudgetMb_, smoothnessSyncDelayFrames_, requiresSourceFramePool);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::UpdateSmoothnessBudget(uint32_t width,  uint32_t height,  DXGI_FORMAT format,  bool logBudget) {


        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        const auto budget = ComputeTexturePoolBudget(width, height, format);
        smoothnessRetainedFrames_ = smoothnessBufferEnabled_ ? budget.retainedExtraFrames : 0u;
        smoothnessRetainedFrameCap_ = budget.retainedFrameCap;
        sourceFramePoolBufferCount_ = allocationLimitedSourceBuffers_ != 0
                                          ? std::min(budget.sourceFramePoolBuffers, allocationLimitedSourceBuffers_)
                                          : budget.sourceFramePoolBuffers;
        smoothnessBudgetSurfaceCount_ = budget.budgetSurfaceCount;
        smoothnessSafetySlots_ = budget.safetySlots;
        smoothnessReservedFreeSlots_ = budget.reservedFreeCopySlots;
        smoothnessSourceFormat_ = format;
        smoothnessCopyFormat_ = retainedFormat;
        smoothnessSourceBytesPerSurface_ = budget.sourceBytesPerSurface;
        smoothnessCopyBytesPerSurface_ = budget.copyBytesPerSurface;
        smoothnessSourceEstimatedVramBytes_ = budget.sourceEstimatedBytes;
        smoothnessCopyEstimatedVramBytes_ = budget.copyEstimatedBytes;
        smoothnessEstimatedVramBytes_ = budget.estimatedBytes;
        smoothnessBudgetExhausted_ = budget.budgetExhausted;
        texturePoolSlotCount_ = budget.copyPoolSlots;
        compactRetainedCopyActive_ = IsCompactRetainedCopy(format, retainedFormat);
        ingressRetainedFrameCap_.store(smoothnessRetainedFrameCap_, std::memory_order_relaxed);
        if (logBudget) {
            const uint32_t budgetFps =
                smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessBudgetFps(smoothnessOutputFps_) : 0;
            const uint32_t desiredFrames = smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                                                                          smoothnessOutputFps_, smoothnessMaxMs_)
                                                                    : 0;
            const uint32_t capShortfall =
                desiredFrames > smoothnessRetainedFrames_ ? desiredFrames - smoothnessRetainedFrames_ : 0;
            const uint64_t capShortfallBytes = capShortfall * smoothnessCopyBytesPerSurface_;
            LogInfo(
                "[WGC] Smoothness buffer budget: enabled=%d targetMs=%u outputFps=%u budgetFps=%u desiredFrames=%u "
                "retainedFrames=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u fmt=%d %ux%u bpp=%u budget=%uMB "
                "sourceFmt=%s retainedFmt=%s compactRetained=%d sourceSurfaceMB=%.1f copySurfaceMB=%.1f "
                "sourceBudgetMB=%.1f copyBudgetMB=%.1f estimated=%lluMB capLimited=%d capShortfall=%u "
                "capShortfallMB=%.0f budgetExhausted=%d",
                smoothnessBufferEnabled_ ? 1 : 0, smoothnessMaxMs_, smoothnessOutputFps_, budgetFps, desiredFrames,
                smoothnessRetainedFrames_, sourceFramePoolBufferCount_, texturePoolSlotCount_,
                smoothnessBudgetSurfaceCount_, smoothnessSyncDelayFrames_, smoothnessRetainedFrames_,
                smoothnessRetainedFrameCap_, smoothnessReservedFreeSlots_, smoothnessSafetySlots_, format, width,
                height, BytesPerPixelForFormat(format), smoothnessVramBudgetMb_, DxgiFormatName(format),
                DxgiFormatName(retainedFormat), compactRetainedCopyActive_ ? 1 : 0,
                static_cast<double>(smoothnessSourceBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessSourceEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                                (1024ull * 1024ull)),
                budget.capLimited ? 1 : 0, capShortfall, static_cast<double>(capShortfallBytes) / (1024.0 * 1024.0),
                budget.budgetExhausted ? 1 : 0);
        }

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ReleaseTexturePool() {


        ReleasePoolConversionResources();
        for (auto* mutex : captureTextureMutexPool_) {
            SafeRelease(mutex);
        }
        for (auto* texture : captureTexturePool_) {
            SafeRelease(texture);
        }
        for (auto* texture : texturePool_) {
            SafeRelease(texture);
        }
        texturePool_.clear();
        captureTexturePool_.clear();
        captureTextureMutexPool_.clear();
        poolSlotLastWriteQpc_.clear();
        poolLeaseState_.reset();
        poolWidth_ = 0;
        poolHeight_ = 0;
        poolSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        compactRetainedCopyActive_ = false;
        poolWriteIndex_.store(0, std::memory_order_relaxed);

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::EnsureGpuTimingQueries() {


        if (gpuTimingDisjoint_ && gpuTimingStart_ && gpuTimingEnd_)
            return true;
        SafeRelease(gpuTimingEnd_);
        SafeRelease(gpuTimingStart_);
        SafeRelease(gpuTimingDisjoint_);
        if (!d3dDevice_)
            return false;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HRESULT hr = d3dDevice_->CreateQuery(&desc, &gpuTimingDisjoint_);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        if (SUCCEEDED(hr))
            hr = d3dDevice_->CreateQuery(&desc, &gpuTimingStart_);
        if (SUCCEEDED(hr))
            hr = d3dDevice_->CreateQuery(&desc, &gpuTimingEnd_);
        if (FAILED(hr)) {
            SafeRelease(gpuTimingEnd_);
            SafeRelease(gpuTimingStart_);
            SafeRelease(gpuTimingDisjoint_);
            LogWarn("[WGC] Nonblocking GPU timing query prewarm failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        return true;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::PollGpuTimingSample() {


        if (!gpuTimingPending_ || !d3dContext_)
            return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        const HRESULT disjointHr =
            d3dContext_->GetData(gpuTimingDisjoint_, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (disjointHr != S_OK)
            return;
        UINT64 start = 0;
        UINT64 end = 0;
        const HRESULT startHr =
            d3dContext_->GetData(gpuTimingStart_, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT endHr = d3dContext_->GetData(gpuTimingEnd_, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (startHr != S_OK || endHr != S_OK)
            return;
        LARGE_INTEGER observed = {};
        QueryPerformanceCounter(&observed);
        const double executionUs =
            !disjoint.Disjoint && disjoint.Frequency > 0 && end >= start
                ? static_cast<double>(end - start) * 1000000.0 / static_cast<double>(disjoint.Frequency)
                : -1.0;
        const int64_t observedLatencyUs = qpcFreq_ > 0 && observed.QuadPart >= gpuTimingSubmitQpc_
                                              ? (observed.QuadPart - gpuTimingSubmitQpc_) * 1000000 / qpcFreq_
                                              : -1;
        LogInfo("[WGC GPU Timing] backend=%s execution=%.1fus submitToObserved=%lldus disjoint=%d",
                useDuplicationBackend_ ? "dxgi_dup" : "wgc", executionUs, static_cast<long long>(observedLatencyUs),
                disjoint.Disjoint ? 1 : 0);
        gpuTimingPending_ = false;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::BeginGpuTimingSample() {


        PollGpuTimingSample();
        const ULONGLONG now = GetTickCount64();
        if (gpuTimingPending_ || gpuTimingActive_ || !EnsureGpuTimingQueries() ||
            (lastGpuTimingSampleTick_ != 0 && now - lastGpuTimingSampleTick_ < 1000)) {
            return;
        }
        d3dContext_->Begin(gpuTimingDisjoint_);
        d3dContext_->End(gpuTimingStart_);
        gpuTimingActive_ = true;
        lastGpuTimingSampleTick_ = now;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::EndGpuTimingSample() {


        if (!gpuTimingActive_ || !d3dContext_)
            return;
        d3dContext_->End(gpuTimingEnd_);
        d3dContext_->End(gpuTimingDisjoint_);
        LARGE_INTEGER submitted = {};
        QueryPerformanceCounter(&submitted);
        gpuTimingSubmitQpc_ = submitted.QuadPart;
        gpuTimingActive_ = false;
        gpuTimingPending_ = true;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::LogVideoMemoryInfo(const char* stage,  bool force) {


        if (!d3dDevice_)
            return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = lastVideoMemoryLogTick_.load(std::memory_order_relaxed);
        if (!force && previous != 0 && now - previous < 1000)
            return;
        if (!lastVideoMemoryLogTick_.compare_exchange_strong(previous, now, std::memory_order_relaxed) && !force) {
            return;
        }
        if (force)
            lastVideoMemoryLogTick_.store(now, std::memory_order_relaxed);

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice)
            hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(hr) && adapter3)
            hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        if (SUCCEEDED(hr)) {
            const bool overBudget = info.CurrentUsage > info.Budget;
            const uint32_t episodes = overBudget
                                          ? videoMemoryOverBudgetEpisodes_.fetch_add(1, std::memory_order_relaxed) + 1
                                          : videoMemoryOverBudgetEpisodes_.load(std::memory_order_relaxed);
            LogInfo(
                "[WGC] Video memory: stage=%s budget=%.1fMB processUsage=%.1fMB reservation=%.1fMB "
                "availableReservation=%.1fMB estimatedPool=%.1fMB overBudget=%d episodes=%u",
                stage ? stage : "periodic", static_cast<double>(info.Budget) / (1024.0 * 1024.0),
                static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0),
                static_cast<double>(info.CurrentReservation) / (1024.0 * 1024.0),
                static_cast<double>(info.AvailableForReservation) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessEstimatedVramBytes_) / (1024.0 * 1024.0), overBudget ? 1 : 0, episodes);
        } else if (force) {
            LogWarn("[WGC] QueryVideoMemoryInfo failed: stage=%s hr=0x%08lX", stage ? stage : "unknown",
                    static_cast<unsigned long>(hr));
        }
        SafeRelease(adapter3);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::IsAllocationExhaustion(HRESULT hr) {


        return hr == E_OUTOFMEMORY || hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
               hr == HRESULT_FROM_WIN32(ERROR_COMMITMENT_LIMIT);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::SetVideoMemoryReservationBytes(uint64_t requestedBytes,  const char* stage) {


        if (!d3dDevice_)
            return;
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice)
            hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        DXGI_QUERY_VIDEO_MEMORY_INFO before = {};
        if (SUCCEEDED(hr) && adapter3)
            hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &before);
        uint64_t clampedBytes = requestedBytes;
        if (SUCCEEDED(hr)) {
            const uint64_t reservable = before.CurrentReservation + before.AvailableForReservation;
            clampedBytes = std::min(requestedBytes, reservable);
            hr = adapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, clampedBytes);
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO after = {};
        const HRESULT readbackHr =
            SUCCEEDED(hr) ? adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after) : hr;
        if (SUCCEEDED(hr) && SUCCEEDED(readbackHr)) {
            activeVideoMemoryReservationBytes_ = after.CurrentReservation;
            LogInfo(
                "[WGC] Video-memory reservation: stage=%s requested=%.1fMB clamped=%.1fMB actual=%.1fMB "
                "available=%.1fMB verified=%d",
                stage ? stage : "unknown", static_cast<double>(requestedBytes) / (1024.0 * 1024.0),
                static_cast<double>(clampedBytes) / (1024.0 * 1024.0),
                static_cast<double>(after.CurrentReservation) / (1024.0 * 1024.0),
                static_cast<double>(after.AvailableForReservation) / (1024.0 * 1024.0),
                after.CurrentReservation >= clampedBytes ? 1 : 0);
        } else {
            LogWarn(
                "[WGC] Video-memory reservation failed: stage=%s requested=%.1fMB hr=0x%08lX "
                "readbackHr=0x%08lX",
                stage ? stage : "unknown", static_cast<double>(requestedBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long>(hr), static_cast<unsigned long>(readbackHr));
        }
        SafeRelease(adapter3);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);

}

#endif

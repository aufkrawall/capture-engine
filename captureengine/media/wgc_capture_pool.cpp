#include "wgc_capture_internal.h"


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

#endif

#if HAS_WGC

#endif

#if HAS_WGC

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

bool WGCCapture::Impl::EnsureTexturePool(uint32_t width,  uint32_t height,  DXGI_FORMAT sourceFormat) {


        const bool splitDevicePool =
            usingDedicatedCaptureDevice_ && encoderDevice_ && d3dDevice_ && encoderDevice_ != d3dDevice_;
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(sourceFormat);
        const bool compactRetainedCopy = IsCompactRetainedCopy(sourceFormat, retainedFormat);
        // Duplication surfaces are presentation-owned resources.  Always
        // rewrite them through the retained-copy shader so VP/NVENC consume a
        // canonical CE-owned render target instead of inheriting the desktop
        // producer dependency through a raw CopyResource.
        const bool canonicalRetainedCopy = compactRetainedCopy || useDuplicationBackend_;
        if (allocationLimitWidth_ != width || allocationLimitHeight_ != height ||
            allocationLimitSourceFormat_ != sourceFormat) {
            allocationLimitedPoolSlots_ = 0;
            allocationLimitWidth_ = width;
            allocationLimitHeight_ = height;
            allocationLimitSourceFormat_ = sourceFormat;
        }
        UpdateSmoothnessBudget(width, height, sourceFormat, false);
        const uint32_t configuredSlots = std::max<uint32_t>(1u, texturePoolSlotCount_);
        const uint32_t desiredSlots =
            allocationLimitedPoolSlots_ != 0 ? std::min(configuredSlots, allocationLimitedPoolSlots_) : configuredSlots;
        if (poolWidth_ == (int32_t)width && poolHeight_ == (int32_t)height && poolSourceFormat_ == sourceFormat &&
            poolFormat_ == retainedFormat && texturePool_.size() == desiredSlots && !texturePool_.empty() &&
            texturePool_[0] && (!splitDevicePool || (!captureTexturePool_.empty() && captureTexturePool_[0])) &&
            (!canonicalRetainedCopy || (!poolRenderTargetViews_.empty() && poolRenderTargetViews_[0]))) {
            LogVideoMemoryInfo("periodic");
            return true;
        }

        LogVideoMemoryInfo("before-pool-allocation", true);
        ReleaseGpuTimingResources();
        ReleaseTexturePool();
        texturePool_.assign(desiredSlots, nullptr);
        captureTexturePool_.assign(desiredSlots, nullptr);
        captureTextureMutexPool_.assign(desiredSlots, nullptr);
        poolRenderTargetViews_.assign(desiredSlots, nullptr);
        poolSlotLastWriteQpc_.assign(desiredSlots, 0);

        D3D11_TEXTURE2D_DESC copyDesc = {};
        copyDesc.Width = width;
        copyDesc.Height = height;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.Format = retainedFormat;
        copyDesc.SampleDesc.Count = 1;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        // CRITICAL: VP input view requires RENDER_TARGET bind flag
        // Using only SHADER_RESOURCE causes D3D11 internal stack corruption (0xC0000409)
        copyDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        copyDesc.MiscFlags = splitDevicePool ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;

        for (uint32_t i = 0; i < desiredSlots; i++) {
            HRESULT hr =
                (splitDevicePool ? encoderDevice_ : d3dDevice_)->CreateTexture2D(&copyDesc, nullptr, &texturePool_[i]);
            if (FAILED(hr)) {
                LogError("[WGC] Failed to create texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                ReleaseTexturePool();
                const uint32_t mandatorySlots = std::max<uint32_t>(
                    ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames,
                    smoothnessSyncDelayFrames_ + smoothnessReservedFreeSlots_ + smoothnessSafetySlots_ + 1u);
                if (IsAllocationExhaustion(hr) && desiredSlots > mandatorySlots) {
                    const uint32_t successfulSlots = i;
                    const uint32_t halvedSlots = std::max<uint32_t>(mandatorySlots, desiredSlots / 2u);
                    allocationLimitedPoolSlots_ = std::max<uint32_t>(
                        mandatorySlots,
                        std::min<uint32_t>(desiredSlots - 1u,
                                           successfulSlots > mandatorySlots ? successfulSlots : halvedSlots));
                    const double reservoirMs = smoothnessOutputFps_ > 0
                                                   ? 1000.0 * static_cast<double>(allocationLimitedPoolSlots_) /
                                                         static_cast<double>(smoothnessOutputFps_)
                                                   : 0.0;
                    LogWarn(
                        "[WGC] Pool allocation exhausted: attempted=%u failedIndex=%u; retrying optional "
                        "reservoir with %u slots (mandatory=%u estimatedDepth=%.1fms)",
                        desiredSlots, i, allocationLimitedPoolSlots_, mandatorySlots, reservoirMs);
                    return EnsureTexturePool(width, height, sourceFormat);
                }
                return false;
            }

            if (splitDevicePool) {
                IDXGIResource* dxgiResource = nullptr;
                hr = texturePool_[i]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
                if (FAILED(hr) || !dxgiResource) {
                    LogError("[WGC] Failed to query IDXGIResource for shared texture pool[%d]: 0x%lX", i,
                             (unsigned long)hr);
                    SafeRelease(dxgiResource);
                    ReleaseTexturePool();
                    return false;
                }

                HANDLE sharedHandle = nullptr;
                hr = dxgiResource->GetSharedHandle(&sharedHandle);
                SafeRelease(dxgiResource);
                if (FAILED(hr) || !sharedHandle) {
                    LogError("[WGC] Failed to get shared handle for texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }

                hr = d3dDevice_->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&captureTexturePool_[i]));
                if (FAILED(hr) || !captureTexturePool_[i]) {
                    LogError("[WGC] Failed to open shared capture texture pool[%d]: 0x%lX", i, (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }

                hr = captureTexturePool_[i]->QueryInterface(IID_PPV_ARGS(&captureTextureMutexPool_[i]));
                if (FAILED(hr) || !captureTextureMutexPool_[i]) {
                    LogError("[WGC] Failed to query keyed mutex for capture texture pool[%d]: 0x%lX", i,
                             (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }
            }

            if (canonicalRetainedCopy) {
                ID3D11Texture2D* renderTarget = captureTexturePool_[i] ? captureTexturePool_[i] : texturePool_[i];
                hr = d3dDevice_->CreateRenderTargetView(renderTarget, nullptr, &poolRenderTargetViews_[i]);
                if (FAILED(hr) || !poolRenderTargetViews_[i]) {
                    LogError("[WGC] Failed to create retained-copy RTV for pool[%u] fmt=%s: 0x%08lX", i,
                             DxgiFormatName(retainedFormat), (unsigned long)hr);
                    ReleaseTexturePool();
                    return false;
                }
            }
        }

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        poolWidth_ = width;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        poolHeight_ = height;
        poolSourceFormat_ = sourceFormat;
        poolFormat_ = retainedFormat;
        poolGeneration_++;
        auto leaseState = std::make_shared<WgcPoolLeaseState>();
        leaseState->Init(desiredSlots, poolGeneration_);
        poolLeaseState_ = std::move(leaseState);
        poolWriteIndex_.store(0, std::memory_order_relaxed);

        LogInfo(
            "[WGC] Texture pool created: %dx%d sourceFmt=%s retainedFmt=%s compactRetained=%d dupCanonical=%d "
            "sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u "
            "syncFrames=%u extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u estimatedSmooth=%lluMB "
            "sourceBudget=%.1fMB copyBudget=%.1fMB convertLast=%lldus "
            "generation=%llu (%s)",
            width, height, DxgiFormatName(sourceFormat), DxgiFormatName(retainedFormat), compactRetainedCopy ? 1 : 0,
            useDuplicationBackend_ ? 1 : 0, sourceFramePoolBufferCount_, desiredSlots, smoothnessBudgetSurfaceCount_,
            smoothnessSyncDelayFrames_, smoothnessRetainedFrames_, smoothnessRetainedFrameCap_,
            smoothnessReservedFreeSlots_, smoothnessSafetySlots_,
            static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                            (1024ull * 1024ull)),
            static_cast<double>(smoothnessSourceEstimatedVramBytes_) / (1024.0 * 1024.0),
            static_cast<double>(smoothnessCopyEstimatedVramBytes_) / (1024.0 * 1024.0),
            static_cast<long long>(lastPoolConvertUs_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(poolGeneration_),
            splitDevicePool ? "dedicated capture device" : "shared device");
        if (desiredSlots < configuredSlots) {
            const double configuredMs = smoothnessOutputFps_ > 0 ? 1000.0 * static_cast<double>(configuredSlots) /
                                                                       static_cast<double>(smoothnessOutputFps_)
                                                                 : 0.0;
            const double allocatedMs = smoothnessOutputFps_ > 0 ? 1000.0 * static_cast<double>(desiredSlots) /
                                                                      static_cast<double>(smoothnessOutputFps_)
                                                                : 0.0;
            LogWarn(
                "[WGC] Optional reservoir reduced after memory exhaustion: configured=%u (%.1fms) "
                "allocated=%u (%.1fms) reduction=%.1fms",
                configuredSlots, configuredMs, desiredSlots, allocatedMs, configuredMs - allocatedMs);
        }
        ApplyConfiguredVideoMemoryReservation();
        LogVideoMemoryInfo("after-pool-allocation", true);
        return true;

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::ShouldAdmitFrameToPool(int64_t sourceFrameQpc,  int64_t rawSourceFrameQpc,  uint32_t poolSize, 
                                const std::shared_ptr<WgcPoolLeaseState>& leaseState) {


        const int64_t timingQpc = rawSourceFrameQpc > 0 ? rawSourceFrameQpc : sourceFrameQpc;
        const uint32_t retainedFrames = ingressRetainedFrames_.load(std::memory_order_relaxed);
        uint32_t retainedFrameCap = ingressRetainedFrameCap_.load(std::memory_order_relaxed);
        if (retainedFrameCap == 0) {
            retainedFrameCap = smoothnessRetainedFrameCap_;
        }
        const uint32_t lowWaterFrames = ingressLowWaterFrames_.load(std::memory_order_relaxed);
        const bool recovering = ingressRecovering_.load(std::memory_order_relaxed);
        const uint32_t leasedCurrent = leaseState ? leaseState->leasedCurrent.load(std::memory_order_relaxed) : 0u;
        const uint32_t pressureRetainedFrames = std::max(retainedFrames, leasedCurrent);
        const uint32_t freeCopySlots = poolSize > leasedCurrent ? (poolSize - leasedCurrent) : 0u;
        const uint32_t outputFps = smoothnessOutputFps_;
        const uint32_t inputMin250Fps = inputMin250Fps_.load(std::memory_order_relaxed);
        const uint32_t inputMin500Fps = inputMin500Fps_.load(std::memory_order_relaxed);
        const bool uniformPlayoutOwnsSurplus = ingressUniformPlayoutOwnsSurplus_.load(std::memory_order_relaxed);

        ce::capture_policy::WgcIngressAdmissionDecision decision{};
        uint32_t reasonCode = kWgcIngressReasonUncapped;
        {
            std::lock_guard<std::mutex> lock(ingressAdmissionMutex_);
            if (timingQpc > 0 && qpcFreq_ > 0 && outputFps > 0) {
                if (ingressCreditLastQpc_ > 0 && timingQpc > ingressCreditLastQpc_) {
                    const double elapsedFrames =
                        (static_cast<double>(timingQpc - ingressCreditLastQpc_) * static_cast<double>(outputFps)) /
                        static_cast<double>(qpcFreq_);
                    ingressCreditFrames_ = std::min(2.0, ingressCreditFrames_ + std::max(0.0, elapsedFrames));
                } else if (ingressCreditLastQpc_ == 0 || timingQpc < ingressCreditLastQpc_) {
                    ingressCreditFrames_ = std::max(ingressCreditFrames_, 1.0);
                }
                ingressCreditLastQpc_ = timingQpc;
            } else {
                ingressCreditFrames_ = std::max(ingressCreditFrames_, 1.0);
            }

            decision = ce::capture_policy::DecideWgcIngressAdmission(
                pressureRetainedFrames, retainedFrameCap, lowWaterFrames, recovering, outputFps, inputMin250Fps,
                inputMin500Fps, ingressCreditFrames_, freeCopySlots, smoothnessReservedFreeSlots_,
                uniformPlayoutOwnsSurplus);
            reasonCode = WgcIngressReasonCodeFromText(decision.reason);
            if (decision.accept && outputFps > 0 &&
                !ce::capture_policy::IsWgcIngressSourceBelowCfrTarget(outputFps, inputMin250Fps, inputMin500Fps)) {
                ingressCreditFrames_ = std::max(0.0, ingressCreditFrames_ - 1.0);
            }
        }

        ingressLastReason_.store(reasonCode, std::memory_order_relaxed);
        if (decision.softReservePressure) {
            ingressSoftReservePressureCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (decision.hardReservePressure) {
            ingressHardReservePressureCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (decision.accept) {
            ingressAcceptedCount_.fetch_add(1, std::memory_order_relaxed);
            switch (reasonCode) {
                case kWgcIngressReasonLowWater:
                    ingressAcceptedLowWaterCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonRecovery:
                    ingressAcceptedRecoveryCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonSourceBelowTarget:
                    ingressAcceptedSourceBelowCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonUniformPlayoutSoftReserve:
                    ingressAcceptedUniformPlayoutSoftReserveCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonUniformPlayoutCredit:
                    ingressAcceptedUniformPlayoutCreditCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case kWgcIngressReasonCredit:
                case kWgcIngressReasonHealthy:
                case kWgcIngressReasonUncapped:
                default:
                    ingressAcceptedHealthyCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
            return true;
        }

        skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        ingressDecimatedCount_.fetch_add(1, std::memory_order_relaxed);
        switch (reasonCode) {
            case kWgcIngressReasonDecimatedHardReserve:
                ingressDecimatedHardReserveCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case kWgcIngressReasonDecimatedCredit:
                ingressDecimatedCreditCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case kWgcIngressReasonDecimatedSoftReserve:
            default:
                ingressDecimatedSoftReserveCount_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        static std::atomic<uint32_t> decimatedLogCount{0};
        const uint32_t logCount = decimatedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 8 || (logCount % 1000u) == 0u) {
            LogInfo(
                "[WGC] Ingress decimated WGC frame before copy: retained=%u pressure=%u/%u lowWater=%u leased=%u "
                "free=%u reservedFree=%u softReservePressure=%d hardReservePressure=%d "
                "inputMin=%u/%u outputFps=%u frameQpc=%lld reason=%s",
                retainedFrames, pressureRetainedFrames, retainedFrameCap, lowWaterFrames, leasedCurrent, freeCopySlots,
                smoothnessReservedFreeSlots_, decision.softReservePressure ? 1 : 0,
                decision.hardReservePressure ? 1 : 0, inputMin250Fps, inputMin500Fps, outputFps,
                static_cast<long long>(sourceFrameQpc), decision.reason);
        }
        return false;

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::CopyFrameToPool(ID3D11Texture2D* sourceTexture,  const D3D11_TEXTURE2D_DESC& sourceDesc,  int64_t sourceFrameQpc, 
                         int64_t rawSourceFrameQpc,  ID3D11Texture2D** out,  int64_t& copyCompleteQpc, 
                         WgcPoolSlotLease& outLease,  uint32_t& outSlot,  uint64_t& outGeneration) {


        if (!sourceTexture || !out) {
            return false;
        }

        *out = nullptr;
        copyCompleteQpc = 0;
        outLease.Reset();
        outSlot = std::numeric_limits<uint32_t>::max();
        outGeneration = 0;

        if (!EnsureTexturePool(sourceDesc.Width, sourceDesc.Height, sourceDesc.Format)) {
            return false;
        }

        const uint32_t poolSize = static_cast<uint32_t>(texturePool_.size());
        if (poolSize == 0) {
            return false;
        }
        const auto leaseState = poolLeaseState_;
        if (!ShouldAdmitFrameToPool(sourceFrameQpc, rawSourceFrameQpc, poolSize, leaseState)) {
            return false;
        }

        const uint32_t startIndex = poolWriteIndex_.fetch_add(1, std::memory_order_relaxed) % poolSize;
        const uint64_t poolGeneration = poolGeneration_;
        for (uint32_t attempt = 0; attempt < poolSize; ++attempt) {
            const uint32_t idx = (startIndex + attempt) % poolSize;
            if (!leaseState || !leaseState->TryAcquire(idx)) {
                poolSlotOverwritePreventedCount_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            WgcPoolSlotLease slotLease(leaseState, idx, poolGeneration);
            ID3D11Texture2D* copyTarget = captureTexturePool_[idx] ? captureTexturePool_[idx] : texturePool_[idx];
            IDXGIKeyedMutex* writeMutex = captureTextureMutexPool_[idx];
            bool mutexAcquired = false;

            if (writeMutex) {
                HRESULT kmHr = writeMutex->AcquireSync(0, 0);
                if (kmHr != S_OK) {
                    // A free lease proves that no queued/reservoir/encoder
                    // consumer can still use this slot. Key 1 therefore means
                    // the previously published frame was discarded before
                    // consumption; reclaim it instead of permanently
                    // poisoning the slot for all future producer writes.
                    kmHr = writeMutex->AcquireSync(1, 0);
                    if (kmHr == S_OK) {
                        const uint32_t reclaimed =
                            keyedMutexAbandonedReclaimCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (reclaimed <= 4) {
                            LogInfo(
                                "[WGC] Reclaimed abandoned published pool slot %u at key 1 "
                                "(generation=%llu reclaim=%u)",
                                idx, static_cast<unsigned long long>(poolGeneration), reclaimed);
                        }
                    }
                }
                if (kmHr != S_OK) {
                    keyedMutexAcquireFailCount_.fetch_add(1, std::memory_order_relaxed);
                    slotLease.Reset();
                    continue;
                }
                mutexAcquired = true;
            }

            LARGE_INTEGER copyStart = {};
            LARGE_INTEGER copyEnd = {};
            QueryPerformanceCounter(&copyStart);
            BeginGpuTimingSample();

            const bool compactRetainedCopy = IsCompactRetainedCopy(sourceDesc.Format, poolFormat_);
            const bool canonicalRetainedCopy = compactRetainedCopy || useDuplicationBackend_;
            bool usedConversionStaging = false;
            bool copySucceeded = true;
            if (canonicalRetainedCopy) {
                const bool linearToSrgb =
                    ce::video_format::ShouldApplySdrLinearToSrgbBeforeRgb10(sourceDesc.Format, captureIsHDR_);
                ID3D11RenderTargetView* targetRtv =
                    idx < poolRenderTargetViews_.size() ? poolRenderTargetViews_[idx] : nullptr;
                copySucceeded =
                    RenderFrameToPoolSlot(sourceTexture, sourceDesc, targetRtv, linearToSrgb, &usedConversionStaging);
            } else {
                d3dContext_->CopyResource(copyTarget, sourceTexture);
            }
            EndGpuTimingSample();
            if (mutexAcquired) {
                if (skipSplitDeviceFlush_) {
                    splitDeviceFlushSkippedCount_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    d3dContext_->Flush();
                    splitDeviceFlushCount_.fetch_add(1, std::memory_order_relaxed);
                }
                // A successful frame is published at key 1. Conversion
                // failures return key 0 to the producer so an
                // uninitialized surface can never enter the consumer state.
                const uint64_t releaseKey = copySucceeded ? 1 : 0;
                const HRESULT releaseHr = writeMutex->ReleaseSync(releaseKey);
                if (releaseHr != S_OK) {
                    keyedMutexReleaseFailCount_.fetch_add(1, std::memory_order_relaxed);
                    LogWarn("[WGC] Shared texture ReleaseSync failed for slot %u: 0x%08lX", idx,
                            (unsigned long)releaseHr);
                    slotLease.Reset();
                    continue;

#endif
#if HAS_WGC
                }
            }

            if (!copySucceeded) {
                skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                poolDropCount_.fetch_add(1, std::memory_order_relaxed);
                slotLease.Reset();
                static std::atomic<uint32_t> conversionFailLogCount{0};
                const uint32_t failLog = conversionFailLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failLog <= 4) {
                    LogWarn(
                        "[WGC] Retained-copy conversion failed; dropped WGC frame "
                        "(sourceFmt=%s retainedFmt=%s slot=%u generation=%llu frameQpc=%lld)",
                        DxgiFormatName(sourceDesc.Format), DxgiFormatName(poolFormat_), idx,
                        static_cast<unsigned long long>(poolGeneration), static_cast<long long>(sourceFrameQpc));
                }
                return false;
            }

            QueryPerformanceCounter(&copyEnd);

            int64_t copyUs = 0;
            if (qpcFreq_ > 0) {
                copyUs = ((copyEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq_;
            }
            if (canonicalRetainedCopy) {
                lastPoolConvertUs_.store(copyUs, std::memory_order_relaxed);
            } else {
                lastPoolConvertUs_.store(0, std::memory_order_relaxed);
            }
            const int64_t previousSlotWriteQpc = poolSlotLastWriteQpc_[idx];
            poolSlotLastWriteQpc_[idx] = copyEnd.QuadPart;
            if (previousSlotWriteQpc > 0 && copyEnd.QuadPart > previousSlotWriteQpc && qpcFreq_ > 0) {
                const int64_t slotRewriteUs = ((copyEnd.QuadPart - previousSlotWriteQpc) * 1000000) / qpcFreq_;
                lastPoolSlotRewriteUs_.store(slotRewriteUs, std::memory_order_relaxed);
                if (slotRewriteUs < 5000) {
                    const uint32_t fastRewrite = poolSlotFastRewriteCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (fastRewrite <= 5) {
                        LogWarn("[WGC] Texture pool slot %u rewritten after %lldus", idx, (long long)slotRewriteUs);
                    }
                }
            }
            lastCopyUs_.store(copyUs, std::memory_order_relaxed);
            const int64_t timingSourceQpc = rawSourceFrameQpc > 0 ? rawSourceFrameQpc : sourceFrameQpc;
            RecordSourceTimingSample(timingSourceQpc);
            RecordSourceToCopyLatency(timingSourceQpc, copyEnd.QuadPart);

            lastCapturedQPC_ = sourceFrameQpc;
            if (targetIntervalQPC_ > 0) {
                if (nextCaptureQPC_ <= 0) {
                    nextCaptureQPC_ = sourceFrameQpc + targetIntervalQPC_;
                } else {
                    do {
                        nextCaptureQPC_ += targetIntervalQPC_;
                    } while (nextCaptureQPC_ <= sourceFrameQpc);
                }
            } else {
                nextCaptureQPC_ = 0;
            }

            texturePool_[idx]->AddRef();
            *out = texturePool_[idx];
            outLease = std::move(slotLease);
            outSlot = idx;
            outGeneration = poolGeneration;
            copyCompleteQpc = copyEnd.QuadPart;
            static std::atomic<uint32_t> copiedLogCount{0};
            const uint32_t copiedLog = copiedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (copiedLog <= 8 || (copiedLog % 1000u) == 0u) {
                LogInfo(
                    "[WGC] Pool frame copied: slot=%u generation=%llu copyUs=%lld sourceQpc=%lld rawQpc=%lld "
                    "sourceFmt=%s retainedFmt=%s compactRetained=%d dupCanonical=%d staging=%d "
                    "copyCpuSubmitUs=%lld convertCpuSubmitUs=%lld timingBasis=cpu_wall "
                    "leasedMax=%u freeMin=%u",
                    idx, static_cast<unsigned long long>(poolGeneration), static_cast<long long>(copyUs),
                    static_cast<long long>(sourceFrameQpc), static_cast<long long>(rawSourceFrameQpc),
                    DxgiFormatName(sourceDesc.Format), DxgiFormatName(poolFormat_), compactRetainedCopy ? 1 : 0,
                    useDuplicationBackend_ ? 1 : 0, usedConversionStaging ? 1 : 0, static_cast<long long>(copyUs),
                    static_cast<long long>(lastPoolConvertUs_.load(std::memory_order_relaxed)),
                    leaseState ? leaseState->leasedMax.load(std::memory_order_relaxed) : 0u,
                    leaseState ? leaseState->freeMin.load(std::memory_order_relaxed) : 0u);
            }
            return true;
        }

        skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
        poolDropCount_.fetch_add(1, std::memory_order_relaxed);
        poolSaturatedDropCount_.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> contentionLogCount{0};
        if (contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            LogWarn(
                "[WGC] Texture pool saturated: safely dropped WGC frame instead of overwriting a live slot "
                "(copyPoolSlots=%u generation=%llu leasedMax=%u freeMin=%u overwritePrevented=%u frameQpc=%lld)",
                poolSize, static_cast<unsigned long long>(poolGeneration),
                leaseState ? leaseState->leasedMax.load(std::memory_order_relaxed) : 0u,
                leaseState ? leaseState->freeMin.load(std::memory_order_relaxed) : 0u,
                poolSlotOverwritePreventedCount_.load(std::memory_order_relaxed), (long long)sourceFrameQpc);
        }
        return false;

}

#endif

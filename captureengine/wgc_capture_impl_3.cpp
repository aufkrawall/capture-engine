#include "wgc_capture_internal.h"


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

#if HAS_WGC

WGCCapture::Impl::SourceFramePreflight WGCCapture::Impl::PreflightSourceFrame(int64_t rawSourceFrameQpc) {


        SourceFramePreflight pre;
        pre.rawSourceFrameQpc = rawSourceFrameQpc;

        inputFrameCount_.fetch_add(1, std::memory_order_relaxed);
        RecordInputFrameEvent();

        if (IsOutOfOrderRawSourceFrameQpc(rawSourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        pre.sourceFrameQpc = NormalizeSourceFrameQpc(rawSourceFrameQpc, &pre.duplicateSourceTimestamp);
        const int64_t lastDeliveredRawSourceQpc = lastDeliveredRawSourceQpc_.load(std::memory_order_relaxed);
        if (pre.duplicateSourceTimestamp ||
            ce::capture_policy::ShouldSkipDeliveredDuplicateWgcSourceTimestamp(
                pre.duplicateSourceTimestamp, rawSourceFrameQpc, lastDeliveredRawSourceQpc, targetIntervalQPC_ > 0)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            duplicateTimestampSkipCount_.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> duplicateSkipLogCount{0};
            if (duplicateSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
                LogInfo(
                    "[WGC] Skipped duplicate/out-of-order source frame before copy: "
                    "rawQpc=%lld dupTs=%d lastDeliveredRawQpc=%lld lastDeliveredNormQpc=%lld",
                    static_cast<long long>(rawSourceFrameQpc), pre.duplicateSourceTimestamp ? 1 : 0,
                    static_cast<long long>(lastDeliveredRawSourceQpc), static_cast<long long>(lastDeliveredSourceQpc));
            }
            return pre;
        }
        if (!pre.duplicateSourceTimestamp && targetIntervalQPC_ > 0 && nextCaptureQPC_ > 0 && pre.sourceFrameQpc > 0 &&
            pre.sourceFrameQpc < nextCaptureQPC_) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            pacingSkipCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        if (throttleFlag_ && throttleFlag_->load(std::memory_order_relaxed)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            throttleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            return pre;
        }

        if (!pre.duplicateSourceTimestamp && IsStaleSourceFrameQpc(pre.sourceFrameQpc)) {
            skippedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            staleSkipCount_.fetch_add(1, std::memory_order_relaxed);
            const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
            if (pre.sourceFrameQpc == lastDeliveredSourceQpc) {
                staleDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            } else if (pre.sourceFrameQpc < lastDeliveredSourceQpc) {
                staleOutOfOrderTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return pre;
        }

        pre.accepted = true;
        return pre;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::DeliverSourceTexture(ID3D11Texture2D* texture,  const D3D11_TEXTURE2D_DESC& desc, 
                              const SourceFramePreflight& pre,  WGCCapturedFrame* outputFrame) {


        // Snapshot identity before any GPU work. If the coordinator advances
        // this already-warmed capture during an inject->WGC commit, an in-flight
        // pre-commit frame must retain the retired epoch.
        const uint64_t sourceEpoch = sourceEpoch_.load(std::memory_order_acquire);
        if (frameWidth_ != 0 && frameHeight_ != 0 && (desc.Width != frameWidth_ || desc.Height != frameHeight_)) {
            LogWarn("[WGC] Source size changed from %ux%u to %ux%u", frameWidth_, frameHeight_, desc.Width,
                    desc.Height);
            FlagResetNeeded("capture size changed");
            return false;
        }

        if (!formatDetected_) {
            formatDetected_ = true;
            LogInfo("[WGC] Source format: fmt=%d %ux%u", desc.Format, desc.Width, desc.Height);
        }

        ID3D11Texture2D* copiedTexture = nullptr;
        int64_t copyCompleteQpc = 0;
        WgcPoolSlotLease poolLease;
        uint32_t poolSlot = std::numeric_limits<uint32_t>::max();
        uint64_t poolGeneration = 0;
        if (!CopyFrameToPool(texture, desc, pre.sourceFrameQpc, pre.rawSourceFrameQpc, &copiedTexture, copyCompleteQpc,
                             poolLease, poolSlot, poolGeneration)) {
            return false;
        }

        const int64_t deliveredTimestamp = pre.sourceFrameQpc > 0 ? pre.sourceFrameQpc : copyCompleteQpc;
        if (pre.sourceFrameQpc > 0) {
            lastDeliveredSourceQpc_.store(pre.sourceFrameQpc, std::memory_order_relaxed);
        }
        if (pre.rawSourceFrameQpc > 0) {
            lastDeliveredRawSourceQpc_.store(pre.rawSourceFrameQpc, std::memory_order_relaxed);
        }
        RecordDeliveredFrameEvent();
        RequestHDRRecheckIfDue();

        int32_t captureLeft = 0;
        int32_t captureTop = 0;
        GetCaptureOrigin(captureLeft, captureTop);
        ce::cursor::SourcePointerObservation cursorObservation;
        if (useDuplicationBackend_ && dupSource_) {
            dupSource_->GetPointerState(&cursorObservation);
        }
        const bool cursorEmbedded = cursorObservation.valid && cursorObservation.embedded;

        if (outputFrame) {
            outputFrame->texture = copiedTexture;
            outputFrame->width = desc.Width;
            outputFrame->height = desc.Height;
            outputFrame->timestamp = deliveredTimestamp;
            outputFrame->rawTimestamp = pre.rawSourceFrameQpc;
            outputFrame->isHDR = captureIsHDR_;
            outputFrame->cursorEmbedded = cursorEmbedded;
            outputFrame->cursorObservation = cursorObservation;
            outputFrame->captureLeft = captureLeft;
            outputFrame->captureTop = captureTop;
            outputFrame->duplicateSourceTimestamp = pre.duplicateSourceTimestamp;
            outputFrame->sourceEpoch = sourceEpoch;
            outputFrame->poolSlot = poolSlot;
            outputFrame->poolGeneration = poolGeneration;
            outputFrame->poolLease = std::move(poolLease);
        } else {
            auto cb = frameCallback_.load(std::memory_order_acquire);
            if (cb) {
                cb(copiedTexture, desc.Width, desc.Height, deliveredTimestamp, pre.rawSourceFrameQpc, captureIsHDR_,
                   cursorEmbedded, pre.duplicateSourceTimestamp, cursorObservation, captureLeft, captureTop,
                   sourceEpoch, std::move(poolLease));
            } else {
                SafeRelease(copiedTexture);
            }
        }

        callbackFrameCount_.fetch_add(1, std::memory_order_relaxed);
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::ProcessCapturedFrame(const winrt::Direct3D11CaptureFrame& winrtFrame,  WGCCapturedFrame* outputFrame) {


        if (!winrtFrame) {
            return false;
        }

        if (targetWindow_) {
            if (!IsWindow(targetWindow_)) {
                FlagResetNeeded("target window became invalid");
                winrtFrame.Close();
                return false;
            }
            if (IsIconic(targetWindow_)) {
                FlagResetNeeded("target window minimized");
                winrtFrame.Close();
                return false;
            }
        }

        const SourceFramePreflight pre = PreflightSourceFrame(GetFrameSourceQpc(winrtFrame));
        if (!pre.accepted) {
            winrtFrame.Close();
            return false;
        }

        bool success = false;
        auto surface = winrtFrame.Surface();
        IDirect3DDxgiInterfaceAccess* access = nullptr;

        if (SUCCEEDED(surface.as<IUnknown>()->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, (void**)&access)) &&
            access) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture) {
                D3D11_TEXTURE2D_DESC desc;
                texture->GetDesc(&desc);
                success = DeliverSourceTexture(texture, desc, pre, outputFrame);
                texture->Release();
            }
            access->Release();
        }

        winrtFrame.Close();
        return success;

}

#endif

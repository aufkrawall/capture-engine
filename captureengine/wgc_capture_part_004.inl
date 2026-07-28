                return;
            }

            const HRESULT hr = session5->put_MinUpdateInterval(interval100ns);
            int64_t readback100ns = -1;
            const HRESULT readbackHr = SUCCEEDED(hr) ? session5->get_MinUpdateInterval(&readback100ns) : hr;
            session5->Release();
            if (FAILED(hr)) {
                LogWarn("[WGC] MinUpdateInterval write failed: requested=%lld (100ns) targetFps=%u hr=0x%08lX",
                        static_cast<long long>(interval100ns), producerTargetFps_, static_cast<unsigned long>(hr));
                return;
            }

            if (FAILED(readbackHr)) {
                LogWarn("[WGC] MinUpdateInterval readback unavailable: requested=%lld (100ns) targetFps=%u hr=0x%08lX",
                        static_cast<long long>(interval100ns), producerTargetFps_,
                        static_cast<unsigned long>(readbackHr));
                return;
            }
            if (readback100ns != interval100ns) {
                LogError(
                    "[WGC] ERROR: MinUpdateInterval readback mismatch: requested=%lld actual=%lld (100ns) "
                    "targetFps=%u",
                    static_cast<long long>(interval100ns), static_cast<long long>(readback100ns), producerTargetFps_);
                return;
            }
            LogInfo("[WGC] MinUpdateInterval contract applied: requested=%lld actual=%lld (100ns) targetFps=%u mode=%s",
                    static_cast<long long>(interval100ns), static_cast<long long>(readback100ns), producerTargetFps_,
                    producerTargetFps_ == 0 ? "max-rate" : "finite-limit");
        } catch (...) {
            LogInfo("[WGC] MinUpdateInterval not available (older WinRT projection/runtime)");
        }
    }

    int64_t GetFrameSourceQpc(const winrt::Direct3D11CaptureFrame& frame) const {
        const auto systemRelativeTime = frame.SystemRelativeTime();
        return HundredNanosecondsToQpcTicks(systemRelativeTime.count(), qpcFreq_);
    }

    bool IsStaleSourceFrameQpc(int64_t sourceFrameQpc) const {
        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
        return lastDeliveredSourceQpc > 0 && sourceFrameQpc <= lastDeliveredSourceQpc;
    }

    bool IsOutOfOrderRawSourceFrameQpc(int64_t sourceFrameQpc) const {
        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        return lastObservedRawSourceQpc > 0 && sourceFrameQpc < lastObservedRawSourceQpc;
    }

    int64_t NormalizeSourceFrameQpc(int64_t sourceFrameQpc, bool* duplicateSourceTimestamp = nullptr) {
        if (duplicateSourceTimestamp) {
            *duplicateSourceTimestamp = false;
        }
        if (sourceFrameQpc <= 0) {
            return 0;
        }

        const int64_t rawSourceFrameQpc = sourceFrameQpc;
        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        const int64_t lastAssignedSourceQpc = lastAssignedSourceQpc_.load(std::memory_order_relaxed);
        if (lastAssignedSourceQpc > 0 && rawSourceFrameQpc < lastAssignedSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastAssignedSourceQpc;
        } else if (lastObservedRawSourceQpc > 0 && rawSourceFrameQpc == lastObservedRawSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastObservedRawSourceQpc;
        }

        if (rawSourceFrameQpc > lastObservedRawSourceQpc) {
            lastObservedRawSourceQpc_.store(rawSourceFrameQpc, std::memory_order_relaxed);
        }
        if (sourceFrameQpc > lastAssignedSourceQpc) {
            lastAssignedSourceQpc_.store(sourceFrameQpc, std::memory_order_relaxed);
        }
        return sourceFrameQpc;
    }

    HMONITOR ResolveTargetMonitor() const {
        if (targetWindow_) {
            return MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTONEAREST);
        }
        if (targetMonitor_) {
            return targetMonitor_;
        }
        return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    }

    bool GetCaptureOrigin(int32_t& left, int32_t& top) const {
        left = 0;
        top = 0;

        if (targetWindow_) {
            const char* originMode = ResolveWindowCaptureOrigin(left, top);
            return originMode != nullptr;
        }

        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = ResolveTargetMonitor();
        if (monitor && GetMonitorInfo(monitor, &monitorInfo)) {
            left = monitorInfo.rcMonitor.left;
            top = monitorInfo.rcMonitor.top;
            return true;
        }

        return false;
    }

    const char* ResolveWindowCaptureOrigin(int32_t& left, int32_t& top) const {
        left = 0;
        top = 0;
        if (!targetWindow_ || !IsWindow(targetWindow_)) {
            return nullptr;
        }

        RECT windowRect = {};
        RECT clientRect = {};
        const bool haveWindowRect = GetWindowRect(targetWindow_, &windowRect) != FALSE;
        const bool haveClientRect = GetWindowClientRectInScreen(targetWindow_, clientRect);
        constexpr LONG kOriginSizeTolerancePx = 8;

        if (frameWidth_ > 0 && frameHeight_ > 0) {
            if (haveClientRect &&
                SizeNearlyMatchesRect(frameWidth_, frameHeight_, clientRect, kOriginSizeTolerancePx)) {
                left = clientRect.left;
                top = clientRect.top;
                return "client-size-match";
            }

            if (haveWindowRect &&
                SizeNearlyMatchesRect(frameWidth_, frameHeight_, windowRect, kOriginSizeTolerancePx)) {
                left = windowRect.left;
                top = windowRect.top;
                return "window-size-match";
            }
        }

        if (haveWindowRect) {
            left = windowRect.left;
            top = windowRect.top;
            return "window-fallback";
        }

        if (haveClientRect) {
            left = clientRect.left;
            top = clientRect.top;
            return "client-fallback";
        }

        return nullptr;
    }

    bool QueryOutputDesc1ForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc1) {
        if (!monitor) {
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            LogWarn("[WGC] CreateDXGIFactory1 failed while probing output format: 0x%lX", (unsigned long)hr);
            return false;
        }

        bool found = false;
        for (UINT adapterIndex = 0; !found; ++adapterIndex) {
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr) || !adapter) {
                continue;
            }

            for (UINT outputIndex = 0; !found; ++outputIndex) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(outputIndex, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(hr) || !output) {
                    continue;
                }

                DXGI_OUTPUT_DESC outputDesc = {};
                if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor) {
                    IDXGIOutput6* output6 = nullptr;
                    hr = output->QueryInterface(IID_PPV_ARGS(&output6));
                    if (SUCCEEDED(hr) && output6) {
                        found = SUCCEEDED(output6->GetDesc1(&desc1));
                        output6->Release();
                    }
                }

                output->Release();
            }

            adapter->Release();
        }

        factory->Release();
        return found;
    }

    void UpdateCaptureFormatSelection() {
        useHighPrecisionCapture_ = false;
        captureIsHDR_ = false;
        outputBitsPerColor_ = 8;
        outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;

        DXGI_OUTPUT_DESC1 desc1 = {};
        const HMONITOR monitor = ResolveTargetMonitor();
        if (!QueryOutputDesc1ForMonitor(monitor, desc1)) {
            if (requireHighPrecisionCapture_) {
                useHighPrecisionCapture_ = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
                LogWarn("[WGC] Output probe unavailable; explicit 10-bit request requires high-precision capture");
            } else {
                LogInfo("[WGC] Output probe unavailable, using BGRA8 capture");
            }
            return;
        }

        outputBitsPerColor_ = desc1.BitsPerColor;
        outputColorSpace_ = desc1.ColorSpace;
        captureIsHDR_ = IsHdrOutputColorSpace(desc1.ColorSpace);
        if (captureIsHDR_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
            captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        } else if (desc1.BitsPerColor > 8 || requireHighPrecisionCapture_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
            captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
        }

        LogInfo(
            "[WGC] Output probe: bpc=%u colorSpace=%d hdr=%s highPrecision=%s requireHighPrecision=%s "
            "captureFormat=%s",
            outputBitsPerColor_, (int)outputColorSpace_, captureIsHDR_ ? "YES" : "NO",
            useHighPrecisionCapture_ ? "YES" : "NO", requireHighPrecisionCapture_ ? "YES" : "NO",
            DescribeCaptureFormat());
    }

    bool EnsureTexturePool(uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat = DXGI_FORMAT_B8G8R8A8_UNORM) {
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

        poolWidth_ = width;
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

    bool ShouldAdmitFrameToPool(int64_t sourceFrameQpc, int64_t rawSourceFrameQpc, uint32_t poolSize,
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

    bool CopyFrameToPool(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc, int64_t sourceFrameQpc,
                         int64_t rawSourceFrameQpc, ID3D11Texture2D** out, int64_t& copyCompleteQpc,
                         WgcPoolSlotLease& outLease, uint32_t& outSlot, uint64_t& outGeneration) {
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

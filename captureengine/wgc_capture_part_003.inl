            smoothnessVramBudgetMb_, smoothnessSyncDelayFrames_, requiresSourceFramePool);
    }
    void UpdateSmoothnessBudget(uint32_t width, uint32_t height, DXGI_FORMAT format, bool logBudget) {
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
    void ReleaseTexturePool() {
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

    bool EnsureGpuTimingQueries() {
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

    void PollGpuTimingSample() {
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

    void BeginGpuTimingSample() {
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

    void EndGpuTimingSample() {
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

    void LogVideoMemoryInfo(const char* stage, bool force = false) {
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

    static bool IsAllocationExhaustion(HRESULT hr) {
        return hr == E_OUTOFMEMORY || hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
               hr == HRESULT_FROM_WIN32(ERROR_COMMITMENT_LIMIT);
    }

    void SetVideoMemoryReservationBytes(uint64_t requestedBytes, const char* stage) {
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

    void ApplyConfiguredVideoMemoryReservation() {
        if (videoMemoryReservationMode_ == VideoMemoryReservationMode::kOff)
            return;
        const uint32_t mandatorySlots =
            std::max<uint32_t>(ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames,
                               smoothnessSyncDelayFrames_ + smoothnessReservedFreeSlots_ + smoothnessSafetySlots_ + 1u);
        const uint64_t mandatoryBytes = smoothnessSourceEstimatedVramBytes_ +
                                        static_cast<uint64_t>(mandatorySlots) * smoothnessCopyBytesPerSurface_;
        const uint64_t fullBytes = smoothnessSourceEstimatedVramBytes_ +
                                   static_cast<uint64_t>(texturePool_.size()) * smoothnessCopyBytesPerSurface_;
        const bool full = videoMemoryReservationMode_ == VideoMemoryReservationMode::kFull;
        SetVideoMemoryReservationBytes(full ? fullBytes : mandatoryBytes, full ? "full" : "mandatory");
    }

    void ResetVideoMemoryReservation() {
        if (activeVideoMemoryReservationBytes_ != 0)
            SetVideoMemoryReservationBytes(0, "teardown");
        activeVideoMemoryReservationBytes_ = 0;
    }

    void EnableMultithreadProtection(ID3D11Device* device, const char* label) {
        if (!device) {
            return;
        }

        ID3D11Multithread* multithread = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
            LogInfo("[WGC] D3D11 multithread protection enabled on %s device", label);
        }
    }

    void ApplyConfiguredGpuPriority(const char* role) {
        if (!d3dDevice_) {
            return;
        }
        const int requested = std::clamp(desiredGpuPriority_.load(std::memory_order_relaxed), -7, 7);
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT setHr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(setHr) || !dxgiDevice) {
            LogWarn("[WGC] GPU priority query failed: role=%s requested=%d hr=0x%08lX", role, requested,
                    static_cast<unsigned long>(setHr));
            return;
        }
        setHr = dxgiDevice->SetGPUThreadPriority(requested);
        INT actual = 0;
        const HRESULT readbackHr = SUCCEEDED(setHr) ? dxgiDevice->GetGPUThreadPriority(&actual) : setHr;
        if (SUCCEEDED(setHr) && SUCCEEDED(readbackHr) && actual == requested) {
            LogInfo("[WGC] GPU thread priority applied: role=%s requested=%d actual=%d verified=1 dedicated=%d", role,
                    requested, actual, usingDedicatedCaptureDevice_ ? 1 : 0);
        } else {
            LogWarn(
                "[WGC] GPU thread priority apply/readback failed: role=%s requested=%d actual=%d setHr=0x%08lX "
                "readbackHr=0x%08lX verified=0 dedicated=%d",
                role, requested, actual, static_cast<unsigned long>(setHr), static_cast<unsigned long>(readbackHr),
                usingDedicatedCaptureDevice_ ? 1 : 0);
        }
        dxgiDevice->Release();
    }

    bool InitializeDevices(ID3D11Device* encoderDevice) {
        if (!encoderDevice) {
            return false;
        }

        ReleaseTexturePool();
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }

        encoderDevice_ = encoderDevice;
        usingDedicatedCaptureDevice_ = false;

        if (sameDeviceCapture_) {
            d3dDevice_ = encoderDevice_;
            d3dDevice_->GetImmediateContext(&d3dContext_);
            if (!d3dContext_) {
                LogError("[WGC] Failed to acquire same-device D3D11 immediate context");
                d3dDevice_ = nullptr;
                return false;
            }
            EnableMultithreadProtection(d3dDevice_, "same-device capture");
            ApplyConfiguredGpuPriority("same-device-capture");
            LogInfo("[WGC] Same-device capture enabled; reusing encoder D3D11 device");
            return true;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = encoderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);

        if (SUCCEEDED(hr) && adapter) {
            DXGI_ADAPTER_DESC adapterDesc = {};
            adapter->GetDesc(&adapterDesc);

            D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                   0, D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dContext_);
            SafeRelease(adapter);

            if (SUCCEEDED(hr) && d3dDevice_ && d3dContext_) {
                usingDedicatedCaptureDevice_ = (d3dDevice_ != encoderDevice_);
                EnableMultithreadProtection(d3dDevice_, "capture");
                ApplyConfiguredGpuPriority("dedicated-capture");
                LogInfo("[WGC] Dedicated capture D3D11 device created (FL=0x%x, adapter=%ls)", featureLevel,
                        adapterDesc.Description);
                return true;
            }

            SafeRelease(d3dContext_);
            SafeRelease(d3dDevice_);
            LogWarn("[WGC] Dedicated capture device creation failed (0x%08lX); falling back to shared device",
                    (unsigned long)hr);
        } else {
            SafeRelease(adapter);
            LogWarn("[WGC] Failed to resolve encoder adapter for dedicated capture device; falling back");
        }

        d3dDevice_ = encoderDevice_;
        d3dDevice_->GetImmediateContext(&d3dContext_);
        if (!d3dContext_) {
            LogError("[WGC] Failed to acquire fallback shared D3D11 immediate context");
            d3dDevice_ = nullptr;
            return false;
        }
        EnableMultithreadProtection(d3dDevice_, "shared capture");
        ApplyConfiguredGpuPriority("shared-fallback-capture");
        return true;
    }

    void ReleaseCapturedFrame(WGCCapturedFrame& frame) {
        SafeRelease(frame.texture);
        frame.poolLease.Reset();
        frame.poolSlot = std::numeric_limits<uint32_t>::max();
        frame.poolGeneration = 0;
    }

    void ResetStats() {
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }
        callbackFrameCount_.store(0, std::memory_order_relaxed);
        inputFrameCount_.store(0, std::memory_order_relaxed);
        lastCallbackStartQpc_.store(0, std::memory_order_relaxed);
        callbackGapAvgUs_.store(0, std::memory_order_relaxed);
        callbackGapMaxUs_.store(0, std::memory_order_relaxed);
        callbackProcessAvgUs_.store(0, std::memory_order_relaxed);
        callbackProcessMaxUs_.store(0, std::memory_order_relaxed);
        callbackDrainMaxCount_.store(0, std::memory_order_relaxed);
        sourceIntervalAvgUs_.store(0, std::memory_order_relaxed);
        sourceJitterAvgUs_.store(0, std::memory_order_relaxed);
        sourceJitterMaxUs_.store(0, std::memory_order_relaxed);
        sourceToCopyLatencyAvgUs_.store(0, std::memory_order_relaxed);
        sourceToCopyLatencyMaxUs_.store(0, std::memory_order_relaxed);
        deliveredRatePerSec_.store(0, std::memory_order_relaxed);
        deliveredMin250Fps_.store(0, std::memory_order_relaxed);
        deliveredMin500Fps_.store(0, std::memory_order_relaxed);
        inputMin250Fps_.store(0, std::memory_order_relaxed);
        inputMin500Fps_.store(0, std::memory_order_relaxed);
        lastDeliveredRateSampleTickMs_.store(0, std::memory_order_relaxed);
        lastInputRateSampleTickMs_.store(0, std::memory_order_relaxed);
        skippedFrameCount_.store(0, std::memory_order_relaxed);
        pacingSkipCount_.store(0, std::memory_order_relaxed);
        throttleSkipCount_.store(0, std::memory_order_relaxed);
        staleSkipCount_.store(0, std::memory_order_relaxed);
        staleDuplicateTimestampCount_.store(0, std::memory_order_relaxed);
        staleOutOfOrderTimestampCount_.store(0, std::memory_order_relaxed);
        normalizedDuplicateTimestampCount_.store(0, std::memory_order_relaxed);
        duplicateTimestampSkipCount_.store(0, std::memory_order_relaxed);
        cursorOnlySkipCount_.store(0, std::memory_order_relaxed);
        poolDropCount_.store(0, std::memory_order_relaxed);
        keyedMutexAcquireFailCount_.store(0, std::memory_order_relaxed);
        keyedMutexReleaseFailCount_.store(0, std::memory_order_relaxed);
        keyedMutexAbandonedReclaimCount_.store(0, std::memory_order_relaxed);
        splitDeviceFlushCount_.store(0, std::memory_order_relaxed);
        splitDeviceFlushSkippedCount_.store(0, std::memory_order_relaxed);
        poolSlotFastRewriteCount_.store(0, std::memory_order_relaxed);
        lastPoolSlotRewriteUs_.store(0, std::memory_order_relaxed);
        poolSlotOverwritePreventedCount_.store(0, std::memory_order_relaxed);
        poolSaturatedDropCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedLowWaterCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedRecoveryCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedSourceBelowCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedHealthyCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedUniformPlayoutSoftReserveCount_.store(0, std::memory_order_relaxed);
        ingressAcceptedUniformPlayoutCreditCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedSoftReserveCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedHardReserveCount_.store(0, std::memory_order_relaxed);
        ingressDecimatedCreditCount_.store(0, std::memory_order_relaxed);
        ingressSoftReservePressureCount_.store(0, std::memory_order_relaxed);
        ingressHardReservePressureCount_.store(0, std::memory_order_relaxed);
        ingressRetainedFrames_.store(0, std::memory_order_relaxed);
        ingressRetainedFrameCap_.store(smoothnessRetainedFrameCap_, std::memory_order_relaxed);
        ingressLowWaterFrames_.store(0, std::memory_order_relaxed);
        ingressRecovering_.store(false, std::memory_order_relaxed);
        ingressUniformPlayoutOwnsSurplus_.store(false, std::memory_order_relaxed);
        ingressLastReason_.store(kWgcIngressReasonUncapped, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> ingressLock(ingressAdmissionMutex_);
            ingressCreditLastQpc_ = 0;
            ingressCreditFrames_ = 1.0;
        }
        if (poolLeaseState_) {
            const uint32_t current = poolLeaseState_->leasedCurrent.load(std::memory_order_relaxed);
            poolLeaseState_->leasedMax.store(current, std::memory_order_relaxed);
            poolLeaseState_->freeMin.store(
                current >= poolLeaseState_->slotCount ? 0u : poolLeaseState_->slotCount - current,
                std::memory_order_relaxed);
            poolLeaseState_->releaseMismatchCount.store(0, std::memory_order_relaxed);
        }
        std::fill(poolSlotLastWriteQpc_.begin(), poolSlotLastWriteQpc_.end(), 0);
        lastCopyUs_.store(0, std::memory_order_relaxed);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);
        lastDeliveredSourceQpc_.store(0, std::memory_order_relaxed);
        lastDeliveredRawSourceQpc_.store(0, std::memory_order_relaxed);
        lastObservedRawSourceQpc_.store(0, std::memory_order_relaxed);
        lastAssignedSourceQpc_.store(0, std::memory_order_relaxed);
        lastObservedSourceQpc_ = 0;
        smoothedSourceIntervalQpc_ = 0;
        sourceIntervalSamples_ = 0;
        sourceIntervalAccumUs_ = 0;
        sourceJitterAccumUs_ = 0;
        sourceJitterMaxUsValue_ = 0;
        sourceToCopyLatencySamples_ = 0;
        sourceToCopyLatencyAccumUs_ = 0;
        sourceToCopyLatencyMaxUsValue_ = 0;
        deliveredRateWindow_.Reset();
        inputRateWindow_.Reset();
        lastCapturedQPC_ = 0;
        nextCaptureQPC_ = 0;
        ApplyProducerInterval();
        std::lock_guard<std::mutex> lock(frameMutex_);
        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void ReleasePendingFramesLocked() {
        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void EnqueueFrameInternal(WGCCapturedFrame&& frame) {
        if (!frame.texture) {
            return;
        }

        const int64_t frameKey = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        if (!pendingFrames_.empty()) {
            WGCCapturedFrame& lastPending = pendingFrames_.back();
            const int64_t lastKey = lastPending.rawTimestamp > 0 ? lastPending.rawTimestamp : lastPending.timestamp;
            if (frameKey > 0 && lastKey > 0 && frameKey == lastKey) {
                ReleaseCapturedFrame(lastPending);
                lastPending = std::move(frame);
                return;
            }
        }

        pendingFrames_.push_back(std::move(frame));
        while (pendingFrames_.size() > kMaxPendingFrames) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }
    }

    void QueuePendingFrame(WGCCapturedFrame&& frame) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        EnqueueFrameInternal(std::move(frame));
    }

    void RecordInputFrameEvent() {
        const uint64_t nowMs = GetTickCount64();
        inputRateWindow_.AddSample(nowMs);
        lastInputRateSampleTickMs_.store(nowMs, std::memory_order_relaxed);
        inputMin250Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        inputMin500Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);
    }

    void RecordDeliveredFrameEvent() {
        const uint64_t nowMs = GetTickCount64();
        deliveredRateWindow_.AddSample(nowMs);
        lastDeliveredRateSampleTickMs_.store(nowMs, std::memory_order_relaxed);
        deliveredRatePerSec_.store(deliveredRateWindow_.RatePerSecond(nowMs, 1000), std::memory_order_relaxed);
        deliveredMin250Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        deliveredMin500Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);
    }

    void RecordSourceTimingSample(int64_t sourceFrameQpc) {
        if (sourceFrameQpc <= 0 || qpcFreq_ <= 0) {
            return;
        }

        if (lastObservedSourceQpc_ > 0 && sourceFrameQpc > lastObservedSourceQpc_) {
            const int64_t intervalQpc = sourceFrameQpc - lastObservedSourceQpc_;
            const int64_t intervalUs = (intervalQpc * 1000000) / qpcFreq_;
            if (intervalUs > 0) {
                sourceIntervalAccumUs_ += static_cast<uint64_t>(intervalUs);
                sourceIntervalSamples_++;
                sourceIntervalAvgUs_.store(static_cast<int64_t>(sourceIntervalAccumUs_ / sourceIntervalSamples_),
                                           std::memory_order_relaxed);

                if (smoothedSourceIntervalQpc_ <= 0) {
                    smoothedSourceIntervalQpc_ = intervalQpc;
                } else {
                    smoothedSourceIntervalQpc_ = (smoothedSourceIntervalQpc_ * 7 + intervalQpc) / 8;
                }

                if (smoothedSourceIntervalQpc_ > 0) {
                    const int64_t jitterQpc = intervalQpc >= smoothedSourceIntervalQpc_
                                                  ? (intervalQpc - smoothedSourceIntervalQpc_)
                                                  : (smoothedSourceIntervalQpc_ - intervalQpc);
                    const int64_t jitterUs = (jitterQpc * 1000000) / qpcFreq_;
                    sourceJitterAccumUs_ += static_cast<uint64_t>(jitterUs);
                    sourceJitterAvgUs_.store(static_cast<int64_t>(sourceJitterAccumUs_ / sourceIntervalSamples_),
                                             std::memory_order_relaxed);
                    sourceJitterMaxUsValue_ =
                        std::max<uint32_t>(sourceJitterMaxUsValue_, static_cast<uint32_t>(jitterUs));
                    sourceJitterMaxUs_.store(sourceJitterMaxUsValue_, std::memory_order_relaxed);
                }
            }
        }

        lastObservedSourceQpc_ = sourceFrameQpc;
    }

    void RecordSourceToCopyLatency(int64_t sourceFrameQpc, int64_t copyCompleteQpc) {
        if (sourceFrameQpc <= 0 || copyCompleteQpc <= sourceFrameQpc || qpcFreq_ <= 0) {
            return;
        }

        const int64_t latencyUs = ((copyCompleteQpc - sourceFrameQpc) * 1000000) / qpcFreq_;
        if (latencyUs < 0) {
            return;
        }

        sourceToCopyLatencyAccumUs_ += static_cast<uint64_t>(latencyUs);
        sourceToCopyLatencySamples_++;
        sourceToCopyLatencyAvgUs_.store(static_cast<int64_t>(sourceToCopyLatencyAccumUs_ / sourceToCopyLatencySamples_),
                                        std::memory_order_relaxed);
        sourceToCopyLatencyMaxUsValue_ =
            std::max<uint32_t>(sourceToCopyLatencyMaxUsValue_, static_cast<uint32_t>(latencyUs));
        sourceToCopyLatencyMaxUs_.store(sourceToCopyLatencyMaxUsValue_, std::memory_order_relaxed);
    }

    void ApplyFrameThrottleInterval() {
        if (targetFps_ > 0 && qpcFreq_ > 0) {
            targetIntervalQPC_ = qpcFreq_ / targetFps_;
        } else {
            targetIntervalQPC_ = 0;
        }
        lastCapturedQPC_ = 0;
        nextCaptureQPC_ = 0;
    }

    void ApplyProducerInterval() {
        if (producerTargetFps_ > 0 && qpcFreq_ > 0) {
            producerIntervalQPC_ = std::max<int64_t>(1, qpcFreq_ / static_cast<int64_t>(producerTargetFps_));
        } else {
            producerIntervalQPC_ = 0;
        }
    }

    void ApplyMinUpdateInterval() {
        if (!session_) {
            return;
        }

        try {
            const int64_t interval100ns =
                producerTargetFps_ > 0 ? std::max<int64_t>(1, 10000000ll / static_cast<int64_t>(producerTargetFps_))
                                       : 0;

            IGraphicsCaptureSession5Abi* session5 = nullptr;
            if (!TryQueryComInterface(session_, IID_IGraphicsCaptureSession5Abi, reinterpret_cast<void**>(&session5)) ||
                !session5) {
                LogInfo("[WGC] MinUpdateInterval not available (older WinRT projection/runtime)");

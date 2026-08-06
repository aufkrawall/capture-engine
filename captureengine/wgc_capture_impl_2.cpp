#include "wgc_capture_internal.h"

#if HAS_WGC

void WGCCapture::Impl::ApplyConfiguredVideoMemoryReservation() {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::ResetVideoMemoryReservation() {

        if (activeVideoMemoryReservationBytes_ != 0)
            SetVideoMemoryReservationBytes(0, "teardown");
        activeVideoMemoryReservationBytes_ = 0;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::EnableMultithreadProtection(ID3D11Device* device,  const char* label) {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::ApplyConfiguredGpuPriority(const char* role) {

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

#endif

#if HAS_WGC

bool WGCCapture::Impl::InitializeDevices(ID3D11Device* encoderDevice) {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::ReleaseCapturedFrame(WGCCapturedFrame& frame) {

        SafeRelease(frame.texture);
        frame.poolLease.Reset();
        frame.poolSlot = std::numeric_limits<uint32_t>::max();
        frame.poolGeneration = 0;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ResetStats() {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::ReleasePendingFramesLocked() {

        while (!pendingFrames_.empty()) {
            WGCCapturedFrame stale = std::move(pendingFrames_.front());
            pendingFrames_.pop_front();
            ReleaseCapturedFrame(stale);
        }

}

#endif

#if HAS_WGC

void WGCCapture::Impl::EnqueueFrameInternal(WGCCapturedFrame&& frame) {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::QueuePendingFrame(WGCCapturedFrame&& frame) {

        std::lock_guard<std::mutex> lock(frameMutex_);
        EnqueueFrameInternal(std::move(frame));

}

#endif

#if HAS_WGC

void WGCCapture::Impl::RecordInputFrameEvent() {

        const uint64_t nowMs = GetTickCount64();
        inputRateWindow_.AddSample(nowMs);
        lastInputRateSampleTickMs_.store(nowMs, std::memory_order_relaxed);
        inputMin250Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        inputMin500Fps_.store(inputRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::RecordDeliveredFrameEvent() {

        const uint64_t nowMs = GetTickCount64();
        deliveredRateWindow_.AddSample(nowMs);
        lastDeliveredRateSampleTickMs_.store(nowMs, std::memory_order_relaxed);
        deliveredRatePerSec_.store(deliveredRateWindow_.RatePerSecond(nowMs, 1000), std::memory_order_relaxed);
        deliveredMin250Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 250, 1000), std::memory_order_relaxed);
        deliveredMin500Fps_.store(deliveredRateWindow_.MinRatePerSecond(nowMs, 500, 1000), std::memory_order_relaxed);

}

#endif

#if HAS_WGC

void WGCCapture::Impl::RecordSourceTimingSample(int64_t sourceFrameQpc) {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::RecordSourceToCopyLatency(int64_t sourceFrameQpc,  int64_t copyCompleteQpc) {

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

#endif

#if HAS_WGC

void WGCCapture::Impl::ApplyFrameThrottleInterval() {

        if (targetFps_ > 0 && qpcFreq_ > 0) {
            targetIntervalQPC_ = qpcFreq_ / targetFps_;
        } else {
            targetIntervalQPC_ = 0;
        }
        lastCapturedQPC_ = 0;
        nextCaptureQPC_ = 0;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ApplyProducerInterval() {

        if (producerTargetFps_ > 0 && qpcFreq_ > 0) {
            producerIntervalQPC_ = std::max<int64_t>(1, qpcFreq_ / static_cast<int64_t>(producerTargetFps_));
        } else {
            producerIntervalQPC_ = 0;
        }

}

#endif

#if HAS_WGC

void WGCCapture::Impl::ApplyMinUpdateInterval() {

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

#endif
#if HAS_WGC
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

#endif

#if HAS_WGC

int64_t WGCCapture::Impl::GetFrameSourceQpc(const winrt::Direct3D11CaptureFrame& frame) const {

        const auto systemRelativeTime = frame.SystemRelativeTime();
        return HundredNanosecondsToQpcTicks(systemRelativeTime.count(), qpcFreq_);

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::IsStaleSourceFrameQpc(int64_t sourceFrameQpc) const {

        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastDeliveredSourceQpc = lastDeliveredSourceQpc_.load(std::memory_order_relaxed);
        return lastDeliveredSourceQpc > 0 && sourceFrameQpc <= lastDeliveredSourceQpc;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::IsOutOfOrderRawSourceFrameQpc(int64_t sourceFrameQpc) const {

        if (sourceFrameQpc <= 0) {
            return false;
        }

        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        return lastObservedRawSourceQpc > 0 && sourceFrameQpc < lastObservedRawSourceQpc;

}

#endif

#if HAS_WGC

#endif

#if HAS_WGC

HMONITOR WGCCapture::Impl::ResolveTargetMonitor() const {

        if (targetWindow_) {
            return MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTONEAREST);
        }
        if (targetMonitor_) {
            return targetMonitor_;
        }
        return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::GetCaptureOrigin(int32_t& left,  int32_t& top) const {

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

#endif

#if HAS_WGC

const char* WGCCapture::Impl::ResolveWindowCaptureOrigin(int32_t& left,  int32_t& top) const {

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

#endif

#if HAS_WGC

#endif

#if HAS_WGC

#endif

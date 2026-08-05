#include "wgc_capture_internal.h"

WGCCapture::WGCCapture() : impl_(std::make_unique<Impl>()) {}

WGCCapture::~WGCCapture() {
    StopCapture();

    // Release WinRT/COM capture objects before apartment teardown.
    impl_.reset();

#if HAS_WGC
    if (roInitialized_) {
        RoUninitialize();
        roInitialized_ = false;
    }
#endif
}

bool WGCCapture::IsSupported() {
#if HAS_WGC
    // Check if GraphicsCaptureSession is supported
    return winrt::GraphicsCaptureSession::IsSupported();
#else
    return false;
#endif
}

bool WGCCapture::IsHdrOutputColorSpace(int colorSpace) {
    return ::IsHdrOutputColorSpace(static_cast<DXGI_COLOR_SPACE_TYPE>(colorSpace));
}

bool WGCCapture::QueryOutputDesc1ForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc1) {
    if (!monitor)
        return false;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
        return false;

    bool found = false;
    for (UINT adapterIndex = 0; !found; ++adapterIndex) {
        IDXGIAdapter1* adapter = nullptr;
        hr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr) || !adapter)
            continue;

        for (UINT outputIndex = 0; !found; ++outputIndex) {
            IDXGIOutput* output = nullptr;
            hr = adapter->EnumOutputs(outputIndex, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || !output)
                continue;

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

bool WGCCapture::Init(ID3D11Device* device) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] Init failed: D3D11 device is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices");
        return false;
    }

    // Initialize COM for WinRT
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);  // Track so we can balance with RoUninitialize

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    // Get primary monitor
    HMONITOR hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!impl_->CreateForMonitor(hmon)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for primary monitor");
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::InitForWindow(ID3D11Device* device, void* hwnd) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForWindow failed: D3D11 device is null");
        return false;
    }
    if (!hwnd) {
        LogError("[WGC] InitForWindow failed: window handle is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for window capture");
        return false;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    if (!impl_->CreateForWindow((HWND)hwnd)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for window 0x%p", hwnd);
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

void WGCCapture::SetCaptureCursor(bool enabled) {
    captureCursor_ = enabled;
}

bool WGCCapture::InitForMonitor(ID3D11Device* device, void* hmonitor) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForMonitor failed: D3D11 device is null");
        return false;
    }
    if (!hmonitor) {
        LogError("[WGC] InitForMonitor failed: monitor handle is null");
        return false;
    }
    device_ = device;
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for monitor capture");
        return false;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogError("[WGC] RoInitialize failed: 0x%lx", hr);
        return false;
    }
    roInitialized_ = SUCCEEDED(hr);

    if (!impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to create WinRT device");
        return false;
    }

    if (!impl_->CreateForMonitor((HMONITOR)hmonitor)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for monitor 0x%p", hmonitor);
    return true;
#else
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::StartCapture() {
    if (!initialized_)
        return false;

#if HAS_WGC
    if ((!impl_->d3dDevice_ || !impl_->d3dContext_) && !impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to rebuild capture devices for restart");
        return false;
    }
    if (!impl_->winrtDevice_ && !impl_->CreateWinRTDevice()) {
        LogError("[WGC] Failed to rebuild WinRT capture device for restart");
        return false;
    }
#endif

    bool result = false;
    try {
        result = impl_->StartCapture(width_, height_, captureCursor_);
    } catch (const winrt::hresult_error& e) {
        LogError("[WGC] Capture start failed with WinRT error 0x%08lX: %ls", static_cast<unsigned long>(e.code().value),
                 e.message().c_str());
        impl_->StopCapture();
    } catch (const std::exception& e) {
        LogError("[WGC] Capture start failed with C++ exception: %s", e.what());
        impl_->StopCapture();
    } catch (...) {
        LogError("[WGC] Capture start failed with an unknown exception");
        impl_->StopCapture();
    }
    if (result) {
        capturing_ = true;
        LogInfo("[WGC] Capture started");
    }
    return result;
}

void WGCCapture::StopCapture() {
    capturing_ = false;
    impl_->StopCapture();
    LogInfo("[WGC] Capture stopped");
}

bool WGCCapture::GetNextFrame(WGCCapturedFrame& frame) {
    if (!capturing_)
        return false;
    return impl_->GetNextFrame(frame);
}

size_t WGCCapture::DrainPendingFrames(std::vector<WGCCapturedFrame>& frames, size_t maxFrames) {
#if HAS_WGC
    if (!capturing_ || !impl_) {
        frames.clear();
        return 0;
    }
    return impl_->DrainPendingFrames(frames, maxFrames);
#else
    (void)maxFrames;
    frames.clear();
    return 0;
#endif
}

bool WGCCapture::GetCaptureOrigin(int32_t& left, int32_t& top) const {
#if HAS_WGC
    if (impl_) {
        return impl_->GetCaptureOrigin(left, top);
    }
#endif
    left = 0;
    top = 0;
    return false;
}

HANDLE WGCCapture::GetFrameArrivedEvent() const {
#if HAS_WGC
    return impl_ ? impl_->frameArrivedEvent_ : NULL;
#else
    return NULL;
#endif
}

void WGCCapture::SetSourceEpoch(uint64_t sourceEpoch) {
#if HAS_WGC
    if (impl_) {
        impl_->sourceEpoch_.store(sourceEpoch, std::memory_order_release);
    }
#else
    (void)sourceEpoch;
#endif
}

uint64_t WGCCapture::GetSourceEpoch() const {
#if HAS_WGC
    return impl_ ? impl_->sourceEpoch_.load(std::memory_order_acquire) : 0;
#else
    return 0;
#endif
}

void WGCCapture::SetDirectFrameCallback(
    std::function<void(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, bool,
                       const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint64_t, WgcPoolSlotLease&&)>
        callback) {
#if HAS_WGC
    if (impl_) {
        // Extract the raw function pointer from std::function.
        // Only static/free functions are ever passed (QueueWgcFrame or nullptr).
        Impl::DirectFrameCallbackFn rawPtr = nullptr;
        if (callback) {
            if (auto target = callback.target<Impl::DirectFrameCallbackFn>()) {
                rawPtr = *target;
            }
        }
        impl_->frameCallback_.store(rawPtr, std::memory_order_release);
    }
#endif
}

void WGCCapture::SetDirectCursorCallback(
    std::function<void(const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint32_t, uint32_t, uint64_t)>
        callback) {
#if HAS_WGC
    if (impl_) {
        Impl::DirectCursorCallbackFn rawPtr = nullptr;
        if (callback) {
            if (auto target = callback.target<Impl::DirectCursorCallbackFn>()) {
                rawPtr = *target;
            }
        }
        impl_->cursorCallback_.store(rawPtr, std::memory_order_release);
    }
#endif
}

void WGCCapture::SetGpuPriority(int priority) {
#if HAS_WGC
    if (!impl_) {
        return;
    }
    impl_->desiredGpuPriority_.store(std::clamp(priority, -7, 7), std::memory_order_relaxed);
    impl_->ApplyConfiguredGpuPriority("runtime-update");
#else
    (void)priority;
#endif
}

void WGCCapture::ResetStats() {
    droppedFrames.store(0, std::memory_order_relaxed);
#if HAS_WGC
    if (impl_) {
        impl_->ResetStats();
    }
#endif
}

void WGCCapture::SetTargetFps(uint32_t fps) {
#if HAS_WGC
    if (impl_) {
        impl_->targetFps_ = fps;
        impl_->producerTargetFps_ = fps;
        impl_->ApplyFrameThrottleInterval();
        impl_->ApplyProducerInterval();
        impl_->ApplyMinUpdateInterval();

        if (fps > 0) {
            if (impl_->targetIntervalQPC_ > 0) {
                LogInfo("[WGC] Frame throttle set to %u fps (interval=%lld QPC ticks)", fps,
                        (long long)impl_->targetIntervalQPC_);
            } else {
                LogInfo("[WGC] Frame throttle armed for %u fps (pending capture start)", fps);
            }
        } else {
            LogInfo("[WGC] Frame throttle disabled");
        }
    }
#endif
}

uint32_t WGCCapture::GetTargetFps() const {
#if HAS_WGC
    return impl_ ? impl_->targetFps_ : 0;
#else
    return 0;
#endif
}

void WGCCapture::SetProducerTargetFps(uint32_t fps) {
#if HAS_WGC
    if (impl_) {
        impl_->producerTargetFps_ = fps;
        impl_->ApplyProducerInterval();
        impl_->ApplyMinUpdateInterval();

        if (fps > 0) {
            if (impl_->producerIntervalQPC_ > 0) {
                LogInfo("[WGC] Producer cadence target set to %u fps (interval=%lld QPC ticks)", fps,
                        static_cast<long long>(impl_->producerIntervalQPC_));
            } else {
                LogInfo("[WGC] Producer cadence target armed for %u fps (pending capture start)", fps);
            }
        } else {
            LogInfo("[WGC] Producer cadence target disabled (max rate)");
        }
    }
#endif
}

uint32_t WGCCapture::GetProducerTargetFps() const {
#if HAS_WGC
    return impl_ ? impl_->producerTargetFps_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSkippedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->skippedFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int32_t WGCCapture::GetInflightCallbackCount() const {
#if HAS_WGC
    return impl_ ? static_cast<int32_t>(impl_->frameCallbackState_->ActiveCount()) : 0;
#else
    return 0;
#endif
}

bool WGCCapture::IsHighPrecisionSource() const {
#if HAS_WGC
    if (impl_) {
        return impl_->useHighPrecisionCapture_;
    }
#endif
    return false;
}

bool WGCCapture::IsWindowTarget() const {
#if HAS_WGC
    return impl_ && impl_->targetWindow_ != nullptr;
#else
    return false;
#endif
}

bool WGCCapture::IsTargetWindowValid() const {
#if HAS_WGC
    return impl_ && (!impl_->targetWindow_ || IsWindow(impl_->targetWindow_));
#else
    return false;
#endif
}

void WGCCapture::GetTargetIdentity(HWND* hwnd, HMONITOR* hmonitor) const {
#if HAS_WGC
    if (hwnd) {
        *hwnd = impl_ ? impl_->targetWindow_ : nullptr;
    }
    if (hmonitor) {
        *hmonitor = impl_ ? impl_->targetMonitor_ : nullptr;
    }
#else
    if (hwnd) {
        *hwnd = nullptr;
    }
    if (hmonitor) {
        *hmonitor = nullptr;
    }
#endif
}

bool WGCCapture::NeedsReset() const {
#if HAS_WGC
    if (impl_) {
        // VFR uses the direct callback path and does not drain the internal
        // frame queue, so service deferred output/HDR probes here as well as
        // from DrainPendingFrames. This remains on the owner/media thread.
        impl_->MaybePerformDeferredHDRRecheck();
        return impl_->NeedsReset();
    }
    return false;
#else
    return false;
#endif
}

std::string WGCCapture::ConsumeResetReason() {
#if HAS_WGC
    return impl_ ? impl_->ConsumeResetReason() : std::string();
#else
    return {};
#endif
}

void WGCCapture::ForceReset() {
#if HAS_WGC
    capturing_ = false;

    if (impl_) {
        HWND targetWindow = impl_->targetWindow_;
        HMONITOR targetMonitor = impl_->targetMonitor_;
        const bool wasWindowCapture = targetWindow != nullptr;
        const bool wasMonitorCapture = targetMonitor != nullptr && targetWindow == nullptr;
        const bool wasDuplicationBackend = impl_->useDuplicationBackend_;
        const bool smoothnessBufferEnabled = impl_->smoothnessBufferEnabled_;
        const uint32_t smoothnessOutputFps = impl_->smoothnessOutputFps_;
        const uint32_t smoothnessMaxMs = impl_->smoothnessMaxMs_;
        const uint32_t smoothnessVramBudgetMb = impl_->smoothnessVramBudgetMb_;
        const uint32_t smoothnessSyncDelayFrames = impl_->smoothnessSyncDelayFrames_;
        const bool skipSplitDeviceFlush = impl_->skipSplitDeviceFlush_;
        const bool sameDeviceCapture = impl_->sameDeviceCapture_;
        const bool allowLossyBgra8Pool = impl_->allowLossyBgra8Pool_;
        const bool requireHighPrecisionCapture = impl_->requireHighPrecisionCapture_;
        const bool allowDuplicationFallback = impl_->allowDuplicationFallback_;
        const uint32_t targetFps = impl_->targetFps_;
        const uint32_t producerTargetFps = impl_->producerTargetFps_;
        const auto* throttleFlag = impl_->throttleFlag_;
        const auto directFrameCallback = impl_->frameCallback_.load(std::memory_order_acquire);
        const auto directCursorCallback = impl_->cursorCallback_.load(std::memory_order_acquire);
        const uint64_t sourceEpoch = impl_->sourceEpoch_.load(std::memory_order_acquire);

        // Stop all producers and synchronously drain the per-instance callback
        // epoch before destroying Impl. A queued WinRT callback can retain the
        // shared gate, but cannot reacquire this owner after StopCapture.
        impl_->frameCallback_.store(nullptr, std::memory_order_release);
        impl_->cursorCallback_.store(nullptr, std::memory_order_release);
        impl_->alive_.store(false, std::memory_order_release);
        impl_->StopCapture();

        impl_.reset();
        impl_ = std::make_unique<Impl>();
        impl_->smoothnessBufferEnabled_ = smoothnessBufferEnabled;
        impl_->smoothnessOutputFps_ = smoothnessOutputFps;
        impl_->smoothnessMaxMs_ = smoothnessMaxMs;
        impl_->smoothnessVramBudgetMb_ = smoothnessVramBudgetMb;
        impl_->smoothnessSyncDelayFrames_ = smoothnessSyncDelayFrames;
        impl_->skipSplitDeviceFlush_ = skipSplitDeviceFlush;
        impl_->sameDeviceCapture_ = wasDuplicationBackend ? false : sameDeviceCapture;
        impl_->allowLossyBgra8Pool_ = allowLossyBgra8Pool;
        impl_->requireHighPrecisionCapture_ = requireHighPrecisionCapture;
        impl_->allowDuplicationFallback_ = allowDuplicationFallback;
        impl_->targetFps_ = targetFps;
        impl_->producerTargetFps_ = producerTargetFps;
        impl_->throttleFlag_ = throttleFlag;
        impl_->sourceEpoch_.store(sourceEpoch, std::memory_order_release);
        impl_->frameCallback_.store(directFrameCallback, std::memory_order_release);
        impl_->cursorCallback_.store(directCursorCallback, std::memory_order_release);
        if (!impl_->InitializeDevices(device_)) {
            LogError("[WGC] ForceReset failed to reinitialize capture devices");
            return;
        }

        if (device_) {
            if (!impl_->CreateWinRTDevice()) {
                LogError("[WGC] ForceReset failed to rebuild WinRT device");
                return;
            }
            if (wasWindowCapture && targetWindow) {
                if (!impl_->CreateForWindow(targetWindow)) {
                    LogWarn("[WGC] ForceReset failed to recreate window target");
                }
            } else if (wasDuplicationBackend && targetMonitor) {
                if (!impl_->CreateForMonitorDuplication(targetMonitor)) {
                    if (!impl_->allowDuplicationFallback_) {
                        LogError(
                            "[WGC] ForceReset failed to recreate strict duplication target; WGC fallback disabled");
                    } else {
                        LogWarn("[WGC] ForceReset failed to recreate duplication target; trying WGC monitor item");
                    }
                    if (impl_->allowDuplicationFallback_ && !impl_->CreateForMonitor(targetMonitor)) {
                        LogWarn("[WGC] ForceReset failed to recreate monitor target");
                    }
                }
            } else if (wasMonitorCapture && targetMonitor) {
                if (!impl_->CreateForMonitor(targetMonitor)) {
                    LogWarn("[WGC] ForceReset failed to recreate monitor target");
                }
            }
        }

        LogWarn("[WGC] ForceReset complete - WGC session recreated");
    }
#endif
}

void WGCCapture::SetThrottleFlag(const std::atomic<bool>* flag) {
#if HAS_WGC
    if (impl_) {
        impl_->throttleFlag_ = flag;
    }
#endif
}

void WGCCapture::SetSkipSplitDeviceFlush(bool enabled) {

#if HAS_WGC
    if (impl_) {
        impl_->skipSplitDeviceFlush_ = enabled;
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetSameDeviceCapture(bool enabled) {
#if HAS_WGC
    if (impl_) {
        const bool effectiveEnabled = impl_->useDuplicationBackend_ ? false : enabled;
        const bool changed = impl_->sameDeviceCapture_ != effectiveEnabled;
        impl_->sameDeviceCapture_ = effectiveEnabled;
        if (changed && impl_->d3dDevice_) {
            impl_->FlagResetNeeded("same-device capture option changed");
        }
        if (impl_->useDuplicationBackend_ && enabled) {
            LogInfo("[DXGIDup] Ignoring WGC same-device option; duplication isolation remains dedicated");
        }
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetAllowLossyBgra8Pool(bool enabled) {
#if HAS_WGC
    if (impl_) {
        impl_->allowLossyBgra8Pool_ = enabled;
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetRequireHighPrecisionCapture(bool enabled) {
#if HAS_WGC
    if (impl_) {
        const bool changed = impl_->requireHighPrecisionCapture_ != enabled;
        impl_->requireHighPrecisionCapture_ = enabled;
        if (changed && (impl_->framePool_ || impl_->dupSource_)) {
            impl_->FlagResetNeeded("high-precision capture requirement changed");
        }
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetAllowDuplicationFallback(bool enabled) {
#if HAS_WGC
    if (impl_) {
        impl_->allowDuplicationFallback_ = enabled;
    }
#else
    (void)enabled;
#endif
}

void WGCCapture::SetSmoothnessBufferBudget(bool enabled, uint32_t outputFps, uint32_t maxMs, uint32_t vramBudgetMb,
                                           uint32_t syncDelayFrames) {
#if HAS_WGC
    if (impl_) {
        impl_->smoothnessBufferEnabled_ = enabled;
        impl_->smoothnessOutputFps_ = outputFps;
        impl_->smoothnessMaxMs_ = maxMs;
        impl_->smoothnessVramBudgetMb_ = vramBudgetMb;
        impl_->smoothnessSyncDelayFrames_ = (enabled && syncDelayFrames == 0)
                                                ? ce::capture_policy::GetWgcEstimatedSyncDelayFramesForBudget(outputFps)
                                                : syncDelayFrames;
        LogInfo("[WGC] Smoothness buffer config: enabled=%d outputFps=%u maxMs=%u budget=%uMB syncFrames=%u",
                enabled ? 1 : 0, outputFps, maxMs, vramBudgetMb, impl_->smoothnessSyncDelayFrames_);
    }
#else
    (void)enabled;
    (void)outputFps;
    (void)maxMs;
    (void)vramBudgetMb;
    (void)syncDelayFrames;
#endif
}

void WGCCapture::SetVideoMemoryReservationMode(const std::string& mode) {
#if HAS_WGC
    if (impl_) {
        Impl::VideoMemoryReservationMode resolved = Impl::VideoMemoryReservationMode::kOff;
        if (mode == "mandatory")
            resolved = Impl::VideoMemoryReservationMode::kMandatory;
        else if (mode == "full")
            resolved = Impl::VideoMemoryReservationMode::kFull;
        if (impl_->videoMemoryReservationMode_ != resolved && impl_->activeVideoMemoryReservationBytes_ != 0)
            impl_->ResetVideoMemoryReservation();
        impl_->videoMemoryReservationMode_ = resolved;
        LogInfo("[WGC] Diagnostic video-memory reservation mode: %s", mode.c_str());
    }
#else
    (void)mode;
#endif
}

void WGCCapture::SetRetainedFramePressure(uint32_t retainedFrames, uint32_t retainedFrameCap, uint32_t lowWaterFrames,
                                          bool recovering, bool uniformPlayoutOwnsSurplus) {
#if HAS_WGC
    if (impl_) {
        const uint32_t effectiveCap = retainedFrameCap > 0 ? retainedFrameCap : impl_->smoothnessRetainedFrameCap_;
        impl_->ingressRetainedFrames_.store(retainedFrames, std::memory_order_relaxed);
        impl_->ingressRetainedFrameCap_.store(effectiveCap, std::memory_order_relaxed);
        impl_->ingressLowWaterFrames_.store(lowWaterFrames, std::memory_order_relaxed);
        impl_->ingressRecovering_.store(recovering, std::memory_order_relaxed);
        impl_->ingressUniformPlayoutOwnsSurplus_.store(uniformPlayoutOwnsSurplus, std::memory_order_relaxed);
    }
#else
    (void)retainedFrames;
    (void)retainedFrameCap;
    (void)lowWaterFrames;
    (void)recovering;
    (void)uniformPlayoutOwnsSurplus;
#endif
}

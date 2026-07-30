};

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

bool WGCCapture::InitForMonitorDuplication(ID3D11Device* device, void* hmonitor) {
#if HAS_WGC
    if (!device) {
        LogError("[WGC] InitForMonitorDuplication failed: D3D11 device is null");
        return false;
    }
    device_ = device;
    // Keep AcquireNextFrame/ReleaseFrame and canonicalization off the encoder
    // immediate context.  The dedicated device is created on the exact same
    // adapter and crosses into the encoder only through keyed pool surfaces.
    impl_->sameDeviceCapture_ = false;
    LogInfo("[DXGIDup] Dedicated same-adapter capture device required for duplication isolation");
    if (!impl_->InitializeDevices(device_)) {
        LogError("[WGC] Failed to initialize capture devices for duplication capture");
        return false;
    }

    // WinRT stays initialized so StartCapture can fall back to a WGC monitor
    // item in place if the duplication becomes unavailable at start time.
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

    if (!impl_->CreateForMonitorDuplication((HMONITOR)hmonitor)) {
        return false;
    }

    initialized_ = true;
    LogInfo("[WGC] Initialized for monitor 0x%p (DXGI duplication backend)", hmonitor);
    return true;
#else
    (void)device;
    (void)hmonitor;
    LogError("[WGC] Not available - WinRT headers not found");
    return false;
#endif
}

bool WGCCapture::IsUsingDesktopDuplication() const {
#if HAS_WGC
    return impl_ && impl_->useDuplicationBackend_;
#else
    return false;
#endif
}

uint64_t WGCCapture::GetDuplicationAcquireTimeoutCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetAcquireTimeoutCount();
    }
#endif
    return 0;
}

uint64_t WGCCapture::GetDuplicationAccumulatedMissedFrameCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetAccumulatedMissedFrameCount();
    }
#endif
    return 0;
}

uint64_t WGCCapture::GetDuplicationPointerUpdateCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetPointerUpdateCount();
    }
#endif
    return 0;
}

uint64_t WGCCapture::GetDuplicationForwardedPointerUpdateCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetForwardedPointerUpdateCount();
    }
#endif
    return 0;
}

bool WGCCapture::IsDuplicationCursorEmbedded() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->IsCursorEmbeddedInFrames();
    }
#endif
    return false;
}

bool WGCCapture::IsDuplicationSeparatePointerVisible() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->IsSeparatePointerVisible();
    }
#endif
    return true;
}

uint64_t WGCCapture::GetDuplicationPointerStateTransitionCount() const {
#if HAS_WGC
    if (impl_ && impl_->dupSource_) {
        return impl_->dupSource_->GetPointerStateTransitionCount();
    }
#endif
    return 0;
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

uint32_t WGCCapture::GetCallbackFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->callbackFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->inputFrameCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetLastCopyTimeUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastCopyUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceIntervalAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceIntervalAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceJitterAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceJitterAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceJitterMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceJitterMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceToCopyLatencyAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceToCopyLatencyAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetSourceToCopyLatencyMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->sourceToCopyLatencyMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredRatePerSec() const {
#if HAS_WGC
    return impl_ ? ce::rate_window::AgeCachedRate(impl_->deliveredRatePerSec_.load(std::memory_order_relaxed),
                                                  impl_->lastDeliveredRateSampleTickMs_.load(std::memory_order_relaxed),
                                                  GetTickCount64(), 1000)
                 : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredMin250Fps() const {
#if HAS_WGC
    return impl_ ? ce::rate_window::AgeCachedRate(impl_->deliveredMin250Fps_.load(std::memory_order_relaxed),
                                                  impl_->lastDeliveredRateSampleTickMs_.load(std::memory_order_relaxed),
                                                  GetTickCount64(), 250)
                 : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDeliveredMin500Fps() const {
#if HAS_WGC
    return impl_ ? ce::rate_window::AgeCachedRate(impl_->deliveredMin500Fps_.load(std::memory_order_relaxed),
                                                  impl_->lastDeliveredRateSampleTickMs_.load(std::memory_order_relaxed),
                                                  GetTickCount64(), 500)
                 : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputMin250Fps() const {
#if HAS_WGC
    return impl_ ? ce::rate_window::AgeCachedRate(impl_->inputMin250Fps_.load(std::memory_order_relaxed),
                                                  impl_->lastInputRateSampleTickMs_.load(std::memory_order_relaxed),
                                                  GetTickCount64(), 250)
                 : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetInputMin500Fps() const {
#if HAS_WGC
    return impl_ ? ce::rate_window::AgeCachedRate(impl_->inputMin500Fps_.load(std::memory_order_relaxed),
                                                  impl_->lastInputRateSampleTickMs_.load(std::memory_order_relaxed),
                                                  GetTickCount64(), 500)
                 : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPacingSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->pacingSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetThrottleSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->throttleSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleDuplicateTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleDuplicateTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetStaleOutOfOrderTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->staleOutOfOrderTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetCursorOnlySkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->cursorOnlySkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolDropCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolDropCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetNormalizedDuplicateTimestampCount() const {
#if HAS_WGC
    return impl_ ? impl_->normalizedDuplicateTimestampCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetDuplicateTimestampSkipCount() const {
#if HAS_WGC
    return impl_ ? impl_->duplicateTimestampSkipCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetKeyedMutexAcquireFailCount() const {
#if HAS_WGC
    return impl_ ? impl_->keyedMutexAcquireFailCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetKeyedMutexReleaseFailCount() const {
#if HAS_WGC
    return impl_ ? impl_->keyedMutexReleaseFailCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetKeyedMutexAbandonedReclaimCount() const {
#if HAS_WGC
    return impl_ ? impl_->keyedMutexAbandonedReclaimCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSplitDeviceFlushCount() const {
#if HAS_WGC
    return impl_ ? impl_->splitDeviceFlushCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSplitDeviceFlushSkippedCount() const {
#if HAS_WGC
    return impl_ ? impl_->splitDeviceFlushSkippedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotFastRewriteCount() const {

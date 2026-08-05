#include "wgc_capture_internal.h"


#if HAS_WGC

void WGCCapture::Impl::OnDuplicationFrame(ID3D11Texture2D* texture,  const D3D11_TEXTURE2D_DESC& desc,  int64_t rawSourceQpc) {


        LARGE_INTEGER callbackStart = {};
        QueryPerformanceCounter(&callbackStart);
        const int64_t previousCallbackStart =
            lastCallbackStartQpc_.exchange(callbackStart.QuadPart, std::memory_order_relaxed);
        if (previousCallbackStart > 0 && callbackStart.QuadPart > previousCallbackStart && qpcFreq_ > 0) {
            const int64_t gapUs = ((callbackStart.QuadPart - previousCallbackStart) * 1000000) / qpcFreq_;
            UpdateSmoothedAtomicUs(callbackGapAvgUs_, gapUs);
            UpdateAtomicMax(callbackGapMaxUs_, gapUs);
        }

        auto recordCallbackProcess = [&]() {
            LARGE_INTEGER callbackEnd = {};
            QueryPerformanceCounter(&callbackEnd);
            if (callbackEnd.QuadPart > callbackStart.QuadPart && qpcFreq_ > 0) {
                const int64_t processUs = ((callbackEnd.QuadPart - callbackStart.QuadPart) * 1000000) / qpcFreq_;
                UpdateSmoothedAtomicUs(callbackProcessAvgUs_, processUs);
                UpdateAtomicMax(callbackProcessMaxUs_, processUs);
            }
            UpdateAtomicMax(callbackDrainMaxCount_, 1u);
        };

        if (!alive_.load(std::memory_order_acquire) || NeedsReset()) {
            recordCallbackProcess();
            return;
        }

        bool processed = false;
        if (!frameCallback_.load(std::memory_order_acquire)) {
            // Pull mode: enqueue into the internal bounded queue like WGC.
            WGCCapturedFrame frame{};
            {
                std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
                const SourceFramePreflight pre = PreflightSourceFrame(rawSourceQpc);
                if (pre.accepted) {
                    processed = DeliverSourceTexture(texture, desc, pre, &frame) && frame.texture;
                }
            }
            if (processed) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                EnqueueFrameInternal(std::move(frame));
            }
        } else {
            std::lock_guard<std::mutex> drainLock(callbackDrainMutex_);
            if (!alive_.load(std::memory_order_acquire)) {
                recordCallbackProcess();
                return;
            }
            std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
            const SourceFramePreflight pre = PreflightSourceFrame(rawSourceQpc);
            if (pre.accepted) {
                processed = DeliverSourceTexture(texture, desc, pre, nullptr);
            }
        }

        if (processed && frameArrivedEvent_) {
            SetEvent(frameArrivedEvent_);
        }
        recordCallbackProcess();

}

#endif

#if HAS_WGC

void WGCCapture::Impl::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender,  winrt::IInspectable const&) {


        EnsureWgcCallbackThreadQoS();

        LARGE_INTEGER callbackStart = {};
        QueryPerformanceCounter(&callbackStart);
        const int64_t previousCallbackStart =
            lastCallbackStartQpc_.exchange(callbackStart.QuadPart, std::memory_order_relaxed);
        if (previousCallbackStart > 0 && callbackStart.QuadPart > previousCallbackStart && qpcFreq_ > 0) {
            const int64_t gapUs = ((callbackStart.QuadPart - previousCallbackStart) * 1000000) / qpcFreq_;
            UpdateSmoothedAtomicUs(callbackGapAvgUs_, gapUs);
            UpdateAtomicMax(callbackGapMaxUs_, gapUs);
        }

        auto recordCallbackProcess = [&](uint32_t drainedCount) {
            LARGE_INTEGER callbackEnd = {};
            QueryPerformanceCounter(&callbackEnd);
            if (callbackEnd.QuadPart > callbackStart.QuadPart && qpcFreq_ > 0) {
                const int64_t processUs = ((callbackEnd.QuadPart - callbackStart.QuadPart) * 1000000) / qpcFreq_;
                UpdateSmoothedAtomicUs(callbackProcessAvgUs_, processUs);
                UpdateAtomicMax(callbackProcessMaxUs_, processUs);
            }
            UpdateAtomicMax(callbackDrainMaxCount_, drainedCount);
        };

        // Check if Impl is still alive
        if (!alive_.load(std::memory_order_acquire)) {
            recordCallbackProcess(0);
            return;
        }

        if (NeedsReset()) {
            uint32_t drainedCount = 0;
            while (alive_.load(std::memory_order_acquire)) {
                auto winrtFrame = sender.TryGetNextFrame();
                if (!winrtFrame) {
                    break;
                }
                ++drainedCount;
                winrtFrame.Close();
            }
            recordCallbackProcess(drainedCount);
            return;
        }

        // Pull mode: drain promptly into an internal bounded queue so the
        // encoder thread only performs CFR scheduling. This preserves recent
        // temporal history across callback bursts and avoids coupling source
        // collection to encoder wakeups.
        if (!frameCallback_.load(std::memory_order_acquire)) {
            bool processedFrame = false;
            uint32_t drainedCount = 0;
            std::vector<WGCCapturedFrame> drainedFrames;
            drainedFrames.reserve(4);
            {
                std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
                while (alive_.load(std::memory_order_acquire)) {
                    auto winrtFrame = sender.TryGetNextFrame();
                    if (!winrtFrame) {
                        break;
                    }
                    ++drainedCount;

                    WGCCapturedFrame frame{};
                    if (ProcessCapturedFrame(winrtFrame, &frame) && frame.texture) {
                        drainedFrames.push_back(std::move(frame));
                        processedFrame = true;
                    }
                }
            }

            if (!drainedFrames.empty()) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                for (auto& frame : drainedFrames) {
                    EnqueueFrameInternal(std::move(frame));
                }
            }

            if (processedFrame && frameArrivedEvent_) {
                SetEvent(frameArrivedEvent_);
            }
            recordCallbackProcess(drainedCount);
            return;
        }

        std::lock_guard<std::mutex> lock(callbackDrainMutex_);
        if (!alive_.load(std::memory_order_acquire)) {
            recordCallbackProcess(0);
            return;
        }

        bool processedFrame = false;
        uint32_t drainedCount = 0;
        {
            std::lock_guard<std::mutex> processLock(frameProcessingMutex_);
            while (alive_.load(std::memory_order_acquire)) {
                auto winrtFrame = sender.TryGetNextFrame();
                if (!winrtFrame) {
                    break;
                }
                ++drainedCount;
                processedFrame = ProcessCapturedFrame(winrtFrame, nullptr) || processedFrame;
            }
        }

        if (processedFrame && frameArrivedEvent_) {
            SetEvent(frameArrivedEvent_);
        }
        recordCallbackProcess(drainedCount);

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::CreateWinRTDevice() {


        // Get DXGI device from D3D11 device
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr))
            return false;

        // Create WinRT device interop
        winrt::com_ptr<IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put());
        dxgiDevice->Release();

        if (FAILED(hr))
            return false;

        winrtDevice_ = inspectable.as<winrt::IDirect3DDevice>();
        return winrtDevice_ != nullptr;

}

#endif

#if HAS_WGC

void WGCCapture::Impl::UnsubscribeItemClosed() noexcept {


        itemCallbackState_->StopAndDrain();
        if (!item_) {
            itemClosedToken_ = {};
            return;
        }
        try {
            item_.Closed(itemClosedToken_);
        } catch (const winrt::hresult_error& e) {
            LogWarn("[WGC] Failed to unsubscribe capture-item Closed handler: 0x%08lX",
                    static_cast<unsigned long>(e.code().value));
        } catch (...) {
            LogWarn("[WGC] Failed to unsubscribe capture-item Closed handler");
        }
        itemClosedToken_ = {};

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::SubscribeItemClosed(const char* targetName,  const char* resetReason) {


        auto callbackState = itemCallbackState_;
        const uint64_t callbackEpoch = itemCallbackState_->Begin(this);
        try {
            itemClosedToken_ = item_.Closed(
                [callbackState = std::move(callbackState), callbackEpoch, targetName, resetReason](auto&&, auto&&) {
                    auto owner = callbackState->Enter(callbackEpoch);
                    if (!owner) {
                        return;
                    }
                    LogWarn("[WGC] %s capture item closed by OS", targetName);
                    owner->FlagResetNeeded(resetReason);
                });
            return true;
        } catch (const winrt::hresult_error& e) {
            LogError("[WGC] Failed to subscribe %s capture-item Closed handler: 0x%08lX", targetName,
                     static_cast<unsigned long>(e.code().value));
        } catch (...) {
            LogError("[WGC] Failed to subscribe %s capture-item Closed handler", targetName);
        }
        itemCallbackState_->StopAndDrain();
        itemClosedToken_ = {};
        return false;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::CreateForMonitor(HMONITOR hmon) {


        winrt::com_ptr<IGraphicsCaptureItemInterop> interopFactory;
        winrt::hstring className = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HRESULT factoryHr = RoGetActivationFactory(reinterpret_cast<HSTRING>(winrt::get_abi(className)),
                                                   IID_IGraphicsCaptureItemInterop, interopFactory.put_void());
        if (FAILED(factoryHr) || !interopFactory) {
            LogError("[WGC] RoGetActivationFactory(CreateForMonitor) failed: 0x%lx", factoryHr);
            return false;
        }

        winrt::GraphicsCaptureItem item{nullptr};
        HRESULT hr =
            interopFactory->CreateForMonitor(hmon, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item));

        if (FAILED(hr) || !item) {
            LogError("[WGC] CreateForMonitor failed: 0x%lx", hr);
            return false;
        }

        UnsubscribeItemClosed();
        item_ = item;
        if (!SubscribeItemClosed("Monitor", "capture item closed")) {
            item_ = nullptr;
            return false;
        }
        targetMonitor_ = hmon;
        targetWindow_ = nullptr;
        useDuplicationBackend_ = false;
        itemCreationMethod_ = WgcItemCreationMethod::kInteropMonitor;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=monitor method=%s hmon=0x%p size=%dx%d",
                WgcItemCreationMethodName(itemCreationMethod_), hmon, item_.Size().Width, item_.Size().Height);
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::CreateForWindow(HWND hwnd) {


        if (GetWgcItemSourcePreference() != WgcItemSourcePreference::kInteropOnly) {
            winrt::GraphicsCaptureItem item{nullptr};
            uint64_t windowIdValue = 0;
            HRESULT helperHr = E_FAIL;
            HRESULT createHr = E_FAIL;
            if (TryCreateCaptureItemFromWindowId(hwnd, item, windowIdValue, helperHr, createHr)) {
                UnsubscribeItemClosed();
                item_ = item;
                if (!SubscribeItemClosed("Window", "window capture item closed")) {
                    item_ = nullptr;
                    return false;
                }
                targetWindow_ = hwnd;
                targetMonitor_ = nullptr;
                useDuplicationBackend_ = false;
                itemCreationMethod_ = WgcItemCreationMethod::kWindowId;
                itemCreationIdValue_ = windowIdValue;
                LogInfo("[WGC] Capture item created: target=window method=%s hwnd=0x%p windowId=0x%llx size=%dx%d",
                        WgcItemCreationMethodName(itemCreationMethod_), hwnd,
                        static_cast<unsigned long long>(itemCreationIdValue_), item_.Size().Width, item_.Size().Height);
                return true;
            }

            LogInfo(
                "[WGC] WindowId capture item creation unavailable/failed for hwnd=0x%p "
                "(helper=0x%08lX create=0x%08lX id=0x%llx); falling back to interop",
                hwnd, static_cast<unsigned long>(helperHr), static_cast<unsigned long>(createHr),
                static_cast<unsigned long long>(windowIdValue));
        } else {
            LogInfo("[WGC] WindowId capture item creation skipped by CE_WGC_ITEM_SOURCE=interop");
        }

        winrt::com_ptr<IGraphicsCaptureItemInterop> interopFactory;
        winrt::hstring className = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        HRESULT factoryHr = RoGetActivationFactory(reinterpret_cast<HSTRING>(winrt::get_abi(className)),
                                                   IID_IGraphicsCaptureItemInterop, interopFactory.put_void());
        if (FAILED(factoryHr) || !interopFactory) {
            LogError("[WGC] RoGetActivationFactory(CreateForWindow) failed: 0x%lx", factoryHr);
            return false;
        }

        winrt::GraphicsCaptureItem item{nullptr};
        HRESULT hr =
            interopFactory->CreateForWindow(hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item));

        if (FAILED(hr) || !item) {
            LogError("[WGC] CreateForWindow failed: 0x%lx", hr);
            return false;
        }

        UnsubscribeItemClosed();
        item_ = item;
        if (!SubscribeItemClosed("Window", "window capture item closed")) {
            item_ = nullptr;
            return false;
        }
        targetWindow_ = hwnd;
        targetMonitor_ = nullptr;
        useDuplicationBackend_ = false;
        itemCreationMethod_ = WgcItemCreationMethod::kInteropWindow;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=window method=%s hwnd=0x%p size=%dx%d",
                WgcItemCreationMethodName(itemCreationMethod_), hwnd, item_.Size().Width, item_.Size().Height);
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::CreateForMonitorDuplication(HMONITOR hmon) {


        if (!d3dDevice_) {
            LogError("[WGC] Duplication target rejected: no capture D3D11 device");
            return false;
        }
        if (!hmon) {
            hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        }

        IDXGIDevice* dxgiDevice = nullptr;

#endif
#if HAS_WGC
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);
        if (FAILED(hr) || !adapter) {
            LogWarn("[WGC] Duplication target rejected: cannot resolve capture adapter (0x%08lX)",
                    static_cast<unsigned long>(hr));
            return false;
        }

        bool outputFound = false;
        bool rotationOk = true;
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
        for (UINT i = 0;; ++i) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                continue;
            }
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmon) {
                outputFound = true;
                rotation = desc.Rotation;
                rotationOk =
                    desc.Rotation == DXGI_MODE_ROTATION_IDENTITY || desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED;
                SafeRelease(output);
                break;
            }
            SafeRelease(output);
        }
        SafeRelease(adapter);

        if (!outputFound) {
            LogWarn("[WGC] Duplication target rejected: monitor 0x%p not on capture adapter (cross-adapter output)",
                    hmon);
            return false;
        }
        if (!rotationOk) {
            LogWarn("[WGC] Duplication target rejected: monitor 0x%p output rotation=%d unsupported by duplication",
                    hmon, static_cast<int>(rotation));
            return false;
        }

        UnsubscribeItemClosed();
        item_ = nullptr;
        targetMonitor_ = hmon;
        targetWindow_ = nullptr;
        useDuplicationBackend_ = true;
        dupInitFailureReason_.clear();
        itemCreationMethod_ = WgcItemCreationMethod::kDxgiDuplication;
        itemCreationIdValue_ = 0;
        LogInfo("[WGC] Capture item created: target=monitor method=%s hmon=0x%p (duplication deferred to start)",
                WgcItemCreationMethodName(itemCreationMethod_), hmon);
        return true;

}

#endif

#if HAS_WGC

bool WGCCapture::Impl::StartDuplicationCapture(uint32_t& width,  uint32_t& height) {


        allocationLimitedPoolSlots_ = 0;
        allocationLimitedSourceBuffers_ = 0;
        allocationLimitSourceFormat_ = DXGI_FORMAT_UNKNOWN;
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFreq_ = freq.QuadPart;
        ApplyFrameThrottleInterval();
        ApplyProducerInterval();
        if (targetFps_ > 0 && targetIntervalQPC_ > 0) {
            LogInfo("[WGC] Frame throttle active at %u fps (interval=%lld QPC ticks)", targetFps_,
                    (long long)targetIntervalQPC_);
        }
        if (producerTargetFps_ > 0) {
            LogInfo(
                "[WGC] Producer cadence target %u fps requested; DXGI duplication has no producer cadence "
                "control (frames arrive at desktop update rate, surplus is decimated by ingress policy)",
                producerTargetFps_);
        }

        // Probe the monitor for HDR/bit-depth so the format policy, retained
        // pool conversion, and encoder 10-bit resolution behave exactly like
        // the WGC backend.
        UpdateCaptureFormatSelection();

        dupSource_ = std::make_unique<DxgiDuplicationSource>();
        std::string failureReason;
        if (!dupSource_->Init(d3dDevice_, targetMonitor_, requireHighPrecisionCapture_, captureIsHDR_,
                              &failureReason)) {
            dupInitFailureReason_ = failureReason;
            dupSource_.reset();
            return false;
        }

        width = dupSource_->GetWidth();
        height = dupSource_->GetHeight();
        frameWidth_ = width;
        frameHeight_ = height;
        targetMonitor_ = dupSource_->GetMonitor();
        // Seed the pool budget from the output-derived selection before the
        // first callback can allocate it; replace this hint with the proven
        // first-texture format immediately after Start returns.
        UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
        const ULONGLONG prewarmStart = GetTickCount64();
        EnsureGpuTimingQueries();
        if (!EnsurePoolCopyShader() || !EnsureTexturePool(width, height, captureDxgiFormat_)) {
            dupInitFailureReason_ = "duplication retained-pool prewarm failed";
            dupSource_.reset();
            return false;
        }
        LogInfo("[WGC] Duplication prewarm complete: duration=%llums hintFormat=%s slots=%zu estimatedPool=%lluMB",
                static_cast<unsigned long long>(GetTickCount64() - prewarmStart), DxgiFormatName(captureDxgiFormat_),
                texturePool_.size(),
                static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                                (1024ull * 1024ull)));
        if (!frameArrivedEvent_) {
            frameArrivedEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset
        }

        // The duplication monitor is immutable for this source lifetime. Cache
        // its origin instead of issuing monitor queries for every mouse report.
        int32_t originLeft = 0;
        int32_t originTop = 0;
        const bool originOk = GetCaptureOrigin(originLeft, originTop);
        DxgiDuplicationFrameSink sink;
        sink.onFrame = [this](ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc,
                              uint32_t /*accumulatedFrames*/) { OnDuplicationFrame(texture, desc, rawSourceQpc); };
        sink.onPointerUpdate = [this, originLeft, originTop](const ce::cursor::SourcePointerObservation& observation,
                                                            bool cursorOnly) {
            if (cursorOnly) {
                cursorOnlySkipCount_.fetch_add(1, std::memory_order_relaxed);
            }
            const auto callback = cursorCallback_.load(std::memory_order_acquire);
            if (!callback) {
                return;
            }
            callback(observation, originLeft, originTop, frameWidth_, frameHeight_,
                     sourceEpoch_.load(std::memory_order_acquire));
        };
        sink.onResetNeeded = [this](const char* reason) { FlagResetNeeded(reason); };
        if (!dupSource_->Start(std::move(sink))) {
            dupInitFailureReason_ = dupSource_->GetStartFailureReason();
            if (dupInitFailureReason_.empty()) {
                dupInitFailureReason_ = "duplication capture thread start failed";
            }
            dupSource_.reset();
            return false;
        }

        // Init's ModeDesc is only a hint. Start waits for the first acquired
        // texture, so this is the proven surface format used by pool sizing,
        // retained-copy selection, and high-precision telemetry.
        captureDxgiFormat_ = dupSource_->GetFormat();
        useHighPrecisionCapture_ =
            ce::capture_policy::GetDxgiDuplicationSourceContentBits(static_cast<uint32_t>(captureDxgiFormat_)) >= 10;
        UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);

        LogInfo(
            "[WGC] Capture session diagnostics: target=monitor method=%s hwnd=0x0 hmon=0x%p itemId=0x0 "
            "framePool=DxgiDuplication sourceBuffers=0 borderless=1 nativeCursorRequested=0 "
            "producerTargetFps=%u localThrottleFps=%u format=%s",
            WgcItemCreationMethodName(itemCreationMethod_), targetMonitor_, producerTargetFps_, targetFps_,
            DescribeCaptureFormat());

        LogInfo("[WGC] Capture origin diagnostics: target=monitor originMode=%s origin=(%d,%d) itemSize=%ux%u",
                originOk ? "monitor" : "unresolved", originLeft, originTop, frameWidth_, frameHeight_);

        LogInfo("[WGC] Capture session started (DXGI duplication): %dx%d", width, height);
        return true;

}

#endif

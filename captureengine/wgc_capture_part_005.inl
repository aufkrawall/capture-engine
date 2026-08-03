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

    // Shared source-agnostic frame admission (timestamp ordering, duplicate
    // normalization, pacing, external throttle, staleness). Used by both the
    // WGC frame-pool path and the DXGI duplication path so every skip counter
    // and cadence policy behaves identically for both backends.
    struct SourceFramePreflight {
        int64_t rawSourceFrameQpc = 0;
        int64_t sourceFrameQpc = 0;
        bool duplicateSourceTimestamp = false;
        bool accepted = false;
    };

    SourceFramePreflight PreflightSourceFrame(int64_t rawSourceFrameQpc) {
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

    // Shared pool copy + delivery for an admitted source texture. The texture
    // only needs to stay valid for the duration of this call (the GPU copy is
    // submitted synchronously), which is exactly the DXGI duplication
    // Acquire/Release contract as well as the WinRT surface lifetime.
    bool DeliverSourceTexture(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc,
                              const SourceFramePreflight& pre, WGCCapturedFrame* outputFrame) {
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

    bool ProcessCapturedFrame(const winrt::Direct3D11CaptureFrame& winrtFrame, WGCCapturedFrame* outputFrame) {
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

    // DXGI duplication sink: mirrors OnFrameArrived's locking, instrumentation,
    // and pull/callback dual-mode dispatch for the duplication capture thread.
    void OnDuplicationFrame(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc, int64_t rawSourceQpc) {
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

    void OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&) {
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

    bool CreateWinRTDevice() {
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

    void UnsubscribeItemClosed() noexcept {
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

    bool SubscribeItemClosed(const char* targetName, const char* resetReason) {
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

    bool CreateForMonitor(HMONITOR hmon) {
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

    bool CreateForWindow(HWND hwnd) {
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

    // Prepare a DXGI Desktop Duplication monitor target. Only light
    // availability validation happens here (adapter/output match, rotation);
    // the duplication object itself is created at StartCapture so an idle
    // primed target does not keep system-wide desktop duplication active
    // (which would suppress MPO/DirectFlip between recordings).
    bool CreateForMonitorDuplication(HMONITOR hmon) {
        if (!d3dDevice_) {
            LogError("[WGC] Duplication target rejected: no capture D3D11 device");
            return false;
        }
        if (!hmon) {
            hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        }

        IDXGIDevice* dxgiDevice = nullptr;

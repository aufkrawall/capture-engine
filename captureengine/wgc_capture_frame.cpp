#include "wgc_capture_internal.h"


#if HAS_WGC

void WGCCapture::Impl::ReleaseCapturedFrame(WGCCapturedFrame& frame) {

        SafeRelease(frame.texture);
        frame.poolLease.Reset();
        frame.poolSlot = std::numeric_limits<uint32_t>::max();
        frame.poolGeneration = 0;

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


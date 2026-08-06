#include "wgc_capture_internal.h"


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

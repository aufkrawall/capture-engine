#include "wgc_capture_internal.h"

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

#if HAS_WGC
    return impl_ ? impl_->poolSlotFastRewriteCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetLastPoolSlotRewriteUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastPoolSlotRewriteUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotFreeCurrentCount() const {
#if HAS_WGC
    if (!impl_ || !impl_->poolLeaseState_) {
        return 0;
    }
    const uint32_t slotCount = impl_->poolLeaseState_->slotCount;
    const uint32_t leased = impl_->poolLeaseState_->leasedCurrent.load(std::memory_order_relaxed);
    return slotCount > leased ? (slotCount - leased) : 0u;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotLeasedMaxCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_) ? impl_->poolLeaseState_->leasedMax.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotFreeMinCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_) ? impl_->poolLeaseState_->freeMin.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSlotOverwritePreventedCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolSlotOverwritePreventedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolSaturatedDropCount() const {
#if HAS_WGC
    return impl_ ? impl_->poolSaturatedDropCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetPoolLeaseMismatchCount() const {
#if HAS_WGC
    return (impl_ && impl_->poolLeaseState_)
               ? impl_->poolLeaseState_->releaseMismatchCount.load(std::memory_order_relaxed)
               : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackGapAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackGapAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackGapMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackGapMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackProcessAvgUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackProcessAvgUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

int64_t WGCCapture::GetCallbackProcessMaxUs() const {
#if HAS_WGC
    return impl_ ? impl_->callbackProcessMaxUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetCallbackDrainMaxCount() const {
#if HAS_WGC
    return impl_ ? impl_->callbackDrainMaxCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

bool WGCCapture::IsUsingDedicatedCaptureDevice() const {
#if HAS_WGC
    return impl_ ? impl_->usingDedicatedCaptureDevice_ : false;
#else
    return false;
#endif
}

uint32_t WGCCapture::GetTexturePoolSlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->texturePoolSlotCount_ : ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSourceFramePoolBufferCount() const {
#if HAS_WGC
    return impl_ ? impl_->sourceFramePoolBufferCount_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessBudgetSurfaceCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessBudgetSurfaceCount_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSyncFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSyncDelayFrames_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSafetySlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSafetySlots_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessRetainedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessRetainedFrames_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessRetainedFrameCap() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessRetainedFrameCap_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessReservedFreeSlotCount() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessReservedFreeSlots_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessSourceEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSourceEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessCopyEstimatedVramBytes() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessCopyEstimatedVramBytes_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessSourceBytesPerSurface() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessSourceBytesPerSurface_ : 0;
#else
    return 0;
#endif
}

uint64_t WGCCapture::GetSmoothnessCopyBytesPerSurface() const {
#if HAS_WGC
    return impl_ ? impl_->smoothnessCopyBytesPerSurface_ : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessSourceDxgiFormat() const {
#if HAS_WGC
    return impl_ ? static_cast<uint32_t>(impl_->smoothnessSourceFormat_) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetSmoothnessCopyDxgiFormat() const {
#if HAS_WGC
    return impl_ ? static_cast<uint32_t>(impl_->smoothnessCopyFormat_) : 0;
#else
    return 0;
#endif
}

bool WGCCapture::IsCompactRetainedCopyActive() const {
#if HAS_WGC
    return impl_ ? impl_->compactRetainedCopyActive_ : false;
#else
    return false;
#endif
}

int64_t WGCCapture::GetLastPoolConvertTimeUs() const {
#if HAS_WGC
    return impl_ ? impl_->lastPoolConvertUs_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedLowWaterCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedLowWaterCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedRecoveryCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedRecoveryCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedSourceBelowCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedSourceBelowCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedHealthyCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedHealthyCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedUniformPlayoutSoftReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedUniformPlayoutSoftReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAcceptedUniformPlayoutCreditCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressAcceptedUniformPlayoutCreditCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedSoftReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedSoftReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedHardReserveCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedHardReserveCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressDecimatedCreditCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressDecimatedCreditCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressSoftReservePressureCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressSoftReservePressureCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressHardReservePressureCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressHardReservePressureCount_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressRetainedFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressRetainedFrames_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressRetainedFrameCap() const {
#if HAS_WGC
    return impl_ ? impl_->ingressRetainedFrameCap_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressLowWaterFrameCount() const {
#if HAS_WGC
    return impl_ ? impl_->ingressLowWaterFrames_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

uint32_t WGCCapture::GetIngressAdmissionReasonCode() const {
#if HAS_WGC
    return impl_ ? impl_->ingressLastReason_.load(std::memory_order_relaxed) : 0;
#else
    return 0;
#endif
}

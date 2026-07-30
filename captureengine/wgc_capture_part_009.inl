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

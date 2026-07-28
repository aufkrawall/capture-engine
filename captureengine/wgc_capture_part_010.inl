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

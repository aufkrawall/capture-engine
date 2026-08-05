#include "wgc_capture_internal.h"

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

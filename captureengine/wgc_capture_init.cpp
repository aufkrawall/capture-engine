#include "wgc_capture_internal.h"


#if HAS_WGC

WGCCapture::Impl::~Impl() {

        alive_.store(false, std::memory_order_release);
        StopCapture();
        UnsubscribeItemClosed();
        itemCallbackState_->DetachAndDrain();
        ReleaseTexturePool();
        SafeRelease(latestFrame_);
        SafeRelease(cachedTexture_);
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }
        if (frameArrivedEvent_) {
            CloseHandle(frameArrivedEvent_);
            frameArrivedEvent_ = NULL;
        }

}

#endif


#if HAS_WGC

const char* WGCCapture::Impl::DescribeCaptureFormat() const {

        switch (captureDxgiFormat_) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
            default:
                return "UNKNOWN";
        }

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::InitializeDevices(ID3D11Device* encoderDevice) {

        if (!encoderDevice) {
            return false;
        }

        ReleaseTexturePool();
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }

        encoderDevice_ = encoderDevice;
        usingDedicatedCaptureDevice_ = false;

        if (sameDeviceCapture_) {
            d3dDevice_ = encoderDevice_;
            d3dDevice_->GetImmediateContext(&d3dContext_);
            if (!d3dContext_) {
                LogError("[WGC] Failed to acquire same-device D3D11 immediate context");
                d3dDevice_ = nullptr;
                return false;
            }
            EnableMultithreadProtection(d3dDevice_, "same-device capture");
            ApplyConfiguredGpuPriority("same-device-capture");
            LogInfo("[WGC] Same-device capture enabled; reusing encoder D3D11 device");
            return true;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = encoderDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        SafeRelease(dxgiDevice);

        if (SUCCEEDED(hr) && adapter) {
            DXGI_ADAPTER_DESC adapterDesc = {};
            adapter->GetDesc(&adapterDesc);

            D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                   0, D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, &d3dContext_);
            SafeRelease(adapter);

            if (SUCCEEDED(hr) && d3dDevice_ && d3dContext_) {
                usingDedicatedCaptureDevice_ = (d3dDevice_ != encoderDevice_);
                EnableMultithreadProtection(d3dDevice_, "capture");
                ApplyConfiguredGpuPriority("dedicated-capture");
                LogInfo("[WGC] Dedicated capture D3D11 device created (FL=0x%x, adapter=%ls)", featureLevel,
                        adapterDesc.Description);
                return true;
            }

            SafeRelease(d3dContext_);
            SafeRelease(d3dDevice_);
            LogWarn("[WGC] Dedicated capture device creation failed (0x%08lX); falling back to shared device",
                    (unsigned long)hr);
        } else {
            SafeRelease(adapter);
            LogWarn("[WGC] Failed to resolve encoder adapter for dedicated capture device; falling back");
        }

        d3dDevice_ = encoderDevice_;
        d3dDevice_->GetImmediateContext(&d3dContext_);
        if (!d3dContext_) {
            LogError("[WGC] Failed to acquire fallback shared D3D11 immediate context");
            d3dDevice_ = nullptr;
            return false;
        }
        EnableMultithreadProtection(d3dDevice_, "shared capture");
        ApplyConfiguredGpuPriority("shared-fallback-capture");
        return true;

}

#endif

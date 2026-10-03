#pragma once

// COM objects that forward every DXGI method to the real object they wrap: the base of the fake runtimes'
// proxy factories and swapchains (a real Streamline or FFX proxy is the same shape). Generated from the
// MinGW dxgi*.h declarations; a fake overrides only what its runtime changes.

#include <dxgi1_6.h>

#include <atomic>

namespace ce::flow::fake {

template <typename Interface>
class ForwardingObject : public Interface {
public:
    explicit ForwardingObject(Interface* real) : real_(real) { real_->AddRef(); }
    virtual ~ForwardingObject() { real_->Release(); }
    ForwardingObject(const ForwardingObject&) = delete;
    ForwardingObject& operator=(const ForwardingObject&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }
    Interface* Real() const { return real_; }

protected:
    Interface* real_;

private:
    std::atomic<ULONG> references_{1};
};

class ForwardingSwapChain : public ForwardingObject<IDXGISwapChain4> {
public:
    using ForwardingObject::ForwardingObject;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
            riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) ||
            riid == __uuidof(IDXGISwapChain4)) {
            *object = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
        return real_->SetPrivateData(guid, data_size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {
        return real_->SetPrivateDataInterface(guid, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
        return real_->GetPrivateData(guid, data_size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {
        return real_->GetParent(riid, parent);
    }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
        return real_->GetDevice(riid, device);
    }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override {
        return real_->Present(sync_interval, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer_idx, REFIID riid, void **surface) override {
        return real_->GetBuffer(buffer_idx, riid, surface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(WINBOOL fullscreen, IDXGIOutput *target) override {
        return real_->SetFullscreenState(fullscreen, target);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(WINBOOL *fullscreen, IDXGIOutput **target) override {
        return real_->GetFullscreenState(fullscreen, target);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override { return real_->GetDesc(desc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
            UINT flags) override {
        return real_->ResizeBuffers(buffer_count, width, height, format, flags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *target_mode_desc) override {
        return real_->ResizeTarget(target_mode_desc);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override {
        return real_->GetContainingOutput(output);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override {
        return real_->GetFrameStatistics(stats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *last_present_count) override {
        return real_->GetLastPresentCount(last_present_count);
    }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { return real_->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override {
        return real_->GetFullscreenDesc(pDesc);
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { return real_->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override {
        return real_->GetCoreWindow(refiid, ppUnk);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
            const DXGI_PRESENT_PARAMETERS *pPresentParameters) override {
        return real_->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }
    WINBOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return real_->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override {
        return real_->GetRestrictToOutput(ppRestrictToOutput);
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override {
        return real_->SetBackgroundColor(pColor);
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override {
        return real_->GetBackgroundColor(pColor);
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {
        return real_->SetRotation(Rotation);
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override {
        return real_->GetRotation(pRotation);
    }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override {
        return real_->SetSourceSize(width, height);
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override {
        return real_->GetSourceSize(width, height);
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
        return real_->SetMaximumFrameLatency(max_latency);
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
        return real_->GetMaximumFrameLatency(max_latency);
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        return real_->GetFrameLatencyWaitableObject();
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
        return real_->SetMatrixTransform(matrix);
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
        return real_->GetMatrixTransform(matrix);
    }
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return real_->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colour_space,
                                                     UINT *colour_space_support) override {
        return real_->CheckColorSpaceSupport(colour_space, colour_space_support);
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE colour_space) override {
        return real_->SetColorSpace1(colour_space);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
            UINT flags, const UINT *node_mask, IUnknown *const *present_queue) override {
        return real_->ResizeBuffers1(buffer_count, width, height, format, flags, node_mask, present_queue);
    }
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata) override {
        return real_->SetHDRMetaData(type, size, metadata);
    }
};

class ForwardingFactory : public ForwardingObject<IDXGIFactory7> {
public:
    using ForwardingObject::ForwardingObject;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
            riid == __uuidof(IDXGIFactory7)) {
            *object = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
        return real_->SetPrivateData(guid, data_size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {
        return real_->SetPrivateDataInterface(guid, object);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
        return real_->GetPrivateData(guid, data_size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {
        return real_->GetParent(riid, parent);
    }
    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT adapter_idx, IDXGIAdapter **adapter) override {
        return real_->EnumAdapters(adapter_idx, adapter);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override {
        return real_->MakeWindowAssociation(window, flags);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *window) override {
        return real_->GetWindowAssociation(window);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
            IDXGISwapChain **swapchain) override {
        return real_->CreateSwapChain(device, desc, swapchain);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE swrast, IDXGIAdapter **adapter) override {
        return real_->CreateSoftwareAdapter(swrast, adapter);
    }
    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override {
        return real_->EnumAdapters1(Adapter, ppAdapter);
    }
    WINBOOL STDMETHODCALLTYPE IsCurrent() override { return real_->IsCurrent(); }
    // IDXGIFactory2
    WINBOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return real_->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *pDevice, HWND hWnd,
            const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
            IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {
        return real_->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *pDevice, IUnknown *pWindow,
            const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
            IDXGISwapChain1 **ppSwapChain) override {
        return real_->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override {
        return real_->GetSharedResourceAdapterLuid(hResource, pLuid);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override {
        return real_->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override {
        return real_->RegisterStereoStatusEvent(hEvent, pdwCookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override {
        real_->UnregisterStereoStatus(dwCookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override {
        return real_->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override {
        return real_->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override {
        real_->UnregisterOcclusionStatus(dwCookie);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
            IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {
        return real_->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    }
    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return real_->GetCreationFlags(); }
    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid, void **adapter) override {
        return real_->EnumAdapterByLuid(luid, iid, adapter);
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid, void **adapter) override {
        return real_->EnumWarpAdapter(iid, adapter);
    }
    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void *support_data,
                                                  UINT support_data_size) override {
        return real_->CheckFeatureSupport(feature, support_data, support_data_size);
    }
    // IDXGIFactory6
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT adapter_idx, DXGI_GPU_PREFERENCE gpu_preference,
            REFIID iid, void **adapter) override {
        return real_->EnumAdapterByGpuPreference(adapter_idx, gpu_preference, iid, adapter);
    }
    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD *cookie) override {
        return real_->RegisterAdaptersChangedEvent(event, cookie);
    }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override {
        return real_->UnregisterAdaptersChangedEvent(cookie);
    }
};

}  // namespace ce::flow::fake

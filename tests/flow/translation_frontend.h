#pragma once

#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include <d3d11.h>
#include <functional>
#include <new>
#include <wrl/client.h>

namespace ce::flow::translation {
using Microsoft::WRL::ComPtr;

__attribute__((noinline)) inline HRESULT PresentFrontend(IDXGISwapChain1* frontend, bool present1) {
    DXGI_PRESENT_PARAMETERS parameters{};
    return present1 ? frontend->Present1(0, 0, &parameters) : frontend->Present(0, 0);
}

__attribute__((noinline)) inline ULONG ReleaseFrontend(IUnknown* frontend) {
    return frontend->Release();
}

template <bool InPlace>
class ApiFrontendStorage : public fake::ForwardingSwapChain {
public:
    ApiFrontendStorage(IDXGISwapChain4* native, IUnknown* apiDevice, bool forwardData = true)
        : ForwardingSwapChain(native), device_(apiDevice), forwardData_(forwardData) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) {
            auto after = std::move(afterFinalRelease);
            if constexpr (InPlace) this->~ApiFrontendStorage();
            else delete this;
            if (after) after();
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** out) override {
        ++deviceQueries;
        return device_ ? device_->QueryInterface(iid, out) : real_->GetDevice(iid, out);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* out) override {
        ++privateDataReads;
        return forwardData_ ? real_->GetPrivateData(guid, size, out) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        ++privateDataWrites;
        return real_->SetPrivateData(guid, size, data);
    }
    unsigned deviceQueries = 0;
    unsigned privateDataWrites = 0;
    unsigned privateDataReads = 0;
    std::function<void()> afterFinalRelease;
private:
    ComPtr<IUnknown> device_;
    bool forwardData_;
    std::atomic<ULONG> references_{1};
};
using ApiFrontend = ApiFrontendStorage<false>;
using PlacementApiFrontend = ApiFrontendStorage<true>;

inline ComPtr<ID3D11Device> CreateFrontendDevice() {
    const auto module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return {};
    const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module, "D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;
    if (create) create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                       D3D11_SDK_VERSION, &device, nullptr, nullptr);
    FreeLibrary(module);
    return device;
}
}  // namespace ce::flow::translation

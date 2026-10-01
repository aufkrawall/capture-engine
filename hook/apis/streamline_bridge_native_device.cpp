#include "streamline_bridge_native_device.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "../common/hook_common.h"
#include "streamline_bridge_device_cache.h"

namespace ce::streamline_bridge {

// Device-creation failures need the exact request in the log: a reset from an adapter-bound
// creation is not distinguishable from a reset for a null/default request otherwise.
void DescribeIid(REFIID iid, char (&text)[40]) {
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", iid.Data1,
                  iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                  iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

namespace {

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// A game may hand us an adapter owned by a short-lived proxy/factory generation. D3D12 only
// needs the same physical adapter, so resolve its LUID through DXGI and obtain a fresh,
// unowned adapter instance before calling Microsoft's API. This preserves multi-GPU intent
// without passing another module's object lifetime into D3D12.
IUnknown* ResolveEquivalentAdapter(IUnknown* adapter) {
    if (!adapter) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> requested;
    HRESULT hr = adapter->QueryInterface(IID_PPV_ARGS(&requested));
    if (FAILED(hr) || !requested) {
        static std::atomic<bool> qiLogged{false};
        if (!qiLogged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: D3D12 adapter lacks IDXGIAdapter (hr=0x%08X)",
                             static_cast<uint32_t>(hr));
        }
        return adapter;
    }

    DXGI_ADAPTER_DESC desc{};
    hr = requested->GetDesc(&desc);
    if (FAILED(hr)) {
        static std::atomic<bool> descLogged{false};
        if (!descLogged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: cannot read D3D12 adapter LUID (hr=0x%08X)",
                             static_cast<uint32_t>(hr));
        }
        return adapter;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        static std::atomic<bool> factoryLogged{false};
        if (!factoryLogged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: cannot create a fresh DXGI factory for device creation "
                             "(hr=0x%08X)",
                             static_cast<uint32_t>(hr));
        }
        return adapter;
    }

    Microsoft::WRL::ComPtr<IUnknown> equivalent;
    hr = factory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&equivalent));
    if (FAILED(hr) || !equivalent) {
        static std::atomic<bool> enumLogged{false};
        if (!enumLogged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant(
                "Streamline bridge: cannot resolve a fresh adapter for LUID %08lx:%08lx (hr=0x%08X)",
                static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                static_cast<unsigned long>(desc.AdapterLuid.LowPart), static_cast<uint32_t>(hr));
        }
        return adapter;
    }

    return equivalent.Detach();
}

}  // namespace

// Creates the device with Microsoft's entry point rather than the 2.x interposer's. The
// interposer returned the same reset for this request (`20260821_234606`). The later native
// attempt still failed while reusing the caller's adapter instance (`20260822_001759`), so we
// first normalize that input to a fresh DXGI adapter by LUID. A native device is the documented
// input to `slSetD3DDevice`.
HRESULT CallNativeD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                    void** ppDevice) {
    if (ppDevice) {
        *ppDevice = nullptr;
        // A second creation for the same adapter is not a second runtime device. Reuse the
        // healthy native device before entering D3D12: a failed redundant call can reset that
        // already-proven device, leaving nothing usable for a recovery attempted afterwards.
        if (TryReuseCreatedDevice(adapter, minimumFeatureLevel, riid, ppDevice)) {
            return S_OK;
        }
    }

    // A null-output request is a capability probe, not device creation. If this bridge already
    // created a device on the same adapter at an equal-or-higher feature level, answer it from
    // that proof instead of asking the driver for redundant validation.
    if (!ppDevice && riid == IID_ID3D12Device && HasRememberedDeviceSupport(adapter, minimumFeatureLevel)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: answered D3D12 capability probe from prior successful "
                             "creation (featureLevel=%u)",
                             static_cast<unsigned>(minimumFeatureLevel));
        }
        return S_OK;
    }

    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) {
        d3d12 = LoadLibraryW(L"d3d12.dll");
    }
    if (!d3d12) {
        static std::atomic<bool> loadLogged{false};
        if (!loadLogged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: cannot load d3d12.dll for native D3D12CreateDevice (error=%lu)",
                             GetLastError());
        }
        return DXGI_ERROR_UNSUPPORTED;
    }
    auto create = reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!create) {
        return DXGI_ERROR_UNSUPPORTED;
    }

    IUnknown* adapterForCreate = ResolveEquivalentAdapter(adapter);
    HRESULT hr = create(adapterForCreate, minimumFeatureLevel, riid, ppDevice);
    bool usedDefaultAdapter = false;

    // The LUID-matched request still returned DEVICE_RESET on Witcher 3's real-device pass
    // (`20260822_003051`) even though the same route had already created a device. A null retry
    // is a narrowly scoped diagnostic fallback for device-lost-class failures only: it asks
    // DXGI/D3D12 to select its default hardware adapter when the explicitly selected object is
    // rejected. If this succeeds on a multi-GPU system, the warning below makes the departure
    // from the game's requested adapter visible rather than silently changing GPU selection.
    const bool deviceLostClass = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
                                 hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    if (FAILED(hr) && deviceLostClass && adapterForCreate) {
        if (ppDevice) {
            *ppDevice = nullptr;
        }
        const HRESULT defaultHr = create(nullptr, minimumFeatureLevel, riid, ppDevice);
        HookLogImportant(
            "Streamline bridge: adapter-bound D3D12CreateDevice returned hr=0x%08X; null/default retry "
            "returned hr=0x%08X (adapter=%p resolved=%p featureLevel=%u ppDevice=%p)",
            static_cast<uint32_t>(hr), static_cast<uint32_t>(defaultHr), static_cast<void*>(adapter),
            static_cast<void*>(adapterForCreate), static_cast<unsigned>(minimumFeatureLevel),
            static_cast<void*>(ppDevice));
        if (SUCCEEDED(defaultHr)) {
            hr = defaultHr;
            usedDefaultAdapter = true;
        }
    }

    if (SUCCEEDED(hr)) {
        RememberCreatedDevice(usedDefaultAdapter ? nullptr : adapterForCreate, minimumFeatureLevel,
                              ppDevice && SUCCEEDED(hr) ? *ppDevice : nullptr);
    }

    if (adapterForCreate != adapter && adapterForCreate) {
        adapterForCreate->Release();
    }
    if (FAILED(hr)) {
        static std::atomic<uint32_t> loggedHr{0};
        const uint32_t encoded = static_cast<uint32_t>(hr);
        if (loggedHr.exchange(encoded, std::memory_order_relaxed) != encoded) {
            char iidText[40] = {};
            DescribeIid(riid, iidText);
            HookLogImportant(
                "Streamline bridge: native D3D12CreateDevice failed after fallback (hr=0x%08X adapter=%p "
                "resolved=%p "
                "featureLevel=%u riid=%s ppDevice=%p)",
                encoded, static_cast<void*>(adapter), static_cast<void*>(adapterForCreate),
                static_cast<unsigned>(minimumFeatureLevel), iidText, static_cast<void*>(ppDevice));
        }
    }
    return hr;
}

}  // namespace ce::streamline_bridge

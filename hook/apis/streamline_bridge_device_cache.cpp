#include "streamline_bridge_device_cache.h"

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <mutex>

#include "../common/hook_common.h"

namespace ce::streamline_bridge {
namespace {

struct RememberedDevice {
    D3D_FEATURE_LEVEL featureLevel;
    Microsoft::WRL::ComPtr<IUnknown> device;
};

std::mutex g_mutex;
std::map<uint64_t, RememberedDevice> g_devicesByAdapter;

uint64_t AdapterKey(IUnknown* adapter) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> descriptor;
    DXGI_ADAPTER_DESC desc{};
    if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&descriptor))) &&
        SUCCEEDED(descriptor->GetDesc(&desc))) {
        return (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32) |
               static_cast<uint32_t>(desc.AdapterLuid.LowPart);
    }
    return 0;
}

}  // namespace

bool HasRememberedDeviceSupport(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto entry = g_devicesByAdapter.find(AdapterKey(adapter));
    return entry != g_devicesByAdapter.end() &&
           entry->second.featureLevel >= static_cast<uint32_t>(featureLevel);
}

void RememberCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, void* device) {
    const uint64_t key = AdapterKey(adapter);
    if (!key || !device) {
        return;
    }
    Microsoft::WRL::ComPtr<IUnknown> retained(static_cast<IUnknown*>(device));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& entry = g_devicesByAdapter[key];
    if (static_cast<uint32_t>(featureLevel) >= static_cast<uint32_t>(entry.featureLevel)) {
        entry.featureLevel = featureLevel;
        entry.device = std::move(retained);
    }
}

bool TryReuseCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid,
                           void** ppDevice) {
    if (!ppDevice || riid == GUID_NULL) {
        return false;
    }
    *ppDevice = nullptr;
    Microsoft::WRL::ComPtr<IUnknown> cached;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto entry = g_devicesByAdapter.find(AdapterKey(adapter));
        if (entry == g_devicesByAdapter.end() ||
            entry->second.featureLevel < static_cast<uint32_t>(featureLevel)) {
            return false;
        }
        cached = entry->second.device;
    }
    if (!cached) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(cached.Get()->QueryInterface(IID_PPV_ARGS(&device))) ||
        FAILED(device->GetDeviceRemovedReason())) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devicesByAdapter.erase(AdapterKey(adapter));
        return false;
    }
    if (FAILED(cached.CopyTo(riid, ppDevice))) {
        return false;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: reused the prior successful D3D12 device for a repeated "
                         "creation request (featureLevel=%u)",
                         static_cast<unsigned>(featureLevel));
    }
    return true;
}

}  // namespace ce::streamline_bridge

#include "streamline_bridge_device_cache.h"

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>

#include "../common/hook_common.h"

namespace ce::streamline_bridge {
namespace {

struct RememberedDevice {
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_1_0_CORE;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
};

std::mutex g_mutex;
std::map<uint64_t, RememberedDevice> g_devicesByAdapter;
std::optional<uint64_t> g_defaultAdapterKey;

uint64_t LuidKey(LUID luid) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
           static_cast<uint32_t>(luid.LowPart);
}

std::optional<uint64_t> AdapterKey(IUnknown* adapter) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> descriptor;
    DXGI_ADAPTER_DESC desc{};
    if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&descriptor))) &&
        SUCCEEDED(descriptor->GetDesc(&desc))) {
        return LuidKey(desc.AdapterLuid);
    }
    return std::nullopt;
}

bool FindRememberedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel,
                          uint64_t* foundKey,
                          Microsoft::WRL::ComPtr<ID3D12Device>* foundDevice) {
    std::optional<uint64_t> key = AdapterKey(adapter);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!adapter) {
        key = g_defaultAdapterKey;
    }
    if (!key) {
        return false;
    }
    const auto entry = g_devicesByAdapter.find(*key);
    if (entry == g_devicesByAdapter.end() ||
        entry->second.featureLevel < static_cast<uint32_t>(featureLevel)) {
        return false;
    }
    *foundKey = *key;
    *foundDevice = entry->second.device;
    return foundDevice->Get() != nullptr;
}

void ForgetIfUnhealthy(uint64_t key, ID3D12Device* device, HRESULT reason) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto entry = g_devicesByAdapter.find(key);
        if (entry != g_devicesByAdapter.end() && entry->second.device.Get() == device) {
            g_devicesByAdapter.erase(entry);
            if (g_defaultAdapterKey == key) {
                g_defaultAdapterKey.reset();
            }
        }
    }
    static std::atomic<uint32_t> loggedReason{0};
    const uint32_t encoded = static_cast<uint32_t>(reason);
    if (loggedReason.exchange(encoded, std::memory_order_relaxed) != encoded) {
        HookLogImportant("Streamline bridge: discarded a retained D3D12 device after its removal reason "
                         "became hr=0x%08X",
                         encoded);
    }
}

bool DeviceIsHealthy(uint64_t key, const Microsoft::WRL::ComPtr<ID3D12Device>& device) {
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        return true;
    }
    ForgetIfUnhealthy(key, device.Get(), reason);
    return false;
}

}  // namespace

bool HasRememberedDeviceSupport(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel) {
    uint64_t key = 0;
    Microsoft::WRL::ComPtr<ID3D12Device> cached;
    return FindRememberedDevice(adapter, featureLevel, &key, &cached) &&
           DeviceIsHealthy(key, cached);
}

void RememberCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, void* device) {
    if (!device) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> retained;
    if (FAILED(static_cast<IUnknown*>(device)->QueryInterface(IID_PPV_ARGS(&retained)))) {
        return;
    }
    const uint64_t key = LuidKey(retained->GetAdapterLuid());
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& entry = g_devicesByAdapter[key];
    if (static_cast<uint32_t>(featureLevel) > static_cast<uint32_t>(entry.featureLevel)) {
        entry.featureLevel = featureLevel;
    }
    // Feature level is a minimum-support query, not immutable device configuration. Keep the
    // newest successfully created object in step with the device handed to Streamline while
    // preserving the strongest support proof observed for this physical adapter.
    entry.device = std::move(retained);
    if (!adapter) {
        g_defaultAdapterKey = key;
    }
}

bool TryReuseCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid,
                           void** ppDevice) {
    if (!ppDevice || riid == GUID_NULL) {
        return false;
    }
    *ppDevice = nullptr;
    uint64_t key = 0;
    Microsoft::WRL::ComPtr<ID3D12Device> cached;
    if (!FindRememberedDevice(adapter, featureLevel, &key, &cached) ||
        !DeviceIsHealthy(key, cached)) {
        return false;
    }
    const HRESULT queryHr = cached.CopyTo(riid, ppDevice);
    if (FAILED(queryHr)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: retained D3D12 device does not expose the requested "
                             "interface (hr=0x%08X iid.Data1=0x%08lX)",
                             static_cast<uint32_t>(queryHr), static_cast<unsigned long>(riid.Data1));
        }
        return false;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: reused the prior successful D3D12 device before a redundant "
                         "driver creation (featureLevel=%u iid.Data1=0x%08lX)",
                         static_cast<unsigned>(featureLevel),
                         static_cast<unsigned long>(riid.Data1));
    }
    return true;
}

}  // namespace ce::streamline_bridge

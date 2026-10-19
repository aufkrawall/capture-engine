#pragma once

// Which Direct3D API a DXGI swapchain belongs to, from the device interfaces it answers. Shared by
// DXGIShared::DetectAPIType (dxgi_shared.cpp) and DetectSwapChainAPITypeForDX11Hook
// (dx11_hook_helpers.cpp), both through ClassifySwapChainAPICached; the decision itself is
// DXGIShared::SelectPrimarySwapChainAPIType.

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

#include "common/logging/log_meter.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"

void HookLog(const char* fmt, ...);

namespace DXGIShared {

inline APIType ClassifySwapChainAPI(IDXGISwapChain* swapChain) {
    if (!swapChain) {
        return APIType::Unknown;
    }
    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;
    unsigned d3d11FeatureLevel = 0;

    ID3D12Device* d3d12Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12Device)) && d3d12Device) {
        d3d12Device->Release();
        hasD3D12Device = true;
    }
    // Always try all three: a D3D10 device answers ID3D11Device too (D3D10-on-D3D11), so stopping
    // at D3D11 misclassifies it; SelectPrimarySwapChainAPIType decides.
    ID3D11Device* d3d11Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11Device)) && d3d11Device) {
        hasD3D11Device = true;
    }
    ID3D10Device* d3d10Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10Device)) && d3d10Device) {
        d3d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d3d10Device1 = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d3d10Device1)) && d3d10Device1) {
            d3d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    // Feature level and module lookups only where both interfaces answer (native D3D10, DXVK D3D10
    // and D3D11): a native D3D11 present pays nothing more.
    const bool dual = !hasD3D12Device && hasD3D11Device && hasD3D10Device;
    if (dual) {
        d3d11FeatureLevel = static_cast<unsigned>(d3d11Device->GetFeatureLevel());
    }
    if (hasD3D11Device) {
        d3d11Device->Release();
    }
    const bool d3d10Runtime = dual && IsD3D10RuntimeModuleLoaded();
    const APIType api = SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device, d3d10Runtime,
                                                      d3d11FeatureLevel);
    if (dual) {
        static ce::log_meter::ChangeGate s_dualLog;
        if (const auto verdict = s_dualLog.Observe(
                ce::log_meter::FieldKey(swapChain, d3d10Runtime, d3d11FeatureLevel, static_cast<int>(api)))) {
            HookLog("[GraphicsAPI] swapchain=%p answers ID3D10Device and ID3D11Device: d3d10Runtime=%d "
                    "featureLevel=0x%X -> %s%s",
                    swapChain, d3d10Runtime ? 1 : 0, d3d11FeatureLevel, api == APIType::D3D10 ? "DX10" : "DX11",
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return api;
}

// {6C3269B1-6288-4469-AC5B-9795388E4252} - CE's verdict, kept on the swapchain itself
inline constexpr GUID kSwapChainApiTagGuid = {
    0x6c3269b1, 0x6288, 0x4469, {0xac, 0x5b, 0x97, 0x95, 0x38, 0x8e, 0x42, 0x52}};
inline constexpr uint32_t kSwapChainApiTagMagic = 0x43454150u;  // "CEAP"

// Plain bytes, no interface: the tag holds no reference on anything.
struct SwapChainApiTag {
    uint32_t magic = 0;
    uint32_t api = 0;
};

inline const char* SwapChainApiLogName(APIType api) {
    switch (api) {
        case APIType::D3D10:
            return "DX10";
        case APIType::D3D11:
            return "DX11";
        case APIType::D3D12:
            return "DX12";
        case APIType::Vulkan:
            return "Vulkan";
        default:
            return "unknown";
    }
}

// The verdict `swapChain` carries; Unknown when it carries none.
inline APIType ReadSwapChainApiTag(IDXGISwapChain* swapChain) {
    SwapChainApiTag tag;
    UINT size = sizeof(tag);
    if (!swapChain || FAILED(swapChain->GetPrivateData(kSwapChainApiTagGuid, &size, &tag)) || size != sizeof(tag) ||
        tag.magic != kSwapChainApiTagMagic) {
        return APIType::Unknown;
    }
    const auto api = static_cast<APIType>(tag.api);
    return api == APIType::D3D10 || api == APIType::D3D11 || api == APIType::D3D12 ? api : APIType::Unknown;
}

// Writes the verdict and reads it back; true only when the swapchain keeps it.
inline bool WriteSwapChainApiTag(IDXGISwapChain* swapChain, APIType api) {
    const SwapChainApiTag tag{kSwapChainApiTagMagic, static_cast<uint32_t>(api)};
    return swapChain && SUCCEEDED(swapChain->SetPrivateData(kSwapChainApiTagGuid, sizeof(tag), &tag)) &&
           ReadSwapChainApiTag(swapChain) == api;
}

// Whether the swapchain keeps a tag at all: writes an empty one, which reads as no verdict.
inline bool SwapChainKeepsApiTag(IDXGISwapChain* swapChain) {
    const SwapChainApiTag probe{kSwapChainApiTagMagic, 0};
    SwapChainApiTag back;
    UINT size = sizeof(back);
    return swapChain && SUCCEEDED(swapChain->SetPrivateData(kSwapChainApiTagGuid, sizeof(probe), &probe)) &&
           SUCCEEDED(swapChain->GetPrivateData(kSwapChainApiTagGuid, &size, &back)) && size == sizeof(back) &&
           back.magic == kSwapChainApiTagMagic;
}

namespace swapchain_api_cache_detail {
// Addresses only, never called through: the last chain this thread tagged, and the last one that
// would not keep a tag.
struct ThreadState {
    IDXGISwapChain* lastTagged = nullptr;
    APIType lastTaggedApi = APIType::Unknown;
    IDXGISwapChain* untaggable = nullptr;
    APIType untaggableApi = APIType::Unknown;
};
inline ThreadState& State() {
    thread_local ThreadState state;
    return state;
}
}  // namespace swapchain_api_cache_detail

inline void ResetSwapChainApiCacheForTesting() {
    swapchain_api_cache_detail::State() = {};
}

// The verdict without asking the swapchain's device again. Each classification is four GetDevice
// calls (three refused on a D3D11 swapchain), each a call through the swapchain's runtime, a
// translation layer's included; the D3D11 overlay asked on every Present (Mirror's Edge on DXUP,
// 20261018_151515). The verdict is kept on the swapchain (DXGI private data), so it dies with the
// object: a new swapchain the allocator places at a dead one's address carries none and is classified
// for itself. The address alone cannot tell them apart - CE tracks swapchains without a reference and
// cannot see one die (swapchain_liveness.h) - and an address-keyed cache handed the new chain the
// dead one's API. A swapchain that drops private data falls back to the thread's last address; that
// fallback re-probes on every call, and once the object there keeps the tag it is a different one.
inline APIType ClassifySwapChainAPICached(IDXGISwapChain* swapChain) {
    if (!swapChain)
        return APIType::Unknown;
    const APIType tagged = ReadSwapChainApiTag(swapChain);
    if (tagged != APIType::Unknown)
        return tagged;
    auto& state = swapchain_api_cache_detail::State();
    APIType previous = swapChain == state.lastTagged ? state.lastTaggedApi : APIType::Unknown;
    if (swapChain == state.untaggable && state.untaggableApi != APIType::Unknown) {
        if (!SwapChainKeepsApiTag(swapChain))
            return state.untaggableApi;  // still an object that drops private data
        previous = state.untaggableApi;  // it keeps the tag now: a different object at that address
    }
    const APIType api = ClassifySwapChainAPI(swapChain);
    if (previous != APIType::Unknown) {
        static ce::log_meter::ChangeGate s_reuseLog;
        if (const auto verdict = s_reuseLog.Observe(
                ce::log_meter::FieldKey(swapChain, static_cast<int>(previous), static_cast<int>(api)))) {
            HookLog("[GraphicsAPI] swapchain=%p is a new object at a classified chain's address: %s -> %s%s", swapChain,
                    SwapChainApiLogName(previous), SwapChainApiLogName(api),
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    if (api == APIType::Unknown)
        return api;  // nothing to keep; asked again next time, as before
    if (WriteSwapChainApiTag(swapChain, api)) {
        state.lastTagged = swapChain;
        state.lastTaggedApi = api;
        if (swapChain == state.untaggable) {
            state.untaggable = nullptr;
            state.untaggableApi = APIType::Unknown;
        }
        return api;
    }
    static ce::log_meter::ChangeGate s_untaggableLog;
    if (const auto verdict = s_untaggableLog.Observe(ce::log_meter::FieldKey(swapChain, static_cast<int>(api)))) {
        HookLog(
            "[GraphicsAPI] swapchain=%p keeps no private data: its %s verdict is cached by address on this "
            "thread%s",
            swapChain, SwapChainApiLogName(api), ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    state.untaggable = swapChain;
    state.untaggableApi = api;
    return api;
}

}  // namespace DXGIShared

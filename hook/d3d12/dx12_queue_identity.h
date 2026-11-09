#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <utility>

namespace ce::dx12_queue_identity {

// Native-interface queries implemented by the graphics interposers themselves.
inline constexpr GUID kStreamlineNative = {0xADEC44E2, 0x61F0, 0x45C3,
                                           {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
inline constexpr GUID kReShadeNative = {0x7F2C9A11, 0x3B4E, 0x4D6A,
                                         {0x81, 0x2F, 0x5E, 0x9C, 0xD3, 0x7A, 0x1B, 0x42}};

inline Microsoft::WRL::ComPtr<ID3D12CommandQueue> AcquireNativeQueue(ID3D12CommandQueue* queue) {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> current = queue;
    // Resolve stacked proxies at the binding boundary. The depth bound also covers a broken
    // provider that returns itself or cycles between proxies.
    for (unsigned depth = 0; current && depth < 4; ++depth) {
        Microsoft::WRL::ComPtr<IUnknown> underlying;
        if (FAILED(current->QueryInterface(kStreamlineNative, reinterpret_cast<void**>(underlying.GetAddressOf()))) ||
            !underlying) {
            if (FAILED(current->QueryInterface(kReShadeNative,
                                               reinterpret_cast<void**>(underlying.ReleaseAndGetAddressOf()))) ||
                !underlying) {
                return current;
            }
        }
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> next;
        if (FAILED(underlying.As(&next)) || !next || next.Get() == current.Get()) {
            return {};
        }
        current = std::move(next);
    }
    return {};
}

}  // namespace ce::dx12_queue_identity

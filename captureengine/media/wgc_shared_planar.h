#pragma once

// Retained NV12/P010/packed P010 reservoir slots shared between a split capture device
// (always used by DXGI duplication) and the encoder's device
// (llm-wiki/wgc-reservoir-vram-design.md). The slot is created on the
// encoder's device with SHARED_KEYEDMUTEX | SHARED_NTHANDLE, opened on the
// capture device through an NT handle, converted there and published at key 1.

#include <d3d11_4.h>

namespace ce::capture_policy {

// The device reports that it can create and open shared textures of this
// slot format (planar NV12/P010, or the packed P010 slot's R16_UINT). The slot creation and open stay authoritative: a failure
// there rebuilds the pool with the RGB reservoir.
inline bool DeviceSharesPlanarFormat(ID3D11Device* device, DXGI_FORMAT format) {
    if (!device) {
        return false;
    }
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support = {};
    support.InFormat = format;
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) ||
        (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) == 0) {
        return false;
    }
    if (format != DXGI_FORMAT_NV12) {
        return true;
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS4 options4 = {};
    return SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &options4, sizeof(options4))) &&
           options4.ExtendedNV12SharedTextureSupported;
}

}  // namespace ce::capture_policy

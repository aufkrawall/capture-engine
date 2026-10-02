#include "d3d9_capture_policy.h"

D3D9SharedFormatSelection SelectD3D9SharedCaptureFormat(D3DFORMAT sourceFormat) {
    switch (sourceFormat) {
        case D3DFMT_A8R8G8B8:
            return {D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, false};
        case D3DFMT_X8R8G8B8:
            // D3D9/D3D11 sharing requires an alpha-bearing 32-bit resource.
            // StretchRect performs the GPU-only X8 -> A8 copy.
            return {D3DFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, true};
        case D3DFMT_A2B10G10R10:
            return {D3DFMT_A2B10G10R10, DXGI_FORMAT_R10G10B10A2_UNORM, false};
        default:
            return {};
    }
}

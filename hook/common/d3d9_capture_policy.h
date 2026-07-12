#pragma once

#include <d3d9.h>
#include <dxgiformat.h>

struct D3D9SharedFormatSelection {
    D3DFORMAT resourceFormat = D3DFMT_UNKNOWN;
    DXGI_FORMAT transportFormat = DXGI_FORMAT_UNKNOWN;
    bool requiresConversion = false;

    explicit operator bool() const {
        return resourceFormat != D3DFMT_UNKNOWN && transportFormat != DXGI_FORMAT_UNKNOWN;
    }
};

// Classic D3D9 devices remain classic. Capture obtains shareable resources from
// an internal helper and opens them on the game device instead of changing the
// application's device type and resource/lost-device semantics.
constexpr bool ShouldPromoteClassicD3D9Device() {
    return false;
}

D3D9SharedFormatSelection SelectD3D9SharedCaptureFormat(D3DFORMAT sourceFormat);

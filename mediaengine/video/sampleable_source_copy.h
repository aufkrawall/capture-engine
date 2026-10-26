#pragma once

#include <d3d11.h>

#include "common/platform/raii_helpers.h"

// A CE-owned, shader-readable copy of a capture source that has no
// D3D11_BIND_SHADER_RESOURCE or is multisampled (resolved), so CE's
// conversion shaders can read it (shader_conversion_route.h). The texture is
// kept across frames and recreated only when the source's size or format
// changes; each creation logs one `[ShaderInput]` line. The caller serializes
// immediate-context use (MediaEngine D3D11 lock).
class SampleableSourceCopy {
public:
    // The copy of `source`'s first subresource (owned here, valid until the
    // next Prepare or Reset), or nullptr after a logged failure.
    ID3D11Texture2D* Prepare(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source);
    ID3D11Texture2D* Texture() const { return copy_.get(); }
    void Reset();

private:
    ce::ComGuard<ID3D11Texture2D> copy_;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

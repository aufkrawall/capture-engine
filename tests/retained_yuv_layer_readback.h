#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdint>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/retained_yuv_overlays.h"

// Readback of a retained-YUV overlay layer (premultiplied FP32 RGBA).
struct RetainedYuvLayerCoverage {
    bool read = false;
    float maxAlpha = 0.0f;
    uint32_t coveredTexels = 0;     // alpha > 0
    uint32_t unpremultiplied = 0;   // a color channel above its alpha
};

inline RetainedYuvLayerCoverage ReadRetainedYuvLayerCoverage(ID3D11Device* device, ID3D11DeviceContext* context,
                                                             const RetainedYuvOverlayLayer& layer) {
    RetainedYuvLayerCoverage coverage;
    if (!layer.texture)
        return coverage;
    D3D11_TEXTURE2D_DESC desc = {};
    layer.texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.addressof())))
        return coverage;
    context->CopyResource(staging.get(), layer.texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return coverage;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) +
                                                         static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
            const float* texel = row + static_cast<size_t>(x) * 4;
            coverage.maxAlpha = std::max(coverage.maxAlpha, texel[3]);
            if (texel[3] > 0.0f)
                ++coverage.coveredTexels;
            if (texel[0] > texel[3] + 1e-5f || texel[1] > texel[3] + 1e-5f || texel[2] > texel[3] + 1e-5f)
                ++coverage.unpremultiplied;
        }
    }
    context->Unmap(staging.get(), 0);
    coverage.read = true;
    return coverage;
}

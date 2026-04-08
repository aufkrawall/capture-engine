#pragma once

#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <algorithm>
#include <climits>
#include <cstdint>

#include "../../common/config.h"

namespace ce::sampler_override {

inline bool IsAnisotropicOverrideEnabled(const GraphicsConfig& gfx) {
    return !gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default" &&
           gfx.anisotropicFiltering != "off";
}

inline UINT GetConfiguredMaxAnisotropy(const GraphicsConfig& gfx) {
    if (gfx.anisotropicFiltering == "2x")
        return 2;
    if (gfx.anisotropicFiltering == "4x")
        return 4;
    if (gfx.anisotropicFiltering == "8x")
        return 8;
    return 16;
}

inline bool IsD3D10ComparisonFilter(D3D10_FILTER filter) {
    return filter >= D3D10_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
}

inline bool IsD3D10AnisotropicFilter(D3D10_FILTER filter) {
    return filter == D3D10_FILTER_ANISOTROPIC || filter == D3D10_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D10_FILTER GetForcedAnisotropicFilter(D3D10_FILTER originalFilter) {
    return IsD3D10ComparisonFilter(originalFilter) ? D3D10_FILTER_COMPARISON_ANISOTROPIC : D3D10_FILTER_ANISOTROPIC;
}

inline bool IsD3D11ReductionFilter(D3D11_FILTER filter) {
    return filter >= D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D11ComparisonFilter(D3D11_FILTER filter) {
    return filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && filter < D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D11AnisotropicFilter(D3D11_FILTER filter) {
    return filter == D3D11_FILTER_ANISOTROPIC || filter == D3D11_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D11_FILTER GetForcedAnisotropicFilter(D3D11_FILTER originalFilter) {
    return IsD3D11ComparisonFilter(originalFilter) ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
}

inline bool IsD3D12ReductionFilter(D3D12_FILTER filter) {
    return filter >= D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D12ComparisonFilter(D3D12_FILTER filter) {
    return filter >= D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT && filter < D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D12AnisotropicFilter(D3D12_FILTER filter) {
    return filter == D3D12_FILTER_ANISOTROPIC || filter == D3D12_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D12_FILTER GetForcedAnisotropicFilter(D3D12_FILTER originalFilter) {
    return IsD3D12ComparisonFilter(originalFilter) ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
}

inline UINT ResolveFullMipCount2D(UINT width, UINT height, UINT mipLevels) {
    if (mipLevels != 0) {
        return mipLevels;
    }

    UINT maxDimension = std::max(width, height);
    UINT resolvedMipLevels = 0;
    do {
        ++resolvedMipLevels;
        maxDimension >>= 1;
    } while (maxDimension > 0);

    return resolvedMipLevels;
}

inline UINT ResolveVisibleMipCount(UINT totalMipLevels, UINT mostDetailedMip, UINT viewMipLevels) {
    if (viewMipLevels == UINT_MAX) {
        return totalMipLevels > mostDetailedMip ? totalMipLevels - mostDetailedMip : 0;
    }
    return viewMipLevels;
}

inline uint64_t HashSamplerOverrideConfig(const GraphicsConfig& gfx) {
    uint64_t hash = 1469598103934665603ull;
    auto mixByte = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    auto mixString = [&mixByte](const std::string& value) {
        for (unsigned char ch : value) {
            mixByte(ch);
        }
        mixByte(0xff);
    };

    mixString(gfx.anisotropicFiltering);
    mixString(gfx.mipMapping);
    mixString(gfx.mipBias);
    mixString(gfx.mipBiasMode);
    mixString(gfx.msaaSamples);
    mixByte(gfx.forceMipBiasClamp ? 1 : 0);
    mixByte(gfx.sgssaa ? 1 : 0);
    mixByte(gfx.disableAutoMipBias ? 1 : 0);
    return hash;
}

}  // namespace ce::sampler_override

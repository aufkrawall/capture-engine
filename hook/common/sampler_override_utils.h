#pragma once

#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

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

inline UINT ResolveFullMipCount2D(UINT width, UINT height, UINT mipLevels);
inline UINT ResolveVisibleMipCount(UINT totalMipLevels, UINT mostDetailedMip, UINT viewMipLevels);

enum class D3D11ForcedAFSamplerDecision {
    Allow,
    OverrideDisabled,
    FixedLOD,
    BorderAddress,
    ReductionFilter,
    ComparisonFilter,
};

inline D3D11ForcedAFSamplerDecision ClassifyD3D11SamplerForForcedAF(const D3D11_SAMPLER_DESC& desc,
                                                                   const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return D3D11ForcedAFSamplerDecision::OverrideDisabled;
    }
    if (desc.MaxLOD == 0.0f || desc.MinLOD == desc.MaxLOD) {
        return D3D11ForcedAFSamplerDecision::FixedLOD;
    }
    if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        return D3D11ForcedAFSamplerDecision::BorderAddress;
    }
    if (IsD3D11ReductionFilter(desc.Filter)) {
        return D3D11ForcedAFSamplerDecision::ReductionFilter;
    }
    if (IsD3D11ComparisonFilter(desc.Filter)) {
        return D3D11ForcedAFSamplerDecision::ComparisonFilter;
    }
    return D3D11ForcedAFSamplerDecision::Allow;
}

inline bool D3D11SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    return ClassifyD3D11SamplerForForcedAF(desc, gfx) == D3D11ForcedAFSamplerDecision::Allow;
}

inline bool IsPotentiallyProblematicD3D11AFFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

enum class D3D11ForcedAFResourceDecision {
    Allow,
    UnsupportedFormat,
    UnsupportedViewDimension,
    Multisampled,
    ArrayResource,
    CubeResource,
    DepthStencilResource,
    RenderTargetResource,
    UnorderedAccessResource,
    ProblematicFormat,
    SingleVisibleMip,
};

struct D3D11Texture2DForcedAFInfo {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D11_SRV_DIMENSION viewDimension = D3D11_SRV_DIMENSION_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT mipLevels = 0;
    UINT mostDetailedMip = 0;
    UINT viewMipLevels = UINT_MAX;
    UINT arraySize = 1;
    UINT sampleCount = 1;
    UINT bindFlags = 0;
    UINT miscFlags = 0;
    bool formatSupported = false;
};

inline D3D11ForcedAFResourceDecision ClassifyD3D11Texture2DForForcedAF(const D3D11Texture2DForcedAFInfo& info) {
    if (!info.formatSupported) {
        return D3D11ForcedAFResourceDecision::UnsupportedFormat;
    }
    if (info.viewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;
    }
    if (info.sampleCount > 1) {
        return D3D11ForcedAFResourceDecision::Multisampled;
    }
    if (info.arraySize > 1) {
        return D3D11ForcedAFResourceDecision::ArrayResource;
    }
    if ((info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0) {
        return D3D11ForcedAFResourceDecision::CubeResource;
    }
    if ((info.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        return D3D11ForcedAFResourceDecision::DepthStencilResource;
    }
    if ((info.bindFlags & D3D11_BIND_RENDER_TARGET) != 0) {
        return D3D11ForcedAFResourceDecision::RenderTargetResource;
    }
    if ((info.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0) {
        return D3D11ForcedAFResourceDecision::UnorderedAccessResource;
    }
    if (IsPotentiallyProblematicD3D11AFFormat(info.format)) {
        return D3D11ForcedAFResourceDecision::ProblematicFormat;
    }

    const UINT totalMipLevels = ResolveFullMipCount2D(info.width, info.height, info.mipLevels);
    const UINT visibleMipLevels = ResolveVisibleMipCount(totalMipLevels, info.mostDetailedMip, info.viewMipLevels);
    if (visibleMipLevels <= 1) {
        return D3D11ForcedAFResourceDecision::SingleVisibleMip;
    }

    return D3D11ForcedAFResourceDecision::Allow;
}

inline bool D3D11Texture2DAllowsForcedAF(const D3D11Texture2DForcedAFInfo& info) {
    return ClassifyD3D11Texture2DForForcedAF(info) == D3D11ForcedAFResourceDecision::Allow;
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

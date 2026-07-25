#pragma once

#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../../common/config.h"

#include "config_and_state.h"

// D3D11 shader sampler usage classification driving safe override decisions.

namespace ce::sampler_override {

inline bool D3D11ShaderSamplerUsesExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesBiasSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesBiasSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesLodSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesLodSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesGradientSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesGradientSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesComparisonSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesComparisonSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesOtherExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesOtherExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesUnsafeExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT &&
           (usage.samplerUsesLodSample[samplerSlot] || usage.samplerUsesComparisonSample[samplerSlot] ||
            usage.samplerUsesOtherExplicitSample[samplerSlot]);
}

inline bool D3D11ShaderSamplerUsesAFSafeSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }
    const bool hasDerivativeFootprint = usage.samplerUsesImplicitSample[samplerSlot] ||
                                        usage.samplerUsesBiasSample[samplerSlot] ||
                                        usage.samplerUsesGradientSample[samplerSlot];
    return hasDerivativeFootprint && !D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, samplerSlot);
}

inline bool D3D11ShaderSamplerUsesOnlyImplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesImplicitSample[samplerSlot] &&
           !usage.samplerUsesExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesTexture(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot, UINT textureSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT &&
           textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
           usage.samplerTextures[samplerSlot][textureSlot];
}

inline bool D3D11ShaderSamplerUsesAnyTexture(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }
    for (bool usesTexture : usage.samplerTextures[samplerSlot]) {
        if (usesTexture) {
            return true;
        }
    }
    return false;
}

inline bool D3D11ShaderSamplerIsAFCandidate(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return D3D11ShaderSamplerUsesAnyTexture(usage, samplerSlot) &&
           D3D11ShaderSamplerUsesAFSafeSample(usage, samplerSlot);
}

inline uint32_t D3D11ShaderSamplerMaskForTextureSlot(const D3D11ShaderSamplerUsage& usage, UINT textureSlot) {
    if (textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }

    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (usage.samplerTextures[sampler][textureSlot]) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderAFSafeSamplerMaskForTextureSlot(const D3D11ShaderSamplerUsage& usage, UINT textureSlot) {
    if (textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }

    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (usage.samplerTextures[sampler][textureSlot] && D3D11ShaderSamplerUsesAFSafeSample(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderSamplerMaskForAnyTexture(const D3D11ShaderSamplerUsage& usage) {
    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (D3D11ShaderSamplerUsesAnyTexture(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderAFSafeSamplerMaskForAnyTexture(const D3D11ShaderSamplerUsage& usage) {
    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (D3D11ShaderSamplerIsAFCandidate(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline UINT CountD3D11ShaderSamplerTextureUses(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot,
                                               UINT* firstTextureSlot = nullptr, UINT* lastTextureSlot = nullptr) {
    if (firstTextureSlot) {
        *firstTextureSlot = UINT_MAX;
    }
    if (lastTextureSlot) {
        *lastTextureSlot = UINT_MAX;
    }
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return 0;
    }

    UINT count = 0;
    for (UINT texture = 0; texture < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++texture) {
        if (!usage.samplerTextures[samplerSlot][texture]) {
            continue;
        }
        if (firstTextureSlot && *firstTextureSlot == UINT_MAX) {
            *firstTextureSlot = texture;
        }
        if (lastTextureSlot) {
            *lastTextureSlot = texture;
        }
        ++count;
    }
    return count;
}

struct D3D11ShaderSamplerUsageSummary {
    UINT samplerCount = 0;
    UINT texturePairCount = 0;
    UINT implicitSamplers = 0;
    UINT biasSamplers = 0;
    UINT lodSamplers = 0;
    UINT gradientSamplers = 0;
    UINT comparisonSamplers = 0;
    UINT otherExplicitSamplers = 0;
    UINT afSafeSamplers = 0;
    UINT unsafeExplicitSamplers = 0;
};

inline D3D11ShaderSamplerUsageSummary SummarizeD3D11ShaderSamplerUsage(const D3D11ShaderSamplerUsage& usage) {
    D3D11ShaderSamplerUsageSummary summary = {};
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        const UINT textureCount = CountD3D11ShaderSamplerTextureUses(usage, sampler);
        if (textureCount == 0) {
            continue;
        }
        ++summary.samplerCount;
        summary.texturePairCount += textureCount;
        if (usage.samplerUsesImplicitSample[sampler]) {
            ++summary.implicitSamplers;
        }
        if (usage.samplerUsesBiasSample[sampler]) {
            ++summary.biasSamplers;
        }
        if (usage.samplerUsesLodSample[sampler]) {
            ++summary.lodSamplers;
        }
        if (usage.samplerUsesGradientSample[sampler]) {
            ++summary.gradientSamplers;
        }
        if (usage.samplerUsesComparisonSample[sampler]) {
            ++summary.comparisonSamplers;
        }
        if (usage.samplerUsesOtherExplicitSample[sampler]) {
            ++summary.otherExplicitSamplers;
        }
        if (D3D11ShaderSamplerUsesAFSafeSample(usage, sampler)) {
            ++summary.afSafeSamplers;
        }
        if (D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, sampler)) {
            ++summary.unsafeExplicitSamplers;
        }
    }
    return summary;
}

enum class D3D11ForcedAFSamplerDecision {
    Allow,
    OverrideDisabled,
    FixedLOD,
    BorderAddress,
    ReductionFilter,
    ComparisonFilter,
    PointMinMag,
};

inline D3D11ForcedAFSamplerDecision ClassifyD3D11SamplerForForcedAF(const D3D11_SAMPLER_DESC& desc,
                                                                    const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return D3D11ForcedAFSamplerDecision::OverrideDisabled;
    }
    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD) {
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
    if (gfx.samplerOverrideMode != "aggressive" && (D3D11_DECODE_MIN_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR ||
                                                    D3D11_DECODE_MAG_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR)) {
        return D3D11ForcedAFSamplerDecision::PointMinMag;
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
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS:
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
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

inline bool IsLikelyColorD3D11AFFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
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
    DepthStencilResource,
    ProblematicFormat,
    NonColorFormat,
    SingleVisibleMip,
};

inline const char* D3D11ForcedAFResourceDecisionName(D3D11ForcedAFResourceDecision decision) {
    switch (decision) {
        case D3D11ForcedAFResourceDecision::Allow:
            return "allow";
        case D3D11ForcedAFResourceDecision::UnsupportedFormat:
            return "unsupported-format";
        case D3D11ForcedAFResourceDecision::UnsupportedViewDimension:
            return "unsupported-view-dimension";
        case D3D11ForcedAFResourceDecision::Multisampled:
            return "multisampled";
        case D3D11ForcedAFResourceDecision::DepthStencilResource:
            return "depth-stencil-resource";
        case D3D11ForcedAFResourceDecision::ProblematicFormat:
            return "problematic-format";
        case D3D11ForcedAFResourceDecision::NonColorFormat:
            return "non-color-format";
        case D3D11ForcedAFResourceDecision::SingleVisibleMip:
            return "single-visible-mip";
    }
    return "unknown";
}

struct D3D11Texture2DForcedAFInfo {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
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
    switch (info.viewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            return D3D11ForcedAFResourceDecision::Multisampled;
        default:
            return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;
    }
    if (info.sampleCount > 1) {
        return D3D11ForcedAFResourceDecision::Multisampled;
    }
    if ((info.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        return D3D11ForcedAFResourceDecision::DepthStencilResource;
    }
    if (IsPotentiallyProblematicD3D11AFFormat(info.format)) {
        return D3D11ForcedAFResourceDecision::ProblematicFormat;
    }
    if (!IsLikelyColorD3D11AFFormat(info.format)) {
        return D3D11ForcedAFResourceDecision::NonColorFormat;
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
    mixString(gfx.samplerOverrideMode);
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

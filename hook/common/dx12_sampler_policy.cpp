#include "dx12_sampler_policy.h"

#include <bit>
#include <cstring>

#include "../apis/lod_helper.h"
#include "sampler_override_utils.h"

namespace ce::dx12_sampler_policy {
namespace {

bool HasSamplerOverride(const GraphicsConfig& gfx) {
    return (!gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default") ||
           (!gfx.mipMapping.empty() && gfx.mipMapping != "default") ||
           (!gfx.mipBias.empty() && gfx.mipBias != "default") || gfx.forceMipBiasClamp;
}

bool IsAggressive(const GraphicsConfig& gfx) {
    return gfx.samplerOverrideMode == "aggressive";
}

bool IsMaterialAddressMode(D3D12_TEXTURE_ADDRESS_MODE mode) {
    return mode == D3D12_TEXTURE_ADDRESS_MODE_WRAP || mode == D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
}

bool IsFiniteFloat(float value) {
    constexpr uint32_t exponentMask = 0x7F800000u;
    static_assert(std::endian::native == std::endian::little);

    // Read the object representation directly so descriptor validation never
    // depends on floating-point comparison behavior or the active FP environment.
    const auto* bytes = reinterpret_cast<const volatile uint8_t*>(&value);
    const uint32_t bits = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    return (bits & exponentMask) != exponentMask;
}

constexpr D3D12_FILTER EncodeBasicFilter(D3D12_FILTER_TYPE minFilter, D3D12_FILTER_TYPE magFilter,
                                         D3D12_FILTER_TYPE mipFilter, D3D12_FILTER_REDUCTION_TYPE reduction) {
    // Some otherwise usable MinGW D3D12 headers expose the decode helpers but omit
    // D3D12_ENCODE_BASIC_FILTER. Encode the documented filter bit layout directly.
    const UINT encoded =
        ((static_cast<UINT>(minFilter) & D3D12_FILTER_TYPE_MASK) << D3D12_MIN_FILTER_SHIFT) |
        ((static_cast<UINT>(magFilter) & D3D12_FILTER_TYPE_MASK) << D3D12_MAG_FILTER_SHIFT) |
        ((static_cast<UINT>(mipFilter) & D3D12_FILTER_TYPE_MASK) << D3D12_MIP_FILTER_SHIFT) |
        ((static_cast<UINT>(reduction) & D3D12_FILTER_REDUCTION_TYPE_MASK)
         << D3D12_FILTER_REDUCTION_TYPE_SHIFT);
    return static_cast<D3D12_FILTER>(encoded);
}

Decision Classify(const D3D12_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!HasSamplerOverride(gfx)) {
        return Decision::OverrideDisabled;
    }
    if (!IsFiniteFloat(desc.MinLOD) || !IsFiniteFloat(desc.MaxLOD) || !IsFiniteFloat(desc.MipLODBias)) {
        return Decision::InvalidDescriptor;
    }
    if (ce::sampler_override::IsD3D12ComparisonFilter(desc.Filter)) {
        return Decision::ComparisonFilter;
    }
    if (ce::sampler_override::IsD3D12ReductionFilter(desc.Filter)) {
        return Decision::ReductionFilter;
    }
    if (desc.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER || desc.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
        desc.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER) {
        return Decision::BorderAddress;
    }
    if (!IsAggressive(gfx)) {
        if (!IsMaterialAddressMode(desc.AddressU) || !IsMaterialAddressMode(desc.AddressV) ||
            !IsMaterialAddressMode(desc.AddressW)) {
            return Decision::ScreenSpaceAddress;
        }
    }
    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD) {
        return Decision::FixedLod;
    }
    if (!IsAggressive(gfx) && (D3D12_DECODE_MIN_FILTER(desc.Filter) != D3D12_FILTER_TYPE_LINEAR ||
                               D3D12_DECODE_MAG_FILTER(desc.Filter) != D3D12_FILTER_TYPE_LINEAR)) {
        return Decision::PointMinMag;
    }
    return Decision::Allow;
}

template <typename Value>
void HashValue(uint64_t& hash, const Value& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t index = 0; index < sizeof(Value); ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

Result ApplyImpl(D3D12_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    Result result;
    result.decision = Classify(desc, gfx);
    if (result.decision != Decision::Allow) {
        return result;
    }

    const D3D12_FILTER originalFilter = desc.Filter;
    const UINT originalAnisotropy = desc.MaxAnisotropy;
    const float originalBias = desc.MipLODBias;

    if (gfx.mipMapping == "bilinear") {
        desc.Filter = EncodeBasicFilter(D3D12_DECODE_MIN_FILTER(desc.Filter), D3D12_DECODE_MAG_FILTER(desc.Filter),
                                        D3D12_FILTER_TYPE_POINT, D3D12_DECODE_FILTER_REDUCTION(desc.Filter));
    } else if (gfx.mipMapping == "trilinear") {
        desc.Filter = EncodeBasicFilter(D3D12_DECODE_MIN_FILTER(desc.Filter), D3D12_DECODE_MAG_FILTER(desc.Filter),
                                        D3D12_FILTER_TYPE_LINEAR, D3D12_DECODE_FILTER_REDUCTION(desc.Filter));
    }

    const D3D12_FILTER filterAfterMipMapping = desc.Filter;
    if (gfx.anisotropicFiltering == "off") {
        if (ce::sampler_override::IsD3D12AnisotropicFilter(desc.Filter)) {
            desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            desc.MaxAnisotropy = 1;
        }
    } else if (ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
        desc.Filter = D3D12_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
    }

    if ((!gfx.mipBias.empty() && gfx.mipBias != "default") || gfx.forceMipBiasClamp) {
        desc.MipLODBias = FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, desc.MipLODBias));
    }

    result.anisotropyModified = filterAfterMipMapping != desc.Filter || originalAnisotropy != desc.MaxAnisotropy;
    result.mipMappingModified = originalFilter != filterAfterMipMapping;
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - bitwise comparison also detects -0.0 vs +0.0 changes
    result.mipBiasModified = std::memcmp(&originalBias, &desc.MipLODBias, sizeof(originalBias)) != 0;
    if (!result.Modified()) {
        result.decision = Decision::AlreadyCompliant;
    }
    return result;
}

}  // namespace

Result Apply(D3D12_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    return ApplyImpl(desc, gfx);
}

Result Apply(D3D12_STATIC_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_SAMPLER_DESC dynamicDesc = {};
    dynamicDesc.Filter = desc.Filter;
    dynamicDesc.AddressU = desc.AddressU;
    dynamicDesc.AddressV = desc.AddressV;
    dynamicDesc.AddressW = desc.AddressW;
    dynamicDesc.MipLODBias = desc.MipLODBias;
    dynamicDesc.MaxAnisotropy = desc.MaxAnisotropy;
    dynamicDesc.ComparisonFunc = desc.ComparisonFunc;
    dynamicDesc.BorderColor[0] = desc.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE ? 1.0f : 0.0f;
    dynamicDesc.BorderColor[1] = dynamicDesc.BorderColor[0];
    dynamicDesc.BorderColor[2] = dynamicDesc.BorderColor[0];
    dynamicDesc.BorderColor[3] = desc.BorderColor == D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK ? 0.0f : 1.0f;
    dynamicDesc.MinLOD = desc.MinLOD;
    dynamicDesc.MaxLOD = desc.MaxLOD;

    const Result result = ApplyImpl(dynamicDesc, gfx);
    if (result.Modified()) {
        desc.Filter = dynamicDesc.Filter;
        desc.MipLODBias = dynamicDesc.MipLODBias;
        desc.MaxAnisotropy = dynamicDesc.MaxAnisotropy;
    }
    return result;
}

const char* DecisionName(Decision decision) {
    switch (decision) {
        case Decision::OverrideDisabled:
            return "override-disabled";
        case Decision::Allow:
            return "modified";
        case Decision::AlreadyCompliant:
            return "already-compliant";
        case Decision::ComparisonFilter:
            return "comparison-filter";
        case Decision::ReductionFilter:
            return "reduction-filter";
        case Decision::BorderAddress:
            return "border-address";
        case Decision::ScreenSpaceAddress:
            return "clamp-or-mirror-once-address";
        case Decision::FixedLod:
            return "fixed-or-no-mip-lod";
        case Decision::PointMinMag:
            return "point-min-or-mag";
        case Decision::InvalidDescriptor:
            return "invalid-descriptor";
    }
    return "unknown";
}

uint64_t Fingerprint(const D3D12_SAMPLER_DESC& desc) {
    uint64_t hash = 1469598103934665603ull;
    HashValue(hash, desc.Filter);
    HashValue(hash, desc.AddressU);
    HashValue(hash, desc.AddressV);
    HashValue(hash, desc.AddressW);
    HashValue(hash, desc.MipLODBias);
    HashValue(hash, desc.MaxAnisotropy);
    HashValue(hash, desc.ComparisonFunc);
    for (float value : desc.BorderColor) {
        HashValue(hash, value);
    }
    HashValue(hash, desc.MinLOD);
    HashValue(hash, desc.MaxLOD);
    return hash;
}

uint64_t Fingerprint(const D3D12_STATIC_SAMPLER_DESC& desc) {
    uint64_t hash = 1469598103934665603ull;
    HashValue(hash, desc.Filter);
    HashValue(hash, desc.AddressU);
    HashValue(hash, desc.AddressV);
    HashValue(hash, desc.AddressW);
    HashValue(hash, desc.MipLODBias);
    HashValue(hash, desc.MaxAnisotropy);
    HashValue(hash, desc.ComparisonFunc);
    HashValue(hash, desc.BorderColor);
    HashValue(hash, desc.MinLOD);
    HashValue(hash, desc.MaxLOD);
    HashValue(hash, desc.ShaderRegister);
    HashValue(hash, desc.RegisterSpace);
    HashValue(hash, desc.ShaderVisibility);
    return hash;
}

}  // namespace ce::dx12_sampler_policy

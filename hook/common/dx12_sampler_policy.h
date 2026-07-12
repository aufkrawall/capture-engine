#pragma once

#include <d3d12.h>

#include <cstdint>

#include "../../common/config.h"

namespace ce::dx12_sampler_policy {

enum class Decision : uint8_t {
    OverrideDisabled,
    Allow,
    AlreadyCompliant,
    ComparisonFilter,
    ReductionFilter,
    BorderAddress,
    ScreenSpaceAddress,
    FixedLod,
    PointMinMag,
    InvalidDescriptor,
};

struct Result {
    Decision decision = Decision::OverrideDisabled;
    bool anisotropyModified = false;
    bool mipMappingModified = false;
    bool mipBiasModified = false;

    bool Modified() const {
        return anisotropyModified || mipMappingModified || mipBiasModified;
    }
};

Result Apply(D3D12_SAMPLER_DESC& desc, const GraphicsConfig& gfx);
Result Apply(D3D12_STATIC_SAMPLER_DESC& desc, const GraphicsConfig& gfx);

const char* DecisionName(Decision decision);
uint64_t Fingerprint(const D3D12_SAMPLER_DESC& desc);
uint64_t Fingerprint(const D3D12_STATIC_SAMPLER_DESC& desc);

}  // namespace ce::dx12_sampler_policy

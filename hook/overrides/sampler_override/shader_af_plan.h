#pragma once

#include <bit>

#include "shader_sampler_usage.h"

namespace ce::sampler_override {

// Immutable shader dependencies, built once from disassembly. A dirty sampler
// needs only its own small record, not a copy/scan of the entire usage matrix.
struct D3D11AFTextureMask {
    std::array<uint64_t, 2> words = {};

    void Add(UINT slot) {
        if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            words[slot / 64] |= uint64_t{1} << (slot % 64);
    }
    UINT Count() const { return std::popcount(words[0]) + std::popcount(words[1]); }
    UINT First() const {
        if (words[0] != 0)
            return std::countr_zero(words[0]);
        return words[1] != 0 ? 64u + std::countr_zero(words[1]) : UINT_MAX;
    }
    UINT Last() const {
        if (words[1] != 0)
            return 127u - std::countl_zero(words[1]);
        return words[0] != 0 ? 63u - std::countl_zero(words[0]) : UINT_MAX;
    }
    UINT TakeFirst() {
        const UINT slot = First();
        if (slot != UINT_MAX)
            words[slot / 64] &= words[slot / 64] - 1;
        return slot;
    }
};

struct D3D11AFSamplerPlan {
    D3D11AFTextureMask textures;
    bool implicitSample = false;
    bool biasSample = false;
    bool lodSample = false;
    bool gradientSample = false;
    bool comparisonSample = false;
    bool otherExplicitSample = false;
    bool explicitSample = false;

    bool SafeSample() const {
        return (implicitSample || biasSample || gradientSample) &&
               !(lodSample || comparisonSample || otherExplicitSample);
    }
};

struct D3D11ShaderAFPlan {
    std::array<D3D11AFSamplerPlan, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers = {};
    std::array<uint32_t, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> textureSamplerMasks = {};
    uint32_t candidateSamplerMask = 0;
    bool available = false;
    bool disassembleFailed = false;

    uint32_t ShaderDirtyMask(uint32_t trackedMask, uint32_t overriddenMask) const {
        return (candidateSamplerMask | overriddenMask) & trackedMask;
    }

    uint32_t ResourceDirtyMask(UINT textureSlot, uint32_t trackedMask) const {
        if (!available)
            return trackedMask;
        return textureSlot < textureSamplerMasks.size() ? textureSamplerMasks[textureSlot] & trackedMask : 0;
    }
};

inline D3D11ShaderAFPlan BuildD3D11ShaderAFPlan(const D3D11ShaderSamplerUsage& usage, bool available,
                                              bool disassembleFailed = false) {
    D3D11ShaderAFPlan plan;
    plan.available = available;
    plan.disassembleFailed = disassembleFailed;
    if (!available)
        return plan;
    for (UINT slot = 0; slot < plan.samplers.size(); ++slot) {
        auto& sampler = plan.samplers[slot];
        sampler.implicitSample = usage.samplerUsesImplicitSample[slot];
        sampler.biasSample = usage.samplerUsesBiasSample[slot];
        sampler.lodSample = usage.samplerUsesLodSample[slot];
        sampler.gradientSample = usage.samplerUsesGradientSample[slot];
        sampler.comparisonSample = usage.samplerUsesComparisonSample[slot];
        sampler.otherExplicitSample = usage.samplerUsesOtherExplicitSample[slot];
        sampler.explicitSample = usage.samplerUsesExplicitSample[slot];
        for (UINT texture = 0; texture < plan.textureSamplerMasks.size(); ++texture) {
            if (usage.samplerTextures[slot][texture]) {
                sampler.textures.Add(texture);
                if (sampler.SafeSample())
                    plan.textureSamplerMasks[texture] |= 1u << slot;
            }
        }
        if (sampler.SafeSample() && sampler.textures.Count() != 0)
            plan.candidateSamplerMask |= 1u << slot;
    }
    return plan;
}

}  // namespace ce::sampler_override

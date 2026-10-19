#pragma once

#include <cstring>

#include "shader_sampler_usage.h"

namespace ce::sampler_override {

inline const GUID kD3D11SamplerCreationPolicyGuid = {
    0xb3e2da59, 0x4d91, 0x48bd, {0xb5, 0x68, 0x45, 0xf2, 0x9b, 0x26, 0x6d, 0x13}};

struct D3D11SamplerCreationPolicy {
    uint64_t hash;
    D3D11_SAMPLER_DESC applicationDesc;
};

// Thirteen 4-byte members (enums, UINTs, floats): no padding bytes can differ.
static_assert(sizeof(D3D11_SAMPLER_DESC) == 13 * 4, "D3D11_SAMPLER_DESC gained padding");

// Bit identity of two sampler descriptors, which is what the creation tag and memo record: a
// -0.0f and a +0.0f bias or LOD clamp count as different application descriptors.
inline bool SameD3D11SamplerDescBits(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - no padding (asserted above); bit identity is meant
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// `hash` is HashSamplerOverrideConfig of the active config. S_FALSE: the sampler already carries
// exactly this tag, because the runtime handed out an existing object for an identical
// descriptor; BioShock Infinite creates samplers on almost every draw, so the write is skipped.
inline HRESULT RecordD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, uint64_t hash,
                                               const D3D11_SAMPLER_DESC& applicationDesc) {
    if (!sampler)
        return E_POINTER;
    D3D11SamplerCreationPolicy existing{0, applicationDesc};
    UINT size = sizeof(existing);
    if (SUCCEEDED(sampler->GetPrivateData(kD3D11SamplerCreationPolicyGuid, &size, &existing)) &&
        size == sizeof(existing) && existing.hash == hash &&
        SameD3D11SamplerDescBits(existing.applicationDesc, applicationDesc))
        return S_FALSE;
    const D3D11SamplerCreationPolicy policy{hash, applicationDesc};
    return sampler->SetPrivateData(kD3D11SamplerCreationPolicyGuid, sizeof(policy), &policy);
}

inline HRESULT RecordD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, const GraphicsConfig& gfx,
                                               const D3D11_SAMPLER_DESC& applicationDesc) {
    return RecordD3D11SamplerCreationPolicy(sampler, HashSamplerOverrideConfig(gfx), applicationDesc);
}

// `desc` initially contains the logical object's current descriptor. The same
// creation policy needs only bind-time AF. A different policy must start from
// what the application requested, not an already offset/clamped descriptor.
// Untagged (late-attach) objects keep the existing whole-policy fallback.
inline bool ResolveD3D11SamplerBindDesc(ID3D11SamplerState* sampler, uint64_t hash, D3D11_SAMPLER_DESC& desc,
                                       bool* rebased = nullptr) {
    if (rebased)
        *rebased = false;
    if (!sampler)
        return false;
    D3D11SamplerCreationPolicy policy{0, desc};
    UINT size = sizeof(policy);
    if (FAILED(sampler->GetPrivateData(kD3D11SamplerCreationPolicyGuid, &size, &policy)) || size != sizeof(policy))
        return false;
    if (policy.hash == hash)
        return true;
    desc = policy.applicationDesc;
    if (rebased)
        *rebased = true;
    return false;
}

inline bool ResolveD3D11SamplerBindDesc(ID3D11SamplerState* sampler, const GraphicsConfig& gfx,
                                       D3D11_SAMPLER_DESC& desc, bool* rebased = nullptr) {
    return ResolveD3D11SamplerBindDesc(sampler, HashSamplerOverrideConfig(gfx), desc, rebased);
}

// Creation already applied mip mapping, bias offsets/clamps and SGSSAA bias.
// Reconciliation can change only the resource-dependent AF decision.
inline bool ApplyD3D11BindTimeAF(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx, bool allow) {
    if (!allow)
        return false;
    const UINT anisotropy = GetConfiguredMaxAnisotropy(gfx);
    const D3D11_FILTER filter = GetForcedAnisotropicFilter(desc.Filter);
    if (desc.Filter == filter && desc.MaxAnisotropy == anisotropy)
        return false;
    desc.Filter = filter;
    desc.MaxAnisotropy = anisotropy;
    return true;
}

}  // namespace ce::sampler_override

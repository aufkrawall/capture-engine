#pragma once

#include "shader_sampler_usage.h"

namespace ce::sampler_override {

inline const GUID kD3D11SamplerCreationPolicyGuid = {
    0xb3e2da59, 0x4d91, 0x48bd, {0xb5, 0x68, 0x45, 0xf2, 0x9b, 0x26, 0x6d, 0x13}};

struct D3D11SamplerCreationPolicy {
    uint64_t hash;
    D3D11_SAMPLER_DESC applicationDesc;
};

inline HRESULT RecordD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, const GraphicsConfig& gfx,
                                               const D3D11_SAMPLER_DESC& applicationDesc) {
    if (!sampler)
        return E_POINTER;
    const D3D11SamplerCreationPolicy policy{HashSamplerOverrideConfig(gfx), applicationDesc};
    return sampler->SetPrivateData(kD3D11SamplerCreationPolicyGuid, sizeof(policy), &policy);
}

// `desc` initially contains the logical object's current descriptor. The same
// creation policy needs only bind-time AF. A different policy must start from
// what the application requested, not an already offset/clamped descriptor.
// Untagged (late-attach) objects keep the existing whole-policy fallback.
inline bool ResolveD3D11SamplerBindDesc(ID3D11SamplerState* sampler, const GraphicsConfig& gfx,
                                       D3D11_SAMPLER_DESC& desc, bool* rebased = nullptr) {
    if (rebased)
        *rebased = false;
    if (!sampler)
        return false;
    D3D11SamplerCreationPolicy policy{0, desc};
    UINT size = sizeof(policy);
    if (FAILED(sampler->GetPrivateData(kD3D11SamplerCreationPolicyGuid, &size, &policy)) || size != sizeof(policy))
        return false;
    if (policy.hash == HashSamplerOverrideConfig(gfx))
        return true;
    desc = policy.applicationDesc;
    if (rebased)
        *rebased = true;
    return false;
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

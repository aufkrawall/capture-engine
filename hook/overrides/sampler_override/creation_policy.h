#pragma once

#include "shader_sampler_usage.h"

namespace ce::sampler_override {

inline const GUID kD3D11SamplerCreationPolicyGuid = {
    0xb3e2da59, 0x4d91, 0x48bd, {0xb5, 0x68, 0x45, 0xf2, 0x9b, 0x26, 0x6d, 0x13}};

inline HRESULT RecordD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, uint64_t hash) {
    if (!sampler)
        return E_POINTER;
    return sampler->SetPrivateData(kD3D11SamplerCreationPolicyGuid, sizeof(hash), &hash);
}

inline HRESULT RecordD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, const GraphicsConfig& gfx) {
    return RecordD3D11SamplerCreationPolicy(sampler, HashSamplerOverrideConfig(gfx));
}

inline bool HasCurrentD3D11SamplerCreationPolicy(ID3D11SamplerState* sampler, const GraphicsConfig& gfx) {
    if (!sampler)
        return false;
    uint64_t hash = 0;
    UINT size = sizeof(hash);
    return SUCCEEDED(sampler->GetPrivateData(kD3D11SamplerCreationPolicyGuid, &size, &hash)) &&
           size == sizeof(hash) && hash == HashSamplerOverrideConfig(gfx);
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

#include "dx11_hook_internal.h"

// Whether a sampler descriptor admits forced AF at all (the shader/view half of the
// decision lives in the shared engine, d3d11_af_engine_slow.cpp).
bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc,  const GraphicsConfig& gfx) {


    using ce::sampler_override::D3D11ForcedAFSamplerDecision;
    const D3D11ForcedAFSamplerDecision decision = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx);
    switch (decision) {
        case D3D11ForcedAFSamplerDecision::Allow:
            return true;
        case D3D11ForcedAFSamplerDecision::OverrideDisabled:
            return false;
        case D3D11ForcedAFSamplerDecision::FixedLOD: {
            int idx = dx11_hook_g_DiagSamplerSkipNoMips.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (fixed/no mips) Filter=0x%X MaxLOD=%.1f MinLOD=%.1f",
                                 desc.Filter, desc.MaxLOD, desc.MinLOD);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::BorderAddress: {
            int idx = dx11_hook_g_DiagSamplerSkipBorder.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (border address) Filter=0x%X U=%d V=%d W=%d", desc.Filter,
                                 desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ReductionFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipReduction.fetch_add(1, std::memory_order_relaxed);
            if (idx < 6) {
                HookLogImportant("DX11: AF skip sampler (reduction filter) Filter=0x%X", desc.Filter);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ComparisonFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipComparison.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (comparison filter) Filter=0x%X Func=%d Addr=%d/%d/%d",
                                 desc.Filter, desc.ComparisonFunc, desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::PointMinMag:
            return false;
    }
    return false;

}

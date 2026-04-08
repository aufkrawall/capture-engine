/**
 * D3D12 Root Signature Parser Implementation
 *
 * Provides static sampler override functionality for D3D12.
 */

#include "root_signature_parser.h"
#include <algorithm>
#include <cstring>
#include "../apis/lod_helper.h"
#include "../common/hook_common.h"
#include "../common/sampler_override_utils.h"

namespace RootSignatureParser {

// ============================================================================
// Helper: Get filter type for anisotropic
// ============================================================================
static D3D12_FILTER GetAnisotropicFilter(D3D12_FILTER originalFilter) {
    return ce::sampler_override::GetForcedAnisotropicFilter(originalFilter);
}

// ============================================================================
// Apply overrides to a single static sampler
// ============================================================================
bool ApplyStaticSamplerOverrides(D3D12_STATIC_SAMPLER_DESC& sampler) {
    const GraphicsConfig gfx = GetActiveGraphicsConfig();
    bool modified = false;

    D3D12_FILTER origFilter = sampler.Filter;
    UINT origAniso = sampler.MaxAnisotropy;
    float origBias = sampler.MipLODBias;

    // Skip samplers with border address mode (commonly used for shadow maps)
    bool hasBorderAddress = (sampler.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                             sampler.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                             sampler.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER);

    // Anisotropic filtering override
    std::string af = gfx.anisotropicFiltering;
    if (af != "default" && !hasBorderAddress) {
        if (af == "off") {
            // Disable anisotropic - convert to linear
            if (ce::sampler_override::IsD3D12AnisotropicFilter(sampler.Filter)) {
                bool isComparison = ce::sampler_override::IsD3D12ComparisonFilter(sampler.Filter);
                sampler.Filter =
                    isComparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.MaxAnisotropy = 1;
                modified = true;
            }
        } else {
            // Enable anisotropic
            UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);

            D3D12_FILTER newFilter = GetAnisotropicFilter(sampler.Filter);
            if (sampler.Filter != newFilter || sampler.MaxAnisotropy != maxAniso) {
                sampler.Filter = newFilter;
                sampler.MaxAnisotropy = maxAniso;
                modified = true;
            }
        }
    }

    // Mip bias override
    std::string biasStr = gfx.mipBias;
    if (biasStr != "default" || gfx.forceMipBiasClamp) {
        float originalBias = sampler.MipLODBias;
        sampler.MipLODBias = ApplyConfiguredMipBias(gfx, sampler.MipLODBias);
        sampler.MipLODBias = FinalizeMipBias(gfx, sampler.MipLODBias);
        if (sampler.MipLODBias != originalBias) {
            modified = true;
        }
    }

    if (modified) {
        HookLog(
            "ApplyStaticSamplerOverrides: MODIFIED, Filter=0x%X->0x%X, "
            "Aniso=%d->%d, Bias=%.2f->%.2f",
            origFilter, sampler.Filter, origAniso, sampler.MaxAnisotropy, origBias, sampler.MipLODBias);
    }

    return modified;
}

// ============================================================================
// Count static samplers in a blob (quick check)
// ============================================================================
UINT CountStaticSamplers(const void* pBlob, SIZE_T blobSize) {
    if (!pBlob || blobSize < sizeof(DWORD) * 4) {
        return 0;
    }

    const DWORD* pHeader = static_cast<const DWORD*>(pBlob);
    DWORD version = pHeader[0];

    // Validate version
    if (version != 0x1 && version != 0x2) {
        return 0;
    }

    // Layout: Version, NumParameters, NumStaticSamplers, Flags
    return pHeader[2];  // NumStaticSamplers
}

}  // namespace RootSignatureParser

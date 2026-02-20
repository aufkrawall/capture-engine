/**
 * D3D12 Root Signature Parser Implementation
 *
 * Provides static sampler override functionality for D3D12.
 */

#include "root_signature_parser.h"
#include "../common/hook_common.h"
#include <algorithm>
#include <cstring>

namespace RootSignatureParser {

// ============================================================================
// Helper: Get filter type for anisotropic
// ============================================================================
static D3D12_FILTER GetAnisotropicFilter(D3D12_FILTER originalFilter) {
  // Check if this is a comparison filter
  bool isComparison =
      (originalFilter >= D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT &&
       originalFilter < D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT);

  if (isComparison) {
    return D3D12_FILTER_COMPARISON_ANISOTROPIC;
  }
  return D3D12_FILTER_ANISOTROPIC;
}

// ============================================================================
// Apply overrides to a single static sampler
// ============================================================================
bool ApplyStaticSamplerOverrides(D3D12_STATIC_SAMPLER_DESC &sampler) {
  const GraphicsConfig gfx = GetActiveGraphicsConfig();
  bool modified = false;

  D3D12_FILTER origFilter = sampler.Filter;
  UINT origAniso = sampler.MaxAnisotropy;
  float origBias = sampler.MipLODBias;

  HookLog("ApplyStaticSamplerOverrides: AF=%s, MipBias=%s, Mode=%s, "
          "Filter=0x%X, Aniso=%d, Bias=%.2f",
          gfx.anisotropicFiltering.c_str(), gfx.mipBias.c_str(),
          gfx.mipBiasMode.c_str(), sampler.Filter, sampler.MaxAnisotropy,
          sampler.MipLODBias);

  // Skip samplers with border address mode (commonly used for shadow maps)
  bool hasBorderAddress =
      (sampler.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
       sampler.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
       sampler.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER);

  // Anisotropic filtering override
  std::string af = gfx.anisotropicFiltering;
  if (af != "default" && !hasBorderAddress) {
    if (af == "off") {
      // Disable anisotropic - convert to linear
      if (sampler.Filter == D3D12_FILTER_ANISOTROPIC ||
          sampler.Filter == D3D12_FILTER_COMPARISON_ANISOTROPIC) {
        bool isComparison =
            (sampler.Filter >= D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT);
        sampler.Filter = isComparison
                             ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                             : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.MaxAnisotropy = 1;
        modified = true;
      }
    } else {
      // Enable anisotropic
      int maxAniso = 16;
      if (af == "2x")
        maxAniso = 2;
      else if (af == "4x")
        maxAniso = 4;
      else if (af == "8x")
        maxAniso = 8;

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
  if (biasStr != "default") {
    try {
      float userBias = std::stof(biasStr);
      std::string mode = gfx.mipBiasMode;

      float originalBias = sampler.MipLODBias;

      if (mode == "offset") {
        sampler.MipLODBias += userBias;
      } else if (mode == "base") {
        if (sampler.MipLODBias >= 0.0f) {
          sampler.MipLODBias = userBias;
        }
      } else {
        // "strict" or "override" - force the value
        sampler.MipLODBias = userBias;
      }

      // Clamp to valid range
      if (sampler.MipLODBias < -16.0f)
        sampler.MipLODBias = -16.0f;
      if (sampler.MipLODBias > 15.99f)
        sampler.MipLODBias = 15.99f;

      if (sampler.MipLODBias != originalBias) {
        modified = true;
      }
    } catch (...) {
    }
  }

  if (modified) {
    HookLog("ApplyStaticSamplerOverrides: MODIFIED, Filter=0x%X->0x%X, "
            "Aniso=%d->%d, Bias=%.2f->%.2f",
            origFilter, sampler.Filter, origAniso, sampler.MaxAnisotropy,
            origBias, sampler.MipLODBias);
  }

  return modified;
}

// ============================================================================
// Count static samplers in a blob (quick check)
// ============================================================================
UINT CountStaticSamplers(const void *pBlob, SIZE_T blobSize) {
  if (!pBlob || blobSize < sizeof(DWORD) * 4) {
    return 0;
  }

  const DWORD *pHeader = static_cast<const DWORD *>(pBlob);
  DWORD version = pHeader[0];

  // Validate version
  if (version != 0x1 && version != 0x2) {
    return 0;
  }

  // Layout: Version, NumParameters, NumStaticSamplers, Flags
  return pHeader[2]; // NumStaticSamplers
}

} // namespace RootSignatureParser

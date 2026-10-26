#pragma once

#include <dxgiformat.h>

#include <cstdint>

#include "common/graphics/sharpen_policy.h"

// DXGI format helpers shared by the D3D11 and D3D12 post-process passes and
// the DX12 FFX/screenshot paths. DXGI formats are API-neutral, so D3D12 code
// includes this header instead of the D3D11 pass.
namespace ce::sharpen {

// True for the formats whose views decode on load and re-encode on store.
bool FormatAppliesSrgbConversion(DXGI_FORMAT format);
uint32_t IntegerColorBits(DXGI_FORMAT format);
DXGI_FORMAT RawUnormViewFormat(DXGI_FORMAT format);
DXGI_FORMAT TypedPostProcessViewFormat(DXGI_FORMAT format);
DXGI_FORMAT TypelessCopyFormat(DXGI_FORMAT format);

// Presentation meaning for a DXGI target, given the swapchain's HDR contract.
// Storage format alone never decides this, which is why `isHDR` is required.
TargetEncoding ResolveDxgiEncoding(DXGI_FORMAT format, bool isHDR);

}  // namespace ce::sharpen

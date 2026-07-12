/**
 * D3D12 Root Signature Parser Implementation
 *
 * Provides static sampler override functionality for D3D12.
 */

#include "root_signature_parser.h"
#include <algorithm>
#include <cstring>
#include "../common/dx12_sampler_policy.h"
#include "../common/hook_common.h"

namespace RootSignatureParser {

// ============================================================================
// Apply overrides to a single static sampler
// ============================================================================
bool ApplyStaticSamplerOverrides(D3D12_STATIC_SAMPLER_DESC& sampler) {
    const GraphicsConfig gfx = GetActiveGraphicsConfig();
    return ce::dx12_sampler_policy::Apply(sampler, gfx).Modified();
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

/**
 * D3D12 Root Signature Parser
 *
 * Parses and modifies serialized root signature blobs to apply
 * anisotropic filtering and mip bias overrides to static samplers.
 */

#pragma once

#include <d3d12.h>
#include <cstdint>
#include <vector>

namespace RootSignatureParser {

/**
 * Apply AF and mip bias overrides to a static sampler.
 *
 * @param sampler The sampler to modify
 * @return true if modifications were made
 */
bool ApplyStaticSamplerOverrides(D3D12_STATIC_SAMPLER_DESC& sampler);

/**
 * Count static samplers in a blob (quick check)
 * Quick check without full parsing.
 *
 * @param pBlob Pointer to the serialized blob
 * @param blobSize Size of the blob
 * @return Number of static samplers in the blob
 */
UINT CountStaticSamplers(const void* pBlob, SIZE_T blobSize);

}  // namespace RootSignatureParser

#pragma once

#include <d3d11.h>
#include <cstdint>

#include "video_format_policy.h"

// The RGB normalization and RGB -> NV12/P010 draws of
// video_color_conversion_shader.h. The encoder's encode-time conversion and
// the retained-reservoir ingress conversion (RetainedYuvConverter) both issue
// their passes through these functions, so a retained frame carries exactly
// the codes the encoder would have produced from the same RGB image.

namespace ce::video_color {

struct RgbYuvDrawKernels {
    ID3D11VertexShader* vertexShader = nullptr;    // VS_Main
    ID3D11PixelShader* normalizeShader = nullptr;  // PS_Main
    ID3D11PixelShader* p010LumaShader = nullptr;   // PS_P010Y (PQ BT.2020)
    ID3D11PixelShader* p010ChromaShader = nullptr;
    ID3D11PixelShader* yuvLumaShader = nullptr;    // PS_YuvY (SDR NV12/P010)
    ID3D11PixelShader* yuvChromaShader = nullptr;
    // PS_PackSdrP010 / PS_PackHdrP010 (packed_p010_shader.h); only the
    // retained ingress converter creates them.
    ID3D11PixelShader* sdrPackShader = nullptr;
    ID3D11PixelShader* hdrPackShader = nullptr;
    ID3D11SamplerState* pointSampler = nullptr;
    ID3D11SamplerState* linearSampler = nullptr;
    ID3D11Buffer* constants = nullptr;  // 32-byte dynamic CopyCB
};

// Chroma plane layout of a semi-planar YUV target (yuvFlags bits 3-4 in
// video_color_conversion_shader.h). 4:2:0 and 4:2:2 chroma are left-sited
// for SDR, top-left / left co-sited for HDR.
enum class ChromaLayout : uint32_t { k420 = 0, k422 = 1, k444 = 2 };

// Chroma plane size for a luma plane of `lumaWidth` x `lumaHeight`.
UINT ChromaPlaneWidth(ChromaLayout layout, UINT lumaWidth);
UINT ChromaPlaneHeight(ChromaLayout layout, UINT lumaHeight);

struct YuvPlaneTargets {
    ID3D11RenderTargetView* luma = nullptr;
    ID3D11RenderTargetView* chroma = nullptr;  // R8G8 / R16G16, interleaved Cb,Cr
    UINT width = 0;  // luma size
    UINT height = 0;
    ChromaLayout layout = ChromaLayout::k420;
};

// A packed P010 slot (common/capture/packed_p010_layout.h) and the
// half-size R16G16_UNORM chroma scratch the chroma pass writes first.
struct PackedP010Targets {
    ID3D11RenderTargetView* packed = nullptr;  // R16_UINT, luma size
    ID3D11RenderTargetView* chromaScratch = nullptr;
    ID3D11ShaderResourceView* chromaScratchView = nullptr;
    UINT width = 0;  // luma size
    UINT height = 0;
};

uint32_t SdrYuvFlags(bool eightBit, bool bt2020, bool fullRange);

// Full-screen PS_Main pass (colorTransform, sdrWhiteNits clamped to 80..1000)
// into `target`, point sampled.
bool DrawRgbNormalize(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                      ID3D11RenderTargetView* target, UINT width, UINT height,
                      ce::video_format::RgbColorTransform transform, float sdrWhiteNits);

// Gamma-encoded SDR RGB -> 8/10-bit semi-planar YUV in the targets' chroma
// layout (NV12/P010 for 4:2:0), bilinear sampled at output size.
bool DrawSdrRgbToYuv(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                     const YuvPlaneTargets& targets, uint32_t yuvFlags);

// PQ BT.2020 RGB -> 10-bit semi-planar YUV in the targets' chroma layout
// (P010 for 4:2:0), bilinear sampled at output size.
bool DrawHdrRgb10ToP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                        ID3D11ShaderResourceView* input, const YuvPlaneTargets& targets, bool fullRange);

// The same two passes into a packed P010 slot: the chroma pass of
// DrawSdrRgbToYuv / DrawHdrRgb10ToP010 into the scratch, then their luma
// expression fused with packing (PS_Pack*P010), so the slot holds exactly the
// P010 codes those draws write.
bool DrawSdrRgbToPackedP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                            ID3D11ShaderResourceView* input, const PackedP010Targets& targets, uint32_t yuvFlags);
bool DrawHdrRgb10ToPackedP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                              ID3D11ShaderResourceView* input, const PackedP010Targets& targets, bool fullRange);

}  // namespace ce::video_color

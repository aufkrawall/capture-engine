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
    ID3D11SamplerState* pointSampler = nullptr;
    ID3D11SamplerState* linearSampler = nullptr;
    ID3D11Buffer* constants = nullptr;  // 32-byte dynamic CopyCB
};

struct YuvPlaneTargets {
    ID3D11RenderTargetView* luma = nullptr;
    ID3D11RenderTargetView* chroma = nullptr;
    UINT width = 0;  // luma size; chroma is half in both directions
    UINT height = 0;
};

uint32_t SdrYuvFlags(bool eightBit, bool bt2020, bool fullRange);

// Full-screen PS_Main pass (colorTransform, sdrWhiteNits clamped to 80..1000)
// into `target`, point sampled.
bool DrawRgbNormalize(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                      ID3D11RenderTargetView* target, UINT width, UINT height,
                      ce::video_format::RgbColorTransform transform, float sdrWhiteNits);

// Gamma-encoded SDR RGB -> NV12/P010 (left-sited 4:2:0), bilinear sampled.
bool DrawSdrRgbToYuv(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                     const YuvPlaneTargets& targets, uint32_t yuvFlags);

// PQ BT.2020 RGB10 -> P010 (top-left-sited 4:2:0), bilinear sampled.
bool DrawHdrRgb10ToP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                        ID3D11ShaderResourceView* input, const YuvPlaneTargets& targets, float lumaSharpenStrength,
                        bool fullRange);

}  // namespace ce::video_color

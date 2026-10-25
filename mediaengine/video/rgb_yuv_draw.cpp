#include "rgb_yuv_draw.h"

#include <algorithm>
#include <cstring>

namespace ce::video_color {

namespace {

struct CopyConstants {
    uint32_t colorTransform;
    uint32_t yuvFlags;
    float outputInvWidth;
    float outputInvHeight;
    float lumaSharpenStrength;
    float sdrWhiteNits;  // PS_P010Y/UV: >= 1 selects full range
    float padding[2];
};
static_assert(sizeof(CopyConstants) == 32);

bool WriteConstants(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const CopyConstants& constants) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(buffer, 0);
    return true;
}

void BindPipeline(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                  ID3D11SamplerState* sampler) {
    context->VSSetShader(kernels.vertexShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &input);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetConstantBuffers(0, 1, &kernels.constants);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
}

void DrawPass(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11PixelShader* shader, UINT width,
              UINT height) {
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShader(shader, nullptr, 0);
    context->Draw(3, 0);
}

void Unbind(ID3D11DeviceContext* context) {
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
}

bool DrawPlanes(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                const YuvPlaneTargets& targets, ID3D11PixelShader* lumaShader, ID3D11PixelShader* chromaShader,
                const CopyConstants& constants) {
    if (!WriteConstants(context, kernels.constants, constants))
        return false;
    BindPipeline(context, kernels, input, kernels.linearSampler);
    DrawPass(context, targets.luma, lumaShader, targets.width, targets.height);
    // 4:2:0 chroma is half-size; dimensions are even by construction.
    DrawPass(context, targets.chroma, chromaShader, targets.width / 2, targets.height / 2);
    Unbind(context);
    return true;
}

// The constants of both SDR / HDR routes; the packed P010 passes must match the planar ones.
CopyConstants SdrYuvConstants(UINT width, UINT height, uint32_t yuvFlags) {
    CopyConstants constants = {};
    constants.yuvFlags = yuvFlags;
    constants.outputInvWidth = 1.0f / static_cast<float>(width);
    constants.outputInvHeight = 1.0f / static_cast<float>(height);
    return constants;
}

CopyConstants HdrP010Constants(UINT width, UINT height, float lumaSharpenStrength, bool fullRange) {
    CopyConstants constants = {};
    constants.outputInvWidth = 1.0f / static_cast<float>(width);
    constants.outputInvHeight = 1.0f / static_cast<float>(height);
    constants.lumaSharpenStrength = lumaSharpenStrength;
    constants.sdrWhiteNits = fullRange ? 1.0f : 0.0f;
    return constants;
}

constexpr UINT kPackChromaRegister = 7;  // packP010Chroma in packed_p010_shader.h

bool DrawPackedPlanes(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                      const PackedP010Targets& targets, ID3D11PixelShader* chromaShader, ID3D11PixelShader* packShader,
                      const CopyConstants& constants) {
    if (!packShader || !WriteConstants(context, kernels.constants, constants))
        return false;
    BindPipeline(context, kernels, input, kernels.linearSampler);
    DrawPass(context, targets.chromaScratch, chromaShader, targets.width / 2, targets.height / 2);
    // The scratch is the pack pass's input: retarget first, or the runtime
    // would refuse an SRV of a texture still bound as the render target.
    context->OMSetRenderTargets(1, &targets.packed, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);  // integer target, exact codes
    context->PSSetShaderResources(kPackChromaRegister, 1, &targets.chromaScratchView);
    DrawPass(context, targets.packed, packShader, targets.width, targets.height);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(kPackChromaRegister, 1, &nullSrv);
    Unbind(context);
    return true;
}

bool HasPackedTargets(const PackedP010Targets& targets) {
    return targets.packed && targets.chromaScratch && targets.chromaScratchView && targets.width >= 2 &&
           targets.height >= 2;
}

}  // namespace

uint32_t SdrYuvFlags(bool eightBit, bool bt2020, bool fullRange) {
    return (eightBit ? 1u : 0u) | (bt2020 ? 2u : 0u) | (fullRange ? 4u : 0u);
}

bool DrawRgbNormalize(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                      ID3D11RenderTargetView* target, UINT width, UINT height,
                      ce::video_format::RgbColorTransform transform, float sdrWhiteNits) {
    if (!context || !input || !target || !kernels.constants)
        return false;
    CopyConstants constants = {};
    constants.colorTransform = static_cast<uint32_t>(transform);
    constants.sdrWhiteNits = std::clamp(sdrWhiteNits, 80.0f, 1000.0f);
    if (!WriteConstants(context, kernels.constants, constants))
        return false;
    BindPipeline(context, kernels, input, kernels.pointSampler);
    DrawPass(context, target, kernels.normalizeShader, width, height);
    Unbind(context);
    return true;
}

bool DrawSdrRgbToYuv(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels, ID3D11ShaderResourceView* input,
                     const YuvPlaneTargets& targets, uint32_t yuvFlags) {
    if (!context || !input || !targets.luma || !targets.chroma || !kernels.constants)
        return false;
    const CopyConstants constants = SdrYuvConstants(targets.width, targets.height, yuvFlags);

    return DrawPlanes(context, kernels, input, targets, kernels.yuvLumaShader, kernels.yuvChromaShader, constants);
}

bool DrawHdrRgb10ToP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                        ID3D11ShaderResourceView* input, const YuvPlaneTargets& targets, float lumaSharpenStrength,
                        bool fullRange) {
    if (!context || !input || !targets.luma || !targets.chroma || !kernels.constants)
        return false;
    const CopyConstants constants = HdrP010Constants(targets.width, targets.height, lumaSharpenStrength, fullRange);

    return DrawPlanes(context, kernels, input, targets, kernels.p010LumaShader, kernels.p010ChromaShader, constants);
}

bool DrawSdrRgbToPackedP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                            ID3D11ShaderResourceView* input, const PackedP010Targets& targets, uint32_t yuvFlags) {
    if (!context || !input || !HasPackedTargets(targets) || !kernels.constants)
        return false;
    const CopyConstants constants = SdrYuvConstants(targets.width, targets.height, yuvFlags);

    return DrawPackedPlanes(context, kernels, input, targets, kernels.yuvChromaShader, kernels.sdrPackShader,
                            constants);
}

bool DrawHdrRgb10ToPackedP010(ID3D11DeviceContext* context, const RgbYuvDrawKernels& kernels,
                              ID3D11ShaderResourceView* input, const PackedP010Targets& targets,
                              float lumaSharpenStrength, bool fullRange) {
    if (!context || !input || !HasPackedTargets(targets) || !kernels.constants)
        return false;
    const CopyConstants constants = HdrP010Constants(targets.width, targets.height, lumaSharpenStrength, fullRange);

    return DrawPackedPlanes(context, kernels, input, targets, kernels.p010ChromaShader, kernels.hdrPackShader,
                            constants);
}

}  // namespace ce::video_color

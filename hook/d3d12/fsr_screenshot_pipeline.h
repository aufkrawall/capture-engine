#pragma once

#include <d3d12.h>

#include <climits>

namespace ce::fsr_screenshot {
inline D3D12_GRAPHICS_PIPELINE_STATE_DESC CompositePipelineDescription() {
    // Disabled blend/depth/stencil stages still carry valid enum defaults.
    constexpr D3D12_RENDER_TARGET_BLEND_DESC target = {FALSE,
                                                       FALSE,
                                                       D3D12_BLEND_ONE,
                                                       D3D12_BLEND_ZERO,
                                                       D3D12_BLEND_OP_ADD,
                                                       D3D12_BLEND_ONE,
                                                       D3D12_BLEND_ZERO,
                                                       D3D12_BLEND_OP_ADD,
                                                       D3D12_LOGIC_OP_NOOP,
                                                       D3D12_COLOR_WRITE_ENABLE_ALL};
    constexpr D3D12_DEPTH_STENCILOP_DESC stencil = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                    D3D12_COMPARISON_FUNC_ALWAYS};
    return {
        .pRootSignature = nullptr,
        .VS = {},
        .PS = {},
        .DS = {},
        .HS = {},
        .GS = {},
        .StreamOutput = {},
        .BlendState = {FALSE, FALSE, {target, target, target, target, target, target, target, target}},
        .SampleMask = UINT_MAX,
        .RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID,
                            .CullMode = D3D12_CULL_MODE_NONE,
                            .FrontCounterClockwise = FALSE,
                            .DepthBias = 0,
                            .DepthBiasClamp = 0.0f,
                            .SlopeScaledDepthBias = 0.0f,
                            .DepthClipEnable = TRUE,
                            .MultisampleEnable = FALSE,
                            .AntialiasedLineEnable = FALSE,
                            .ForcedSampleCount = 0,
                            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF},
        .DepthStencilState = {.DepthEnable = FALSE,
                              .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO,
                              .DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
                              .StencilEnable = FALSE,
                              .StencilReadMask = 0,
                              .StencilWriteMask = 0,
                              .FrontFace = stencil,
                              .BackFace = stencil},
        .InputLayout = {},
        .IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
        .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        .NumRenderTargets = 1,
        .RTVFormats = {},
        .DSVFormat = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {1, 0},
        .NodeMask = 0,
        .CachedPSO = {},
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
}
}  // namespace ce::fsr_screenshot

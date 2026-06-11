// Temporal AA / temporal upscaling (TAAU) resolve for the FG test apps' all-FG-off mode.
//
// Consumes the render-resolution scene color + motion vectors produced with a jittered projection
// and accumulates into a display-resolution output (aux.hudlessColor) using a classic TAA resolve:
// un-jittered current sample, history reprojection via the UV-space motion vectors, 3x3
// neighborhood clamp, and an exponential history blend. At render == display size this is plain
// TAA; below display size it is a temporal upscaler. The history is a private display-resolution
// snapshot of the output (CopyResource after the resolve) so the output texture itself can keep
// serving frame generation / present-compose unchanged.
//
// Runtime-compiled HLSL like SceneRenderer (no offline shader pipeline in the test apps). The SRV
// descriptors are rewritten per frame into one of kDescriptorSets slot groups so descriptors the
// GPU may still read for in-flight frames are never overwritten.

#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>

#include "dx12_fg_resources.h"
#include "testapp_common.h"

namespace testapp::dx12fg {

using Microsoft::WRL::ComPtr;

class TemporalUpscaler {
public:
    static constexpr UINT kDescriptorSets = 4;  // >= swapchain buffer ceiling
    static constexpr UINT kSrvPerSet = 3;       // scene color, motion vectors, history

    bool valid() const {
        return valid_;
    }

    // colorFormat is the scene-color/output/history format (FP16 in the switch app: the temporal
    // accumulation must not quantize to 8 bit or slow gradients band).
    bool Initialize(ID3D12Device* device, DXGI_FORMAT colorFormat = kColorFormat);
    void Release();

    // Invalidates the accumulated history (call on mode switches / renderer re-init).
    void Reset() {
        historyValid_ = false;
    }

    // Resolves aux.sceneColor + aux.motionVectors (render res, both already in kColorReadState)
    // into aux.hudlessColor (display res) and snapshots the result into the private history.
    // Leaves hudlessColor in kColorReadState.
    void Render(ID3D12GraphicsCommandList* commandList, AuxiliaryResources& aux, UINT frameIndex, float jitterX,
                float jitterY);

private:
    bool EnsureHistory(UINT width, UINT height);

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12Resource> history_;
    D3D12_RESOURCE_STATES historyState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    DXGI_FORMAT colorFormat_ = kColorFormat;
    UINT srvStride_ = 0;
    UINT historyWidth_ = 0;
    UINT historyHeight_ = 0;
    bool historyValid_ = false;
    bool valid_ = false;
};

inline bool TemporalUpscaler::Initialize(ID3D12Device* device, DXGI_FORMAT colorFormat) {
    Release();
    if (!device) {
        return false;
    }
    device_ = device;
    colorFormat_ = colorFormat;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kSrvPerSet;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 8;  // renderSize, displaySize(unused slot kept), jitter, historyValid, blend
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof(params);
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = _countof(samplers);
    rsDesc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
    if (FAILED(hr) ||
        FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature_)))) {
        testapp::Log("[FG-DIAG] TemporalUpscaler: root signature failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "");
        return false;
    }

    static const char kShaderSource[] =
        "cbuffer TaaCB : register(b0) {\n"
        "    float2 renderSize;\n"
        "    float2 jitter;\n"        // pixels at render resolution
        "    float historyValid;\n"
        "    float blendFactor;\n"    // history weight
        "    float2 _pad;\n"
        "};\n"
        "Texture2D sceneTex : register(t0);\n"
        "Texture2D<float2> motionTex : register(t1);\n"
        "Texture2D historyTex : register(t2);\n"
        "SamplerState linearClamp : register(s0);\n"
        "SamplerState pointClamp : register(s1);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(VSOut i) : SV_Target0 {\n"
        "    float2 renderTexel = 1.0 / renderSize;\n"
        // The jittered projection shifts rendered content by +jitter texels in UV space, so the
        // un-jittered current-frame sample for this output pixel lives at uv + jitter*texel.
        "    float2 curUv = i.uv + jitter * renderTexel;\n"
        "    float3 current = sceneTex.SampleLevel(linearClamp, curUv, 0).rgb;\n"
        "    float3 nmin = current;\n"
        "    float3 nmax = current;\n"
        "    [unroll] for (int y = -1; y <= 1; ++y) {\n"
        "        [unroll] for (int x = -1; x <= 1; ++x) {\n"
        "            float3 c = sceneTex.SampleLevel(pointClamp, curUv + float2(x, y) * renderTexel, 0).rgb;\n"
        "            nmin = min(nmin, c);\n"
        "            nmax = max(nmax, c);\n"
        "        }\n"
        "    }\n"
        // Motion is UV-space prevUV - curUV (jitter-free), so the history sample sits at uv+motion.
        "    float2 motion = motionTex.SampleLevel(pointClamp, curUv, 0).rg;\n"
        "    float2 histUv = i.uv + motion;\n"
        "    float3 result = current;\n"
        "    if (historyValid > 0.5 && all(histUv >= 0.0) && all(histUv <= 1.0)) {\n"
        "        float3 hist = historyTex.SampleLevel(linearClamp, histUv, 0).rgb;\n"
        "        hist = clamp(hist, nmin, nmax);\n"
        "        result = lerp(current, hist, blendFactor);\n"
        "    }\n"
        "    return float4(result, 1.0);\n"
        "}\n";

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    ComPtr<ID3DBlob> error;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fg_taa", nullptr, nullptr, "VSMain", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error);
    if (SUCCEEDED(hr)) {
        error.Reset();
        hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fg_taa", nullptr, nullptr, "PSMain", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error);
    }
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] TemporalUpscaler: shader compile failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     error ? static_cast<const char*>(error->GetBufferPointer()) : "");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = colorFormat_;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    for (auto& renderTarget : psoDesc.BlendState.RenderTarget) {
        renderTarget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] TemporalUpscaler: PSO failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kDescriptorSets * kSrvPerSet;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)))) {
        testapp::Log("[FG-DIAG] TemporalUpscaler: SRV heap failed\n");
        return false;
    }
    srvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    valid_ = true;
    historyValid_ = false;
    testapp::Log("[FG-DIAG] TemporalUpscaler initialized (descriptorSets=%u)\n", kDescriptorSets);
    return true;
}

inline bool TemporalUpscaler::EnsureHistory(UINT width, UINT height) {
    if (history_ && historyWidth_ == width && historyHeight_ == height) {
        return true;
    }
    history_.Reset();
    historyValid_ = false;
    const D3D12_HEAP_PROPERTIES heapProps = DefaultHeapProperties();
    D3D12_RESOURCE_DESC desc = Texture2DDesc(colorFormat_, width, height, D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&history_)))) {
        testapp::Log("[FG-DIAG] TemporalUpscaler: history texture %ux%u failed\n", width, height);
        return false;
    }
    historyState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    historyWidth_ = width;
    historyHeight_ = height;
    testapp::Log("[FG-DIAG] TemporalUpscaler history created %ux%u\n", width, height);
    return true;
}

inline void TemporalUpscaler::Render(ID3D12GraphicsCommandList* commandList, AuxiliaryResources& aux, UINT frameIndex,
                                     float jitterX, float jitterY) {
    if (!valid_ || !commandList || !aux.valid || !EnsureHistory(aux.displayWidth, aux.displayHeight)) {
        return;
    }

    const UINT slot = (frameIndex % kDescriptorSets) * kSrvPerSet;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    cpuBase.ptr += static_cast<SIZE_T>(slot) * srvStride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv = {};
    colorSrv.Format = colorFormat_;
    colorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrv.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC motionSrv = colorSrv;
    motionSrv.Format = kMotionVectorFormat;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuBase;
    device_->CreateShaderResourceView(aux.sceneColor.Get(), &colorSrv, handle);
    handle.ptr += srvStride_;
    device_->CreateShaderResourceView(aux.motionVectors.Get(), &motionSrv, handle);
    handle.ptr += srvStride_;
    device_->CreateShaderResourceView(history_.Get(), &colorSrv, handle);

    Transition(commandList, aux.hudlessColor.Get(), aux.hudlessState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(commandList, history_.Get(), historyState_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = aux.HudlessRtv();
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(aux.displayWidth),
                               static_cast<float>(aux.displayHeight), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(aux.displayWidth), static_cast<LONG>(aux.displayHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetPipelineState(pso_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    gpuBase.ptr += static_cast<UINT64>(slot) * srvStride_;
    commandList->SetGraphicsRootDescriptorTable(0, gpuBase);
    const float constants[8] = {static_cast<float>(aux.renderWidth), static_cast<float>(aux.renderHeight),
                                jitterX, jitterY, historyValid_ ? 1.0f : 0.0f, 0.9f, 0.0f, 0.0f};
    commandList->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    // Snapshot the resolve into the private history for the next frame's reprojection.
    Transition(commandList, aux.hudlessColor.Get(), aux.hudlessState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(commandList, history_.Get(), historyState_, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(history_.Get(), aux.hudlessColor.Get());
    Transition(commandList, history_.Get(), historyState_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, aux.hudlessColor.Get(), aux.hudlessState, kColorReadState);
    historyValid_ = true;
}

inline void TemporalUpscaler::Release() {
    history_.Reset();
    srvHeap_.Reset();
    pso_.Reset();
    rootSignature_.Reset();
    device_.Reset();
    historyState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    srvStride_ = 0;
    historyWidth_ = 0;
    historyHeight_ = 0;
    historyValid_ = false;
    valid_ = false;
}

// Present blit: samples the (FP16) display-res hudless color into the 8-bit backbuffer with a
// per-pixel temporal dither, replacing the old CopyResource compose. The dither decorrelates the
// final 8-bit quantization so smooth gradients (which the FP16 temporal chain preserves) do not
// band on screen; values pass through otherwise unchanged (no tonemap/gamma), so the image matches
// the previous copy-based compose.
class PresentBlitPass {
public:
    static constexpr UINT kDescriptorSets = 4;

    bool valid() const {
        return valid_;
    }

    bool Initialize(ID3D12Device* device, DXGI_FORMAT sourceFormat, DXGI_FORMAT targetFormat = kColorFormat);
    void Release();

    // Source must be in kColorReadState; the target RTV must already be bound-able
    // (RENDER_TARGET state is the caller's responsibility).
    void Render(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, UINT width, UINT height, UINT frameIndex,
                uint32_t ditherSeed);

private:
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    DXGI_FORMAT sourceFormat_ = kColorFormat;
    UINT srvStride_ = 0;
    bool valid_ = false;
};

inline bool PresentBlitPass::Initialize(ID3D12Device* device, DXGI_FORMAT sourceFormat, DXGI_FORMAT targetFormat) {
    Release();
    if (!device) {
        return false;
    }
    device_ = device;
    sourceFormat_ = sourceFormat;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4;  // ditherSeed, unused x3
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof(params);
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
    if (FAILED(hr) ||
        FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature_)))) {
        testapp::Log("[FG-DIAG] PresentBlitPass: root signature failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "");
        return false;
    }

    static const char kShaderSource[] =
        "cbuffer BlitCB : register(b0) { uint ditherSeed; uint3 _pad; };\n"
        "Texture2D sourceTex : register(t0);\n"
        "SamplerState pointClamp : register(s0);\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n"
        "float Hash(float2 p) {\n"
        "    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);\n"
        "}\n"
        "float4 PSMain(VSOut i) : SV_Target0 {\n"
        "    float3 color = sourceTex.SampleLevel(pointClamp, i.uv, 0).rgb;\n"
        // +-0.5 LSB temporal dither decorrelates the 8-bit quantization (no visible banding).
        "    float dither = (Hash(i.pos.xy + float2(ditherSeed & 1023u, (ditherSeed >> 10) & 1023u)) - 0.5)\n"
        "                   / 255.0;\n"
        "    return float4(saturate(color + dither), 1.0);\n"
        "}\n";

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    ComPtr<ID3DBlob> error;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fg_blit", nullptr, nullptr, "VSMain", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error);
    if (SUCCEEDED(hr)) {
        error.Reset();
        hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fg_blit", nullptr, nullptr, "PSMain", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error);
    }
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] PresentBlitPass: shader compile failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     error ? static_cast<const char*>(error->GetBufferPointer()) : "");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = targetFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    for (auto& renderTarget : psoDesc.BlendState.RenderTarget) {
        renderTarget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] PresentBlitPass: PSO failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kDescriptorSets;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)))) {
        testapp::Log("[FG-DIAG] PresentBlitPass: SRV heap failed\n");
        return false;
    }
    srvStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    valid_ = true;
    return true;
}

inline void PresentBlitPass::Render(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                                    D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, UINT width, UINT height, UINT frameIndex,
                                    uint32_t ditherSeed) {
    if (!valid_ || !commandList || !source) {
        return;
    }
    const UINT slot = frameIndex % kDescriptorSets;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(slot) * srvStride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = sourceFormat_;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(source, &srvDesc, cpuHandle);

    commandList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetPipelineState(pso_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<UINT64>(slot) * srvStride_;
    commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);
    const uint32_t constants[4] = {ditherSeed, 0, 0, 0};
    commandList->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

inline void PresentBlitPass::Release() {
    srvHeap_.Reset();
    pso_.Reset();
    rootSignature_.Reset();
    device_.Reset();
    sourceFormat_ = kColorFormat;
    srvStride_ = 0;
    valid_ = false;
}

}  // namespace testapp::dx12fg

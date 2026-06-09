// Shared spinning-cube scene + motion-vector renderer for the DX12 frame-generation test apps.
//
// Renders a real 3D cube into the hud-less color target (SV_Target0) and the motion-vector
// target (SV_Target1) with depth, so frame generation can interpolate the scene (the cube
// runs at the generated/output frame rate, e.g. 2x under FSR/DLSS FG). The HUD/UI is drawn
// separately into the UI-layer texture by the app and excluded from FG via the runtime UI
// resource, so it animates at the base (rendered) frame rate without ghosting.
//
// The cube emits per-pixel motion vectors (prevUV - curUV, matching the test apps' existing
// motion-vector sign convention with motionVectorScale = {1,1}) so the interpolated cube does
// not ghost. Geometry/constant buffers live in upload heaps for simplicity (small static mesh).
//
// Matrices are hand-rolled row-major (row-vector convention, v' = v * M) to avoid a
// DirectXMath dependency, which is fragile on the project's MinGW/clang toolchain. The HLSL
// cbuffer therefore declares the matrices as row_major.

#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "dx12_fg_resources.h"
#include "testapp_common.h"

namespace testapp::dx12fg {

using Microsoft::WRL::ComPtr;

struct Mat4 {
    float m[16];
};

inline Mat4 Mat4Identity() {
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 Mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = s;
        }
    }
    return r;
}

inline Mat4 Mat4RotationX(float angle) {
    Mat4 r = Mat4Identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

inline Mat4 Mat4RotationY(float angle) {
    Mat4 r = Mat4Identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

inline Mat4 Mat4Translation(float x, float y, float z) {
    Mat4 r = Mat4Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

// Left-handed perspective (DirectX convention, row-vector / row-major).
inline Mat4 Mat4PerspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;
    Mat4 r = {};
    r.m[0] = xScale;
    r.m[5] = yScale;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}

struct CubeSceneConstants {
    float curMVP[16];   // row-major (HLSL declares row_major)
    float prevMVP[16];  // previous rendered-frame MVP for per-pixel motion vectors
};

class CubeScene {
public:
    static constexpr UINT kFrameCount = 4;  // >= kMaxSwapChainBuffers ceiling

    bool valid() const {
        return valid_;
    }

    bool Initialize(ID3D12Device* device);
    // Renders the cube into aux.hudlessColor (SV_Target0) + aux.motionVectors (SV_Target1) with
    // aux.depth as the depth target. The caller must have transitioned those targets to
    // RENDER_TARGET / DEPTH_WRITE and cleared them first. cbIndex selects a per-frame CB slot.
    void Render(ID3D12GraphicsCommandList* commandList, const AuxiliaryResources& aux, UINT cbIndex, int width,
                int height, float timeSeconds);
    void Release();

private:
    static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 size, const void* initialData);

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    ComPtr<ID3D12Resource> indexBuffer_;
    ComPtr<ID3D12Resource> constantBuffers_[kFrameCount];
    uint8_t* constantPtr_[kFrameCount] = {};
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_ = {};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_ = {};
    UINT indexCount_ = 0;
    Mat4 prevMVP_ = {};
    bool hasPrev_ = false;
    bool valid_ = false;
};

inline ComPtr<ID3D12Resource> CubeScene::CreateUploadBuffer(ID3D12Device* device, UINT64 size,
                                                            const void* initialData) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    if (initialData) {
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        if (SUCCEEDED(resource->Map(0, &readRange, &mapped)) && mapped) {
            std::memcpy(mapped, initialData, static_cast<size_t>(size));
            resource->Unmap(0, nullptr);
        }
    }
    return resource;
}

inline bool CubeScene::Initialize(ID3D12Device* device) {
    Release();
    if (!device) {
        return false;
    }

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] CubeScene: D3D12SerializeRootSignature failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "");
        return false;
    }
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&rootSignature_));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] CubeScene: CreateRootSignature failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    static const char kShaderSource[] =
        "cbuffer SceneCB : register(b0) {\n"
        "    row_major float4x4 curMVP;\n"
        "    row_major float4x4 prevMVP;\n"
        "};\n"
        "struct VSIn { float3 pos : POSITION; float3 color : COLOR; };\n"
        "struct VSOut {\n"
        "    float4 pos : SV_Position;\n"
        "    float3 color : COLOR;\n"
        "    float4 curClip : TEXCOORD0;\n"
        "    float4 prevClip : TEXCOORD1;\n"
        "};\n"
        "struct PSOut { float4 color : SV_Target0; float2 motion : SV_Target1; };\n"
        "VSOut VSMain(VSIn input) {\n"
        "    VSOut output;\n"
        "    float4 p = float4(input.pos, 1.0);\n"
        "    output.pos = mul(p, curMVP);\n"
        "    output.curClip = output.pos;\n"
        "    output.prevClip = mul(p, prevMVP);\n"
        "    output.color = input.color;\n"
        "    return output;\n"
        "}\n"
        "PSOut PSMain(VSOut input) {\n"
        "    PSOut output;\n"
        "    float2 curNdc = input.curClip.xy / input.curClip.w;\n"
        "    float2 prevNdc = input.prevClip.xy / input.prevClip.w;\n"
        "    float2 curUV = float2(curNdc.x * 0.5 + 0.5, -curNdc.y * 0.5 + 0.5);\n"
        "    float2 prevUV = float2(prevNdc.x * 0.5 + 0.5, -prevNdc.y * 0.5 + 0.5);\n"
        "    output.color = float4(input.color, 1.0);\n"
        "    output.motion = prevUV - curUV;\n"
        "    return output;\n"
        "}\n";

    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> compileError;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "cube_scene", nullptr, nullptr, "VSMain", "vs_5_0",
                    compileFlags, 0, &vsBlob, &compileError);
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] CubeScene: VS compile failed hr=0x%08lx %s\n", static_cast<unsigned long>(hr),
                     compileError ? static_cast<const char*>(compileError->GetBufferPointer()) : "");
        return false;
    }
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "cube_scene", nullptr, nullptr, "PSMain", "ps_5_0",
                    compileFlags, 0, &psBlob, &compileError);
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] CubeScene: PS compile failed hr=0x%08lx %s\n", static_cast<unsigned long>(hr),
                     compileError ? static_cast<const char*>(compileError->GetBufferPointer()) : "");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 2;
    psoDesc.RTVFormats[0] = kColorFormat;
    psoDesc.RTVFormats[1] = kMotionVectorFormat;
    psoDesc.DSVFormat = kDepthFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    for (auto& renderTarget : psoDesc.BlendState.RenderTarget) {
        renderTarget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState_));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] CubeScene: CreateGraphicsPipelineState failed hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    struct Vertex {
        float px, py, pz;
        float r, g, b;
    };
    static const Vertex kVertices[] = {
        {-0.5f, -0.5f, -0.5f, 1.0f, 0.15f, 0.15f}, {0.5f, -0.5f, -0.5f, 0.15f, 1.0f, 0.15f},
        {0.5f, 0.5f, -0.5f, 0.15f, 0.35f, 1.0f},   {-0.5f, 0.5f, -0.5f, 1.0f, 0.9f, 0.15f},
        {-0.5f, -0.5f, 0.5f, 1.0f, 0.15f, 1.0f},   {0.5f, -0.5f, 0.5f, 0.15f, 1.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 0.95f, 0.95f, 0.95f},   {-0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f},
    };
    static const uint16_t kIndices[] = {
        0, 1, 2, 0, 2, 3,  // back
        4, 6, 5, 4, 7, 6,  // front
        4, 5, 1, 4, 1, 0,  // bottom
        3, 2, 6, 3, 6, 7,  // top
        4, 0, 3, 4, 3, 7,  // left
        1, 5, 6, 1, 6, 2,  // right
    };
    indexCount_ = _countof(kIndices);

    vertexBuffer_ = CreateUploadBuffer(device, sizeof(kVertices), kVertices);
    indexBuffer_ = CreateUploadBuffer(device, sizeof(kIndices), kIndices);
    if (!vertexBuffer_ || !indexBuffer_) {
        testapp::Log("[FG-DIAG] CubeScene: failed to create vertex/index buffers\n");
        return false;
    }
    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.SizeInBytes = sizeof(kVertices);
    vertexBufferView_.StrideInBytes = sizeof(Vertex);
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.SizeInBytes = sizeof(kIndices);
    indexBufferView_.Format = DXGI_FORMAT_R16_UINT;

    const UINT cbSize = (sizeof(CubeSceneConstants) + 255u) & ~255u;
    for (UINT i = 0; i < kFrameCount; ++i) {
        constantBuffers_[i] = CreateUploadBuffer(device, cbSize, nullptr);
        if (!constantBuffers_[i]) {
            testapp::Log("[FG-DIAG] CubeScene: failed to create constant buffer %u\n", i);
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        if (FAILED(constantBuffers_[i]->Map(0, &readRange, &mapped)) || !mapped) {
            testapp::Log("[FG-DIAG] CubeScene: failed to map constant buffer %u\n", i);
            return false;
        }
        constantPtr_[i] = static_cast<uint8_t*>(mapped);
    }

    valid_ = true;
    testapp::Log("[FG-DIAG] CubeScene initialized indices=%u cbSize=%u\n", indexCount_, cbSize);
    return true;
}

inline void CubeScene::Render(ID3D12GraphicsCommandList* commandList, const AuxiliaryResources& aux, UINT cbIndex,
                              int width, int height, float timeSeconds) {
    if (!valid_ || !commandList || width <= 0 || height <= 0) {
        return;
    }
    if (cbIndex >= kFrameCount) {
        cbIndex %= kFrameCount;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 model =
        Mat4Mul(Mat4Mul(Mat4RotationX(timeSeconds * 0.7f), Mat4RotationY(timeSeconds * 0.9f)),
                Mat4Translation(0.6f * std::sin(timeSeconds * 0.8f), 0.0f, 0.0f));
    const Mat4 view = Mat4Translation(0.0f, 0.0f, 3.0f);  // camera at (0,0,-3) looking +Z (LH)
    const Mat4 proj = Mat4PerspectiveFovLH(0.7853982f, aspect, 0.1f, 1000.0f);
    const Mat4 mvp = Mat4Mul(Mat4Mul(model, view), proj);

    CubeSceneConstants constants;
    std::memcpy(constants.curMVP, mvp.m, sizeof(mvp.m));
    std::memcpy(constants.prevMVP, (hasPrev_ ? prevMVP_.m : mvp.m), sizeof(mvp.m));
    std::memcpy(constantPtr_[cbIndex], &constants, sizeof(constants));
    prevMVP_ = mvp;
    hasPrev_ = true;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {aux.HudlessRtv(), aux.MotionRtv()};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = aux.DepthDsv();
    commandList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetPipelineState(pipelineState_.Get());
    commandList->SetGraphicsRootConstantBufferView(0, constantBuffers_[cbIndex]->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList->IASetIndexBuffer(&indexBufferView_);
    commandList->DrawIndexedInstanced(indexCount_, 1, 0, 0, 0);
}

inline void CubeScene::Release() {
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (constantBuffers_[i] && constantPtr_[i]) {
            constantBuffers_[i]->Unmap(0, nullptr);
        }
        constantPtr_[i] = nullptr;
        constantBuffers_[i].Reset();
    }
    vertexBuffer_.Reset();
    indexBuffer_.Reset();
    pipelineState_.Reset();
    rootSignature_.Reset();
    vertexBufferView_ = {};
    indexBufferView_ = {};
    indexCount_ = 0;
    hasPrev_ = false;
    valid_ = false;
}

}  // namespace testapp::dx12fg

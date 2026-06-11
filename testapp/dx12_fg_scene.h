// Shared 3D scene renderer for the DX12 frame-generation test apps.
//
// Renders a small "real game" scene into the hud-less color target (SV_Target0) and the
// motion-vector target (SV_Target1) with depth:
//   - a sky gradient (fullscreen),
//   - a procedural checkerboard ground plane receding to the horizon with distance fog
//     (spatial depth), and
//   - a lit cube that moves/rotates within the scene.
// The camera is static, so only the cube carries motion vectors; the floor/sky are static
// (zero motion) which is correct and avoids ghosting there. The cube's motion vectors are
// emitted in UV space (prevUV - curUV); the caller scales them per runtime (FSR wants
// renderSize, DLSS wants ~{2,-2} to reach NDC [-1,1]). Frame generation therefore interpolates
// the moving cube to the output rate (e.g. ~2x under FSR/DLSS FG) while the HUD, drawn into the
// UI-layer texture by the app, stays at the base rate without ghosting.
//
// Geometry/constant buffers live in upload heaps (small static mesh). Per-face cube normals are
// computed in the pixel shader via screen-space derivatives, so no normal attribute is needed.
// Matrices are hand-rolled row-major (row-vector, v' = v * M) to avoid DirectXMath, which is
// fragile on the project's MinGW/clang toolchain; the HLSL cbuffer declares them row_major.

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

// Analytic inverse of Mat4PerspectiveFovLH (clip -> view for the row-vector convention):
// view.x = clip.x/xScale, view.y = clip.y/yScale, view.z = clip.w, and the homogeneous column
// reconstructs 1 from clip.z*1/m14 + clip.w*(-m10/m14).
inline Mat4 Mat4PerspectiveFovLHInverse(const Mat4& proj) {
    Mat4 r = {};
    r.m[0] = 1.0f / proj.m[0];
    r.m[5] = 1.0f / proj.m[5];
    r.m[11] = 1.0f / proj.m[14];
    r.m[14] = 1.0f;
    r.m[15] = -proj.m[10] / proj.m[14];
    return r;
}

// Applies a sub-pixel camera jitter (in pixels at render resolution) to a projection matrix:
// clip.x += w * 2*jx/renderW and clip.y += w * -2*jy/renderH (w == view z for this projection),
// the standard FSR/DLSS jittered-projection convention.
inline Mat4 Mat4ApplyJitter(const Mat4& proj, float jitterX, float jitterY, int renderWidth, int renderHeight) {
    Mat4 r = proj;
    if (renderWidth > 0 && renderHeight > 0) {
        r.m[8] += 2.0f * jitterX / static_cast<float>(renderWidth);
        r.m[9] += -2.0f * jitterY / static_cast<float>(renderHeight);
    }
    return r;
}

// Orthonormal camera basis (right/up/forward) for a left-handed look-at; shared by the view matrix
// and the upscaler camera constants (Streamline sl::Constants, FFX dispatch camera vectors).
struct CameraBasis {
    float eye[3];
    float right[3];
    float up[3];
    float forward[3];
};

inline CameraBasis ComputeLookAtBasisLH(float ex, float ey, float ez, float ax, float ay, float az, float ux, float uy,
                                        float uz) {
    float zx = ax - ex, zy = ay - ey, zz = az - ez;
    float zl = std::sqrt(zx * zx + zy * zy + zz * zz);
    zx /= zl;
    zy /= zl;
    zz /= zl;
    float xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
    float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
    xx /= xl;
    xy /= xl;
    xz /= xl;
    float yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    return {{ex, ey, ez}, {xx, xy, xz}, {yx, yy, yz}, {zx, zy, zz}};
}

// Left-handed look-at (DirectX convention, row-vector / row-major).
inline Mat4 Mat4LookAtLH(float ex, float ey, float ez, float ax, float ay, float az, float ux, float uy, float uz) {
    const CameraBasis b = ComputeLookAtBasisLH(ex, ey, ez, ax, ay, az, ux, uy, uz);
    Mat4 r = Mat4Identity();
    r.m[0] = b.right[0];
    r.m[1] = b.up[0];
    r.m[2] = b.forward[0];
    r.m[4] = b.right[1];
    r.m[5] = b.up[1];
    r.m[6] = b.forward[1];
    r.m[8] = b.right[2];
    r.m[9] = b.up[2];
    r.m[10] = b.forward[2];
    r.m[12] = -(b.right[0] * ex + b.right[1] * ey + b.right[2] * ez);
    r.m[13] = -(b.up[0] * ex + b.up[1] * ey + b.up[2] * ez);
    r.m[14] = -(b.forward[0] * ex + b.forward[1] * ey + b.forward[2] * ez);
    return r;
}

struct SceneConstants {
    float curModel[16];   // row-major
    float prevModel[16];  // previous-frame model for per-pixel motion vectors
    float viewProj[16];   // shared camera view*proj
    float params[4];      // x = material id (0 cube, 1 floor), y = fog far distance
};

// Camera state of the last rendered frame, exposed for upscaler constants (Streamline
// sl::Constants matrices/vectors, FFX dispatch camera fields). proj/projInverse are UNJITTERED.
struct SceneCamera {
    CameraBasis basis = {};
    Mat4 view = {};
    Mat4 proj = {};
    Mat4 projInverse = {};
    float fovY = 0.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    float aspect = 0.0f;
    bool valid = false;
};

class SceneRenderer {
public:
    static constexpr UINT kFrameCount = 4;  // >= kMaxSwapChainBuffers ceiling
    static constexpr UINT kObjectsPerFrame = 2;  // floor + cube

    bool valid() const {
        return valid_;
    }

    // colorFormat is the SV_Target0 (scene color) format; motion/depth formats are fixed.
    bool Initialize(ID3D12Device* device, DXGI_FORMAT colorFormat = kColorFormat);
    // Renders sky + floor + cube into the bound color target (SV_Target0) + aux.motionVectors
    // (SV_Target1) with aux.depth at render resolution width x height. The caller must have
    // transitioned those targets to RENDER_TARGET / DEPTH_WRITE and cleared depth (to 1.0) and
    // motion (to 0) first. jitterX/jitterY are the sub-pixel camera jitter in pixels; the SAME
    // jittered viewProj is used for current and previous clip positions, so the UV-space motion
    // vectors (prevUV - curUV) cancel the jitter exactly and stay jitter-free.
    void Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
                const AuxiliaryResources& aux, UINT frameIndex, int width, int height, float timeSeconds,
                float jitterX = 0.0f, float jitterY = 0.0f);
    void Release();

    const SceneCamera& Camera() const {
        return camera_;
    }

private:
    static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 size, const void* initialData);
    bool CreatePipeline(ID3D12Device* device, const void* vs, size_t vsSize, const void* ps, size_t psSize,
                        bool depthEnable, bool hasInputLayout, ComPtr<ID3D12PipelineState>& outPso);

    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> skyPso_;
    ComPtr<ID3D12PipelineState> scenePso_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    ComPtr<ID3D12Resource> indexBuffer_;
    ComPtr<ID3D12Resource> constantBuffers_[kFrameCount * kObjectsPerFrame];
    uint8_t* constantPtr_[kFrameCount * kObjectsPerFrame] = {};
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_ = {};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_ = {};
    UINT floorIndexCount_ = 0;
    UINT cubeIndexCount_ = 0;
    UINT cubeBaseVertex_ = 0;
    Mat4 prevCubeModel_ = {};
    SceneCamera camera_ = {};
    DXGI_FORMAT colorFormat_ = kColorFormat;
    bool hasPrev_ = false;
    bool valid_ = false;
};

inline ComPtr<ID3D12Resource> SceneRenderer::CreateUploadBuffer(ID3D12Device* device, UINT64 size,
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

inline bool SceneRenderer::CreatePipeline(ID3D12Device* device, const void* vs, size_t vsSize, const void* ps,
                                          size_t psSize, bool depthEnable, bool hasInputLayout,
                                          ComPtr<ID3D12PipelineState>& outPso) {
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.VS = {vs, vsSize};
    psoDesc.PS = {ps, psSize};
    if (hasInputLayout) {
        psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    }
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 2;
    psoDesc.RTVFormats[0] = colorFormat_;
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
    psoDesc.DepthStencilState.DepthEnable = depthEnable ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = depthEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] SceneRenderer: CreateGraphicsPipelineState failed hr=0x%08lx depth=%d\n",
                     static_cast<unsigned long>(hr), depthEnable ? 1 : 0);
        return false;
    }
    return true;
}

inline bool SceneRenderer::Initialize(ID3D12Device* device, DXGI_FORMAT colorFormat) {
    Release();
    if (!device) {
        return false;
    }
    colorFormat_ = colorFormat;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] SceneRenderer: SerializeRootSignature failed hr=0x%08lx %s\n",
                     static_cast<unsigned long>(hr),
                     rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "");
        return false;
    }
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&rootSignature_));
    if (FAILED(hr)) {
        testapp::Log("[FG-DIAG] SceneRenderer: CreateRootSignature failed hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    static const char kShaderSource[] =
        "cbuffer SceneCB : register(b0) {\n"
        "    row_major float4x4 curModel;\n"
        "    row_major float4x4 prevModel;\n"
        "    row_major float4x4 viewProj;\n"
        "    float4 params;\n"  // x = material id, y = fog far
        "};\n"
        "struct SkyOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "struct MrtOut { float4 color : SV_Target0; float2 motion : SV_Target1; };\n"
        "SkyOut VSSky(uint id : SV_VertexID) {\n"
        "    SkyOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n"
        "MrtOut PSSky(SkyOut i) {\n"
        "    MrtOut o;\n"
        "    float3 top = float3(0.16, 0.28, 0.55);\n"
        "    float3 horizon = float3(0.58, 0.66, 0.80);\n"
        "    o.color = float4(lerp(top, horizon, saturate(i.uv.y)), 1.0);\n"
        "    o.motion = float2(0.0, 0.0);\n"
        "    return o;\n"
        "}\n"
        "struct VSIn { float3 pos : POSITION; float3 color : COLOR; };\n"
        "struct VSOut {\n"
        "    float4 pos : SV_Position;\n"
        "    float3 color : COLOR;\n"
        "    float3 worldPos : TEXCOORD0;\n"
        "    float4 curClip : TEXCOORD1;\n"
        "    float4 prevClip : TEXCOORD2;\n"
        "};\n"
        "VSOut VSScene(VSIn input) {\n"
        "    VSOut o;\n"
        "    float4 world = mul(float4(input.pos, 1.0), curModel);\n"
        "    o.worldPos = world.xyz;\n"
        "    o.pos = mul(world, viewProj);\n"
        "    o.curClip = o.pos;\n"
        "    o.prevClip = mul(mul(float4(input.pos, 1.0), prevModel), viewProj);\n"
        "    o.color = input.color;\n"
        "    return o;\n"
        "}\n"
        "MrtOut PSScene(VSOut i) {\n"
        "    MrtOut o;\n"
        "    float3 n = normalize(cross(ddx(i.worldPos), ddy(i.worldPos)));\n"
        "    float3 L = normalize(float3(0.45, 0.8, -0.35));\n"
        "    float ndl = saturate(dot(n, L));\n"
        "    float3 albedo = i.color;\n"
        "    if (params.x > 0.5) {\n"
        "        float2 cell = floor(i.worldPos.xz * 0.5);\n"
        "        float checker = frac((cell.x + cell.y) * 0.5) * 2.0;\n"
        "        albedo = lerp(float3(0.16, 0.18, 0.22), float3(0.32, 0.35, 0.42), checker);\n"
        "    }\n"
        "    float3 lit = albedo * (0.28 + 0.85 * ndl);\n"
        "    float fog = saturate((i.curClip.w - 8.0) / params.y);\n"
        "    lit = lerp(lit, float3(0.58, 0.66, 0.80), fog);\n"
        "    o.color = float4(lit, 1.0);\n"
        "    float2 curUV = float2(i.curClip.x / i.curClip.w * 0.5 + 0.5, -i.curClip.y / i.curClip.w * 0.5 + 0.5);\n"
        "    float2 prevUV = float2(i.prevClip.x / i.prevClip.w * 0.5 + 0.5, -i.prevClip.y / i.prevClip.w * 0.5 + 0.5);\n"
        "    o.motion = prevUV - curUV;\n"
        "    return o;\n"
        "}\n";

    struct ShaderBlob {
        const char* entry;
        const char* target;
        ComPtr<ID3DBlob> blob;
    };
    ShaderBlob shaders[] = {
        {"VSSky", "vs_5_0", {}},
        {"PSSky", "ps_5_0", {}},
        {"VSScene", "vs_5_0", {}},
        {"PSScene", "ps_5_0", {}},
    };
    for (auto& shader : shaders) {
        ComPtr<ID3DBlob> error;
        hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "fg_scene", nullptr, nullptr, shader.entry,
                        shader.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader.blob, &error);
        if (FAILED(hr)) {
            testapp::Log("[FG-DIAG] SceneRenderer: %s compile failed hr=0x%08lx %s\n", shader.entry,
                         static_cast<unsigned long>(hr),
                         error ? static_cast<const char*>(error->GetBufferPointer()) : "");
            return false;
        }
    }

    if (!CreatePipeline(device, shaders[0].blob->GetBufferPointer(), shaders[0].blob->GetBufferSize(),
                        shaders[1].blob->GetBufferPointer(), shaders[1].blob->GetBufferSize(), /*depth*/ false,
                        /*inputLayout*/ false, skyPso_) ||
        !CreatePipeline(device, shaders[2].blob->GetBufferPointer(), shaders[2].blob->GetBufferSize(),
                        shaders[3].blob->GetBufferPointer(), shaders[3].blob->GetBufferSize(), /*depth*/ true,
                        /*inputLayout*/ true, scenePso_)) {
        return false;
    }

    struct Vertex {
        float px, py, pz;
        float r, g, b;
    };
    static const Vertex kVertices[] = {
        // Floor quad (matId selects checker in the shader; vertex color unused).
        {-80.0f, 0.0f, -24.0f, 0.0f, 0.0f, 0.0f},
        {80.0f, 0.0f, -24.0f, 0.0f, 0.0f, 0.0f},
        {80.0f, 0.0f, 200.0f, 0.0f, 0.0f, 0.0f},
        {-80.0f, 0.0f, 200.0f, 0.0f, 0.0f, 0.0f},
        // Cube (side ~1.1, warm color; lit by per-face normal in the shader).
        {-0.55f, -0.55f, -0.55f, 0.95f, 0.55f, 0.22f},
        {0.55f, -0.55f, -0.55f, 0.95f, 0.55f, 0.22f},
        {0.55f, 0.55f, -0.55f, 0.95f, 0.55f, 0.22f},
        {-0.55f, 0.55f, -0.55f, 0.95f, 0.55f, 0.22f},
        {-0.55f, -0.55f, 0.55f, 0.95f, 0.55f, 0.22f},
        {0.55f, -0.55f, 0.55f, 0.95f, 0.55f, 0.22f},
        {0.55f, 0.55f, 0.55f, 0.95f, 0.55f, 0.22f},
        {-0.55f, 0.55f, 0.55f, 0.95f, 0.55f, 0.22f},
    };
    static const uint16_t kIndices[] = {
        // Floor (verts 0..3).
        0, 1, 2, 0, 2, 3,
        // Cube (0-based; drawn with BaseVertexLocation = 4).
        0, 1, 2, 0, 2, 3,  // back
        4, 6, 5, 4, 7, 6,  // front
        4, 5, 1, 4, 1, 0,  // bottom
        3, 2, 6, 3, 6, 7,  // top
        4, 0, 3, 4, 3, 7,  // left
        1, 5, 6, 1, 6, 2,  // right
    };
    floorIndexCount_ = 6;
    cubeIndexCount_ = _countof(kIndices) - 6;
    cubeBaseVertex_ = 4;

    vertexBuffer_ = CreateUploadBuffer(device, sizeof(kVertices), kVertices);
    indexBuffer_ = CreateUploadBuffer(device, sizeof(kIndices), kIndices);
    if (!vertexBuffer_ || !indexBuffer_) {
        testapp::Log("[FG-DIAG] SceneRenderer: failed to create vertex/index buffers\n");
        return false;
    }
    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.SizeInBytes = sizeof(kVertices);
    vertexBufferView_.StrideInBytes = sizeof(Vertex);
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.SizeInBytes = sizeof(kIndices);
    indexBufferView_.Format = DXGI_FORMAT_R16_UINT;

    const UINT cbSize = (sizeof(SceneConstants) + 255u) & ~255u;
    for (UINT i = 0; i < kFrameCount * kObjectsPerFrame; ++i) {
        constantBuffers_[i] = CreateUploadBuffer(device, cbSize, nullptr);
        if (!constantBuffers_[i]) {
            testapp::Log("[FG-DIAG] SceneRenderer: failed to create constant buffer %u\n", i);
            return false;
        }
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        if (FAILED(constantBuffers_[i]->Map(0, &readRange, &mapped)) || !mapped) {
            testapp::Log("[FG-DIAG] SceneRenderer: failed to map constant buffer %u\n", i);
            return false;
        }
        constantPtr_[i] = static_cast<uint8_t*>(mapped);
    }

    valid_ = true;
    testapp::Log("[FG-DIAG] SceneRenderer initialized floorIdx=%u cubeIdx=%u cbSize=%u\n", floorIndexCount_,
                 cubeIndexCount_, cbSize);
    return true;
}

inline void SceneRenderer::Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
                                  const AuxiliaryResources& aux, UINT frameIndex, int width, int height,
                                  float timeSeconds, float jitterX, float jitterY) {
    if (!valid_ || !commandList || width <= 0 || height <= 0) {
        return;
    }
    if (frameIndex >= kFrameCount) {
        frameIndex %= kFrameCount;
    }

    constexpr float kFovY = 1.04719755f;
    constexpr float kNearZ = 0.1f;
    constexpr float kFarZ = 1000.0f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    camera_.basis = ComputeLookAtBasisLH(0.0f, 2.4f, -5.5f, 0.0f, 0.7f, 1.4f, 0.0f, 1.0f, 0.0f);
    camera_.view = Mat4LookAtLH(0.0f, 2.4f, -5.5f, 0.0f, 0.7f, 1.4f, 0.0f, 1.0f, 0.0f);
    camera_.proj = Mat4PerspectiveFovLH(kFovY, aspect, kNearZ, kFarZ);
    camera_.projInverse = Mat4PerspectiveFovLHInverse(camera_.proj);
    camera_.fovY = kFovY;
    camera_.nearZ = kNearZ;
    camera_.farZ = kFarZ;
    camera_.aspect = aspect;
    camera_.valid = true;
    const Mat4 jitteredProj = Mat4ApplyJitter(camera_.proj, jitterX, jitterY, width, height);
    const Mat4 viewProj = Mat4Mul(camera_.view, jitteredProj);

    const float sweep = 2.6f * std::sin(timeSeconds * 0.55f);
    const float bob = 0.85f + 0.18f * std::sin(timeSeconds * 1.7f);
    const Mat4 cubeModel =
        Mat4Mul(Mat4Mul(Mat4RotationX(timeSeconds * 0.7f), Mat4RotationY(timeSeconds * 0.9f)),
                Mat4Translation(sweep, bob, 1.4f));
    const Mat4 floorModel = Mat4Identity();
    const float fogFar = 70.0f;

    const UINT floorSlot = frameIndex * kObjectsPerFrame + 0;
    const UINT cubeSlot = frameIndex * kObjectsPerFrame + 1;

    SceneConstants floorCB;
    std::memcpy(floorCB.curModel, floorModel.m, sizeof(floorModel.m));
    std::memcpy(floorCB.prevModel, floorModel.m, sizeof(floorModel.m));
    std::memcpy(floorCB.viewProj, viewProj.m, sizeof(viewProj.m));
    floorCB.params[0] = 1.0f;
    floorCB.params[1] = fogFar;
    floorCB.params[2] = 0.0f;
    floorCB.params[3] = 0.0f;
    std::memcpy(constantPtr_[floorSlot], &floorCB, sizeof(floorCB));

    SceneConstants cubeCB;
    std::memcpy(cubeCB.curModel, cubeModel.m, sizeof(cubeModel.m));
    std::memcpy(cubeCB.prevModel, (hasPrev_ ? prevCubeModel_.m : cubeModel.m), sizeof(cubeModel.m));
    std::memcpy(cubeCB.viewProj, viewProj.m, sizeof(viewProj.m));
    cubeCB.params[0] = 0.0f;
    cubeCB.params[1] = fogFar;
    cubeCB.params[2] = 0.0f;
    cubeCB.params[3] = 0.0f;
    std::memcpy(constantPtr_[cubeSlot], &cubeCB, sizeof(cubeCB));
    prevCubeModel_ = cubeModel;
    hasPrev_ = true;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {colorRtv, aux.MotionRtv()};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = aux.DepthDsv();
    commandList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Sky (fullscreen triangle; ignores the bound CB, no depth).
    commandList->SetPipelineState(skyPso_.Get());
    commandList->SetGraphicsRootConstantBufferView(0, constantBuffers_[floorSlot]->GetGPUVirtualAddress());
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->DrawInstanced(3, 1, 0, 0);

    // Floor + cube (depth-tested geometry).
    commandList->SetPipelineState(scenePso_.Get());
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList->IASetIndexBuffer(&indexBufferView_);
    commandList->SetGraphicsRootConstantBufferView(0, constantBuffers_[floorSlot]->GetGPUVirtualAddress());
    commandList->DrawIndexedInstanced(floorIndexCount_, 1, 0, 0, 0);
    commandList->SetGraphicsRootConstantBufferView(0, constantBuffers_[cubeSlot]->GetGPUVirtualAddress());
    commandList->DrawIndexedInstanced(cubeIndexCount_, 1, floorIndexCount_, cubeBaseVertex_, 0);
}

inline void SceneRenderer::Release() {
    for (UINT i = 0; i < kFrameCount * kObjectsPerFrame; ++i) {
        if (constantBuffers_[i] && constantPtr_[i]) {
            constantBuffers_[i]->Unmap(0, nullptr);
        }
        constantPtr_[i] = nullptr;
        constantBuffers_[i].Reset();
    }
    vertexBuffer_.Reset();
    indexBuffer_.Reset();
    skyPso_.Reset();
    scenePso_.Reset();
    rootSignature_.Reset();
    vertexBufferView_ = {};
    indexBufferView_ = {};
    floorIndexCount_ = 0;
    cubeIndexCount_ = 0;
    cubeBaseVertex_ = 0;
    camera_ = {};
    colorFormat_ = kColorFormat;
    hasPrev_ = false;
    valid_ = false;
}

}  // namespace testapp::dx12fg

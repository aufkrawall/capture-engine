#pragma once

// Test-only stand-in for FidelityFX's no-callback UI composition. The caller owns
// ordering and waits for the recorded list before rewriting these descriptors.
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <ffx_api.h>
#include <ffx_api_types.h>

namespace ce::flow::fake {

class FFXUiCompositor {
public:
    static bool Supports(uint32_t state) {
        return state == FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ || state == FFX_API_RESOURCE_STATE_PIXEL_READ ||
               state == FFX_API_RESOURCE_STATE_COMPUTE_READ;
    }
    static D3D12_RESOURCE_STATES NativeState(uint32_t state) {
        return state == FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ
            ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : (state == FFX_API_RESOURCE_STATE_PIXEL_READ ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    bool Initialize(ID3D12Device* device, DXGI_FORMAT targetFormat) {
        signature_.Reset();
        straight_.Reset();
        premultiplied_.Reset();
        srv_.Reset();
        rtv_.Reset();
        device_ = device;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, signatureErrors;
        if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &signatureErrors)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&signature_))))
            return false;
        constexpr char shader[] = R"(
Texture2D<float4> ui : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    uint width, height; ui.GetDimensions(width, height);
    uint2 p = uint2(position.xy);
    return p.x < width && p.y < height ? ui.Load(int3(p, 0)) : float4(0, 0, 0, 0);
})";
        const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        struct CompilerGuard {
            HMODULE module;
            ~CompilerGuard() { if (module) FreeLibrary(module); }
        } compilerGuard{compiler};
        const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"))
                                      : nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
        const bool compiled = compile &&
            SUCCEEDED(compile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors)) &&
            SUCCEEDED(compile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors));
        if (!compiled)
            return false;
        constexpr D3D12_RENDER_TARGET_BLEND_DESC kDefaultBlend = {
            FALSE,           FALSE,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
        constexpr D3D12_DEPTH_STENCILOP_DESC kDefaultStencilOp = {
            D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        constexpr D3D12_RASTERIZER_DESC kDefaultRasterizer = {
            D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, D3D12_DEFAULT_DEPTH_BIAS, 0.0f, 0.0f, TRUE, FALSE,
            FALSE,                 0,                    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
        constexpr D3D12_DEPTH_STENCIL_DESC kDefaultDepthStencil = {
            FALSE,           D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS, FALSE,
            D3D12_DEFAULT_STENCIL_READ_MASK, D3D12_DEFAULT_STENCIL_WRITE_MASK,
            kDefaultStencilOp, kDefaultStencilOp};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {
            signature_.Get(),
            {vs->GetBufferPointer(), vs->GetBufferSize()},
            {ps->GetBufferPointer(), ps->GetBufferSize()},
            {},
            {},
            {},
            {},
            {FALSE,
             FALSE,
             {kDefaultBlend, kDefaultBlend, kDefaultBlend, kDefaultBlend, kDefaultBlend, kDefaultBlend, kDefaultBlend,
              kDefaultBlend}},
            UINT_MAX,
            kDefaultRasterizer,
            kDefaultDepthStencil,
            {},
            D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            1,
            {targetFormat},
            DXGI_FORMAT_UNKNOWN,
            {1, 0},
            0,
            {},
            D3D12_PIPELINE_STATE_FLAG_NONE};
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&straight_))))
            return false;
        blend.SrcBlend = D3D12_BLEND_ONE;
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&premultiplied_))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = 1;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_))))
            return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        return SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_)));
    }

    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* ui, ID3D12Resource* output, uint32_t state,
                bool premultiplied) {
        const D3D12_RESOURCE_STATES uiState = NativeState(state);
        Transition(list, ui, uiState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ui->GetDesc().Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(ui, &view, srv_->GetCPUDescriptorHandleForHeapStart());
        const auto rtv = rtv_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(output, nullptr, rtv);
        const auto desc = output->GetDesc();
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
        D3D12_RECT rect{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &rect);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {srv_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(signature_.Get());
        list->SetPipelineState(premultiplied ? premultiplied_.Get() : straight_.Get());
        list->SetGraphicsRootDescriptorTable(0, srv_->GetGPUDescriptorHandleForHeapStart());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        Transition(list, ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, uiState);
    }

private:
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> straight_, premultiplied_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, rtv_;
};

}  // namespace ce::flow::fake
